#include "ReloadMakerController.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
bool ReloadMakerController::isBoltActionConfiguration() const {
    return currentWeapon()&&(currentWeapon()->editorId.compare("C14Timberwolf",Qt::CaseInsensitive)==0||isL118Configuration());
}
bool ReloadMakerController::prepareBoltActionMotion() {
    _boltCycle={};_boltFiredPose.clear();_boltCycleSource={};
    const auto cycle=std::find_if(_animationActions.cbegin(),_animationActions.cend(),[](const auto& a){return a.family==ActionFamily::Cycle;});
    const auto fire=std::find_if(_animationActions.cbegin(),_animationActions.cend(),[](const auto& a){return a.family==ActionFamily::Fire&&!a.automatic;});
    if(cycle==_animationActions.cend()||fire==_animationActions.cend()){_simulationError="A native bolt-cycle and firing clip are required.";return false;}
    QString error;
    const auto bytes=readAnimationResource(cycle->resourcePath,error),baseBytes=readAnimationResource(cycle->baselineResourcePath,error);
    const auto clip=HkxReader::readClip(bytes,error),base=HkxReader::readClip(baseBytes,error);
    const auto fireBytes=readAnimationResource(fire->resourcePath,error);
    const auto firing=HkxReader::readClip(fireBytes,error);
    if(!clip.valid()||!base.valid()||!firing.valid()){_simulationError="Cannot read bolt-cycle sources: "+error;return false;}
    for(const auto& name:{"WeaponBolt","WeaponExtra1","WeaponOptics2"}) {
        int track=-1;
        for(int i=0;i<_reloadRecipe->tracks.size();++i)if(_reloadRecipe->tracks[i].surface&&_reloadRecipe->tracks[i].anchor.anchorName==name) {
            if(track>=0){_simulationError="Ambiguous bolt follower: "+QString(name);return false;}track=i;
        }
        if(track<0){_simulationError="Missing bolt follower: "+QString(name);return false;}
        _boltCycle.tracks.append(track);_boltCycle.poses.append(QVector<Transformf>{});
    }
    _boltCycle.duration=clip.durationSeconds;
    const int frames=int(std::ceil(clip.durationSeconds*120))+1;
    if(frames<3||frames>maximumReloadFrames){_simulationError="Native bolt cycle exceeds the sampling limit.";return false;}
    QVector<Transformf> sampled,baseline,local,global,boundLocal,boundGlobal,sceneLocal,sceneWorld;
    if(!samplePose(_skeleton,base,0,&_readyLocalPose,sampled,baseline,global,error)){_simulationError=error;return false;}
    const auto sample=[&](const HkxClip& source,float seconds,QVector<Transformf>& result) {
        if(!samplePose(_skeleton,source,seconds,&baseline,sampled,local,global,error)||
           !evaluateSceneBindings(_skeleton,_nodeBindings,local,baseline,global,boundLocal,boundGlobal,error)||
           !nativeMechanismScene(_reloadScenePlan,_nodeBindings,boundLocal,sceneLocal,sceneWorld,error))return false;
        Transformf inverse;if(!invertUniformTransform(global[_weaponBoneIndex],inverse)){error="Invalid bolt reference frame.";return false;}
        result.clear();for(const int track:_boltCycle.tracks)result.append(composed(inverse,sceneWorld[_reloadTrackNodes[track]]));
        return true;
    };
    QVector<Transformf> poses;
    for(int f=0;f<frames;++f) {
        if(!sample(clip,clip.durationSeconds*f/(frames-1),poses)){_simulationError=error;return false;}
        for(int i=0;i<poses.size();++i)_boltCycle.poses[i].append(poses[i]);
    }
    float eject=-1;
    for(const auto& event:clip.annotations)if(event.text.compare("EjectShellCasing",Qt::CaseInsensitive)==0){eject=event.timeSeconds;break;}
    if(!analyzeBoltCycle(_boltCycle,eject,error)||!sample(firing,firing.durationSeconds,_boltFiredPose)){_simulationError=error;return false;}
    const auto hash=[](const QByteArray& data){return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());};
    _boltCycleSource={{"animation",cycle->resourcePath},{"animationSha256",hash(bytes)},
        {"baseline",cycle->baselineResourcePath},{"baselineSha256",hash(baseBytes)},
        {"fireAnimation",fire->resourcePath},{"fireSha256",hash(fireBytes)},{"ejectSeconds",eject}};
    _simulationRig->slideTracks=_boltCycle.tracks;
    return true;
}
void ReloadMakerController::applyBoltActionTargets(QHash<int,Transformf>& targets) const {
    if(_simulation.action==SimulationAction::Fire||_simulation.action==SimulationAction::Reload)return;
    for(int i=0;i<_boltCycle.tracks.size();++i) {
        const int track=_boltCycle.tracks[i];
        const auto pose=_simulation.slide<=.001f?(_simulation.spentCase?_boltFiredPose[i]:_boltCycle.poses[i].back()):boltCyclePose(_boltCycle,track,_simulation.slide,_simulation.boltClosing);
        targets.insert(_reloadTrackNodes[track],pose);
    }
}
QJsonObject ReloadMakerController::exportBoltActionMotion() const {
    if(_boltCycle.poses.isEmpty()||_boltFiredPose.size()!=_boltCycle.tracks.size()||_boltCycle.opening.isEmpty()||_boltCycle.closing.isEmpty())return {};
    QString error;QJsonArray tracks;
    for(int i=0;i<_boltCycle.tracks.size();++i) {
        QJsonArray poses;for(const auto& pose:_boltCycle.poses[i]){QJsonObject value;if(!encodePoseTransform(pose,value,error))return {};poses.append(value);}
        QJsonObject fired;if(!encodePoseTransform(_boltFiredPose[i],fired,error))return {};
        tracks.append(QJsonObject{{"track",_boltCycle.tracks[i]},{"poses",poses},{"firedRest",fired}});
    }
    const auto progress=[](const QVector<float>& values){QJsonArray a;for(float v:values)a.append(v);return a;};
    return {{"source",_boltCycleSource},{"duration",_boltCycle.duration},{"firstFrame",_boltCycle.first},{"rearFrame",_boltCycle.rear},{"lastFrame",_boltCycle.last},
        {"openingProgress",progress(_boltCycle.opening)},{"closingProgress",progress(_boltCycle.closing)},{"ejectProgress",_boltCycle.ejectProgress},
        {"inputAxis",QJsonArray{_boltCycle.inputAxis.x,_boltCycle.inputAxis.y,_boltCycle.inputAxis.z}},{"inputTravel",_boltCycle.inputTravel},{"tracks",tracks},{"release","hold"}};
}
}
