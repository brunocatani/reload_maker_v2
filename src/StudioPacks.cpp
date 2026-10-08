// Export reload mods to one MO2 mod folder or one archive: the ESP projects
// ticked in the list (a saved version of each, the latest by default), the
// ESP open in the picker, or everything the loaded source has saved work for.
// Export reads committed versions only; unsaved edits are named, not saved.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "InstallationSetup.h"
#include "areas/project/EspLibrary.h"
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
namespace {
QString localTime(const QString& utc) {
    return QDateTime::fromString(utc,Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
}
}

void ReloadMakerController::showPackBuilder(ExportScope scope) {
    const bool chosenList=scope==ExportScope::Chosen;
    if(scope==ExportScope::OpenEsp&&!_esp){setStatus("Open a weapon of the ESP to export first.",true);return;}
    QDialog dialog(_window);dialog.setObjectName("ReloadExportDialog");
    dialog.setWindowTitle(scope==ExportScope::Everything?"Export everything":scope==ExportScope::OpenEsp?"Export ESP reloads":"Export reload mods");
    dialog.resize(chosenList?900:720,chosenList?700:460);
    auto* layout=new QVBoxLayout(&dialog);
    auto* intro=new QLabel(
        scope==ExportScope::Everything?QString("Every saved ESP project with weapons in the loaded source: the reloads DIAMOND can use, one ESL-flagged ESP per source ESP with its magazine meshes, and the weapons' True Scopes profiles. Weapons the source does not contain are skipped and listed. Only the latest saved versions are exported."):
        scope==ExportScope::OpenEsp?QString("The reloads of %1's latest saved version: one ESL-flagged ESP with its magazine meshes.").arg(_esp->plugin):
        QString("Choose the source ESPs to export. Each exports its chosen saved version, the latest by default, as one ESL-flagged ESP with its magazine meshes. Only saved versions are exported."));
    intro->setWordWrap(true);layout->addWidget(intro);
    auto* never=new QLabel("MO2 activation and load order are never changed.");never->setObjectName("Hint");layout->addWidget(never);
    QSettings settings;
    const auto selected=settings.value("packBuilder/selected").toStringList();
    auto* list=new QTreeWidget;list->setColumnCount(4);list->setHeaderLabels({"Source ESP","Saved version","Weapons","Last export"});list->setRootIsDecorated(false);
    list->header()->setSectionResizeMode(0,QHeaderView::Stretch);list->setColumnWidth(1,260);list->setColumnWidth(2,150);list->setColumnWidth(3,170);layout->addWidget(list,1);
    list->setVisible(chosenList);
    const QDir root(espLibraryRoot());
    for(const auto& folder:root.entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
        if(espProjectKey(folder).isEmpty())continue;
        QString error;const auto index=readEspProject(folder,error);
        if(index.isEmpty()||index["saves"].toArray().isEmpty())continue;
        auto* item=new QTreeWidgetItem(list);item->setText(0,index["plugin"].toString());item->setData(0,Qt::UserRole,index["plugin"].toString());
        item->setCheckState(0,selected.contains(index["plugin"].toString(),Qt::CaseInsensitive)?Qt::Checked:Qt::Unchecked);
        auto* versions=new QComboBox;auto saves=index["saves"].toArray();
        for(int i=saves.size()-1;i>=0;--i){const auto save=saves[i].toObject();int ready=0,total=0;for(const auto& w:save["weapons"].toArray()){++total;ready+=w.toObject()["exportable"].toBool();}
            versions->addItem(QString("%1%2 · %3/%4 exportable").arg(localTime(save["savedAt"].toString()),save["id"]==index["latest"]?QStringLiteral(" (latest)"):QString()).arg(ready).arg(total),save["id"].toString());
            if(save["id"]==index["latest"]){item->setText(2,QString("%1 of %2 ready").arg(ready).arg(total));}}
        list->setItemWidget(item,1,versions);
        QFile published(QDir(espProjectFolder(index["plugin"].toString())).filePath("export/published.json"));
        if(published.open(QIODevice::ReadOnly)){const auto last=QJsonDocument::fromJson(published.readAll()).object()["publications"].toArray();
            if(!last.isEmpty()){const auto entry=last.last().toObject();item->setText(3,localTime(entry["exportedAt"].toString()));item->setToolTip(3,entry["destination"].toObject()["path"].toString());}}
        else item->setText(3,"Never");
    }
    auto* unsaved=new QHBoxLayout;layout->addLayout(unsaved);
    auto* unsavedText=new QLabel;unsavedText->setWordWrap(true);auto* saveNow=new QPushButton("Save now");unsaved->addWidget(unsavedText,1);unsaved->addWidget(saveNow);
    layout->addWidget(new QLabel("MO2 destination - one mod folder"));
    auto* destinationRow=new QHBoxLayout;layout->addLayout(destinationRow);
    const auto setup=loadInstallationSetup();
    const auto instance=QFileInfo(setup.mo2Instance).isFile()?QFileInfo(setup.mo2Instance).absolutePath():setup.mo2Instance;
    const auto defaultMod=instance.isEmpty()?QString():QDir(instance).filePath("mods/DIAMOND Reload Packs");
    auto* mo2=new QLineEdit(settings.value("packBuilder/mo2",defaultMod).toString());destinationRow->addWidget(mo2,1);
    auto* browse=new QPushButton("Choose mods folder...");destinationRow->addWidget(browse);
    connect(browse,&QPushButton::clicked,&dialog,[&]{const auto folder=QFileDialog::getExistingDirectory(&dialog,"MO2 mods folder",instance);if(!folder.isEmpty())mo2->setText(QDir(folder).filePath("DIAMOND Reload Packs"));});
    auto* report=new QPlainTextEdit;report->setReadOnly(true);layout->addWidget(report,1);
    if(!chosenList)report->setMinimumHeight(140);
    auto* actions=new QHBoxLayout;layout->addLayout(actions);
    auto* toMo2=new QPushButton("Export to MO2");auto* toArchive=new QPushButton("Export ZIP / 7z");auto* cancel=new QPushButton("Cancel export");
    for(auto* button:{toMo2,toArchive,cancel})actions->addWidget(button);
    const auto refreshUnsaved=[&]{const auto names=espDirtyWeapons();const bool show=_esp&&!names.isEmpty();
        unsavedText->setText(show?QString("Unsaved edits in %1 (%2) are not part of any saved version and will not be exported.").arg(_esp->plugin,names.join(", ")):QString());
        unsavedText->setVisible(show);saveNow->setVisible(show);};
    const auto update=[&]{
        const auto state=packCommand({});const auto phase=state["phase"].toString();const bool running=phase=="running"||phase=="cancelling";
        for(auto* button:{toMo2,toArchive,saveNow})button->setEnabled(!running);
        cancel->setEnabled(running);refreshUnsaved();
        if(phase=="idle")report->setPlainText("Fallout 4 VR requires ESL support such as Daytripper. Enable only the generated ESPs for the source mods you use.");
        else if(running)report->setPlainText(state["operation"]=="library-save"?QString("Saving %1…").arg(state["plugin"].toString()):QString("Building source-plugin ESL-flagged ESPs and validating the complete output..."));
        else if(phase=="failed")report->setPlainText("Failed: "+state["error"].toString());
        else if(state["operation"]=="library-export") {
            const auto result=state["result"].toObject();
            QString text=QString("Export complete: %1 ESL-flagged ESPs, %2 magazine objects").arg(result["plugins"].toArray().size()).arg(result["activeObjects"].toInt());
            if(result.contains("scopeProfiles"))text+=QString(", %1 True Scopes profiles").arg(result["scopeProfiles"].toInt());
            text+="\n"+result["installedTo"].toString(result["archive"].toString());
            if(result.contains("written"))text+=QString("\n%1 files written, %2 unchanged files left in place").arg(result["written"].toInt()).arg(result["unchanged"].toInt());
            for(const auto& value:result["plugins"].toArray()){const auto group=value.toObject();text+="\n"+group["master"].toString()+" -> "+group["plugin"].toString()+QString(" · %1 objects").arg(group["objects"].toInt());}
            for(const auto& value:result["skipped"].toArray()){const auto weapon=value.toObject();text+="\nNot exported: "+weapon["name"].toString()+" ("+weapon["reason"].toString()+")";}
            for(const auto& value:result["warnings"].toArray()){const auto weapon=value.toObject();text+="\nNote: "+weapon["name"].toString()+" - "+weapon["warning"].toString();}
            report->setPlainText(text+"\n\nEnable the desired ESL-flagged ESPs in MO2. Source plugins and profile settings were not changed.");
        }
    };
    connect(this,&ReloadMakerController::packJobChanged,&dialog,update);
    connect(saveNow,&QPushButton::clicked,&dialog,[&]{try{startEspSave({});update();}catch(const std::exception& e){report->setPlainText(e.what());}});
    const auto run=[&](const QString& destination,const QString& key){
        if(destination.isEmpty())return;
        QJsonObject request{{"operation","export"},{key,destination}};
        if(scope==ExportScope::Everything){request["scope"]="everything";}
        else if(scope==ExportScope::OpenEsp){request["selections"]=QJsonArray{QJsonObject{{"plugin",_esp->plugin}}};}
        else{
            QJsonArray selections;QStringList chosen;
            for(int i=0;i<list->topLevelItemCount();++i){auto* item=list->topLevelItem(i);if(item->checkState(0)!=Qt::Checked)continue;
                const auto plugin=item->data(0,Qt::UserRole).toString();chosen.append(plugin);
                auto* versions=qobject_cast<QComboBox*>(list->itemWidget(item,1));selections.append(QJsonObject{{"plugin",plugin},{"save",versions?versions->currentData().toString():QString()}});}
            if(selections.isEmpty()){report->setPlainText("Tick at least one source ESP to export.");return;}
            settings.setValue("packBuilder/selected",chosen);request["selections"]=selections;
        }
        settings.setValue("packBuilder/mo2",mo2->text());
        try{packCommand(request);update();}
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
    if(chosenList&&list->topLevelItemCount()==0)report->setPlainText("No saved ESP projects yet. Save a weapon's authoring first.");
    update();dialog.exec();
}
}
