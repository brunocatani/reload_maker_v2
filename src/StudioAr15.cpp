#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include <QComboBox>
#include <QFileInfo>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
bool ReloadMakerController::isAr15Configuration() const {
    return currentWeapon()&&currentWeapon()->editorId.compare("WPNRU556",Qt::CaseInsensitive)==0;
}

bool ReloadMakerController::prepareAr15Motion() {
    _arBoltCycle.clear();_arBoltOpen=0;_arFireInterval=0;_arBoltSource.clear();
    const auto single=std::find_if(_animationActions.cbegin(),_animationActions.cend(),[](const auto& a){return a.family==ActionFamily::Fire&&!a.automatic;});
    const auto automatic=std::find_if(_animationActions.cbegin(),_animationActions.cend(),[](const auto& a){return a.family==ActionFamily::Fire&&a.automatic;});
    if(single==_animationActions.cend()||automatic==_animationActions.cend()){_simulationError="AR-15 needs both its single-shot motion and automatic-fire cadence.";return false;}
    QString error;
    const auto bytes=readAnimationResource(single->resourcePath,error);
    const auto clip=HkxReader::readClip(bytes,error);
    const auto base=HkxReader::readClip(readAnimationResource(single->baselineResourcePath,error),error);
    const auto automaticClip=HkxReader::readClip(readAnimationResource(automatic->resourcePath,error),error);
    if(!clip.valid()||!base.valid()||!automaticClip.valid()){_simulationError="Cannot read AR-15 fire source: "+error;return false;}
    QVector<float> events;
    for(const auto& event:automaticClip.annotations)if(event.text.compare("weaponfire",Qt::CaseInsensitive)==0)events.append(event.timeSeconds);
    std::sort(events.begin(),events.end());
    if(events.size()<2){_simulationError="Automatic-fire source has no repeated weaponfire cadence.";return false;}
    _arFireInterval=events[1]-events[0];
    if(_arFireInterval<.01f||_arFireInterval>2){_simulationError="Invalid automatic-fire cadence.";return false;}
    for(int i=2;i<events.size();++i)if(std::abs(events[i]-events[i-1]-_arFireInterval)>.002f){_simulationError="Variable fire cadence needs an explicit event schedule.";return false;}
    const int frames=int(std::ceil(clip.durationSeconds*120))+1;
    if(frames<3||frames>maximumReloadFrames){_simulationError="AR-15 firing motion exceeds the sampling limit.";return false;}
    QVector<Transformf> sampled,baseline,local,global,boundLocal,boundGlobal,sceneLocal,sceneWorld;
    if(!samplePose(_skeleton,base,0,&_readyLocalPose,sampled,baseline,global,error)){_simulationError=error;return false;}
    for(int f=0;f<frames;++f) {
        if(!samplePose(_skeleton,clip,clip.durationSeconds*f/(frames-1),&baseline,sampled,local,global,error)||
           !evaluateSceneBindings(_skeleton,_nodeBindings,local,baseline,global,boundLocal,boundGlobal,error)||
           !nativeMechanismScene(_reloadScenePlan,_nodeBindings,boundLocal,sceneLocal,sceneWorld,error)){_simulationError=error;return false;}
        Transformf inverse;if(!invertUniformTransform(global[_weaponBoneIndex],inverse)){_simulationError="Invalid firing reference frame.";return false;}
        _arBoltCycle.append(composed(inverse,sceneWorld[_reloadTrackNodes[_simulationRig->bolt]]));
    }
    float travel=0;
    for(int f=0;f<frames;++f){const float d=reloadPoseDistance(_arBoltCycle.front(),_arBoltCycle[f]);if(d>travel){travel=d;_arBoltOpen=f;}}
    int closed=-1;
    for(int f=_arBoltOpen+1;f<frames;++f)if(reloadPoseDistance(_arBoltCycle.front(),_arBoltCycle[f])<=travel*.002f){closed=f;break;}
    if(travel<.15f||closed<0){_simulationError="Single-fire source has no complete bolt stroke.";return false;}
    _arBoltDuration=clip.durationSeconds*closed/(frames-1);_arSingleDuration=clip.durationSeconds;
    _arBoltCycle.resize(closed+1);_arBoltSource=single->resourcePath;
    return true;
}

Transformf ReloadMakerController::ar15BoltPose(float progress) const {
    const float index=(_arBoltCycle.size()-1)+(_arBoltOpen-(_arBoltCycle.size()-1))*std::clamp(progress,0.f,1.f);
    const int first=int(index),last=std::min(first+1,int(_arBoltCycle.size()-1));
    return interpolatedTransform(_arBoltCycle[first],_arBoltCycle[last],index-first);
}

Transformf ReloadMakerController::ar15FireBoltPose() const {
    const float duration=_simulationAmmo.automatic?_arFireInterval:_arBoltDuration;
    float index=std::clamp(_simulation.shotSeconds/duration,0.f,1.f)*(_arBoltCycle.size()-1);
    if(_simulation.willLockAfterShot()&&index>=_arBoltOpen)index=float(_arBoltOpen);
    const int first=int(index),last=std::min(first+1,int(_arBoltCycle.size()-1));
    return interpolatedTransform(_arBoltCycle[first],_arBoltCycle[last],index-first);
}

float ReloadMakerController::simulationPoseSeconds() const {
    return isAr15Configuration()&&_simulation.action==SimulationAction::Fire&&_clip.durationSeconds>0
        ?std::fmod(_simulation.seconds,_clip.durationSeconds):_simulation.seconds;
}

void ReloadMakerController::setSimulationTrigger(bool pressed) {
    if(!isAr15Configuration())return;
    if(!pressed){_simulation.triggerHeld=false;return;}
    if(_simulation.action==SimulationAction::Fire){_simulation.triggerHeld=_simulationAmmo.automatic;return;}
    runSimulationAction("fire");
    if(_simulation.action==SimulationAction::Fire)_simulation.triggerHeld=_simulationAmmo.automatic;
}

bool ReloadMakerController::selectTestbed(const QString& name) {
    if(_loadThread||_reloadCapturing||_rebuildTimer->isActive()){setStatus("Wait for the current weapon to finish loading.",true);return false;}
    if(name!="glock"&&name!="ru556"){setStatus("Unknown weapon testbed.",true);return false;}
    const auto current=isGlockConfiguration()?QString("glock"):isAr15Configuration()?QString("ru556"):QString();
    if(current==name){setWorkspace(OperateWorkspace);return true;}
    if(!current.isEmpty()) {
        const auto path=_sessionPath.isEmpty()?studioDataPath(current=="glock"?"workspace/Glock19x-magazine-grips.project.json":"workspace/RU556-native-reload.project.json"):_sessionPath;
        if(!saveSession(path))return false;
        QSettings().setValue("testbeds/"+current,path);
    }
    const auto defaultProject=studioDataPath(name=="glock"?"workspace/Glock19x-magazine-grips.project.json":"workspace/RU556-native-reload.project.json");
    const auto project=QSettings().value("testbeds/"+name,defaultProject).toString();
    if(!QFileInfo::exists(project)){setStatus("Open and save the "+name+" project before selecting this testbed.",true);return false;}
    openSession(project);
    if(!_pendingSession.isEmpty())_pendingSession["workspace"]=OperateWorkspace;
    return _lastError.isEmpty();
}
}
