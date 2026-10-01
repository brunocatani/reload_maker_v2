#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QUuid>
#include <QVBoxLayout>
#include <QUrl>
#include <memory>
#include <stdexcept>

namespace reloadmaker {
QJsonObject ReloadMakerController::packCommand(const QJsonObject& request) {
    const auto operation=request.value("operation").toString("state");
    if(operation=="state")return _packJob.isEmpty()?QJsonObject{{"phase","idle"}}:_packJob;
    if(operation=="cancel") {
        if(_packProcess) {
            QFile cancel(_packCancelFile);
            if(!cancel.open(QIODevice::WriteOnly))throw std::runtime_error("Cannot request cancellation");
            cancel.close();_packJob["phase"]="cancelling";emit packJobChanged();
        }
        return _packJob;
    }
    if(_packProcess)throw std::runtime_error("A pack job is already running");
    const bool authoring=operation=="export"||operation=="inspect-project";
    if(operation!="build"&&operation!="validate"&&!authoring)throw std::runtime_error("Unknown pack operation");
    const auto packs=request["packs"].toArray();
    if(!authoring&&(packs.isEmpty()||packs.size()>256))throw std::runtime_error("Select between one and 256 finished pack folders");
    const QDir application(QCoreApplication::applicationDirPath());
    const auto python=application.filePath("runtime/python/python.exe");
    const auto entry=application.filePath("tools/product_cli.py");
    if(!QFileInfo::exists(python)||!QFileInfo::exists(entry))throw std::runtime_error("The bundled object builder is missing. Install the complete Reload Maker tool package.");
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    _packCancelFile=studioDataPath("local/tmp/pack-"+id+".cancel");
    QStringList arguments{"-B","-I",entry,"--workspace",studioRoot(),"--cancel-file",_packCancelFile,"packs",operation};
    if(authoring) {
        const auto project=request["project"].toString();
        if(!QFileInfo(project).isFile())throw std::runtime_error("Select an authored pack project");
        arguments<<"--project"<<project;
        if(operation=="export") {
            const auto output=request["output"].toString();
            if(output.isEmpty())throw std::runtime_error("Choose a new local export folder");
            arguments<<"--output"<<output;
        }
    } else for(const auto& pack:packs){if(!pack.isString()||!QFileInfo(pack.toString()).isDir())throw std::runtime_error("A selected pack folder is missing");arguments<<"--pack"<<pack.toString();}
    if(operation=="build") {
        const auto installation=request["installation"].toString();
        if(installation.isEmpty())throw std::runtime_error("Select the installation ledger folder");
        arguments<<"--installation"<<installation;
        if(request["initialize"].toBool())arguments<<"--initialize";
    }
    auto* process=new QProcess(this);_packProcess=process;
    process->setProgram(python);process->setArguments(arguments);process->setWorkingDirectory(studioRoot());
    _packJob={{"id",id},{"phase","running"},{"operation",operation},{"packs",packs}};
    auto output=std::make_shared<QByteArray>();
    const auto collect=[process,output]{output->append(process->readAllStandardOutput());if(output->size()>2*1024*1024)output->remove(0,output->size()-2*1024*1024);};
    connect(process,&QProcess::readyReadStandardOutput,this,collect);
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){_packJob["phase"]="failed";_packJob["error"]=process->errorString();_packProcess=nullptr;process->deleteLater();emit packJobChanged();}
    });
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,output,collect](int code,QProcess::ExitStatus status){
        collect();const auto lines=output->trimmed().split('\n');const auto result=QJsonDocument::fromJson(lines.isEmpty()?QByteArray():lines.back()).object();
        const bool success=code==0&&status==QProcess::NormalExit&&result["ok"].toBool();
        _packJob["phase"]=success?"complete":"failed";_packJob["result"]=result;
        if(!success)_packJob["error"]=result.value("error").toString(QString::fromUtf8(process->readAllStandardError()).right(8192));
        QFile::remove(_packCancelFile);_packProcess=nullptr;process->deleteLater();emit packJobChanged();
    });
    process->start();emit packJobChanged();return _packJob;
}

void ReloadMakerController::showPackExporter() {
    QDialog dialog(_window);dialog.setWindowTitle("Export complete reload pack");dialog.resize(820,620);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Publish prepared weapon behavior, magazine fits, sounds, grab points and finished collision models together. Select a pack project to inspect its exact coverage before export.");
    intro->setWordWrap(true);layout->addWidget(intro);
    QSettings settings;
    auto* row=new QHBoxLayout;layout->addLayout(row);
    auto* project=new QLineEdit(settings.value("packExporter/project").toString());row->addWidget(project,1);
    auto* browse=new QPushButton("Choose pack project…");row->addWidget(browse);
    connect(browse,&QPushButton::clicked,&dialog,[&]{const auto file=QFileDialog::getOpenFileName(&dialog,"Authored pack project",studioDataPath("workspace/packs"),"Pack project (*.json)");if(!file.isEmpty())project->setText(file);});
    layout->addWidget(new QLabel("New export folder"));
    auto* output=new QLineEdit(studioDataPath("exports/packs/export-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")));layout->addWidget(output);
    auto* report=new QPlainTextEdit;report->setReadOnly(true);layout->addWidget(report,1);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* inspect=new QPushButton("Check project");auto* publish=new QPushButton("Export complete pack");
    auto* cancel=new QPushButton("Cancel job");auto* open=new QPushButton("Open exported folder");
    for(auto* button:{inspect,publish,cancel,open})actions->addWidget(button);
    QString exported;
    const auto update=[&]{
        const auto state=packCommand({});const auto phase=state["phase"].toString();
        const bool running=phase=="running"||phase=="cancelling";
        inspect->setEnabled(!running);publish->setEnabled(!running);cancel->setEnabled(running);open->setEnabled(!exported.isEmpty());
        if(phase=="idle")report->setPlainText("Prepared pack projects are organized by source ESP and weapon under workspace/packs. Each export contains full/empty objects and all included configurations. Original weapon plugins remain dependencies.");
        else if(running)report->setPlainText(phase=="cancelling"?"Cancellation requested. Waiting for the native tool to finish…":"Checking frozen authoring inputs and finished models…");
        else if(phase=="failed")report->setPlainText(state["error"].toString());
        else if(state["operation"]=="inspect-project") {
            const auto result=state["result"].toObject();QString text=result["name"].toString()+"\n";
            for(const auto& value:result["weapons"].toArray()) {const auto weapon=value.toObject();text+=QString("\n%1 · %2\n%3 configurations · %4 magazines\n%5\n").arg(weapon["name"].toString(),weapon["record"].toString()).arg(weapon["variants"].toInt()).arg(weapon["magazines"].toInt()).arg(weapon["template"].toString());}
            report->setPlainText(text+"\nReady to validate native collision and publish.");
        } else if(state["operation"]=="export") {
            const auto result=state["result"].toObject();exported=result["output"].toString();open->setEnabled(!exported.isEmpty());
            report->setPlainText(QString("Export complete: %1 configurations, %2 magazine variations, %3 full/empty objects.\n\n%4\n\nThis folder is the shareable pack. Install it with its source mods, then use Build installed reload packs to generate the local object ESP. No game files or MO2 settings were changed.").arg(result["variants"].toInt()).arg(result["magazines"].toInt()).arg(result["objects"].toInt()).arg(QDir::toNativeSeparators(exported)));
        } else report->setPlainText("Choose a prepared pack project and check its coverage before export.");
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    const auto run=[&](const QString& operation){settings.setValue("packExporter/project",project->text());try{packCommand({{"operation",operation},{"project",project->text()},{"output",output->text()}});update();}catch(const std::exception& e){report->setPlainText(e.what());}};
    connect(inspect,&QPushButton::clicked,&dialog,[&]{run("inspect-project");});
    connect(publish,&QPushButton::clicked,&dialog,[&]{run("export");});
    connect(cancel,&QPushButton::clicked,&dialog,[&]{try{packCommand({{"operation","cancel"}});}catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(open,&QPushButton::clicked,&dialog,[&]{QDesktopServices::openUrl(QUrl::fromLocalFile(exported));});
    update();dialog.exec();
}

void ReloadMakerController::showPackBuilder() {
    QDialog dialog(_window);dialog.setWindowTitle("Build installed reload packs");dialog.resize(800,640);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Select finished packs to generate one local object ESP. Collision is already inside their models; Creation Kit is not needed. Keep one ledger folder for this game installation across all profiles.");intro->setWordWrap(true);layout->addWidget(intro);
    auto* list=new QListWidget;layout->addWidget(list);
    QSettings settings;list->addItems(settings.value("packBuilder/packs").toStringList());
    auto* files=new QHBoxLayout;layout->addLayout(files);
    auto* add=new QPushButton("Add pack folder");auto* remove=new QPushButton("Remove selected");files->addWidget(add);files->addWidget(remove);
    connect(add,&QPushButton::clicked,&dialog,[&]{const auto folder=QFileDialog::getExistingDirectory(&dialog,"Finished reload pack",studioDataPath("exports"));if(!folder.isEmpty()&&list->findItems(folder,Qt::MatchExactly).isEmpty())list->addItem(folder);});
    connect(remove,&QPushButton::clicked,&dialog,[&]{delete list->takeItem(list->currentRow());});
    layout->addWidget(new QLabel("Installation ledger folder"));
    auto* row=new QHBoxLayout;layout->addLayout(row);
    auto* ledger=new QLineEdit(settings.value("packBuilder/installation",studioDataPath("workspace/installations/default")).toString());row->addWidget(ledger,1);
    auto* browse=new QPushButton("Browse…");row->addWidget(browse);
    connect(browse,&QPushButton::clicked,&dialog,[&]{const auto folder=QFileDialog::getExistingDirectory(&dialog,"Installation ledger",ledger->text());if(!folder.isEmpty())ledger->setText(folder);});
    auto* report=new QPlainTextEdit;report->setReadOnly(true);layout->addWidget(report,1);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* validate=new QPushButton("Validate packs");auto* build=new QPushButton("Rebuild installation");auto* initialize=new QPushButton("Create new installation");auto* cancel=new QPushButton("Cancel job");
    for(auto* button:{validate,build,initialize,cancel})actions->addWidget(button);
    const auto update=[&]{const auto state=packCommand({});const auto phase=state["phase"].toString();const bool running=phase=="running"||phase=="cancelling";
        validate->setEnabled(!running);build->setEnabled(!running);initialize->setEnabled(!running);cancel->setEnabled(running);
        QString text;
        if(phase=="idle")text="Add the finished packs you want to use, then validate or build. Generated output stays separate from your game and original weapon plugins.";
        else if(phase=="running")text="Processing selected packs…\nYou can cancel; the last successful installation will be retained.";
        else if(phase=="cancelling")text="Cancellation requested. Waiting for the current native tool to finish safely…";
        else if(phase=="failed")text="Build did not complete.\n"+state["error"].toString();
        else {
            const auto result=state["result"].toObject();
            if(state["operation"]=="build")text=QString("Build complete.\n%1 objects from the selected packs.\n\nInstall the authored packs and this generated companion folder:\n%2\n\nKeep its ESP and object bindings together. This build has not changed MO2 enablement.").arg(result["activeObjects"].toInt()).arg(QDir::toNativeSeparators(result["output"].toString()));
            else if(state["operation"]=="validate")text=QString("Validated %1 pack(s). The content hashes and object definitions match their export.").arg(result["packs"].toArray().size());
            else text="Add the finished packs to include, then validate or build their local object ESP.";
        }
        report->setPlainText(text);};
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    const auto run=[&](const QString& operation,bool initial){QJsonArray packs;QStringList saved;for(int i=0;i<list->count();++i){packs.append(list->item(i)->text());saved.append(list->item(i)->text());}
        settings.setValue("packBuilder/packs",saved);settings.setValue("packBuilder/installation",ledger->text());
        try{packCommand({{"operation",operation},{"packs",packs},{"installation",ledger->text()},{"initialize",initial}});update();}
        catch(const std::exception& e){report->setPlainText(QString::fromUtf8(e.what()));}};
    connect(validate,&QPushButton::clicked,&dialog,[&]{run("validate",false);});
    connect(build,&QPushButton::clicked,&dialog,[&]{run("build",false);});
    connect(initialize,&QPushButton::clicked,&dialog,[&]{run("build",true);});
    connect(cancel,&QPushButton::clicked,&dialog,[&]{try{packCommand({{"operation","cancel"}});}catch(const std::exception& e){report->setPlainText(e.what());}});
    update();dialog.exec();
}
}
