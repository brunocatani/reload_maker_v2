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
bool buildSimulationRig(const ReloadRecipe& r,SimulationRig& output,QString& error,WeaponMechanism profile,bool deriveHandleFromBolt,const QVector<Transformf>& boltOverride,const SimulationParts* selected) {
    error.clear();SimulationRig rig;rig.recipeId=r.id;rig.mechanism=profile;
    if(r.frames<3||r.tracks.isEmpty()){error="Capture a native reload before simulating.";return false;}
    for(int i=0;i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(t.poses.size()!=r.frames||t.parent>=r.tracks.size()||t.parent< -1){error="Invalid simulation source tracks.";return false;}
        if(t.surface&&(selected?i==selected->operating:t.anchor.anchorName.compare(profile==WeaponMechanism::Ar15||profile==WeaponMechanism::M1A?"WeaponExtra1":"WeaponBolt",Qt::CaseInsensitive)==0)) {
            if(rig.slide>=0){error="This assembly has multiple slide owners.";return false;}rig.slide=i;
        }
        if(t.surface&&(selected?i==selected->bolt:t.anchor.anchorName.compare("WeaponBolt",Qt::CaseInsensitive)==0))rig.bolt=i;
        if(t.surface&&(selected?i==selected->magazine:t.anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)==0)) {
            if(rig.magazine>=0){error="This assembly has multiple magazine owners.";return false;}rig.magazine=i;
        }
    }
    if(rig.slide<0||rig.magazine<0){error="Direct simulation currently requires a resolved slide and magazine.";return false;}
    if(profile==WeaponMechanism::PistolSlide&&sameOwner(r.tracks[rig.slide],r.tracks[rig.magazine])) {
        if(!selected){error="Slide and magazine share a source model. Select their separate native branches in Operating parts.";return false;}
        if(descendant(r,rig.slide,rig.magazine)||descendant(r,rig.magazine,rig.slide)) {
            error="The magazine carrier and operating part must be independent native branches.";return false;
        }
        rig.magazineBranchOnly=true;
    }
    if(profile!=WeaponMechanism::PistolSlide&&rig.bolt<0){error="The rifle bolt track is missing.";return false;}
    if(selected){
        if((selected->openFrame<0)!=(selected->closedFrame<0)||selected->openFrame>=r.frames||selected->closedFrame>=r.frames){error="Operating window is outside this capture.";return false;}
        if(rig.slide==rig.magazine||(profile==WeaponMechanism::Ar15&&rig.slide==rig.bolt)){error="Operating, magazine and independent charging-handle roles overlap.";return false;}
        for(auto it=selected->curves.cbegin();it!=selected->curves.cend();++it){if(it.key()<0||it.key()>=r.tracks.size()||it.value().size()!=r.frames){error="Incomplete authored movement samples.";return false;}for(const auto& pose:it.value()){QJsonObject value;if(!encodePoseTransform(pose,value,error))return false;}}
        rig.authoredCurves=selected->curves;
    }
    if(deriveHandleFromBolt) {
        if(profile!=WeaponMechanism::Ar15||rig.bolt<0){error="Native bolt translation requires an independent AR handle.";return false;}
        if(!boltOverride.isEmpty()&&boltOverride.size()!=r.frames){error="Incomplete native bolt override.";return false;}
        rig.derivedBolt=boltOverride;
        const auto& handle=simulationTrackCurve(r,rig,rig.slide);const auto& bolt=boltOverride.isEmpty()?simulationTrackCurve(r,rig,rig.bolt):boltOverride;
        for(const auto& p:handle)if(reloadPoseDistance(p,handle.back())>.01f){error="Cannot replace an already animated charging handle.";return false;}
        for(const auto& p:bolt){auto h=handle.back();h.translation.x+=p.translation.x-bolt.back().translation.x;h.translation.y+=p.translation.y-bolt.back().translation.y;h.translation.z+=p.translation.z-bolt.back().translation.z;rig.derivedHandle.append(h);}
    }
    const bool explicitWindow=selected&&selected->openFrame>=0;
    const auto& slide=simulationTrackCurve(r,rig,rig.slide);const auto& rest=explicitWindow?slide[selected->closedFrame]:slide.back();
    float travel=0;QVector<float> distance;distance.reserve(r.frames);
    for(int frame=0;frame<slide.size();++frame) {
        if(explicitWindow&&(frame<selected->openFrame||frame>selected->closedFrame)){distance.append(0);continue;}
        const auto& p=slide[frame];
        if(!reloadPoseVisible(p)){error="The slide disappears within its source motion.";return false;}
        const float d=reloadPoseDistance(rest,p);distance.append(d);travel=std::max(travel,d);
    }
    if(travel<kCompleteStroke){error="The reload has no measurable slide stroke.";return false;}
    // The last open-to-closed stroke avoids the spare-magazine relocation in
    // an empty reload's opening frames. No weapon-specific frame numbers.
    if(selected&&selected->openFrame>=0){rig.openFrame=selected->openFrame;rig.closedFrame=selected->closedFrame;
        if(rig.openFrame<0||rig.closedFrame>=r.frames||rig.closedFrame<=rig.openFrame){error="Choose an open frame followed by its closed frame.";return false;}}
    else {for(int f=r.frames-2;f>=0;--f)if(distance[f]>=travel*.995f){rig.openFrame=f;break;}
        rig.closedFrame=-1;for(int f=rig.openFrame+1;f<r.frames;++f)if(distance[f]<=travel*.002f){rig.closedFrame=f;break;}}
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
        if(rig.magazineBranchOnly?descendant(r,i,rig.magazine):sameOwner(t,r.tracks[rig.magazine]))rig.magazineTracks.append(i);
        // Receiver followers (barrel/catch) and independently animated magazine
        // descendants (feed cartridge) keep their own sampled trajectories.
        // A sibling spent-magazine copy is deliberately not a slide follower.
        if(i!=rig.slide&&!sameOwner(t,r.tracks[rig.slide])&&!descendant(r,i,rig.magazine))continue;
        float motion=0;
        for(int f=rig.openFrame;f<=rig.closedFrame;++f)
            motion=std::max(motion,reloadPoseDistance(t.poses[rig.closedFrame],t.poses[f]));
        if(i==rig.slide||motion>.01f)rig.slideTracks.append(i);
    }
    if(selected){
        if(selected->followers.isEmpty()||selected->followers.size()>16||!selected->followers.contains(rig.slide)){error="Include the operating part and every required follower (maximum 16).";return false;}
        QVector<int> followers;
        for(int index:selected->followers){if(index<0||index>=r.tracks.size()||!r.tracks[index].surface||followers.contains(index)){error="Select distinct native follower surfaces.";return false;}followers.append(index);}
        rig.slideTracks=followers;
    }
    output=std::move(rig);return true;
}
const QVector<Transformf>& simulationTrackCurve(const ReloadRecipe& r,const SimulationRig& rig,int track) {
    if(track==rig.slide&&!rig.derivedHandle.isEmpty())return rig.derivedHandle;
    if(track==rig.bolt&&!rig.derivedBolt.isEmpty())return rig.derivedBolt;
    const auto authored=rig.authoredCurves.constFind(track);if(authored!=rig.authoredCurves.cend())return authored.value();
    return r.tracks[track].poses;
}
Transformf simulationTrackPose(const ReloadRecipe& r,const SimulationRig& rig,int track,float progress) {
    const float frame=rig.closedFrame+(rig.openFrame-rig.closedFrame)*std::clamp(progress,0.f,1.f);
    if(!rig.authoredCurves.contains(track)&&!(track==rig.slide&&!rig.derivedHandle.isEmpty())&&!(track==rig.bolt&&!rig.derivedBolt.isEmpty()))return reloadTrackPose(r,track,r.duration*frame/(r.frames-1));
    const auto& curve=simulationTrackCurve(r,rig,track);const int a=int(frame),b=std::min(a+1,int(curve.size()-1));return interpolatedTransform(curve[a],curve[b],frame-a);
}
float reloadTrackStroke(const ReloadRecipe& r,int track) {
    float stroke=0;
    if(track<0||track>=r.tracks.size()||r.tracks[track].poses.isEmpty())return stroke;
    const auto& poses=r.tracks[track].poses;
    for(const auto& pose:poses)stroke=std::max(stroke,reloadPoseDistance(pose,poses.back()));
    return stroke;
}
int surfaceTrack(const ReloadRecipe& r,const QString& anchorName) {
    int found=-1;
    for(int i=0;i<r.tracks.size();++i) {
        if(!r.tracks[i].surface||r.tracks[i].anchor.anchorName.compare(anchorName,Qt::CaseInsensitive)!=0)continue;
        if(found>=0)return -1;
        found=i;
    }
    return found;
}
QVector<int> boltDrivenTracks(const ReloadRecipe& r,const SimulationRig& rig) {
    QVector<int> driven;
    const bool ar15=rig.mechanism==WeaponMechanism::Ar15;
    if((!ar15&&rig.mechanism!=WeaponMechanism::ReciprocatingBolt)||rig.bolt<0)return driven;
    const float boltStroke=reloadTrackStroke(r,rig.bolt);
    if(boltStroke<kCompleteStroke)return driven;
    const float handleStroke=ar15?reloadTrackStroke(r,rig.slide):0.f;
    const auto distances=[&](int track){
        QVector<double> values;values.reserve(r.frames);
        for(const auto& pose:r.tracks[track].poses)values.append(reloadPoseDistance(pose,r.tracks[track].poses.back()));
        return values;
    };
    const auto correlation=[](const QVector<double>& a,const QVector<double>& b){
        double ma=0,mb=0;for(int i=0;i<a.size();++i){ma+=a[i];mb+=b[i];}ma/=a.size();mb/=b.size();
        double ab=0,aa=0,bb=0;for(int i=0;i<a.size();++i){ab+=(a[i]-ma)*(b[i]-mb);aa+=(a[i]-ma)*(a[i]-ma);bb+=(b[i]-mb)*(b[i]-mb);}
        return aa>0&&bb>0?ab/std::sqrt(aa*bb):0.0;
    };
    const auto bolt=distances(rig.bolt);
    const auto handle=ar15?distances(rig.slide):QVector<double>();
    for(int i=0;i<r.tracks.size();++i) {
        if(i==rig.slide||i==rig.bolt||!r.tracks[i].surface||rig.magazineTracks.contains(i)||rig.slideTracks.contains(i))continue;
        const float stroke=reloadTrackStroke(r,i);
        if(stroke<=.01f)continue;
        const auto part=distances(i);
        int moving=0,withBolt=0,withHandle=0;
        for(int f=0;f<r.frames;++f) {
            if(part[f]<=.1*stroke)continue;
            ++moving;
            withBolt+=bolt[f]>.1*boltStroke?1:0;
            withHandle+=handleStroke>.01f&&handle[f]>.1*handleStroke?1:0;
        }
        if(moving>0&&withBolt>=.8*moving&&withHandle<=.2*moving&&correlation(part,bolt)>=.7)driven.append(i);
    }
    return driven;
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
    if(rig.customSetup){QJsonArray curves;auto indices=rig.authoredCurves.keys();std::sort(indices.begin(),indices.end());for(int index:indices){QJsonArray poses;QString error;for(const auto& p:rig.authoredCurves[index]){QJsonObject value;if(!encodePoseTransform(p,value,error))return {};poses.append(value);}curves.append(QJsonObject{{"track",index},{"poses",poses}});}result["operatingSetup"]=rig.authoring;result["authoredCurves"]=curves;result["motionSource"]=rig.mechanicalSource;}
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
