// Development testbeds: the automated corpus by name and the dev bench
// weapons. workspace/library/development-sources.json names the source each
// one loads from; its authoring always comes from the weapon's ESP project,
// exactly as when the same mod is loaded through MO2.
#include "ReloadMakerController.h"
#include <QDir>
#include <QFileInfo>
#include <QTimer>

namespace reloadmaker {
// The testbed of the open weapon: the one that loads this weapon record from
// the current source. A record shared by two testbeds (a weapon and its
// overhaul, as MK18 had) takes the testbed of the record's winning plugin.
QString ReloadMakerController::currentTestbed() {
    const auto* weapon=currentWeapon();
    if(!weapon||!currentPlugin())return {};
    if(!_testbedSourcesLoaded)refreshDevelopmentSources();
    const auto record=poseRecordKey(weapon->formId);
    const auto winner=currentPlugin()->origins.value(weapon->formId).winner;
    QString found;
    int matches=0;
    for(const auto& value:_testbedSources) {
        const auto entry=value.toObject();
        if(entry["weapon"].toString().compare(record,Qt::CaseInsensitive)!=0)continue;
        if(QDir::cleanPath(entry["source"].toString()).compare(QDir::cleanPath(_package.sourcePath),Qt::CaseInsensitive)!=0)continue;
        if(entry["plugin"].toString().compare(winner,Qt::CaseInsensitive)==0)return entry["id"].toString();
        found=entry["id"].toString();
        ++matches;
    }
    return matches==1?found:QString();
}

// Opens the testbed's source when another one is loaded, then its weapon.
bool ReloadMakerController::selectTestbed(const QString& name) {
    if(_loadThread||_reloadCapturing||_rebuildTimer->isActive()||_saveAll||espBusy()){setStatus("Wait for the current weapon to finish loading or saving.",true);return false;}
    if(!_testbedSourcesLoaded)refreshDevelopmentSources();
    QJsonObject entry;
    for(const auto& value:_testbedSources)if(value.toObject()["id"].toString()==name){entry=value.toObject();break;}
    if(entry.isEmpty()){setStatus("Unknown weapon testbed.",true);return false;}
    const auto weapon=entry["weapon"].toString().toLower();
    const bool sameSource=!_libraryMode&&QDir::cleanPath(entry["source"].toString()).compare(QDir::cleanPath(_package.sourcePath),Qt::CaseInsensitive)==0;
    if(sameSource) {
        for(int i=0;i<_weaponSelections.size();++i) {
            if(selectionRecordKey(i)!=weapon)continue;
            if(i!=_selectedWeapon)selectWeapon(i);
            if(_selectedWeapon!=i)return false;
            setWorkspace(OperateWorkspace);alignPluginToWeapon();refreshWeaponPicker();
            return _lastError.isEmpty();
        }
        setStatus("The testbed weapon is absent from its source.",true);
        return false;
    }
    if(!QFileInfo::exists(entry["source"].toString())){setStatus("The "+name+" testbed source is missing: "+entry["source"].toString(),true);return false;}
    if(!leaveDocument(QStringLiteral("changing testbeds"),[this,name]{selectTestbed(name);}))return false;
    _pendingSession={{"weapon",entry["weapon"]},{"browserPlugin",entry["plugin"]},{"workspace",OperateWorkspace}};
    startPackageLoad(entry["source"].toString(),entry["gameData"].toString().isEmpty()?_gameDataText:entry["gameData"].toString(),true);
    if(!_loadThread){_pendingSession={};return false;}
    return _lastError.isEmpty();
}
}
