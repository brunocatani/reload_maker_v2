// Development testbeds: the automated corpus by name and the dev bench weapons
// (areas/sources/Testbeds.h), each opened through its saved project.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "areas/sources/Testbeds.h"
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <iterator>

namespace reloadmaker {
// The testbed of the open weapon: the one whose saved project holds this
// weapon record. A record shared by two testbeds (a weapon and its overhaul,
// as MK18 had) takes the testbed opened from the record's winning plugin
// (rework 14, item 25).
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
        if(entry["plugin"].toString().compare(winner,Qt::CaseInsensitive)==0)return entry["id"].toString();
        found=entry["id"].toString();
        ++matches;
    }
    return matches==1?found:QString();
}
QString ReloadMakerController::testbedProject(const QString& name) const {
    if(name.startsWith("srep-"))return studioDataPath("workspace/"+name+"-native-reload.project.json");
    return studioDataPath(name=="glock"?"workspace/Glock19x-magazine-grips.project.json":
        name=="ru556"?"workspace/RU556-native-reload.project.json":name=="m1a"?"workspace/M1A-native-reload.project.json":
        name=="l118a2"?"workspace/L118A2-native-reload.project.json":name=="thanatos"?"workspace/Thanatos-native-reload.project.json":"workspace/Timberwolf-native-reload.project.json");
}

bool ReloadMakerController::selectTestbed(const QString& name) {
    if(_loadThread||_reloadCapturing||_rebuildTimer->isActive()){setStatus("Wait for the current weapon to finish loading.",true);return false;}
    // A dev bench weapon always opens the project it was discovered from.
    if(!_testbedSourcesLoaded)refreshDevelopmentSources();
    QString benchProject;
    for(const auto& value:_testbedSources){const auto entry=value.toObject();if(entry["bench"].toBool()&&entry["id"]==name){benchProject=entry["project"].toString();break;}}
    if(!isCorpusTestbed(name)&&benchProject.isEmpty()){setStatus("Unknown weapon testbed.",true);return false;}
    const auto current=currentTestbed();
    const auto project=isCorpusTestbed(name)?QSettings().value("testbeds/"+name,testbedProject(name)).toString():benchProject;
    if(!QFileInfo::exists(project)){setStatus("Open and save the "+name+" project before selecting this testbed.",true);return false;}
    if(current==name&&!_libraryMode&&QFileInfo(_sessionPath).absoluteFilePath()==QFileInfo(project).absoluteFilePath()){setWorkspace(OperateWorkspace);alignPluginToWeapon();refreshWeaponPicker();return true;}
    if(!leaveDocument(QStringLiteral("changing testbeds")))return false;
    if(isCorpusTestbed(current)&&!_libraryMode)QSettings().setValue("testbeds/"+current,_sessionPath);
    openSession(project);
    if(!_pendingSession.isEmpty()) {
        _pendingSession["workspace"]=OperateWorkspace;
        for(const auto& value:_testbedSources){const auto entry=value.toObject();if(entry["id"]==name){_pendingSession["browserPlugin"]=entry["plugin"];break;}}
    }
    return _lastError.isEmpty();
}
}
