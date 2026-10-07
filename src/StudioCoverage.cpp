#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "areas/project/EspLibrary.h"
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <stdexcept>

namespace reloadmaker {
QString ReloadMakerController::selectionRecordKey(int index) const {
    if(index<0||index>=_weaponSelections.size())return {};
    const auto& selection=_weaponSelections[index];const auto& plugin=_plugins[selection.pluginIndex];const auto id=plugin.weapons[selection.weaponIndex].formId;
    const auto origin=plugin.origins.constFind(id);
    if(origin!=plugin.origins.cend())return QString("%1:%2").arg(origin->plugin).arg(origin->objectId,6,16,QChar('0')).toLower();
    const auto master=id>>24;return QString("%1:%2").arg(master<quint32(plugin.masters.size())?plugin.masters[master]:plugin.fileName).arg(id&0xffffffu,6,16,QChar('0')).toLower();
}
// Each library ESP project with its latest version's weapons. Reading
// project.json files is cheap, so this runs after every save, delete or export.
void ReloadMakerController::updatePackCoverage() {
    QJsonArray projects;_coverageLabels.clear();
    const QDir root(espLibraryRoot());
    for(const auto& folder:root.entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
        if(espProjectKey(folder).isEmpty())continue;
        QString error;const auto index=readEspProject(folder,error);
        if(index.isEmpty()){if(!error.isEmpty())projects.append(QJsonObject{{"plugin",folder},{"error",error},{"weapons",QJsonArray{}}});continue;}
        QJsonObject latest;for(const auto& value:index["saves"].toArray())if(value.toObject()["id"]==index["latest"])latest=value.toObject();
        projects.append(QJsonObject{{"plugin",index["plugin"]},{"latest",index["latest"]},{"savedAt",latest["savedAt"]},{"versions",index["saves"].toArray().size()},{"weapons",latest["weapons"]}});
        for(const auto& value:latest["weapons"].toArray()){const auto weapon=value.toObject();
            _coverageLabels[weapon["record"].toString().toLower()]=weapon["exportable"].toBool()?QString("Saved · %1 configurations · %2 magazines").arg(weapon["configurations"].toInt()).arg(weapon["magazines"].toInt()):QString("Saved · no reload behavior yet");}
    }
    _packCoverage={{"projects",projects},{"scope","esp-library"},{"gameEnablement","not-checked"}};
    refreshWeaponPicker();
}
QString ReloadMakerController::coverageLabel(const QString& record) const {
    if(_esp&&_esp->weapons.value(record.toLower()).dirty)return "Unsaved edits";
    if(_packCoverage.isEmpty())return {};
    return _coverageLabels.value(record.toLower(),"Not authored");
}
QJsonObject ReloadMakerController::packCoverageState(bool refresh) {
    if(refresh||_packCoverage.isEmpty())updatePackCoverage();
    auto result=_packCoverage;result["scanned"]=true;QJsonArray loaded;
    for(int i=0;i<_weaponSelections.size();++i){const auto& selection=_weaponSelections[i];const auto& plugin=_plugins[selection.pluginIndex];const auto& weapon=plugin.weapons[selection.weaponIndex];
        const auto record=selectionRecordKey(i);const auto winner=plugin.origins.value(weapon.formId).winner;
        loaded.append(QJsonObject{{"record",record},{"selection",i},{"name",weapon.name.isEmpty()?weapon.editorId:weapon.name},{"editorId",weapon.editorId},
            {"owner",record.section(':',0,0)},{"winner",winner.isEmpty()?plugin.fileName:winner},{"coverage",coverageLabel(record)}});}
    result["loadedWeapons"]=loaded;result["weaponFilter"]=_weaponLibraryReport;result["sourceMode"]=_libraryMode?"mo2-profile":!_package.resourceData.isEmpty()?"explicit-data":"ordered-packages";result["profile"]=_libraryMode?_launchSnapshot.profileName:QString();
    return result;
}
void ReloadMakerController::showPackCoverage() {
    if(auto* existing=_window->findChild<QDialog*>("PackCoverageDialog")){existing->show();existing->raise();return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("PackCoverageDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Reload library");dialog->resize(1040,780);
    auto* layout=new QVBoxLayout(dialog);auto* intro=new QLabel("Weapons by their originating ESP: the loaded source's player weapons and every ESP project saved in this workspace. Saved coverage does not establish current source compatibility or game enablement.");intro->setWordWrap(true);layout->addWidget(intro);
    auto* search=new QLineEdit;search->setPlaceholderText("Filter weapon, ESP or winning record…");layout->addWidget(search);
    auto* tree=new QTreeWidget;tree->setColumnCount(3);tree->setHeaderLabels({"ESP / weapon","Saved authoring","Winning weapon record"});tree->setColumnWidth(0,410);tree->setColumnWidth(1,300);layout->addWidget(tree,3);
    auto* detail=new QPlainTextEdit;detail->setReadOnly(true);layout->addWidget(detail,1);
    auto* buttons=new QHBoxLayout;layout->addLayout(buttons);auto* configure=new QPushButton("Configure selected weapon");auto* exportEsp=new QPushButton("Export selected ESP…");auto* source=new QPushButton("Setup / connect source");for(auto* b:{configure,exportEsp,source})buttons->addWidget(b);
    const auto populate=[=,this]{
        const auto state=packCoverageState(true);const auto needle=search->text().trimmed();tree->clear();QMap<QString,QJsonObject> weapons;QMap<QString,QJsonObject> projects;
        for(const auto& row:state["loadedWeapons"].toArray()){const auto weapon=row.toObject();weapons[weapon["record"].toString()]=weapon;}
        for(const auto& row:state["projects"].toArray()){const auto project=row.toObject();projects[project["plugin"].toString().toLower()]=project;
            for(const auto& item:project["weapons"].toArray()){auto weapon=item.toObject();const auto record=weapon["record"].toString().toLower();
                if(!weapons.contains(record)){weapon["record"]=record;weapon["selection"]=-1;weapon["owner"]=record.section(':',0,0);weapon["winner"]="Not loaded in this source";weapons[record]=weapon;}}}
        QMap<QString,QTreeWidgetItem*> plugins;
        for(auto it=weapons.cbegin();it!=weapons.cend();++it){const auto& weapon=it.value();const QString text=it.key()+" "+weapon["name"].toString()+" "+weapon["winner"].toString();if(!needle.isEmpty()&&!text.contains(needle,Qt::CaseInsensitive))continue;
            const auto owner=weapon["owner"].toString();
            if(!plugins.contains(owner)){const auto project=projects.value(owner);auto* item=new QTreeWidgetItem(tree,QStringList{project["plugin"].toString(owner),project.isEmpty()?QString("No saved versions"):QString("%1 saved version(s) · latest %2").arg(project["versions"].toInt()).arg(QDateTime::fromString(project["savedAt"].toString(),Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm")),QString()});item->setData(0,Qt::UserRole+1,project);plugins[owner]=item;}
            auto* row=new QTreeWidgetItem(plugins[owner],QStringList{weapon["name"].toString(),coverageLabel(it.key()),weapon["winner"].toString()});row->setData(0,Qt::UserRole,weapon);row->setData(0,Qt::UserRole+1,projects.value(owner));row->setToolTip(0,it.key());
        }
        tree->expandToDepth(0);configure->setEnabled(false);exportEsp->setEnabled(false);
        detail->setPlainText(QString("Source: %1 %2\nEach ESP project keeps dated versions of all its weapons. Testbed, MO2 and package sources share the same project.").arg(state["sourceMode"].toString(),state["profile"].toString()));
    };
    connect(tree,&QTreeWidget::currentItemChanged,dialog,[=](QTreeWidgetItem* item,QTreeWidgetItem*){const auto weapon=item?item->data(0,Qt::UserRole).toJsonObject():QJsonObject{},project=item?item->data(0,Qt::UserRole+1).toJsonObject():QJsonObject{};
        configure->setEnabled(weapon["selection"].toInt(-1)>=0);exportEsp->setEnabled(!project["latest"].toString().isEmpty());
        QString text=weapon.isEmpty()?project["plugin"].toString():weapon["record"].toString()+"\n"+weapon["editorId"].toString();
        if(!project.isEmpty())text+=QString("\n\nLatest saved version %1\n%2 saved version(s)").arg(project["latest"].toString()).arg(project["versions"].toInt());
        if(weapon.contains("notUpdated"))text+=QString("\n%1 configuration(s) kept their earlier behavior at the last save").arg(weapon["notUpdated"].toInt());
        detail->setPlainText(text);});
    connect(search,&QLineEdit::textChanged,dialog,[=]{populate();});
    connect(configure,&QPushButton::clicked,dialog,[=,this]{if(_loadThread||_reloadCapturing||_reloadCaptureQueued){detail->setPlainText("Wait for the current source capture.");return;}auto* item=tree->currentItem();if(!item)return;const auto weapon=item->data(0,Qt::UserRole).toJsonObject();const int index=weapon["selection"].toInt(-1);if(index<0||selectionRecordKey(index)!=weapon["record"].toString()){detail->setPlainText("Source changed; reopen the library before selecting this weapon.");return;}selectWeapon(index);dialog->close();setWorkspace(OperateWorkspace);});
    connect(exportEsp,&QPushButton::clicked,dialog,[=,this]{auto* item=tree->currentItem();if(!item)return;const auto project=item->data(0,Qt::UserRole+1).toJsonObject();if(project["plugin"].toString().isEmpty())return;QSettings().setValue("packBuilder/selected",QStringList{project["plugin"].toString()});dialog->close();showPackBuilder();});
    connect(source,&QPushButton::clicked,dialog,[this,dialog]{dialog->close();showSetup();});populate();dialog->show();
}
}
