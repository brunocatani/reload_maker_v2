#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include <QComboBox>
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
void ReloadMakerController::updatePackCoverage(const QJsonObject& result) {
    _packCoverage=result;_coverageLabels.clear();QHash<QString,int> counts;QSet<QString> review;
    for(const auto& row:result["projects"].toArray()){const auto project=row.toObject();for(const auto& item:project["weapons"].toArray()) {
        const auto record=item.toObject()["record"].toString().toLower();++counts[record];if(project["status"]!="saved")review.insert(record);
    }}
    for(auto it=counts.cbegin();it!=counts.cend();++it)_coverageLabels[it.key()]=QString(review.contains(it.key())?"Review · %1 authored project(s)":"Authored · %1 project(s)").arg(it.value());
    QHash<QString,int> drafts;for(const auto& row:result["authoringProjects"].toArray())for(const auto& w:row.toObject()["weapons"].toArray())++drafts[w.toObject()["record"].toString().toLower()];
    for(auto it=drafts.cbegin();it!=drafts.cend();++it){const auto label=QString("%1 editor project(s)").arg(it.value());_coverageLabels[it.key()]=_coverageLabels.contains(it.key())?_coverageLabels.value(it.key())+" · "+label:"Draft · "+label;}
    refreshWeaponPicker();
}
QString ReloadMakerController::coverageLabel(const QString& record) const {
    if(_packCoverage.isEmpty())return {};
    return _coverageLabels.value(record.toLower(),"No authored pack");
}
QJsonObject ReloadMakerController::packCoverageState(bool refresh) {
    if(refresh) {
        QSettings settings;QSet<QString> paths;
        for(const auto& key:settings.allKeys())if((key.startsWith("packAuthoring/")&&key.endsWith("/project"))||key=="packExporter/project") {
            const auto value=settings.value(key).toString();if(!value.isEmpty())paths.insert(QFileInfo(value).absoluteFilePath());
        }
        QJsonArray projects;for(const auto& path:paths)projects.append(path);return packCommand({{"operation","inventory"},{"projects",projects}});
    }
    auto result=_packCoverage;result["scanned"]=!_packCoverage.isEmpty();QJsonArray loaded;
    for(int i=0;i<_weaponSelections.size();++i){const auto& selection=_weaponSelections[i];const auto& plugin=_plugins[selection.pluginIndex];const auto& weapon=plugin.weapons[selection.weaponIndex];
        const auto record=selectionRecordKey(i);const auto winner=plugin.origins.value(weapon.formId).winner;
        loaded.append(QJsonObject{{"record",record},{"selection",i},{"name",weapon.name.isEmpty()?weapon.editorId:weapon.name},{"editorId",weapon.editorId},
            {"owner",record.section(':',0,0)},{"winner",winner.isEmpty()?plugin.fileName:winner},{"coverage",coverageLabel(record)}});}
    result["loadedWeapons"]=loaded;result["sourceMode"]=_libraryMode?"mo2-profile":!_package.resourceData.isEmpty()?"explicit-data":"ordered-packages";result["profile"]=_libraryMode?_launchSnapshot.profileName:QString();
    return result;
}
void ReloadMakerController::showPackCoverage() {
    if(auto* existing=_window->findChild<QDialog*>("PackCoverageDialog")){existing->show();existing->raise();return;}
    auto* dialog=new QDialog(_window);dialog->setObjectName("PackCoverageDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Reload library");dialog->resize(1040,780);
    auto* layout=new QVBoxLayout(dialog);auto* intro=new QLabel("Browse weapons by their originating ESP and inspect saved pack coverage. Counts describe authored configurations. Check current source compatibility before export; this view does not change game enablement.");intro->setWordWrap(true);layout->addWidget(intro);
    auto* search=new QLineEdit;search->setPlaceholderText("Filter weapon, ESP, winning record or pack name…");layout->addWidget(search);
    auto* tree=new QTreeWidget;tree->setColumnCount(3);tree->setHeaderLabels({"ESP / weapon / authored pack","Coverage","Winning weapon record"});tree->setColumnWidth(0,410);tree->setColumnWidth(1,265);layout->addWidget(tree,3);
    auto* detail=new QPlainTextEdit;detail->setReadOnly(true);layout->addWidget(detail,1);
    auto* buttons=new QHBoxLayout;layout->addLayout(buttons);auto* refresh=new QPushButton("Refresh workspace coverage");auto* configure=new QPushButton("Configure selected weapon");auto* inspect=new QPushButton("Inspect selected pack");auto* source=new QPushButton("Setup / connect source");for(auto* b:{refresh,configure,inspect,source})buttons->addWidget(b);
    const auto populate=[=,this]{
        const auto state=packCoverageState();const auto needle=search->text().trimmed();tree->clear();QMap<QString,QJsonObject> weapons;
        for(const auto& row:state["loadedWeapons"].toArray()){const auto weapon=row.toObject();weapons[weapon["record"].toString()]=weapon;}
        QMap<QString,QJsonArray> projects;auto all=state["projects"].toArray();for(const auto& p:state["authoringProjects"].toArray())all.append(p);
        for(const auto& row:all){const auto project=row.toObject();for(const auto& item:project["weapons"].toArray()){auto weapon=item.toObject();const auto record=weapon["record"].toString().toLower();
            if(!weapons.contains(record)){weapon["record"]=record;weapon["selection"]=-1;weapon["owner"]=record.section(':',0,0);weapon["winner"]="Not loaded in this source";weapons[record]=weapon;}
            auto entry=project;entry.remove("weapons");entry["weapon"]=weapon;projects[record].append(entry);}}
        QMap<QString,QTreeWidgetItem*> plugins;
        for(auto it=weapons.cbegin();it!=weapons.cend();++it){const auto& weapon=it.value();const auto packs=projects.value(it.key());QString text=it.key()+" "+weapon["name"].toString()+" "+weapon["winner"].toString();for(const auto& p:packs)text+=' '+p.toObject()["name"].toString();if(!needle.isEmpty()&&!text.contains(needle,Qt::CaseInsensitive))continue;
            const auto owner=weapon["owner"].toString();if(!plugins.contains(owner))plugins[owner]=new QTreeWidgetItem(tree,QStringList{owner});
            auto* row=new QTreeWidgetItem(plugins[owner],QStringList{weapon["name"].toString(),state["scanned"].toBool()?coverageLabel(it.key()):"Coverage not scanned",weapon["winner"].toString()});row->setData(0,Qt::UserRole,weapon);row->setToolTip(0,it.key());
            for(const auto& p:packs){const auto pack=p.toObject(),w=pack["weapon"].toObject();const auto counts=pack["kind"]=="authoring"?"Editor project · "+pack["context"].toString():pack["status"]=="invalid"?QString("Needs repair · counts unavailable"):QString("%1 configurations · %2 magazines · %3").arg(w["variants"].toInt()).arg(w["magazines"].toInt()).arg(pack["status"].toString());auto* child=new QTreeWidgetItem(row,QStringList{pack["name"].toString(),counts,QString()});child->setData(0,Qt::UserRole,weapon);child->setData(0,Qt::UserRole+1,pack);child->setToolTip(0,pack["project"].toString());}
        }
        QTreeWidgetItem* invalid=nullptr;for(const auto& row:all){const auto p=row.toObject();if(p["status"]!="invalid")continue;if(!invalid)invalid=new QTreeWidgetItem(tree,QStringList{"Projects needing repair"});auto* child=new QTreeWidgetItem(invalid,QStringList{QFileInfo(p["project"].toString()).fileName(),"Cannot read coverage",QString()});child->setData(0,Qt::UserRole+1,p);}
        tree->expandToDepth(0);configure->setEnabled(false);inspect->setEnabled(false);
        detail->setPlainText(QString("Source: %1 %2\n%3\nRefresh after saving or changing pack projects. Multiple authored projects remain separate; no coverage is silently merged.").arg(state["sourceMode"].toString(),state["profile"].toString(),state["scanned"].toBool()?"Workspace coverage loaded. Source hashes have not been checked against this session.":"Choose Refresh to read saved workspace projects."));
    };
    connect(tree,&QTreeWidget::currentItemChanged,dialog,[=](QTreeWidgetItem* item,QTreeWidgetItem*){const auto weapon=item?item->data(0,Qt::UserRole).toJsonObject():QJsonObject{},pack=item?item->data(0,Qt::UserRole+1).toJsonObject():QJsonObject{};
        configure->setEnabled(weapon["selection"].toInt(-1)>=0);inspect->setEnabled(!pack["project"].toString().isEmpty());
        inspect->setText(pack["kind"]=="authoring"?"Open editor project":"Inspect selected pack");
        QString text=weapon["record"].toString()+"\n"+weapon["editorId"].toString();if(!pack.isEmpty())text+="\n\n"+pack["project"].toString()+QString("\nRevision %1 · %2\n").arg(pack["revision"].toInt()).arg(pack["status"].toString())+pack["error"].toString()+"\n"+pack["weapon"].toObject()["template"].toString();detail->setPlainText(text);});
    connect(search,&QLineEdit::textChanged,dialog,[=]{populate();});
    connect(refresh,&QPushButton::clicked,dialog,[=,this]{try{packCoverageState(true);}catch(const std::exception& e){detail->setPlainText(e.what());}});
    connect(this,&ReloadMakerController::packJobChanged,dialog,[=,this]{const auto state=packCommand({});const bool busy=state["phase"]=="running"||state["phase"]=="cancelling";refresh->setEnabled(!busy);if(state["operation"]!="inventory")return;if(busy)detail->setPlainText("Reading authored projects and verifying their saved input files…");else if(state["phase"]=="failed")detail->setPlainText(state["error"].toString());else populate();});
    connect(configure,&QPushButton::clicked,dialog,[=,this]{if(_loadThread||_reloadCapturing||_reloadCaptureQueued){detail->setPlainText("Wait for the current source capture.");return;}auto* item=tree->currentItem();if(!item)return;const auto weapon=item->data(0,Qt::UserRole).toJsonObject();const int index=weapon["selection"].toInt(-1);if(index<0||selectionRecordKey(index)!=weapon["record"].toString()){detail->setPlainText("Source changed; refresh the library before selecting this weapon.");return;}_weaponCombo->setCurrentIndex(index);dialog->close();setWorkspace(OperateWorkspace);});
    connect(inspect,&QPushButton::clicked,dialog,[=,this]{auto* item=tree->currentItem();if(!item)return;const auto entry=item->data(0,Qt::UserRole+1).toJsonObject();const auto path=entry["project"].toString();if(path.isEmpty())return;
        if(entry["kind"]=="authoring"){if(_loadThread||_reloadCapturing||!saveSession())return;dialog->close();openSession(path);return;}
        QSettings().setValue("packExporter/project",path);dialog->close();showPackExporter();});
    connect(source,&QPushButton::clicked,dialog,[this,dialog]{dialog->close();showSetup();});populate();dialog->show();
}
}
