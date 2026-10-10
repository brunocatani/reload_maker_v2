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
// The track's pose in its ancestor's frame never changes over the reload.
bool ridesOn(const ReloadRecipe& r,int child,int ancestor) {
    if(child==ancestor||!descendant(r,child,ancestor))return false;
    const auto& c=r.tracks[child].poses;const auto& a=r.tracks[ancestor].poses;
    if(c.isEmpty()||c.size()!=a.size())return false;
    const auto local=[&](int i) {
        const auto& q=a[i].rotation;const Quaternionf inverse{-q.x,-q.y,-q.z,q.w};
        Transformf pose;
        pose.translation=rotated(inverse,Vec3f{c[i].translation.x-a[i].translation.x,c[i].translation.y-a[i].translation.y,c[i].translation.z-a[i].translation.z});
        pose.rotation=multiplied(inverse,c[i].rotation);
        return pose;
    };
    const auto first=local(0);
    for(int i=1;i<c.size();++i)if(reloadPoseDistance(first,local(i))>1e-3f)return false;
    return true;
}
bool magazineSurface(const ReloadTrack& t) {
    return t.surface&&t.anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)==0;
}
}
int magazineCarrier(const ReloadRecipe& r,int track) {
    // Only a second WeaponMagazine node is lifted; an inner node chosen as the
    // carrier (the USP's WeaponMagazineChild2) stays the author's choice.
    if(track<0||track>=r.tracks.size()||!magazineSurface(r.tracks[track]))return track;
    int carrier=track;
    for(int a=r.tracks[track].parent,count=0;a>=0&&a<r.tracks.size()&&count<r.tracks.size();a=r.tracks[a].parent,++count) {
        if(magazineSurface(r.tracks[a])&&ridesOn(r,track,a))carrier=a;
    }
    return carrier;
}
int detectMagazineTrack(const ReloadRecipe& r) {
    int found=-1;
    for(int i=0;i<r.tracks.size();++i) {
        if(!magazineSurface(r.tracks[i]))continue;
        const int carrier=magazineCarrier(r,i);
        if(found>=0&&found!=carrier)return -1;
        found=carrier;
    }
    return found;
}
bool buildSimulationRig(const ReloadRecipe& r,SimulationRig& output,QString& error,WeaponMechanism profile,bool deriveHandleFromBolt,const QVector<Transformf>& boltOverride,const SimulationParts* selected) {
    error.clear();SimulationRig rig;rig.recipeId=r.id;rig.mechanism=profile;
    if(r.frames<3||r.tracks.isEmpty()){error="Capture a native reload before simulating.";return false;}
    for(int i=0;i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(t.poses.size()!=r.frames||t.parent>=r.tracks.size()||t.parent< -1){error="Invalid simulation source tracks.";return false;}
        if(t.surface&&(selected?i==selected->operating:t.anchor.anchorName.compare(profile==WeaponMechanism::Ar15||profile==WeaponMechanism::M1A||profile==WeaponMechanism::CellLever?"WeaponExtra1":"WeaponBolt",Qt::CaseInsensitive)==0)) {
            if(rig.slide>=0){error="This assembly has multiple slide owners.";return false;}rig.slide=i;
        }
        if(t.surface&&(selected?i==selected->bolt:t.anchor.anchorName.compare("WeaponBolt",Qt::CaseInsensitive)==0))rig.bolt=i;
        if(selected&&t.surface&&i==selected->magazine)rig.magazine=i;
    }
    if(!selected) {
        bool any=false;for(const auto& t:r.tracks)any|=magazineSurface(t);
        rig.magazine=detectMagazineTrack(r);
        if(any&&rig.magazine<0){error="This assembly has multiple magazine owners.";return false;}
    }
    if(profile==WeaponMechanism::CellLever)rig.bolt=rig.slide;
    if(rig.slide<0||rig.magazine<0){error="Direct simulation currently requires a resolved slide and magazine.";return false;}
    // Some bolt-action receivers contain both the action and magazine carrier.
    // Sharing a NIF must not make the detached magazine carry the whole bolt.
    if((profile==WeaponMechanism::PistolSlide||profile==WeaponMechanism::BoltAction||profile==WeaponMechanism::CellLever)&&sameOwner(r.tracks[rig.slide],r.tracks[rig.magazine])) {
        if(profile==WeaponMechanism::PistolSlide&&!selected){error="Slide and magazine share a source model. Select their separate native branches in Operating parts.";return false;}
        if(descendant(r,rig.slide,rig.magazine)||descendant(r,rig.magazine,rig.slide)) {
            error="The magazine carrier and operating part must be independent native branches.";return false;
        }
        rig.magazineBranchOnly=true;
    }
    if(profile!=WeaponMechanism::PistolSlide&&rig.bolt<0){error="The rifle bolt track is missing.";return false;}
    if(selected){
        if((selected->openFrame<0)!=(selected->closedFrame<0)||selected->openFrame>=r.frames||selected->closedFrame>=r.frames){error="Operating window is outside this capture.";return false;}
        if(rig.slide==rig.magazine||(profile==WeaponMechanism::Ar15&&rig.slide==rig.bolt&&!selected->singleHandle)){error="Operating, magazine and independent charging-handle roles overlap.";return false;}
        rig.singleHandle=selected->singleHandle;
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
    if(profile==WeaponMechanism::Ar15&&!explicitWindow&&rig.bolt>=0) {
        // Independent handles can seat before the bolt finishes travelling
        // (MP5's slap does). Keep that entire closing sequence in the window.
        const auto& bolt=simulationTrackCurve(r,rig,rig.bolt);
        const float stroke=reloadPoseDistance(bolt[rig.openFrame],bolt.back());
        if(stroke>=kCompleteStroke) {
            int closed=-1;for(int f=rig.openFrame+1;f<bolt.size();++f)if(reloadPoseDistance(bolt[f],bolt.back())<=stroke*.002f){closed=f;break;}
            if(closed<0){error="The independent bolt never finishes closing.";return false;}
            rig.closedFrame=std::max(rig.closedFrame,closed);
        }
    }
    float previous=distance[rig.openFrame];
    for(int f=rig.openFrame+1;f<=rig.closedFrame;++f) {
        if(distance[f]>previous+travel*.03f){error="The source slide stroke reverses; review its motion before direct simulation.";return false;}
        previous=distance[f];
    }
    for(int i=0;i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(profile==WeaponMechanism::Ar15||profile==WeaponMechanism::M1A||profile==WeaponMechanism::ReciprocatingBolt||profile==WeaponMechanism::CellLever) {
            if(descendant(r,i,rig.magazine))rig.magazineTracks.append(i);
            if(i==rig.slide||(profile==WeaponMechanism::M1A&&i==rig.bolt))rig.slideTracks.append(i);
            continue;
        }
        if((rig.magazineBranchOnly||profile==WeaponMechanism::BoltAction)?descendant(r,i,rig.magazine):sameOwner(t,r.tracks[rig.magazine]))rig.magazineTracks.append(i);
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
    const auto native=rig.nativeCurves.constFind(track);if(native!=rig.nativeCurves.cend())return native.value();
    return r.tracks[track].poses;
}
int simulationFrameCount(const ReloadRecipe& r,const SimulationRig& rig){return rig.motionFrames>0?rig.motionFrames:r.frames;}
float simulationStrokeDuration(const ReloadRecipe& r,const SimulationRig& rig){return rig.strokeSeconds>=0?rig.strokeSeconds:(rig.motionDuration>0?rig.motionDuration:r.duration)*(rig.closedFrame-rig.openFrame)/(simulationFrameCount(r,rig)-1);}
float simulationNativeFrame(const ReloadRecipe& r,const SimulationRig& rig,float frame) {
    if(rig.nativeProgress.isEmpty())return simulationFrameCount(r,rig)==r.frames?frame:frame*(r.frames-1)/(simulationFrameCount(r,rig)-1);
    frame=std::clamp(frame,0.f,float(rig.nativeProgress.size()-1));
    const int a=int(frame),b=std::min(a+1,int(rig.nativeProgress.size()-1));
    return (rig.nativeProgress[a]+(rig.nativeProgress[b]-rig.nativeProgress[a])*(frame-a))*(r.frames-1);
}
namespace {
float simulationFrame(const SimulationRig& rig,float progress){return rig.closedFrame+(rig.openFrame-rig.closedFrame)*std::clamp(progress,0.f,1.f);}
}
bool simulationTrackAuthored(const SimulationRig& rig,int track) {
    return rig.authoredCurves.contains(track)||(track==rig.slide&&!rig.derivedHandle.isEmpty())||(track==rig.bolt&&!rig.derivedBolt.isEmpty());
}
Transformf simulationNativeTrackPose(const ReloadRecipe& r,const SimulationRig& rig,int track,float progress) {
    return reloadTrackPose(r,track,r.duration*simulationNativeFrame(r,rig,simulationFrame(rig,progress))/(r.frames-1));
}
Transformf simulationTrackPose(const ReloadRecipe& r,const SimulationRig& rig,int track,float progress) {
    if(!simulationTrackAuthored(rig,track))return simulationNativeTrackPose(r,rig,track,progress);
    const float frame=simulationFrame(rig,progress);
    const auto& curve=simulationTrackCurve(r,rig,track);const int a=int(frame),b=std::min(a+1,int(curve.size()-1));return interpolatedTransform(curve[a],curve[b],frame-a);
}
Transformf simulationGrabPose(const SimulationRig& rig,int track,float progress) {
    const auto& curve=rig.grabCurves[track];
    const float f=std::clamp(progress,0.f,1.f)*(curve.size()-1);
    const int a=int(f),b=std::min(a+1,int(curve.size()-1));
    return interpolatedTransform(curve[a],curve[b],f-a);
}
bool buildGrabDeployment(const ReloadRecipe& recipe,SimulationRig& rig,double sourceSeconds,QString& error,bool independentHandle) {
    if(sourceSeconds<0)return true;
    if(sourceSeconds>(rig.motionDuration>0?rig.motionDuration:recipe.duration)||rig.slide==rig.bolt){error="Deployment needs a separate operating handle and a pose inside the reload clip";return false;}
    const auto closed=simulationTrackPose(recipe,rig,rig.bolt,0).translation;
    const auto open=simulationTrackPose(recipe,rig,rig.bolt,1).translation;
    const QVector3D axis(open.x-closed.x,open.y-closed.y,open.z-closed.z);
    const float length=axis.lengthSquared();
    if(length<1e-6f){error="The bolt has no measurable pull stroke";return false;}
    const auto sourcePose=[&](int track) {
        if(!rig.authoredCurves.contains(track))return reloadTrackPose(recipe,track,float(sourceSeconds));
        const auto& curve=rig.authoredCurves[track];
        const float progress=float(sourceSeconds)/(rig.motionDuration>0?rig.motionDuration:recipe.duration);
        float frame=progress*(curve.size()-1);
        if(!rig.nativeProgress.isEmpty()) {
            const auto it=std::lower_bound(rig.nativeProgress.cbegin(),rig.nativeProgress.cend(),progress);
            const int b=std::clamp(int(it-rig.nativeProgress.cbegin()),1,int(rig.nativeProgress.size()-1)),a=b-1;
            const float span=rig.nativeProgress[b]-rig.nativeProgress[a];
            frame=a+(span>0?std::clamp((progress-rig.nativeProgress[a])/span,0.f,1.f):0.f);
        }
        const int a=std::clamp(int(frame),0,int(curve.size()-1)),b=std::min(a+1,int(curve.size()-1));
        return interpolatedTransform(curve[a],curve[b],std::clamp(frame-a,0.f,1.f));
    };
    const auto deploy=sourcePose(rig.slide);
    // Independent handles can unfold at their forward stop while the bolt is locked open.
    const auto reference=independentHandle?closed:sourcePose(rig.bolt).translation;
    rig.grabCurves.clear();
    auto heldTracks=rig.slideTracks;if(!heldTracks.contains(rig.bolt))heldTracks.append(rig.bolt);
    for(int i=0;i<=64;++i) {
        const float u=float(i)/64;
        float low=0,high=1;
        for(int step=0;step<24;++step) {
            const float p=(low+high)*.5f;
            const auto b=simulationTrackPose(recipe,rig,rig.bolt,p).translation;
            const float depth=QVector3D::dotProduct(QVector3D(b.x-closed.x,b.y-closed.y,b.z-closed.z),axis)/length;
            if(depth<u)low=p;else high=p;
        }
        const float progress=i==0?0:i==64?1:(low+high)*.5f;
        const auto bolt=simulationTrackPose(recipe,rig,rig.bolt,progress).translation;
        for(int track:heldTracks) {
            auto pose=simulationTrackPose(recipe,rig,track,progress);
            if(track==rig.slide||(independentHandle&&track!=rig.bolt)) {
                pose=track==rig.slide?deploy:sourcePose(track);
                pose.translation.x+=bolt.x-reference.x;pose.translation.y+=bolt.y-reference.y;pose.translation.z+=bolt.z-reference.z;
            }
            rig.grabCurves[track].append(pose);
        }
    }
    return true;
}
Vec3f simulationControllerStroke(const ReloadRecipe& r,const SimulationRig& rig,const Vec3f& contact) {
    const int track=rig.mechanism==WeaponMechanism::PistolSlide?rig.slide:rig.bolt;
    const auto a=simulationTrackPose(r,rig,track,0).translation,b=simulationTrackPose(r,rig,track,1).translation;
    Vec3f delta{b.x-a.x,b.y-a.y,b.z-a.z};
    if(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z<.0001f) {
        const auto start=composed(simulationTrackPose(r,rig,rig.slide,0),Transformf{contact}).translation;
        const auto end=composed(simulationTrackPose(r,rig,rig.slide,1),Transformf{contact}).translation;
        delta={end.x-start.x,end.y-start.y,end.z-start.z};
    }
    return delta;
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
        {"springReturn",springReturn&&rig.mechanism!=WeaponMechanism::BoltAction&&rig.mechanism!=WeaponMechanism::CellLever},{"slideTracks",records(rig.slideTracks)},{"magazineTracks",records(rig.magazineTracks)}};
    if(rig.motionFrames>0&&rig.motionFrames!=r.frames){result["motionFrames"]=rig.motionFrames;result["motionDuration"]=rig.motionDuration;}
    if(!rig.nativeProgress.isEmpty()){QJsonArray values;for(float p:rig.nativeProgress)values.append(p);result["nativeProgress"]=values;}
    if(!rig.derivedHandle.isEmpty()){QJsonArray poses;QString error;for(const auto& p:rig.derivedHandle){QJsonObject value;if(!encodePoseTransform(p,value,error))return {};poses.append(value);}result["derivedHandleMotion"]=QJsonObject{{"method","native-bolt-translation"},{"sourceTrack",rig.bolt},{"track",rig.slide},{"poses",poses}};}
    if(!rig.derivedBolt.isEmpty()){QJsonArray poses;QString error;for(const auto& p:rig.derivedBolt){QJsonObject value;if(!encodePoseTransform(p,value,error))return {};poses.append(value);}result["derivedBoltMotion"]=QJsonObject{{"track",rig.bolt},{"source",rig.mechanicalSource},{"poses",poses}};}
    if(rig.customSetup){QJsonArray curves;auto indices=rig.authoredCurves.keys();std::sort(indices.begin(),indices.end());for(int index:indices){QJsonArray poses;QString error;for(const auto& p:rig.authoredCurves[index]){QJsonObject value;if(!encodePoseTransform(p,value,error))return {};poses.append(value);}curves.append(QJsonObject{{"track",index},{"poses",poses}});}result["operatingSetup"]=rig.authoring;result["authoredCurves"]=curves;result["motionSource"]=rig.mechanicalSource;}
    return result;
}
bool WeaponSimulation::fire() {
    if(!canFire())return false;
    if(openBolt||cellFed())--magazineRounds;
    chambered=false;++shotsFired;_cycleArmed=false;_fireCycleIndex=0;
    if(mechanism==WeaponMechanism::BoltAction)spentCase=true;
    action=SimulationAction::Fire;seconds=0;shotSeconds=0;return true;
}
bool WeaponSimulation::reload(float rackAt,float feedAt) {
    if(!ammoKnown()||action==SimulationAction::Fire||action==SimulationAction::Reload||action==SimulationAction::Held||magazine.busy())return false;
    reloadStartedLoaded=chambered;reloadStartedOpen=slideLocked||slide>=.95f;
    reloadRackAt=rackAt;reloadFeedAt=feedAt;reloadCycled=false;reloadFed=false;
    action=SimulationAction::Reload;seconds=0;slide=0;handle=0;notchLatched=false;triggerHeld=false;magazine.reset();return true;
}
bool WeaponSimulation::dropMagazine() {
    if(cellFed())return false; // Only the physical lever unlocks the cell.
    if(action==SimulationAction::Fire||action==SimulationAction::Reload||action==SimulationAction::Held||!magazine.visible()||magazine.busy())return false;
    magazine.drop();magazineRounds=0;return true;
}
bool WeaponSimulation::grab() {
    if((action!=SimulationAction::Idle&&action!=SimulationAction::Returning)||magazine.busy())return false;
    if(mechanism==WeaponMechanism::Ar15&&action==SimulationAction::Returning&&handle<=.001f)return false;
    grabStart=inputProgress();_pullFromLock=slideLocked;_pulledRearward=false;
    _pullSnapshot={chambered,slideLocked,_cycleArmed,magazineRounds,roundsEjected,slide,handle,spentCase,boltClosing,casesEjected,notchLatched};
    action=SimulationAction::Held;return true;
}
void WeaponSimulation::drag(float value) {
    if(action!=SimulationAction::Held||!std::isfinite(value))return;
    if(cellFed()) {
        slide=std::clamp(value,0.f,1.f);
        if(slide<=.001f)_cycleArmed=false;
        if(!_cycleArmed&&slide>=.95f){_cycleArmed=true;if(magazine.seated()){magazine.drop();magazineRounds=0;}}
        return;
    }
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
        if(openBolt){handle=std::clamp(value,0.f,1.f);if(handle>=.95f){slide=1;slideLocked=true;}else if(!slideLocked)slide=handle;return;}
        if(notchLatched){
            if(magazine.seated()&&magazineRounds>0&&std::abs(value-grabStart)>.025f){notchLatched=slideLocked=false;_cycleArmed=true;}
            else {handle=slide=1;return;}
        }
        handle=std::clamp(value,0.f,1.f);
        if(handle>=.95f) {
            slideLocked=false;
            if(!_cycleArmed){if(chambered){chambered=false;++roundsEjected;}_cycleArmed=true;}
            if(hkSlap&&magazineRounds==0&&!_pullSnapshot.notch){notchLatched=slideLocked=true;handle=slide=1;return;}
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
    if(cellFed()){action=SimulationAction::Idle;return;} // Ejection is committed; cancelling cannot recreate a cell.
    if(cancel) {
        slide=_pullSnapshot.bolt;handle=_pullSnapshot.handle;chambered=_pullSnapshot.chambered;slideLocked=_pullSnapshot.locked;_cycleArmed=_pullSnapshot.cycle;
        magazineRounds=_pullSnapshot.rounds;roundsEjected=_pullSnapshot.ejected;
        spentCase=_pullSnapshot.spent;boltClosing=_pullSnapshot.closing;casesEjected=_pullSnapshot.cases;
        notchLatched=_pullSnapshot.notch;
        action=SimulationAction::Idle;return;
    }
    if(mechanism==WeaponMechanism::BoltAction){action=SimulationAction::Idle;return;}
    if(mechanism==WeaponMechanism::Ar15) {
        if(notchLatched){handle=slide=1;slideLocked=true;action=SimulationAction::Idle;return;}
        if(_cycleArmed&&pistolLock&&magazine.seated()&&magazineRounds==0){slide=1;slideLocked=true;}
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
    if((mechanism==WeaponMechanism::Ar15||mechanism==WeaponMechanism::ReciprocatingBolt||
        ((mechanism==WeaponMechanism::PistolSlide||cellFed())&&fireCycleSeconds>0))&&action==SimulationAction::Fire) {
        if(!std::isfinite(fireCycleSeconds)||fireCycleSeconds<.01f||delta>60)return false;
        seconds+=delta;shotSeconds+=delta;
        // Capacity is bounded; each iteration consumes a real round. Large
        // deterministic steps obey the same feed/catch order as live frames.
        for(int remaining=capacity+1;remaining>0;--remaining) {
            const float interval=currentShotInterval();
            if(!std::isfinite(interval)||interval<.01f||shotSeconds<interval)break;
            shotSeconds-=interval;
            if(!fireCycleSchedule.isEmpty())_fireCycleIndex=(_fireCycleIndex+1)%fireCycleSchedule.size();
            if(cellFed()){
                if(triggerHeld&&magazine.seated()&&magazineRounds>0){--magazineRounds;++shotsFired;continue;}
                triggerHeld=false;action=SimulationAction::Idle;seconds=0;return true;
            }
            if(openBolt){
                if(triggerHeld&&magazine.seated()&&magazineRounds>0){--magazineRounds;++shotsFired;continue;}
                slideLocked=magazineRounds>0||!triggerHeld;slide=slideLocked?1.f:std::max(0.f,1.f-shotSeconds*4.f);triggerHeld=false;action=!slideLocked&&slide>0?SimulationAction::Returning:SimulationAction::Idle;seconds=0;return true;
            }
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
            if(openBolt){slide=1;slideLocked=true;chambered=false;_cycleArmed=false;}
            else if(lastShot){slide=1;slideLocked=true;}
            else {slide=0;finishClosing();boltClosing=false;}
            return true;
        }
    }
    return false;
}
void WeaponSimulation::finishClosing() {
    if(openBolt||cellFed()){chambered=false;_cycleArmed=false;return;}
    if(_cycleArmed&&!chambered&&magazine.seated()&&magazineRounds>0){--magazineRounds;chambered=true;}
    _cycleArmed=false;slideLocked=false;
}
void WeaponSimulation::configureAmmunition(int count,bool lockPistol,WeaponMechanism profile){capacity=std::clamp(count,0,65535);mechanism=profile;pistolLock=profile!=WeaponMechanism::BoltAction&&(lockPistol||profile!=WeaponMechanism::PistolSlide);reset();}
void WeaponSimulation::reset(){const int count=capacity;const bool lock=pistolLock,hk=hkSlap,open=openBolt;const auto profile=mechanism;const float cycle=fireCycleSeconds,eject=boltEjectProgress;auto schedule=std::move(fireCycleSchedule);*this={};capacity=count;pistolLock=lock;hkSlap=hk;openBolt=open;mechanism=profile;fireCycleSeconds=cycle;fireCycleSchedule=std::move(schedule);boltEjectProgress=eject;chambered=!open&&!cellFed()&&count>0;magazineRounds=std::max(0,count-((open||cellFed())?0:1));if(open){slideLocked=count>0;slide=slideLocked?1.f:0.f;}}
bool WeaponSimulation::slideRelease() {
    if(cellFed()||mechanism==WeaponMechanism::BoltAction||(openBolt&&magazine.seated()&&magazineRounds>0))return false;
    if(!ammoKnown()||action!=SimulationAction::Idle||magazine.busy()||(!slideLocked&&slide<=.001f))return false;
    if(slideLocked||slide>=.95f)_cycleArmed=true;
    notchLatched=false;slideLocked=false;action=SimulationAction::Returning;return true;
}
bool WeaponSimulation::magazineOut(){return !cellFed()&&action==SimulationAction::Idle&&magazine.animateOut();}
bool WeaponSimulation::magazineIn(const MagazineRail& rail) {
    if((cellFed()&&slide<.95f)||!ammoKnown()||action!=SimulationAction::Idle||!magazine.animateIn(rail))return false;
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
