// Development testbeds: the named weapons and their saved projects. P7 migrates
// these projects and deletes the testbed picker.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

namespace reloadmaker {
QString ReloadMakerController::currentTestbed() const {
    if(isSrepConfiguration()) {
        const auto name=currentWeapon()->editorId.toLower();
        return name=="srep_ar10"?"srep-ar10":name=="srep_xm16e1"?"srep-xm16e1":name.endsWith("honeybadger")?"srep-honeybadger":"srep-service";
    }
    const auto name=currentWeapon()?currentWeapon()->editorId.toLower():QString();
    return isGlockConfiguration()?"glock":isMk18Configuration()?(isMk18OverhaulConfiguration()?"mk18-empire":"mk18"):name=="wpnru556"?"ru556":name=="wpnm1a"?"m1a":isThanatosConfiguration()?"thanatos":isL118Configuration()?"l118a2":name=="c14timberwolf"?"timberwolf":QString();
}
QString ReloadMakerController::testbedProject(const QString& name) const {
    if(name.startsWith("srep-"))return studioDataPath("workspace/"+name+"-native-reload.project.json");
    if(name=="mk18"||name=="mk18-empire")return studioDataPath(name=="mk18"?"workspace/MK18-native-reload.project.json":"workspace/MK18-Empire-native-reload.project.json");
    return studioDataPath(name=="glock"?"workspace/Glock19x-magazine-grips.project.json":
        name=="ru556"?"workspace/RU556-native-reload.project.json":name=="m1a"?"workspace/M1A-native-reload.project.json":
        name=="l118a2"?"workspace/L118A2-native-reload.project.json":name=="thanatos"?"workspace/Thanatos-native-reload.project.json":"workspace/Timberwolf-native-reload.project.json");
}

bool ReloadMakerController::selectTestbed(const QString& name) {
    if(_loadThread||_reloadCapturing||_rebuildTimer->isActive()){setStatus("Wait for the current weapon to finish loading.",true);return false;}
    if(name!="glock"&&name!="ru556"&&name!="m1a"&&name!="timberwolf"&&name!="l118a2"&&name!="thanatos"&&name!="mk18"&&name!="mk18-empire"&&
       name!="srep-service"&&name!="srep-honeybadger"&&name!="srep-ar10"&&name!="srep-xm16e1"){setStatus("Unknown weapon testbed.",true);return false;}
    const auto current=currentTestbed();
    const auto defaultProject=testbedProject(name);
    const auto project=QSettings().value("testbeds/"+name,defaultProject).toString();
    if(!QFileInfo::exists(project)){setStatus("Open and save the "+name+" project before selecting this testbed.",true);return false;}
    if(current==name&&!_libraryMode&&QFileInfo(_sessionPath).absoluteFilePath()==QFileInfo(project).absoluteFilePath()){setWorkspace(OperateWorkspace);alignPluginToWeapon();refreshWeaponPicker();return true;}
    if(!saveSession())return false;
    if(!current.isEmpty()&&!_libraryMode)QSettings().setValue("testbeds/"+current,_sessionPath);
    openSession(project);
    if(!_pendingSession.isEmpty()) {
        _pendingSession["workspace"]=OperateWorkspace;
        if(!_testbedSourcesLoaded)refreshDevelopmentSources();
        for(const auto& value:_testbedSources){const auto entry=value.toObject();if(entry["id"]==name){_pendingSession["browserPlugin"]=entry["plugin"];break;}}
    }
    return _lastError.isEmpty();
}
}
