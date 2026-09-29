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
bool buildSimulationRig(const ReloadRecipe& r,SimulationRig& output,QString& error) {
    error.clear();SimulationRig rig;rig.recipeId=r.id;
    if(r.frames<3||r.tracks.isEmpty()){error="Capture a native reload before simulating.";return false;}
    for(int i=0;i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(t.poses.size()!=r.frames||t.parent>=r.tracks.size()||t.parent< -1){error="Invalid simulation source tracks.";return false;}
        if(t.surface&&t.anchor.anchorName.compare("WeaponBolt",Qt::CaseInsensitive)==0) {
            if(rig.slide>=0){error="This assembly has multiple slide owners.";return false;}rig.slide=i;
        }
        if(t.surface&&t.anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)==0) {
            if(rig.magazine>=0){error="This assembly has multiple magazine owners.";return false;}rig.magazine=i;
        }
    }
    if(rig.slide<0||rig.magazine<0){error="Direct simulation currently requires a resolved slide and magazine.";return false;}
    if(sameOwner(r.tracks[rig.slide],r.tracks[rig.magazine])) {
        error="This weapon shares one mesh owner for slide and magazine; separate magazine visibility is required.";return false;
    }
    const auto& slide=r.tracks[rig.slide].poses;const auto& rest=slide.back();
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
    return reloadTrackPose(r,track,r.duration*frame/(r.frames-1));
}
QJsonObject simulationRigJson(const ReloadRecipe& r,const SimulationRig& rig,bool springReturn) {
    const auto records=[&](const QVector<int>& indices) {
        QJsonArray result;for(int i:indices){const auto& t=r.tracks[i];result.append(QJsonObject{{"track",i},{"name",t.anchor.anchorName},{"label",t.label},{"record",t.anchor.ownerRecord},{"model",t.anchor.ownerModel},{"path",t.anchor.anchorPath}});}return result;
    };
    return {{"format","reload-maker-simulation"},{"version",1},{"recipeId",rig.recipeId},
        {"slideTrack",rig.slide},{"magazineTrack",rig.magazine},{"openFrame",rig.openFrame},{"closedFrame",rig.closedFrame},
        {"springReturn",springReturn},{"slideTracks",records(rig.slideTracks)},{"magazineTracks",records(rig.magazineTracks)}};
}
bool WeaponSimulation::fire() {
    if(action!=SimulationAction::Idle||slide>.001f||magazine.busy())return false;
    action=SimulationAction::Fire;seconds=0;return true;
}
bool WeaponSimulation::reload() {
    if(action==SimulationAction::Fire||action==SimulationAction::Reload||action==SimulationAction::Held||magazine.busy())return false;
    action=SimulationAction::Reload;seconds=0;slide=0;magazine.reset();return true;
}
bool WeaponSimulation::dropMagazine() {
    if(action==SimulationAction::Fire||action==SimulationAction::Reload||action==SimulationAction::Held||!magazine.visible()||magazine.busy())return false;
    magazine.drop();return true;
}
bool WeaponSimulation::grab() {
    if((action!=SimulationAction::Idle&&action!=SimulationAction::Returning)||magazine.busy())return false;
    grabStart=slide;action=SimulationAction::Held;return true;
}
void WeaponSimulation::drag(float value) {if(action==SimulationAction::Held&&std::isfinite(value))slide=std::clamp(value,0.f,1.f);}
void WeaponSimulation::release(bool cancel,bool springReturn) {
    if(action!=SimulationAction::Held)return;
    if(cancel)slide=grabStart;
    action=!cancel&&springReturn&&slide>0?SimulationAction::Returning:SimulationAction::Idle;
}
bool WeaponSimulation::tick(float delta,float duration) {
    if(!std::isfinite(delta)||delta<0)return false;
    if(action==SimulationAction::Returning){slide=std::max(0.f,slide-delta*4.f);if(slide==0)action=SimulationAction::Idle;}
    else if(action==SimulationAction::Fire||action==SimulationAction::Reload) {
        seconds=std::min(seconds+delta,duration);
        if(seconds>=duration){action=SimulationAction::Idle;slide=0;seconds=0;return true;}
    }
    return false;
}
void WeaponSimulation::reset(){*this={};}
}
