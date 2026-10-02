#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "InstallationSetup.h"
#include <QAbstractItemModel>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace reloadmaker {
namespace {
int pluginIndex(const QComboBox* combo,const QString& name) {
    for(int i=0;i<combo->count();++i)if(combo->itemData(i).toString().compare(name,Qt::CaseInsensitive)==0)return i;
    return -1;
}
}

void ReloadMakerController::refreshDevelopmentSources() {
    _testbedSources={};_testbedSourcesLoaded=true;QSettings settings;
    for(const auto* id:{"glock","ru556","m1a","timberwolf","l118a2","thanatos","mk18","mk18-empire","srep-service","srep-honeybadger","srep-ar10","srep-xm16e1"}) {
        const auto project=settings.value("testbeds/"+QString(id),testbedProject(id)).toString();QFile file(project);
        if(file.size()>64*1024*1024||!file.open(QIODevice::ReadOnly))continue;
        const auto doc=QJsonDocument::fromJson(file.readAll()).object();const auto weapon=doc["weapon"].toString();
        if(doc["library"].toBool()||weapon.isEmpty())continue;
        auto plugin=weapon.section(':',0,0);const auto source=doc["package"].toString();
        if(source.endsWith(".reloadsources.json",Qt::CaseInsensitive)) {
            QFile definition(source);if(definition.size()<=65536&&definition.open(QIODevice::ReadOnly)) {
                const auto inputs=QJsonDocument::fromJson(definition.readAll()).object()["plugins"].toArray();
                if(!inputs.isEmpty())plugin=QFileInfo(inputs.last().toString()).fileName();
            }
        }
        _testbedSources.append(QJsonObject{{"id",id},{"plugin",plugin},{"weapon",weapon},{"project",project},{"source",source},
            {"name",doc["weaponName"].toString(doc["weaponEditorId"].toString(id))}});
    }
}

QStringList ReloadMakerController::selectionPlugins(int index) const {
    if(index<0||index>=_weaponSelections.size())return {};
    const auto& selection=_weaponSelections[index];const auto& plugin=_plugins[selection.pluginIndex];
    const auto id=plugin.weapons[selection.weaponIndex].formId;const auto origin=plugin.origins.constFind(id);
    if(origin!=plugin.origins.cend()&&!origin->contributors.isEmpty())return origin->contributors;
    return {plugin.fileName};
}
QStringList ReloadMakerController::sourcePluginOrder() const {
    if(_libraryMode)return _launchSnapshot.plugins;
    QStringList result;const auto add=[&](const QString& name){if(!result.contains(name,Qt::CaseInsensitive))result.append(name);};
    for(const auto& plugin:_plugins){if(plugin.sourcePlugins.isEmpty())add(plugin.fileName);else for(const auto& name:plugin.sourcePlugins)add(name);}
    for(const auto& value:_testbedSources)add(value.toObject()["plugin"].toString());
    return result;
}
QStringList ReloadMakerController::sourcePluginMasters(const QString& name) const {
    QHash<QString,QStringList> dependencies;
    for(const auto& plugin:_plugins){dependencies.insert(plugin.fileName.toLower(),plugin.masters);
        for(auto it=plugin.sourceMasters.cbegin();it!=plugin.sourceMasters.cend();++it)dependencies[it.key().toLower()]=it.value();}
    QStringList result,remaining=dependencies.value(name.toLower());QSet<QString> seen{name.toLower()};
    while(!remaining.isEmpty()&&seen.size()<256) {const auto master=remaining.takeFirst();if(seen.contains(master.toLower()))continue;
        seen.insert(master.toLower());result.append(master);remaining.append(dependencies.value(master.toLower()));}
    return result;
}

void ReloadMakerController::createSourceBar(QToolBar* bar) {
    _mo2Instance=QSettings().value("paths/mo2Instance").toString();
    _sourceLabel=new QLabel("No source",bar);_sourceLabel->setObjectName("SourceLabel");bar->addWidget(_sourceLabel);
    _pluginFilter=new QComboBox(bar);_pluginFilter->setObjectName("SourcePluginPicker");_pluginFilter->setAccessibleName("Plugin");
    _pluginFilter->setMinimumContentsLength(24);_pluginFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _pluginFilter->setMaxVisibleItems(30);_pluginFilter->setPlaceholderText("Select a plugin…");bar->addWidget(_pluginFilter);
    _pluginSearch=new QToolButton(bar);_pluginSearch->setObjectName("SourcePluginSearch");
    _pluginSearch->setAccessibleName("Search plugins");_pluginSearch->setToolTip("Search plugins by name");
    QPixmap icon(32,32);icon.fill(Qt::transparent);
    {QPainter painter(&icon);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(QColor("#dbe7e9"),2.8));
        painter.drawEllipse(QRectF(5,5,15,15));painter.drawLine(QPointF(18,18),QPointF(27,27));}
    _pluginSearch->setIcon(QIcon(icon));_pluginSearch->setIconSize(QSize(20,20));
    _pluginSearch->setPopupMode(QToolButton::InstantPopup);bar->addWidget(_pluginSearch);
    auto* searchMenu=new QMenu(_pluginSearch);_pluginSearch->setMenu(searchMenu);
    auto* panel=new QWidget(searchMenu);panel->setMinimumWidth(400);auto* layout=new QVBoxLayout(panel);
    auto* search=new QLineEdit(panel);search->setObjectName("PluginSearchText");search->setPlaceholderText("Search plugins…");search->setClearButtonEnabled(true);layout->addWidget(search);
    auto* matches=new QSortFilterProxyModel(panel);matches->setSourceModel(_pluginFilter->model());
    matches->setFilterRole(Qt::UserRole);matches->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto* results=new QListView(panel);results->setObjectName("PluginSearchResults");results->setModel(matches);
    results->setMinimumHeight(250);layout->addWidget(results);
    auto* searchAction=new QWidgetAction(searchMenu);searchAction->setDefaultWidget(panel);searchMenu->addAction(searchAction);
    connect(search,&QLineEdit::textChanged,panel,[matches,results](const QString& text){matches->setFilterFixedString(text);results->setCurrentIndex(matches->index(0,0));});
    const auto choose=[this,searchMenu](const QModelIndex& index){if(!index.isValid())return;const auto name=index.data(Qt::UserRole).toString();searchMenu->hide();(void)selectSourcePlugin(name);};
    connect(results,&QListView::clicked,panel,choose);connect(results,&QListView::activated,panel,choose);
    connect(search,&QLineEdit::returnPressed,panel,[results,choose]{choose(results->currentIndex());});
    connect(searchMenu,&QMenu::aboutToShow,panel,[this,search,results,matches]{search->clear();results->setCurrentIndex(matches->index(_pluginFilter->currentIndex(),0));search->setFocus();});
    _weaponPicker=new QComboBox(bar);_weaponPicker->setObjectName("SourceWeaponPicker");_weaponPicker->setAccessibleName("Weapon");
    _weaponPicker->setMinimumContentsLength(24);_weaponPicker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _weaponPicker->setMaxVisibleItems(30);_weaponPicker->setPlaceholderText("Choose a plugin first…");bar->addWidget(_weaponPicker);
    connect(_pluginFilter,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)selectSourcePlugin(_pluginFilter->itemData(i).toString());});
    connect(_weaponPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){if(i>=0)(void)selectSourceWeapon(_weaponPicker->itemData(i).toInt());});
}

void ReloadMakerController::refreshSourceBar() {
    if(!_sourceLabel)return;
    if(!_libraryMode&&!_package.sourcePath.isEmpty()&&!_testbedSourcesLoaded)refreshDevelopmentSources();
    _sourceLabel->setText(_libraryMode?"MO2":_package.sourcePath.isEmpty()?"No source":"Development source");
    _sourceLabel->setToolTip(_libraryMode?_launchSnapshot.instancePath+"\nProfile: "+_launchSnapshot.profileName+"\nWinning records follow this profile's load order.":_package.sourcePath);
    const auto key=_libraryMode?"mo2:"+_launchSnapshot.instancePath+"/"+_launchSnapshot.profileName:"package:"+_package.sourcePath;
    auto chosen=_browserSourceKey==key?_pluginFilter->currentData().toString():QString();_browserSourceKey=key;
    if(!_pendingSession.isEmpty())chosen=_pendingSession.value("browserPlugin").toString(_pendingSession["weapon"].toString().section(':',0,0));
    const QSignalBlocker block(_pluginFilter);_pluginFilter->clear();
    QHash<QString,int> counts;for(int i=0;i<_weaponSelections.size();++i)for(const auto& name:selectionPlugins(i))++counts[name.toLower()];
    for(const auto& name:sourcePluginOrder())_pluginFilter->addItem(name,name);
    _pluginFilter->setCurrentIndex(pluginIndex(_pluginFilter,chosen));
    _pluginFilter->setEnabled(!_loadThread&&_pluginFilter->count()>0);
    _pluginSearch->setEnabled(_pluginFilter->isEnabled());
    for(int i=0;i<_pluginFilter->count();++i){const auto name=_pluginFilter->itemData(i).toString();
        _pluginFilter->setItemData(i,QString("%1\n%2 editable weapon record(s) in the loaded source.\nAmber: selected plugin · teal: required master.").arg(name).arg(counts.value(name.toLower())),Qt::ToolTipRole);}
    if(_pluginFilter->currentIndex()<0)alignPluginToWeapon();
    updatePluginColors();refreshWeaponPicker();
}

void ReloadMakerController::updatePluginColors() {
    if(!_pluginFilter)return;
    const auto selected=_pluginFilter->currentData().toString();const auto masters=sourcePluginMasters(selected);
    for(int i=0;i<_pluginFilter->count();++i){const auto name=_pluginFilter->itemData(i).toString();const bool active=name.compare(selected,Qt::CaseInsensitive)==0;
        const bool master=masters.contains(name,Qt::CaseInsensitive);QFont font=_pluginFilter->font();font.setBold(active);
        _pluginFilter->setItemData(i,QBrush(active?QColor("#e8c17c"):master?QColor("#80d2c7"):QColor("#dbe7e9")),Qt::ForegroundRole);
        _pluginFilter->setItemData(i,active?QVariant::fromValue(QBrush(QColor("#334946"))):QVariant(),Qt::BackgroundRole);
        _pluginFilter->setItemData(i,font,Qt::FontRole);_pluginFilter->setItemText(i,name+(master?"  · master":QString()));
    }
}

void ReloadMakerController::alignPluginToWeapon() {
    if(!_pluginFilter)return;
    const auto allowed=selectionPlugins(_weaponCombo->currentIndex());if(allowed.isEmpty())return;
    if(!allowed.contains(_pluginFilter->currentData().toString(),Qt::CaseInsensitive)){
        const auto origin=selectionRecordKey(_weaponCombo->currentIndex()).section(':',0,0);
        const QSignalBlocker block(_pluginFilter);_pluginFilter->setCurrentIndex(pluginIndex(_pluginFilter,allowed.contains(origin,Qt::CaseInsensitive)?origin:allowed.first()));
    }
    updatePluginColors();
}

void ReloadMakerController::refreshWeaponPicker() {
    if(!_weaponPicker)return;
    const QSignalBlocker blocker(_weaponPicker);_weaponPicker->clear();
    const auto filter=_pluginFilter->currentData().toString();
    if(!filter.isEmpty())for(int index=0;index<_weaponSelections.size();++index){
        if(!selectionPlugins(index).contains(filter,Qt::CaseInsensitive))continue;
        const auto& selection=_weaponSelections[index];const auto& plugin=_plugins[selection.pluginIndex];const auto& weapon=plugin.weapons[selection.weaponIndex];
        const auto covered=coverageLabel(selectionRecordKey(index));_weaponPicker->addItem((weapon.name.isEmpty()?weapon.editorId:weapon.name)+(covered.isEmpty()?QString():" · "+covered),index);
        const auto origin=plugin.origins.value(weapon.formId);
        _weaponPicker->setItemData(_weaponPicker->count()-1,QString("%1\n%2\nWinning record: %3").arg(weapon.editorId,selectionRecordKey(index),origin.winner.isEmpty()?plugin.fileName:origin.winner),Qt::ToolTipRole);
    }
    _weaponPicker->model()->sort(0);_weaponPicker->setCurrentIndex(_weaponPicker->findData(_weaponCombo->currentIndex()));
    _weaponPicker->setPlaceholderText(filter.isEmpty()?"Choose a plugin first…":_weaponPicker->count()?"Select a weapon…":"No editable weapons in this plugin");
    _weaponPicker->setEnabled(!_loadThread&&_weaponPicker->count()>0);
}

bool ReloadMakerController::selectSourceMode(const QString& mode) {
    if(_loadThread||_reloadCapturing||_packProcess){setStatus("Finish the current source or authoring job first.",true);return false;}
    if(mode=="dynamic") {const auto check=checkInstallation(loadInstallationSetup());if(!check.gameReady||!check.profileReady){showSetup();return false;}return connectInstalledProfile();}
    if(mode!="development"){setStatus("Choose dynamic or development source.",true);return false;}
    refreshDevelopmentSources();
    if(!_libraryMode&&!_package.sourcePath.isEmpty()){refreshSourceBar();return true;}
    if(_testbedSources.isEmpty()){setStatus("No saved testbeds in this workspace. Use MENU > Source to open a plugin or mod folder.",true);return false;}
    return selectTestbed(_testbedSources.first().toObject()["id"].toString());
}

bool ReloadMakerController::selectSourcePlugin(const QString& name) {
    if(_loadThread||_reloadCapturing||_packProcess||_rebuildTimer->isActive()){alignPluginToWeapon();setStatus("Wait for the current source or capture.",true);return false;}
    const int selected=pluginIndex(_pluginFilter,name);if(selected<0){setStatus("Choose a plugin from this source.",true);return false;}
    {const QSignalBlocker block(_pluginFilter);_pluginFilter->setCurrentIndex(selected);}
    refreshWeaponPicker();
    if(!_libraryMode)for(const auto& value:_testbedSources){const auto entry=value.toObject();
        if(entry["plugin"].toString().compare(name,Qt::CaseInsensitive)==0&&QDir::cleanPath(entry["source"].toString()).compare(QDir::cleanPath(_package.sourcePath),Qt::CaseInsensitive)!=0){
            const bool started=selectTestbed(entry["id"].toString());if(started&&!_pendingSession.isEmpty())_pendingSession["browserPlugin"]=name;return started;
        }}
    if(currentWeapon()&&!selectionPlugins(_weaponCombo->currentIndex()).contains(name,Qt::CaseInsensitive)) {
        if(!saveSession()){alignPluginToWeapon();return false;}
        {const QSignalBlocker block(_weaponCombo);_weaponCombo->setCurrentIndex(-1);}
        clearAssetCaches();_sessionPath.clear();refreshAttachments();
    }
    updatePluginColors();refreshWeaponPicker();++_revision;return true;
}

bool ReloadMakerController::selectSourceWeapon(int index) {
    if(_loadThread||_reloadCapturing||_packProcess||index<0||index>=_weaponSelections.size()||
       !selectionPlugins(index).contains(_pluginFilter->currentData().toString(),Qt::CaseInsensitive)){setStatus("Choose a weapon from the selected plugin.",true);return false;}
    if(!_libraryMode)for(const auto& value:_testbedSources){const auto entry=value.toObject();
        if(entry["plugin"].toString().compare(_pluginFilter->currentData().toString(),Qt::CaseInsensitive)==0&&entry["weapon"].toString().compare(selectionRecordKey(index),Qt::CaseInsensitive)==0)
            return selectTestbed(entry["id"].toString());
    }
    _weaponCombo->setCurrentIndex(index);return true;
}

QJsonObject ReloadMakerController::sourceBrowserState() const {
    QJsonArray plugins,weapons,menus;const auto selected=_pluginFilter->currentData().toString();const auto masters=sourcePluginMasters(selected);
    for(int i=0;i<_pluginFilter->count();++i){const auto name=_pluginFilter->itemData(i).toString();plugins.append(QJsonObject{{"name",name},
        {"role",name.compare(selected,Qt::CaseInsensitive)==0?"selected":masters.contains(name,Qt::CaseInsensitive)?"master":"available"},{"masters",QJsonArray::fromStringList(sourcePluginMasters(name))}});}
    for(int i=0;i<_weaponPicker->count();++i){const auto index=_weaponPicker->itemData(i).toInt();weapons.append(QJsonObject{{"index",index},{"name",_weaponPicker->itemText(i)},{"record",selectionRecordKey(index)}});}
    if(auto* menu=_window->findChild<QMenu*>("MainMenu"))for(auto* action:menu->actions())if(!action->isSeparator())menus.append(action->text());
    return {{"mode",_libraryMode?"dynamic":"development"},{"plugin",selected},{"plugins",plugins},{"weapons",weapons},{"menu",menus},
        {"developmentSources",_testbedSources}};
}
}
