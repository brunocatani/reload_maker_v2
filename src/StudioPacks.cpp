#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
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
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QUrl>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::showConfigurationSaver() {
    QDialog dialog(_window);dialog.setWindowTitle("Save configured reload to pack");dialog.resize(840,600);auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Save replaces this receiver / magazine / operating-part configuration. Other configurations stay intact. Magazine hand fits and models are shared by every configuration using that magazine record. Save all configurations does the same for every receiver, operating part and magazine choice of this weapon in one write; a new magazine needs one single save with its models first.");intro->setWordWrap(true);layout->addWidget(intro);
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
    // Save all results: one row per configuration; double-click opens its parts.
    auto* results=new QTreeWidget;results->setColumnCount(3);results->setHeaderLabels({"Configuration","Result","Reason"});results->setColumnWidth(0,330);results->setColumnWidth(1,90);results->setRootIsDecorated(false);results->hide();layout->addWidget(results,2);
    const auto partName=[this](const QString& record){if(const auto* config=configuration())for(const auto& slot:config->entries())for(const auto id:slot.choices)if(poseRecordKey(id).compare(record,Qt::CaseInsensitive)==0)if(const auto* mod=config->mod(id))return mod->name;return record;};
    QString ownedJob;const auto inspect=[&]{ownedJob.clear();id->setEnabled(!QFileInfo::exists(project->text()));};
    connect(project,&QLineEdit::textChanged,&dialog,inspect);inspect();
    auto* actions=new QHBoxLayout;layout->addLayout(actions);auto* save=new QPushButton("Save selected configuration");auto* saveAll=new QPushButton("Save all configurations");auto* cancel=new QPushButton("Cancel job");actions->addWidget(save);actions->addWidget(saveAll);actions->addWidget(cancel);
    const auto showAll=[&](const QJsonObject& state){
        const bool running=state["phase"]=="running"||state["phase"]=="cancelling";
        if(running){report->setPlainText(state["stage"]=="saving"?QString("Saving %1 captured configurations in one write…").arg(state["done"].toInt()-state["exceptions"].toArray().size()):QString("Assembling and capturing configuration %1 of %2…").arg(state["done"].toInt()+1).arg(state["total"].toInt()));return;}
        const auto result=state["result"].toObject();
        if(state["phase"]=="cancelled"){report->setPlainText("Cancelled. The pack project is unchanged.");return;}
        if(state["phase"]=="failed"){report->setPlainText(state["error"].toString());return;}
        report->setPlainText(QString("Saved %1 · up to date %2 · not saved %3 of %4 configurations.%5\nDouble-click a configuration to open its parts.").arg(result["saved"].toInt()).arg(result["upToDate"].toInt())
            .arg(result["failed"].toInt()+state["exceptions"].toArray().size()).arg(state["total"].toInt()).arg(result["saved"].toInt()?QString("\nRevision %1 · %2").arg(result["revision"].toInt()).arg(result["project"].toString()):QString()));
        results->clear();results->show();
        const auto add=[&](const QStringList& records,const QString& status,const QString& reason){QStringList names;for(const auto& r:records)names.append(partName(r));auto* row=new QTreeWidgetItem(results,{names.join(" · "),status,reason});row->setData(0,Qt::UserRole,records);row->setToolTip(2,reason);};
        for(const auto& value:state["exceptions"].toArray()){const auto e=value.toObject();QStringList records;for(const auto& p:e["parts"].toArray())records.append(p.toObject()["record"].toString());add(records,"not saved",e["error"].toString());}
        for(const auto& value:result["rows"].toArray()){const auto r=value.toObject(),s=r["selection"].toObject();QStringList records;for(const auto* k:{"receiver","operatingPart","magazine"})if(s.contains(QLatin1String(k))&&!records.contains(s[QLatin1String(k)].toString(),Qt::CaseInsensitive))records.append(s[QLatin1String(k)].toString());
            add(records,r["status"]=="failed"?QString("not saved"):r["status"].toString(),r["error"].toString());}
    };
    connect(results,&QTreeWidget::itemDoubleClicked,&dialog,[&,this](QTreeWidgetItem* item){if(selectConfigurationRecords(item->data(0,Qt::UserRole).toStringList()))report->setPlainText("Opened "+item->text(0)+". Fix it in Operate or the Reload builder, then save all configurations again.");});
    const auto update=[&]{const auto state=packCommand({});const bool running=state["phase"]=="running"||state["phase"]=="cancelling";save->setEnabled(!running);saveAll->setEnabled(!running);cancel->setEnabled(running);
        if(state["operation"]=="save-all-configurations"&&state["id"].toString()==ownedJob){showAll(state);return;}
        if(state["operation"]!="save-configuration"||state["id"].toString()!=ownedJob)return;
        if(running)report->setPlainText("Validating live motion, both hand fits and finished magazine models…");
        else if(state["phase"]=="failed")report->setPlainText(state["error"].toString());
        else{const auto result=state["result"].toObject();report->setPlainText(QString("Saved revision %1.\n%2 configurations · %3 magazines\n\n%4\n\nUse Export complete reload pack to publish this project.").arg(result["revision"].toInt()).arg(result["variants"].toInt()).arg(result["magazines"].toInt()).arg(result["project"].toString()));}
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    connect(save,&QPushButton::clicked,&dialog,[&]{try{settings.setValue(setting,project->text());settings.setValue("packExporter/project",project->text());ownedJob=saveConfiguredReload({{"project",project->text()},{"id",id->text()},{"full",full->text()},{"empty",empty->text()}})["id"].toString();update();}catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(saveAll,&QPushButton::clicked,&dialog,[&]{try{settings.setValue(setting,project->text());settings.setValue("packExporter/project",project->text());results->clear();ownedJob=startSaveAllConfigurations({{"project",project->text()}})["id"].toString();update();}catch(const std::exception& e){report->setPlainText(e.what());}});
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
    auto* inspect=new QPushButton("Check project");auto* publish=new QPushButton("Export complete pack");
    auto* cancel=new QPushButton("Cancel job");auto* open=new QPushButton("Open exported folder");
    for(auto* button:{inspect,publish,cancel,open})actions->addWidget(button);
    QString exported;
    // Weapons an export leaves out because their checklist is not complete.
    const auto skippedText=[](const QJsonObject& state,const QString& heading){const auto skipped=state["skipped"].toArray();if(skipped.isEmpty())return QString();
        QString text="\n\n"+heading;for(const auto& value:skipped){const auto w=value.toObject();text+=QString("\n  %1 · %2: %3").arg(w["name"].toString(),w["record"].toString(),w["reason"].toString());}
        return text+"\nComplete their checklists, then export again to include them.";};
    const auto update=[&]{
        const auto state=packCommand({});const auto phase=state["phase"].toString();
        const bool running=phase=="running"||phase=="cancelling";
        inspect->setEnabled(!running);publish->setEnabled(!running);cancel->setEnabled(running);open->setEnabled(!exported.isEmpty());
        if(phase=="idle")report->setPlainText("Prepared pack projects are organized by source ESP and weapon under workspace/packs. Each export contains full/empty objects and all included configurations. Original weapon plugins remain dependencies.");
        else if(running)report->setPlainText(phase=="cancelling"?"Cancellation requested. Waiting for the native tool to finish…":"Checking frozen authoring inputs and finished models…");
        else if(phase=="failed")report->setPlainText(state["error"].toString()+skippedText(state,"Left out:"));
        else if(state["operation"]=="inspect-project") {
            const auto result=state["result"].toObject();QString text=result["name"].toString()+"\n";
            for(const auto& value:result["weapons"].toArray()) {const auto weapon=value.toObject();text+=QString("\n%1 · %2\n%3 configurations · %4 magazines\n%5\n").arg(weapon["name"].toString(),weapon["record"].toString()).arg(weapon["variants"].toInt()).arg(weapon["magazines"].toInt()).arg(weapon["template"].toString());}
            text+="\nReady to validate native collision and publish.";
            text+=skippedText(state,"Export will leave out:");
            report->setPlainText(text);
        } else if(state["operation"]=="export") {
            const auto result=state["result"].toObject();exported=result["output"].toString();open->setEnabled(!exported.isEmpty());
            report->setPlainText(QString("Export complete: %1 configurations, %2 magazine variations, %3 full/empty objects.\n\n%4\n\nThis folder is the shareable pack. Install it with its source mods, then use Build installed reload packs to generate the local object ESP. No game files or MO2 settings were changed.").arg(result["variants"].toInt()).arg(result["magazines"].toInt()).arg(result["objects"].toInt()).arg(QDir::toNativeSeparators(exported))+skippedText(state,"Not exported:"));
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
