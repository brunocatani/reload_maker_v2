#include "WeaponSimulation.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
namespace {
bool sameOwner(const ReloadTrack& a,const ReloadTrack& b) {
    return a.anchor.ownerRecord.compare(b.anchor.ownerRecord,Qt::CaseInsensitive)==0 &&
        a.anchor.ownerModel.compare(b.anchor.ownerModel,Qt::CaseInsensitive)==0;
}
bool descendant(const ReloadRecipe& r,int child,int parent) {
    for(int count=0;child>=0&&count<r.tracks.size();++count) {
        if(child==parent)return true;
        child=r.tracks[child].parent;
    }
    return false;
}
}
bool buildSimulationRig(const ReloadRecipe& r,SimulationRig& output,QString& error,WeaponMechanism profile,bool deriveHandleFromBolt,const QVector<Transformf>& boltOverride) {
    error.clear();SimulationRig rig;rig.recipeId=r.id;rig.mechanism=profile;
    if(r.frames<3||r.tracks.isEmpty()){error="Capture a native reload before simulating.";return false;}
    for(int i=0;i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(t.poses.size()!=r.frames||t.parent>=r.tracks.size()||t.parent< -1){error="Invalid simulation source tracks.";return false;}
        if(t.surface&&t.anchor.anchorName.compare(profile==WeaponMechanism::Ar15||profile==WeaponMechanism::M1A?"WeaponExtra1":"WeaponBolt",Qt::CaseInsensitive)==0) {
            if(rig.slide>=0){error="This assembly has multiple slide owners.";return false;}rig.slide=i;
        }
        if(t.surface&&t.anchor.anchorName.compare("WeaponBolt",Qt::CaseInsensitive)==0)rig.bolt=i;
        if(t.surface&&t.anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)==0) {
            if(rig.magazine>=0){error="This assembly has multiple magazine owners.";return false;}rig.magazine=i;
        }
    }
    if(rig.slide<0||rig.magazine<0){error="Direct simulation currently requires a resolved slide and magazine.";return false;}
    if(profile==WeaponMechanism::PistolSlide&&sameOwner(r.tracks[rig.slide],r.tracks[rig.magazine])) {
        error="This weapon shares one mesh owner for slide and magazine; separate magazine visibility is required.";return false;
    }
    if(profile!=WeaponMechanism::PistolSlide&&rig.bolt<0){error="The rifle bolt track is missing.";return false;}
    if(deriveHandleFromBolt) {
        if(profile!=WeaponMechanism::Ar15||rig.bolt<0){error="Native bolt translation requires an independent AR handle.";return false;}
        if(!boltOverride.isEmpty()&&boltOverride.size()!=r.frames){error="Incomplete native bolt override.";return false;}
        rig.derivedBolt=boltOverride;
        const auto& handle=r.tracks[rig.slide].poses;const auto& bolt=boltOverride.isEmpty()?r.tracks[rig.bolt].poses:boltOverride;
        for(const auto& p:handle)if(reloadPoseDistance(p,handle.back())>.01f){error="Cannot replace an already animated charging handle.";return false;}
        for(const auto& p:bolt){auto h=handle.back();h.translation.x+=p.translation.x-bolt.back().translation.x;h.translation.y+=p.translation.y-bolt.back().translation.y;h.translation.z+=p.translation.z-bolt.back().translation.z;rig.derivedHandle.append(h);}
    }
    const auto& slide=rig.derivedHandle.isEmpty()?r.tracks[rig.slide].poses:rig.derivedHandle;const auto& rest=slide.back();
    float travel=0;QVector<float> distance;distance.reserve(r.frames);
    for(const auto& p:slide) {
        if(!reloadPoseVisible(p)){error="The slide disappears within its source motion.";return false;}
        const float d=reloadPoseDistance(rest,p);distance.append(d);travel=std::max(travel,d);
    }
    if(travel<.15f){error="The reload has no measurable slide stroke.";return false;}
    // The last open-to-closed stroke avoids the spare-magazine relocation in
    // an empty reload's opening frames. No weapon-specific frame numbers.
    for(int f=r.frames-2;f>=0;--f)if(distance[f]>=travel*.995f){rig.openFrame=f;break;}
    rig.closedFrame=-1;
    for(int f=rig.openFrame+1;f<r.frames;++f)if(distance[f]<=travel*.002f){rig.closedFrame=f;break;}
    if(rig.closedFrame<=rig.openFrame){error="The reload does not finish a complete slide-closing stroke.";return false;}
    float previous=distance[rig.openFrame];
    for(int f=rig.openFrame+1;f<=rig.closedFrame;++f) {
        if(distance[f]>previous+travel*.03f){error="The source slide stroke reverses; review its motion before direct simulation.";return false;}
        previous=distance[f];
    }
    for(int i=0;i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(profile==WeaponMechanism::Ar15||profile==WeaponMechanism::M1A||profile==WeaponMechanism::ReciprocatingBolt) {
            if(descendant(r,i,rig.magazine))rig.magazineTracks.append(i);
            if(i==rig.slide||(profile==WeaponMechanism::M1A&&i==rig.bolt))rig.slideTracks.append(i);
            continue;
        }
        if(sameOwner(t,r.tracks[rig.magazine]))rig.magazineTracks.append(i);
        // Receiver followers (barrel/catch) and independently animated magazine
        // descendants (feed cartridge) keep their own sampled trajectories.
        // A sibling spent-magazine copy is deliberately not a slide follower.
        if(i!=rig.slide&&!sameOwner(t,r.tracks[rig.slide])&&!descendant(r,i,rig.magazine))continue;
        float motion=0;
        for(int f=rig.openFrame;f<=rig.closedFrame;++f)
            motion=std::max(motion,reloadPoseDistance(t.poses[rig.closedFrame],t.poses[f]));
        if(i==rig.slide||motion>.01f)rig.slideTracks.append(i);
    }
    output=std::move(rig);return true;
}
Transformf simulationTrackPose(const ReloadRecipe& r,const SimulationRig& rig,int track,float progress) {
    const float frame=rig.closedFrame+(rig.openFrame-rig.closedFrame)*std::clamp(progress,0.f,1.f);
    if(track==rig.slide&&!rig.derivedHandle.isEmpty()){const int a=int(frame),b=std::min(a+1,int(rig.derivedHandle.size()-1));return interpolatedTransform(rig.derivedHandle[a],rig.derivedHandle[b],frame-a);}
    if(track==rig.bolt&&!rig.derivedBolt.isEmpty()){const int a=int(frame),b=std::min(a+1,int(rig.derivedBolt.size()-1));return interpolatedTransform(rig.derivedBolt[a],rig.derivedBolt[b],frame-a);}
    return reloadTrackPose(r,track,r.duration*frame/(r.frames-1));
}
QJsonObject simulationRigJson(const ReloadRecipe& r,const SimulationRig& rig,bool springReturn) {
    const auto records=[&](const QVector<int>& indices) {
        QJsonArray result;for(int i:indices){const auto& t=r.tracks[i];result.append(QJsonObject{{"track",i},{"name",t.anchor.anchorName},{"label",t.label},{"record",t.anchor.ownerRecord},{"model",t.anchor.ownerModel},{"path",t.anchor.anchorPath}});}return result;
    };
    QJsonObject result{{"format","reload-maker-simulation"},{"version",1},{"recipeId",rig.recipeId},
        {"slideTrack",rig.slide},{"magazineTrack",rig.magazine},{"openFrame",rig.openFrame},{"closedFrame",rig.closedFrame},
        {"springReturn",springReturn&&rig.mechanism!=WeaponMechanism::BoltAction},{"slideTracks",records(rig.slideTracks)},{"magazineTracks",records(rig.magazineTracks)}};
    if(!rig.derivedHandle.isEmpty()){QJsonArray poses;QString error;for(const auto& p:rig.derivedHandle){QJsonObject value;if(!encodePoseTransform(p,value,error))return {};poses.append(value);}result["derivedHandleMotion"]=QJsonObject{{"method","native-bolt-translation"},{"sourceTrack",rig.bolt},{"track",rig.slide},{"poses",poses}};}
    if(!rig.derivedBolt.isEmpty()){QJsonArray poses;QString error;for(const auto& p:rig.derivedBolt){QJsonObject value;if(!encodePoseTransform(p,value,error))return {};poses.append(value);}result["derivedBoltMotion"]=QJsonObject{{"track",rig.bolt},{"source",rig.mechanicalSource},{"poses",poses}};}
    return result;
}
bool WeaponSimulation::fire() {
    if(!canFire())return false;
    chambered=false;++shotsFired;_cycleArmed=false;
    if(mechanism==WeaponMechanism::BoltAction)spentCase=true;
    action=SimulationAction::Fire;seconds=0;shotSeconds=0;return true;
}
bool WeaponSimulation::reload(float rackAt,float feedAt) {
    if(!ammoKnown()||action==SimulationAction::Fire||action==SimulationAction::Reload||action==SimulationAction::Held||magazine.busy())return false;
    reloadStartedLoaded=chambered;reloadStartedOpen=slideLocked||slide>=.95f;
    reloadRackAt=rackAt;reloadFeedAt=feedAt;reloadCycled=false;reloadFed=false;
    action=SimulationAction::Reload;seconds=0;slide=0;handle=0;triggerHeld=false;magazine.reset();return true;
}
bool WeaponSimulation::dropMagazine() {
    if(action==SimulationAction::Fire||action==SimulationAction::Reload||action==SimulationAction::Held||!magazine.visible()||magazine.busy())return false;
    magazine.drop();magazineRounds=0;return true;
}
bool WeaponSimulation::grab() {
    if((action!=SimulationAction::Idle&&action!=SimulationAction::Returning)||magazine.busy())return false;
    if(mechanism==WeaponMechanism::Ar15&&action==SimulationAction::Returning&&handle<=.001f)return false;
    grabStart=inputProgress();_pullFromLock=slideLocked;_pulledRearward=false;
    _pullSnapshot={chambered,slideLocked,_cycleArmed,magazineRounds,roundsEjected,slide,handle,spentCase,boltClosing,casesEjected};
    action=SimulationAction::Held;return true;
}
void WeaponSimulation::drag(float value) {
    if(action!=SimulationAction::Held||!std::isfinite(value))return;
    if(mechanism==WeaponMechanism::BoltAction) {
        slide=std::clamp(value,0.f,1.f);
        if(slide>=boltEjectProgress) {
            if(chambered){chambered=false;++roundsEjected;}
            if(spentCase){spentCase=false;++casesEjected;}
        }
        if(slide>=std::max(.999f,boltEjectProgress)){_cycleArmed=true;boltClosing=true;}
        if(slide<=.001f){finishClosing();boltClosing=false;}
        return;
    }
    if(mechanism==WeaponMechanism::Ar15) {
        handle=std::clamp(value,0.f,1.f);
        if(handle>=.95f) {
            slideLocked=false;
            if(!_cycleArmed){if(chambered){chambered=false;++roundsEjected;}_cycleArmed=true;}
        }
        if(!slideLocked)slide=handle;
        if(slide<=.001f&&!slideLocked)finishClosing();
        return;
    }
    if(value>grabStart+.025f)_pulledRearward=true;
    if(_pullFromLock&&_pulledRearward)slideLocked=false;
    slide=_pullFromLock&&!_pulledRearward?1.f:std::clamp(value,0.f,1.f);
    if(value>=.95f&&!_cycleArmed) {
        if(chambered){chambered=false;++roundsEjected;}
        _cycleArmed=true;
    }
    if(slide<=.001f&&!slideLocked)finishClosing();
}
void WeaponSimulation::release(bool cancel,bool springReturn) {
    if(action!=SimulationAction::Held)return;
    if(cancel) {
        slide=_pullSnapshot.bolt;handle=_pullSnapshot.handle;chambered=_pullSnapshot.chambered;slideLocked=_pullSnapshot.locked;_cycleArmed=_pullSnapshot.cycle;
        magazineRounds=_pullSnapshot.rounds;roundsEjected=_pullSnapshot.ejected;
        spentCase=_pullSnapshot.spent;boltClosing=_pullSnapshot.closing;casesEjected=_pullSnapshot.cases;
        action=SimulationAction::Idle;return;
    }
    if(mechanism==WeaponMechanism::BoltAction){action=SimulationAction::Idle;return;}
    if(mechanism==WeaponMechanism::Ar15) {
        if(_cycleArmed&&magazine.seated()&&magazineRounds==0){slide=1;slideLocked=true;}
        action=springReturn&&handle>0?SimulationAction::Returning:SimulationAction::Idle;
        return;
    }
    if((_pullFromLock&&!_pulledRearward)||(_cycleArmed&&pistolLock&&magazine.seated()&&magazineRounds==0)) {
        slide=1;slideLocked=true;action=SimulationAction::Idle;return;
    }
    slideLocked=false;
    action=springReturn&&slide>0?SimulationAction::Returning:SimulationAction::Idle;
}
bool WeaponSimulation::tick(float delta,float duration) {
    if(!std::isfinite(delta)||delta<0)return false;
    if(mechanism==WeaponMechanism::Ar15&&action==SimulationAction::Returning) {
        handle=std::max(0.f,handle-delta*4.f);
        if(!slideLocked)slide=std::max(0.f,slide-delta*4.f);
        if(handle==0&&(slide==0||slideLocked)){if(!slideLocked)finishClosing();action=SimulationAction::Idle;}
        return false;
    }
    if((mechanism==WeaponMechanism::Ar15||mechanism==WeaponMechanism::ReciprocatingBolt)&&action==SimulationAction::Fire) {
        if(!std::isfinite(fireCycleSeconds)||fireCycleSeconds<.01f||delta>60)return false;
        seconds+=delta;shotSeconds+=delta;
        // Capacity is bounded; each iteration consumes a real round. Large
        // deterministic steps obey the same feed/catch order as live frames.
        for(int remaining=capacity+1;shotSeconds>=fireCycleSeconds&&remaining>0;--remaining) {
            shotSeconds-=fireCycleSeconds;
            if(willLockAfterShot()){slide=1;slideLocked=true;triggerHeld=false;action=SimulationAction::Idle;seconds=0;return true;}
            _cycleArmed=true;slide=0;finishClosing();
            if(!triggerHeld||!chambered){triggerHeld=false;action=SimulationAction::Idle;seconds=0;return true;}
            chambered=false;++shotsFired;
        }
        return false;
    }
    if(action==SimulationAction::Returning){slide=std::max(0.f,slide-delta*4.f);if(slide==0){finishClosing();action=SimulationAction::Idle;}}
    else if(action==SimulationAction::Fire||action==SimulationAction::Reload) {
        seconds=std::min(seconds+delta,duration);
        if(action==SimulationAction::Reload) {
            if(!reloadCycled&&reloadRackAt>=0&&seconds>=reloadRackAt) {
                if(chambered){chambered=false;++roundsEjected;}
                if(spentCase){spentCase=false;++casesEjected;}
                reloadCycled=true;
            }
            if(!reloadFed&&reloadFeedAt>=0&&seconds>=reloadFeedAt) {
                magazineRounds=capacity;_cycleArmed=true;finishClosing();reloadFed=true;
            }
        }
        if(seconds>=duration){
            if(mechanism==WeaponMechanism::BoltAction&&action==SimulationAction::Fire){action=SimulationAction::Idle;seconds=0;return true;}
            const bool lastShot=willLockAfterShot();
            if(action==SimulationAction::Reload){magazine.reset();if(!reloadFed)magazineRounds=capacity;slideLocked=false;}
            action=SimulationAction::Idle;seconds=0;_cycleArmed=true;
            if(lastShot){slide=1;slideLocked=true;}
            else {slide=0;finishClosing();boltClosing=false;}
            return true;
        }
    }
    return false;
}
void WeaponSimulation::finishClosing() {
    if(_cycleArmed&&!chambered&&magazine.seated()&&magazineRounds>0){--magazineRounds;chambered=true;}
    _cycleArmed=false;slideLocked=false;
}
void WeaponSimulation::configureAmmunition(int count,bool lockPistol,WeaponMechanism profile){capacity=std::clamp(count,0,65535);mechanism=profile;pistolLock=profile!=WeaponMechanism::BoltAction&&(lockPistol||profile!=WeaponMechanism::PistolSlide);reset();}
void WeaponSimulation::reset(){const int count=capacity;const bool lock=pistolLock;const auto profile=mechanism;const float cycle=fireCycleSeconds,eject=boltEjectProgress;*this={};capacity=count;pistolLock=lock;mechanism=profile;fireCycleSeconds=cycle;boltEjectProgress=eject;chambered=count>0;magazineRounds=std::max(0,count-1);}
bool WeaponSimulation::slideRelease() {
    if(mechanism==WeaponMechanism::BoltAction)return false;
    if(!ammoKnown()||action!=SimulationAction::Idle||magazine.busy()||(!slideLocked&&slide<=.001f))return false;
    if(slideLocked||slide>=.95f)_cycleArmed=true;
    slideLocked=false;action=SimulationAction::Returning;return true;
}
bool WeaponSimulation::magazineOut(){return action==SimulationAction::Idle&&magazine.animateOut();}
bool WeaponSimulation::magazineIn(const MagazineRail& rail) {
    if(!ammoKnown()||action!=SimulationAction::Idle||!magazine.animateIn(rail))return false;
    magazineRounds=capacity;return true;
}
bool WeaponSimulation::grabMagazine(const MagazineRail& rail) {
    if(!ammoKnown()||action!=SimulationAction::Idle||!magazine.grab(rail))return false;
    magazineRounds=capacity;return true;
}
void WeaponSimulation::cancelMagazine(){if(magazine.carried()){magazine.cancel();magazineRounds=0;}}
QString WeaponSimulation::magazineDescription()const {
    if(magazine.seated())return magazineRounds==0?"Seated empty":magazineRounds==capacity?"Seated full":"Seated partial";
    if(magazine.phase==MagazinePhase::Outgoing)return magazineRounds==0?"Empty magazine out":"Loaded magazine out";
    if(magazine.phase==MagazinePhase::OutsideEmpty)return magazineRounds==0?"Empty magazine clear":"Loaded magazine clear";
    return magazinePhaseName(magazine.phase);
}
}
