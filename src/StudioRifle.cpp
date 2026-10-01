#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include <QComboBox>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
bool ReloadMakerController::isAr15Configuration() const {
    return (isSrepConfiguration()&&currentWeapon()->editorId.compare("SREP_AR10",Qt::CaseInsensitive)!=0)||isMk18Configuration()||(currentWeapon()&&currentWeapon()->editorId.compare("WPNRU556",Qt::CaseInsensitive)==0);
}
bool ReloadMakerController::isSrepConfiguration() const {
    if(!currentWeapon())return false;
    const auto& name=currentWeapon()->editorId;
    return name.compare("SREP_ServiceRifle",Qt::CaseInsensitive)==0||name.compare("SREP_ServiceRifle_Unique_HoneyBadger",Qt::CaseInsensitive)==0||
        name.compare("SREP_AR10",Qt::CaseInsensitive)==0||name.compare("SREP_XM16E1",Qt::CaseInsensitive)==0;
}
bool ReloadMakerController::isSrepEmbeddedMagazine() const {
    if(!isSrepConfiguration()||!configuration())return false;
    for(const auto id:configuration()->activeMods()) {
        const auto* mod=configuration()->mod(id);
        if(mod&&mod->editorId.contains("receiver_",Qt::CaseInsensitive)&&mod->editorId.contains("SMG",Qt::CaseInsensitive))return true;
    }
    return false;
}
bool ReloadMakerController::isMk18Configuration() const {
    return currentWeapon()&&(currentWeapon()->editorId.compare("WPNMK18",Qt::CaseInsensitive)==0||currentWeapon()->editorId.compare("WPNMK18UNIQUE",Qt::CaseInsensitive)==0);
}
bool ReloadMakerController::isMk18OverhaulConfiguration() const {
    return isMk18Configuration()&&currentPlugin()->origins.value(currentWeapon()->formId).winner.compare("MK18AttachmentOverhaul.esp",Qt::CaseInsensitive)==0;
}
bool ReloadMakerController::isM1AConfiguration() const {
    return currentWeapon()&&currentWeapon()->editorId.compare("WPNM1A",Qt::CaseInsensitive)==0;
}
bool ReloadMakerController::isThanatosConfiguration() const {
    return currentWeapon()&&currentWeapon()->editorId.compare("DiaM107",Qt::CaseInsensitive)==0;
}
bool ReloadMakerController::isL118Configuration() const {
    return currentWeapon()&&currentWeapon()->editorId.compare("L118A2",Qt::CaseInsensitive)==0;
}
WeaponMechanism ReloadMakerController::simulationMechanism() const {
    return isBoltActionConfiguration()?WeaponMechanism::BoltAction:isM1AConfiguration()?WeaponMechanism::M1A:
        isAr15Configuration()?WeaponMechanism::Ar15:isThanatosConfiguration()||isSrepConfiguration()?WeaponMechanism::ReciprocatingBolt:WeaponMechanism::PistolSlide;
}
bool ReloadMakerController::isRifleConfiguration() const {return isAr15Configuration()||isM1AConfiguration()||isThanatosConfiguration()||isSrepConfiguration();}
QString ReloadMakerController::currentTestbed() const {
    if(isSrepConfiguration()) {
        const auto name=currentWeapon()->editorId.toLower();
        return name=="srep_ar10"?"srep-ar10":name=="srep_xm16e1"?"srep-xm16e1":name.endsWith("honeybadger")?"srep-honeybadger":"srep-service";
    }
    return isGlockConfiguration()?"glock":isMk18Configuration()?(isMk18OverhaulConfiguration()?"mk18-empire":"mk18"):isAr15Configuration()?"ru556":isM1AConfiguration()?"m1a":isThanatosConfiguration()?"thanatos":isL118Configuration()?"l118a2":isBoltActionConfiguration()?"timberwolf":QString();
}
QString ReloadMakerController::testbedProject(const QString& name) const {
    if(name.startsWith("srep-"))return studioDataPath("workspace/"+name+"-native-reload.project.json");
    if(name=="mk18"||name=="mk18-empire")return studioDataPath(name=="mk18"?"workspace/MK18-native-reload.project.json":"workspace/MK18-Empire-native-reload.project.json");
    return studioDataPath(name=="glock"?"workspace/Glock19x-magazine-grips.project.json":
        name=="ru556"?"workspace/RU556-native-reload.project.json":name=="m1a"?"workspace/M1A-native-reload.project.json":
        name=="l118a2"?"workspace/L118A2-native-reload.project.json":name=="thanatos"?"workspace/Thanatos-native-reload.project.json":"workspace/Timberwolf-native-reload.project.json");
}

bool ReloadMakerController::buildCurrentSimulationRig(SimulationRig& rig,QString& error) {
    // SREP's native bolt cycles while its separate charging handle is static.
    // Derive only the manual handle translation from that measured bolt stroke.
    if(isSrepConfiguration()) {
        for(const auto id:configuration()->activeMods()) {
            const auto* mod=configuration()->mod(id);
            if(mod&&(mod->editorId=="mod_SREP_M203Dummy"||mod->editorId=="mod_SREP_M26MASSDummy")) {
                error="The active underbarrel weapon uses its own reload. Select rifle mode for physical rifle reloads.";return false;
            }
        }
        if(!buildSimulationRig(*_reloadRecipe,rig,error,simulationMechanism(),isAr15Configuration()))return false;
        for(int i=0;i<_reloadRecipe->tracks.size();++i)if(_reloadRecipe->tracks[i].surface&&_reloadRecipe->tracks[i].anchor.anchorName=="WeaponExtra2")rig.slideTracks.append(i);
        return true;
    }
    if(!isMk18Configuration())return buildSimulationRig(*_reloadRecipe,rig,error,simulationMechanism());
    const auto& r=*_reloadRecipe;int bolt=-1;float stroke=0;
    for(int i=0;i<r.tracks.size();++i)if(r.tracks[i].anchor.anchorName=="WeaponBolt")bolt=i;
    if(bolt<0){error="MK18 has no native bolt binding.";return false;}
    for(const auto& p:r.tracks[bolt].poses)stroke=std::max(stroke,reloadPoseDistance(p,r.tracks[bolt].poses.back()));
    if(stroke>=.15f)return buildSimulationRig(r,rig,error,simulationMechanism(),true);
    // Tactical/double-mag clips may never cycle the bolt. Their magazine and
    // hands stay native; the manual action uses the standard MK18 empty reload.
    const QString path="meshes/actors/character/_1stperson/animations/mk18anims/wpnreload.hkx";
    const auto bytes=readAnimationResource(path,error);const auto clip=HkxReader::readClip(bytes,error);
    const auto base=HkxReader::readClip(readAnimationResource(r.source["baseline"].toString(),error),error);
    if(!clip.valid()||!base.valid())return false;
    QVector<Transformf> sampled,baseline,local,global,boundLocal,boundGlobal,sceneLocal,sceneWorld,poses;
    if(!samplePose(_skeleton,base,0,&_readyLocalPose,sampled,baseline,global,error))return false;
    for(int f=0;f<r.frames;++f){
        if(!samplePose(_skeleton,clip,clip.durationSeconds*f/(r.frames-1),&baseline,sampled,local,global,error)||
           !evaluateSceneBindings(_skeleton,_nodeBindings,local,baseline,global,boundLocal,boundGlobal,error)||
           !nativeMechanismScene(_reloadScenePlan,_nodeBindings,boundLocal,sceneLocal,sceneWorld,error))return false;
        Transformf inverse;if(!invertUniformTransform(global[_weaponBoneIndex],inverse)){error="Invalid native bolt source frame.";return false;}
        poses.append(composed(inverse,sceneWorld[_reloadTrackNodes[bolt]]));
    }
    if(!buildSimulationRig(r,rig,error,simulationMechanism(),true,poses))return false;
    rig.mechanicalSource={{"animation",path},{"animationSha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},{"duration",clip.durationSeconds}};
    return true;
}

bool ReloadMakerController::prepareRifleMotion() {
    _rifleBoltCycle.clear();_rifleBoltOpen=0;_rifleFireInterval=0;_rifleBoltSource.clear();
    const auto single=std::find_if(_animationActions.cbegin(),_animationActions.cend(),[](const auto& a){return a.family==ActionFamily::Fire&&!a.automatic;});
    const auto automatic=std::find_if(_animationActions.cbegin(),_animationActions.cend(),[](const auto& a){return a.family==ActionFamily::Fire&&a.automatic;});
    if(single==_animationActions.cend()||(!isThanatosConfiguration()&&automatic==_animationActions.cend())){_simulationError="Rifle needs its native firing motion and applicable cadence.";return false;}
    QString error;
    const auto bytes=readAnimationResource(single->resourcePath,error);
    const auto clip=HkxReader::readClip(bytes,error);
    const auto base=HkxReader::readClip(readAnimationResource(single->baselineResourcePath,error),error);
    if(!clip.valid()||!base.valid()){_simulationError="Cannot read rifle fire source: "+error;return false;}
    if(isThanatosConfiguration()) {
        // The installed Thanatos receivers are semi-automatic. Its inherited
        // automatic graph clip has a variable cadence and is not their source.
        if(_simulationAmmo.automatic){_simulationError="This Thanatos profile requires a semi-automatic receiver.";return false;}
        _rifleFireInterval=clip.durationSeconds;
    } else {
        const auto automaticClip=HkxReader::readClip(readAnimationResource(automatic->resourcePath,error),error);
        if(!automaticClip.valid()){_simulationError="Cannot read rifle fire source: "+error;return false;}
        QVector<float> events;
        for(const auto& event:automaticClip.annotations)if(event.text.compare("weaponfire",Qt::CaseInsensitive)==0)events.append(event.timeSeconds);
        std::sort(events.begin(),events.end());
        if(events.size()<2){_simulationError="Automatic-fire source has no repeated weaponfire cadence.";return false;}
        _rifleFireInterval=events[1]-events[0];
        if(_rifleFireInterval<.01f||_rifleFireInterval>2){_simulationError="Invalid automatic-fire cadence.";return false;}
        for(int i=2;i<events.size();++i)if(std::abs(events[i]-events[i-1]-_rifleFireInterval)>.002f){_simulationError="Variable fire cadence needs an explicit event schedule.";return false;}
    }
    const int frames=int(std::ceil(clip.durationSeconds*120))+1;
    if(frames<3||frames>maximumReloadFrames){_simulationError="Rifle firing motion exceeds the sampling limit.";return false;}
    QVector<Transformf> sampled,baseline,local,global,boundLocal,boundGlobal,sceneLocal,sceneWorld;
    if(!samplePose(_skeleton,base,0,&_readyLocalPose,sampled,baseline,global,error)){_simulationError=error;return false;}
    for(int f=0;f<frames;++f) {
        if(!samplePose(_skeleton,clip,clip.durationSeconds*f/(frames-1),&baseline,sampled,local,global,error)||
           !evaluateSceneBindings(_skeleton,_nodeBindings,local,baseline,global,boundLocal,boundGlobal,error)||
           !nativeMechanismScene(_reloadScenePlan,_nodeBindings,boundLocal,sceneLocal,sceneWorld,error)){_simulationError=error;return false;}
        Transformf inverse;if(!invertUniformTransform(global[_weaponBoneIndex],inverse)){_simulationError="Invalid firing reference frame.";return false;}
        _rifleBoltCycle.append(composed(inverse,sceneWorld[_reloadTrackNodes[_simulationRig->bolt]]));
    }
    float travel=0;
    for(int f=0;f<frames;++f){const float d=reloadPoseDistance(_rifleBoltCycle.front(),_rifleBoltCycle[f]);if(d>travel){travel=d;_rifleBoltOpen=f;}}
    int closed=-1;
    for(int f=_rifleBoltOpen+1;f<frames;++f)if(reloadPoseDistance(_rifleBoltCycle.front(),_rifleBoltCycle[f])<=travel*.002f){closed=f;break;}
    if(isMk18Configuration()&&travel<.15f) {
        // MK18 fires with a static bolt track. Its native reload supplies the
        // complete stroke; replay that measured path at its firing cadence.
        const auto& rig=*_simulationRig;const auto& source=rig.derivedBolt.isEmpty()?_reloadRecipe->tracks[rig.bolt].poses:rig.derivedBolt;
        _rifleBoltCycle.clear();
        for(int f=rig.closedFrame;f>=rig.openFrame;--f)_rifleBoltCycle.append(source[f]);
        _rifleBoltOpen=_rifleBoltCycle.size()-1;
        for(int f=rig.openFrame+1;f<=rig.closedFrame;++f)_rifleBoltCycle.append(source[f]);
        if(_rifleBoltOpen<1||reloadPoseDistance(_rifleBoltCycle.front(),_rifleBoltCycle[_rifleBoltOpen])<.15f){_simulationError="MK18 native reload has no complete bolt stroke.";return false;}
        _rifleBoltDuration=_rifleFireInterval;_rifleSingleDuration=clip.durationSeconds;
        _rifleBoltSource=rig.derivedBolt.isEmpty()?_reloadRecipe->source["animation"].toString():rig.mechanicalSource["animation"].toString();return true;
    }
    if(travel<.15f||closed<0){_simulationError="Single-fire source has no complete bolt stroke.";return false;}
    _rifleBoltDuration=clip.durationSeconds*closed/(frames-1);_rifleSingleDuration=clip.durationSeconds;
    _rifleBoltCycle.resize(closed+1);_rifleBoltSource=single->resourcePath;
    return true;
}

Transformf ReloadMakerController::rifleBoltPose(float progress) const {
    const float index=(_rifleBoltCycle.size()-1)+(_rifleBoltOpen-(_rifleBoltCycle.size()-1))*std::clamp(progress,0.f,1.f);
    const int first=int(index),last=std::min(first+1,int(_rifleBoltCycle.size()-1));
    return interpolatedTransform(_rifleBoltCycle[first],_rifleBoltCycle[last],index-first);
}

Transformf ReloadMakerController::rifleFireBoltPose() const {
    const float duration=_simulationAmmo.automatic?_rifleFireInterval:_rifleBoltDuration;
    float index=std::clamp((isM1AConfiguration()?_simulation.seconds:_simulation.shotSeconds)/duration,0.f,1.f)*(_rifleBoltCycle.size()-1);
    if(_simulation.willLockAfterShot()&&index>=_rifleBoltOpen)index=float(_rifleBoltOpen);
    const int first=int(index),last=std::min(first+1,int(_rifleBoltCycle.size()-1));
    return interpolatedTransform(_rifleBoltCycle[first],_rifleBoltCycle[last],index-first);
}

float ReloadMakerController::simulationPoseSeconds() const {
    return isRifleConfiguration()&&_simulation.action==SimulationAction::Fire&&_clip.durationSeconds>0
        ?std::fmod(_simulation.seconds,_clip.durationSeconds):_simulation.seconds;
}

void ReloadMakerController::setSimulationTrigger(bool pressed) {
    if(pressed&&_simulationTriggerDown)return;
    _simulationTriggerDown=pressed;
    if(!pressed){_simulation.triggerHeld=false;return;}
    if(_simulation.action==SimulationAction::Fire){_simulation.triggerHeld=_simulationAmmo.automatic;return;}
    runSimulationAction("fire");
    if(_simulation.action==SimulationAction::Fire)_simulation.triggerHeld=_simulationAmmo.automatic;
}

bool ReloadMakerController::selectTestbed(const QString& name) {
    if(_loadThread||_reloadCapturing||_rebuildTimer->isActive()){setStatus("Wait for the current weapon to finish loading.",true);return false;}
    if(name!="glock"&&name!="ru556"&&name!="m1a"&&name!="timberwolf"&&name!="l118a2"&&name!="thanatos"&&name!="mk18"&&name!="mk18-empire"&&
       name!="srep-service"&&name!="srep-honeybadger"&&name!="srep-ar10"&&name!="srep-xm16e1"){setStatus("Unknown weapon testbed.",true);return false;}
    const auto current=currentTestbed();
    if(current==name){setWorkspace(OperateWorkspace);return true;}
    if(!current.isEmpty()) {
        const auto path=_sessionPath.isEmpty()?testbedProject(current):_sessionPath;
        if(!saveSession(path))return false;
        QSettings().setValue("testbeds/"+current,path);
    }
    const auto defaultProject=testbedProject(name);
    const auto project=QSettings().value("testbeds/"+name,defaultProject).toString();
    if(!QFileInfo::exists(project)){setStatus("Open and save the "+name+" project before selecting this testbed.",true);return false;}
    openSession(project);
    if(!_pendingSession.isEmpty())_pendingSession["workspace"]=OperateWorkspace;
    return _lastError.isEmpty();
}
}
