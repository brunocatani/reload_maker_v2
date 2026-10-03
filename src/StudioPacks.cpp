#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>
#include <QUrl>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showConfigurationSaver() {
    QDialog dialog(_window);dialog.setWindowTitle("Save configured reload to pack");dialog.resize(840,600);auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Save replaces this receiver / magazine / operating-part configuration. Other configurations stay intact. Magazine hand fits and models are shared by every configuration using that magazine record.");intro->setWordWrap(true);layout->addWidget(intro);
    QSettings settings;
    auto field=[&](const QString& title,const QString& value,bool output){layout->addWidget(new QLabel(title));auto* row=new QHBoxLayout;layout->addLayout(row);auto* edit=new QLineEdit(value);row->addWidget(edit,1);auto* browse=new QPushButton("Choose…");row->addWidget(browse);
        connect(browse,&QPushButton::clicked,&dialog,[&,edit,output]{const auto path=output?QFileDialog::getSaveFileName(&dialog,"New or existing pack project",studioDataPath("workspace/packs"),"Pack project (*.json)",nullptr,QFileDialog::DontConfirmOverwrite):QFileDialog::getOpenFileName(&dialog,"Finished magazine model with cook receipt",studioDataPath("exports/objects"),"Cooked magazine (*.nif)");if(!path.isEmpty())edit->setText(path);});return edit;};
    const auto weaponKey=currentWeapon()?poseRecordKey(currentWeapon()->formId).toLower():QString("unassigned");
    const auto plugin=weaponKey.section(':',0,0),context=QFileInfo(suggestedSessionPath()).dir().dirName();const auto setting="packAuthoring/"+weaponKey+"/"+context+"/project";
    const auto destination=studioDataPath("workspace/packs/"+plugin+"/"+weaponKey.section(':',1,1)+"/"+context+"/pack.project.json");
    auto* project=field("Pack project",settings.value(setting,destination).toString(),true);
    layout->addWidget(new QLabel("Namespace for a new pack"));auto* id=new QLineEdit;layout->addWidget(id);
    if(currentWeapon()){auto slug=QFileInfo(plugin).completeBaseName();slug.replace(QRegularExpression("[^a-z0-9_-]+",QRegularExpression::CaseInsensitiveOption),"-");id->setText("reload."+slug.left(48)+"."+weaponKey.section(':',1,1)+"."+context.left(8));}
    auto* full=field("Full model · leave both blank to keep the existing assignment",{},false);
    auto* empty=field("Empty model",{},false);
    auto* report=new QPlainTextEdit;report->setReadOnly(true);layout->addWidget(report,1);
    QString expected,ownedJob;const auto inspect=[&]{QFile file(project->text());expected.clear();ownedJob.clear();if(file.exists()){if(file.open(QIODevice::ReadOnly))expected=QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());id->setEnabled(false);}else id->setEnabled(true);};
    connect(project,&QLineEdit::textChanged,&dialog,inspect);inspect();
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* save=new QPushButton("Save selected configuration");auto* cancel=new QPushButton("Cancel job");actions->addWidget(save);actions->addWidget(cancel);
    const auto update=[&]{const auto state=packCommand({});const bool running=state["phase"]=="running"||state["phase"]=="cancelling";save->setEnabled(!running);cancel->setEnabled(running);
        if(state["operation"]!="save-configuration"||state["id"].toString()!=ownedJob)return;
        if(running)report->setPlainText("Validating live motion, both hand fits, source identities and finished magazine models…");
        else if(state["phase"]=="failed")report->setPlainText(state["error"].toString());
        else{const auto result=state["result"].toObject();expected=result["sha256"].toString();report->setPlainText(QString("Saved revision %1.\n%2 configurations · %3 magazines\n\n%4\n\nUse Export complete reload pack to publish this project.").arg(result["revision"].toInt()).arg(result["variants"].toInt()).arg(result["magazines"].toInt()).arg(result["project"].toString()));}
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    connect(save,&QPushButton::clicked,&dialog,[&]{try{settings.setValue(setting,project->text());settings.setValue("packExporter/project",project->text());ownedJob=saveConfiguredReload({{"project",project->text()},{"expected",expected},{"id",id->text()},{"full",full->text()},{"empty",empty->text()}})["id"].toString();update();}catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(cancel,&QPushButton::clicked,&dialog,[&]{packCommand({{"operation","cancel"}});});
    update();dialog.exec();
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
    auto* inspect=new QPushButton("Check project");auto* sourceCheck=new QPushButton("Check loaded configuration sources");auto* publish=new QPushButton("Export complete pack");
    auto* cancel=new QPushButton("Cancel job");auto* open=new QPushButton("Open exported folder");
    for(auto* button:{inspect,sourceCheck,publish,cancel,open})actions->addWidget(button);
    QString exported,sourceJob;
    const auto update=[&]{
        const auto state=packCommand({});const auto phase=state["phase"].toString();
        const bool running=phase=="running"||phase=="cancelling";
        inspect->setEnabled(!running);sourceCheck->setEnabled(!running&&currentWeapon());publish->setEnabled(!running);cancel->setEnabled(running);open->setEnabled(!exported.isEmpty());
        if(phase=="idle")report->setPlainText("Prepared pack projects are organized by source ESP and weapon under workspace/packs. Each export contains full/empty objects and all included configurations. Original weapon plugins remain dependencies.");
        else if(running)report->setPlainText(phase=="cancelling"?"Cancellation requested. Waiting for the native tool to finish…":"Checking frozen authoring inputs and finished models…");
        else if(phase=="failed")report->setPlainText(state["error"].toString());
        else if(state["operation"]=="inspect-project") {
            const auto result=state["result"].toObject();QString text=result["name"].toString()+"\n";
            for(const auto& value:result["weapons"].toArray()) {const auto weapon=value.toObject();text+=QString("\n%1 · %2\n%3 configurations · %4 magazines\n%5\n").arg(weapon["name"].toString(),weapon["record"].toString()).arg(weapon["variants"].toInt()).arg(weapon["magazines"].toInt()).arg(weapon["template"].toString());}
            if(!result["sourceConflicts"].toArray().isEmpty())text+="\nSome configurations were captured from different source versions. Review those inputs before publishing.";
            else text+="\nReady to validate native collision and publish.";
            if(state["id"].toString()==sourceJob)try{text+=loadedConfigurationSourceReport(result);}catch(const std::exception& e){text+="\n\n"+QString::fromUtf8(e.what());}
            report->setPlainText(text);
        } else if(state["operation"]=="export") {
            const auto result=state["result"].toObject();exported=result["output"].toString();open->setEnabled(!exported.isEmpty());
            report->setPlainText(QString("Export complete: %1 configurations, %2 magazine variations, %3 full/empty objects.\n\n%4\n\nThis folder is the shareable pack. Install it with its source mods, then use Build installed reload packs to generate the local object ESP. No game files or MO2 settings were changed.").arg(result["variants"].toInt()).arg(result["magazines"].toInt()).arg(result["objects"].toInt()).arg(QDir::toNativeSeparators(exported)));
        } else report->setPlainText("Choose a prepared pack project and check its coverage before export.");
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    const auto run=[&](const QString& operation){settings.setValue("packExporter/project",project->text());try{packCommand({{"operation",operation},{"project",project->text()},{"output",output->text()}});update();}catch(const std::exception& e){report->setPlainText(e.what());}};
    connect(inspect,&QPushButton::clicked,&dialog,[&]{run("inspect-project");});
    connect(sourceCheck,&QPushButton::clicked,&dialog,[&]{try{sourceJob=packCommand({{"operation","inspect-project"},{"project",project->text()}})["id"].toString();update();}catch(const std::exception& e){report->setPlainText(e.what());}});
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
            if(state["operation"]=="build") {
                text=QString("Build complete.\n%1 objects from the selected packs.\n\nGenerated ESP and bindings:\n%2\n\nUse this folder alongside the authored packs. You control activation and load order in MO2.").arg(result["activeObjects"].toInt()).arg(QDir::toNativeSeparators(result["output"].toString()));
            }
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
