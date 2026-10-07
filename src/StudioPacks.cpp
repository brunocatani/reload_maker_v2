#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "InstallationSetup.h"
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
        else{const auto result=state["result"].toObject();report->setPlainText(QString("Saved revision %1.\n%2 configurations · %3 magazines\n\n%4\n\nUse MENU > Export > Export current reload to publish this project.").arg(result["revision"].toInt()).arg(result["variants"].toInt()).arg(result["magazines"].toInt()).arg(result["project"].toString()));}
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    connect(save,&QPushButton::clicked,&dialog,[&]{try{settings.setValue(setting,project->text());settings.setValue("packExporter/project",project->text());ownedJob=saveConfiguredReload({{"project",project->text()},{"id",id->text()},{"full",full->text()},{"empty",empty->text()}})["id"].toString();update();}catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(saveAll,&QPushButton::clicked,&dialog,[&]{try{settings.setValue(setting,project->text());settings.setValue("packExporter/project",project->text());results->clear();ownedJob=startSaveAllConfigurations({{"project",project->text()}})["id"].toString();update();}catch(const std::exception& e){report->setPlainText(e.what());}});
    connect(cancel,&QPushButton::clicked,&dialog,[&]{packCommand({{"operation","cancel"}});});
    update();dialog.exec();
}

void ReloadMakerController::showPackExporter() {
    QSettings settings;
    QString project;
    if(currentWeapon()) {
        const auto key=poseRecordKey(currentWeapon()->formId).toLower(),context=QFileInfo(suggestedSessionPath()).dir().dirName();
        const auto destination=studioDataPath("workspace/packs/"+key.section(':',0,0)+"/"+key.section(':',1,1)+"/"+context+"/pack.project.json");
        project=settings.value("packAuthoring/"+key+"/"+context+"/project",destination).toString();
    }
    settings.setValue("packBuilder/packs",QFileInfo(project).isFile()?QStringList{project}:QStringList{});
    showPackBuilder();
}

void ReloadMakerController::showPackBuilder() {
    QDialog dialog(_window);dialog.setObjectName("ReloadExportDialog");dialog.setWindowTitle("Export reload mods");dialog.resize(850,690);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel("Select saved weapon projects or finished packs. Each source ESP/ESM gets one ESL-flagged ESP and a matching magazine-mesh folder. Export the complete selection to one MO2 mod or one archive. Source-group updates retain previously exported sibling weapons. Enable only the ESL-flagged ESPs for the source mods you use.");
    intro->setWordWrap(true);layout->addWidget(intro);
    QSettings settings;
    auto* list=new QListWidget;list->addItems(settings.value("packBuilder/packs").toStringList());layout->addWidget(list,1);
    auto* choices=new QHBoxLayout;layout->addLayout(choices);
    auto* projects=new QPushButton("Add saved projects...");auto* packs=new QPushButton("Add finished pack folder...");auto* remove=new QPushButton("Remove selected");
    for(auto* button:{projects,packs,remove})choices->addWidget(button);
    const auto add=[&](const QString& path){if(!path.isEmpty()&&list->findItems(path,Qt::MatchExactly).isEmpty())list->addItem(path);};
    connect(projects,&QPushButton::clicked,&dialog,[&]{for(const auto& path:QFileDialog::getOpenFileNames(&dialog,"Saved reload projects",studioDataPath("workspace/packs"),"Pack projects (*.json)"))add(path);});
    connect(packs,&QPushButton::clicked,&dialog,[&]{add(QFileDialog::getExistingDirectory(&dialog,"Finished reload packs",studioDataPath("exports")));});
    connect(remove,&QPushButton::clicked,&dialog,[&]{delete list->takeItem(list->currentRow());});
    layout->addWidget(new QLabel("MO2 destination - one mod folder"));
    auto* destinationRow=new QHBoxLayout;layout->addLayout(destinationRow);
    const auto setup=loadInstallationSetup();
    const auto instance=QFileInfo(setup.mo2Instance).isFile()?QFileInfo(setup.mo2Instance).absolutePath():setup.mo2Instance;
    const auto defaultMod=instance.isEmpty()?QString():QDir(instance).filePath("mods/DIAMOND Reload Packs");
    auto* mo2=new QLineEdit(settings.value("packBuilder/mo2",defaultMod).toString());destinationRow->addWidget(mo2,1);
    auto* browse=new QPushButton("Choose mods folder...");destinationRow->addWidget(browse);
    connect(browse,&QPushButton::clicked,&dialog,[&]{const auto folder=QFileDialog::getExistingDirectory(&dialog,"MO2 mods folder",instance);if(!folder.isEmpty())mo2->setText(QDir(folder).filePath("DIAMOND Reload Packs"));});
    layout->addWidget(new QLabel("Saved export identity - keep this folder between exports"));
    auto* ledger=new QLineEdit(settings.value("packBuilder/installation",studioDataPath("workspace/installations/default")).toString());layout->addWidget(ledger);
    auto* report=new QPlainTextEdit;report->setReadOnly(true);layout->addWidget(report,1);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* toMo2=new QPushButton("Export to MO2");auto* toArchive=new QPushButton("Export ZIP / 7z");auto* cancel=new QPushButton("Cancel export");
    for(auto* button:{toMo2,toArchive,cancel})actions->addWidget(button);
    const auto update=[&]{
        const auto state=packCommand({});const auto phase=state["phase"].toString();const bool running=phase=="running"||phase=="cancelling";
        for(auto* button:{toMo2,toArchive,projects,packs,remove})button->setEnabled(!running);
        cancel->setEnabled(running);
        if(phase=="idle")report->setPlainText("The export includes reload behavior, ESL-flagged ESPs and finished assets. Fallout 4 VR requires ESL support such as Daytripper. MO2 activation and load order are never changed.");
        else if(running)report->setPlainText("Preparing selected reloads, building source-plugin ESL-flagged ESPs and validating the complete output...");
        else if(phase=="failed")report->setPlainText("Export failed: "+state["error"].toString());
        else if(state["operation"]=="build") {
            const auto result=state["result"].toObject();
            QString text=QString("Export complete: %1 ESL-flagged ESPs, %2 magazine objects\n\n%3\n").arg(result["plugins"].toArray().size()).arg(result["activeObjects"].toInt()).arg(result["installedTo"].toString(result["archive"].toString(result["output"].toString())));
            for(const auto& value:result["plugins"].toArray()){const auto group=value.toObject();text+="\n"+group["master"].toString()+" -> "+group["plugin"].toString();}
            report->setPlainText(text+"\n\nEnable the desired ESL-flagged ESPs in MO2. Source plugins and profile settings were not changed.");
        }
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    const auto run=[&](const QString& destination,const QString& key){
        if(destination.isEmpty())return;
        QJsonArray inputs;QStringList saved;for(int i=0;i<list->count();++i){inputs.append(list->item(i)->text());saved.append(list->item(i)->text());}
        settings.setValue("packBuilder/packs",saved);settings.setValue("packBuilder/installation",ledger->text());settings.setValue("packBuilder/mo2",mo2->text());
        try{packCommand({{"operation","build"},{"packs",inputs},{"installation",ledger->text()},{"initialize",!QFileInfo(QDir(ledger->text()).filePath("current.json")).exists()},{key,destination}});update();}
        catch(const std::exception& e){report->setPlainText(e.what());}
    };
    connect(toMo2,&QPushButton::clicked,&dialog,[&]{if(mo2->text().trimmed().isEmpty()){report->setPlainText("Choose your MO2 mods folder first.");return;}run(mo2->text(),"mo2");});
    connect(toArchive,&QPushButton::clicked,&dialog,[&]{
        QString filter;auto file=QFileDialog::getSaveFileName(&dialog,"Export reload mods",studioDataPath("exports/DIAMOND Reload Packs.zip"),"ZIP archive (*.zip);;7-Zip archive (*.7z)",&filter);
        if(!file.isEmpty()&&filter.startsWith("7-Zip")&&!file.endsWith(".7z",Qt::CaseInsensitive))file=QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName()+".7z");
        if(!file.isEmpty()&&QFileInfo(file).suffix().isEmpty())file+=filter.startsWith("7-Zip")?".7z":".zip";
        run(file,"archive");
    });
    connect(cancel,&QPushButton::clicked,&dialog,[&]{try{packCommand({{"operation","cancel"}});}catch(const std::exception& e){report->setPlainText(e.what());}});
    update();dialog.exec();
}
}
