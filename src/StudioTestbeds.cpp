// Development testbeds: the automated corpus by name and the dev bench weapons
// (areas/sources/Testbeds.h), each opened through its saved project.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "ReloadTemplates.h"
#include "areas/sources/Testbeds.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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
        if(QDir::cleanPath(entry["source"].toString()).compare(QDir::cleanPath(_package.sourcePath),Qt::CaseInsensitive)!=0)continue;
        if(entry["plugin"].toString().compare(winner,Qt::CaseInsensitive)==0)return entry["id"].toString();
        found=entry["id"].toString();
        ++matches;
    }
    return matches==1?found:QString();
}

QString ReloadMakerController::resolvedTestbedProject(const QString& name) const {
    const auto canonical=testbedProject(name);
    const auto configured=QSettings().value("testbeds/"+name,canonical).toString();
    const auto read=[](const QString& path){
        QFile file(path);
        if(file.size()>64*1024*1024||!file.open(QIODevice::ReadOnly))return QJsonObject{};
        const auto record=QJsonDocument::fromJson(file.readAll()).object();
        if(record["format"]!=1||record["kind"]!="reload-maker-v2-project")return QJsonObject{};
        return record;
    };
    const auto base=read(canonical),selected=read(configured);
    const auto weapon=base["weapon"].toString();
    // A Save As draft from the same source is not an authored testbed. Older
    // settings may point to one; retain the authored canonical project.
    const auto selectedPreset=selected["reloadTemplates"].toObject()[selected["weapon"].toString().toLower()].toString();
    if(!selected.isEmpty()&&(weapon.isEmpty()||selected["weapon"].toString().compare(weapon,Qt::CaseInsensitive)==0)&&
       templateMechanism(selectedPreset)&&!selected["library"].toBool())return configured;
    return canonical;
}

bool ReloadMakerController::selectSavedWeapon(int index) {
    if(_libraryMode||!_pendingSession.isEmpty()||index<0||index>=_weaponSelections.size())return false;
    if(!_testbedSourcesLoaded)refreshDevelopmentSources();
    const auto weapon=selectionRecordKey(index);
    QJsonObject selected;
    for(const auto& value:_testbedSources) {
        const auto entry=value.toObject();
        if(entry["weapon"].toString().compare(weapon,Qt::CaseInsensitive)!=0)continue;
        const bool sameSource=QDir::cleanPath(entry["source"].toString()).compare(QDir::cleanPath(_package.sourcePath),Qt::CaseInsensitive)==0;
        if(sameSource){selected=entry;break;}
        if(selected.isEmpty()||entry["plugin"].toString().compare(_browserPlugin,Qt::CaseInsensitive)==0)selected=entry;
    }
    if(selected.isEmpty())return false;
    selectTestbed(selected["id"].toString());
    return true;
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
    const auto project=isCorpusTestbed(name)?resolvedTestbedProject(name):benchProject;
    if(!QFileInfo::exists(project)){setStatus("Open and save the "+name+" project before selecting this testbed.",true);return false;}
    if(current==name&&!_libraryMode&&QFileInfo(_sessionPath).absoluteFilePath()==QFileInfo(project).absoluteFilePath()){setWorkspace(OperateWorkspace);alignPluginToWeapon();refreshWeaponPicker();return true;}
    if(!leaveDocument(QStringLiteral("changing testbeds")))return false;
    openSession(project);
    if(!_pendingSession.isEmpty()) {
        _pendingSession["workspace"]=OperateWorkspace;
        for(const auto& value:_testbedSources){const auto entry=value.toObject();if(entry["id"]==name){_pendingSession["browserPlugin"]=entry["plugin"];break;}}
    }
    return _lastError.isEmpty();
}
}
