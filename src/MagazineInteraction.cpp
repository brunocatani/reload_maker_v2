#include "MagazineInteraction.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>

namespace reloadmaker {
namespace {
QVector3D v(Vec3f p){return {p.x,p.y,p.z};}
bool finite(const QVector3D& p){return std::isfinite(p.x())&&std::isfinite(p.y())&&std::isfinite(p.z());}
}
bool buildMagazineRail(const ReloadRecipe& r,int full,const QString& name,const Transformf& connection,
    const QVector<QVector3D>& body,const QVector<QVector3D>& weapon,MagazineRail& out,QString& error,bool finalApproach,int selectedEmpty) {
    error.clear();MagazineRail rail;rail.fullTrack=full;rail.connection=connection;rail.connectionName=name;
    if(full<0||full>=r.tracks.size()||body.size()<3||weapon.isEmpty()||name.isEmpty()){error="Magazine connection or native geometry is missing.";return false;}
    const auto& main=r.tracks[full];
    if(selectedEmpty>=r.tracks.size()||(selectedEmpty>=0&&!r.tracks[selectedEmpty].surface)){error="Empty magazine needs a native surface track.";return false;}
    rail.emptyTrack=selectedEmpty;
    for(int i=0;selectedEmpty<0&&i<r.tracks.size();++i) {
        const auto& t=r.tracks[i];
        if(i!=full&&t.surface&&t.parent<0&&t.anchor.ownerRecord==main.anchor.ownerRecord&&t.anchor.ownerModel==main.anchor.ownerModel&&
            reloadPoseDistance(t.poses.front(),main.poses.front())<.1f) {
            if(rail.emptyTrack>=0){error="Several empty-magazine copies share this attachment; identify the intended one.";return false;}rail.emptyTrack=i;
        }
    }
    // The Glock drum uses one body and a separate cartridge. Its own native
    // extraction track supplies the rail; ammunition controls the cartridge.
    if(rail.emptyTrack<0)rail.emptyTrack=full;
    QVector3D low=body.front(),high=low;
    for(const auto& p:body){if(!finite(p)){error="Invalid native magazine vertex.";return false;}for(int a=0;a<3;++a){low[a]=std::min(low[a],p[a]);high[a]=std::max(high[a],p[a]);}}
    rail.handle=(low+high)*.5f;
    const float size=(high-low).length();
    if(size<.01f){error="Magazine geometry has no extent.";return false;}
    const auto& empty=r.tracks[rail.emptyTrack];
    const auto& rest=finalApproach?empty.poses.back():empty.poses.front();
    QVector3D approach;
    for(int sample=0;sample<empty.poses.size();++sample) {
        // A shared drum body begins rotating as soon as it is removed. Its
        // final approach to the seated frame gives the aligned insertion
        // direction, reversed here for extraction. Use a shorter sample
        // distance because the drum's bulk is much wider than its magwell stem.
        const auto& pose=empty.poses[rail.sharedBody()?empty.poses.size()-1-sample:sample];
        const auto delta=v(pose.translation)-v(rest.translation);const float length=delta.length();
        const auto& a=pose.rotation;const auto& b=rest.rotation;
        const float dot=std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);
        // Use motion while the empty magazine remains aligned with the well,
        // before the hand's free rotation/throw. A connection has no universal
        // axis convention; source motion establishes its outward direction.
        if(length>=size*(rail.sharedBody()?.035f:.12f)&&length<=size*.4f&&reloadPoseVisible(pose)) {
            if(approach.isNull())approach=delta.normalized();
            if(dot>.9998f){rail.outward=delta.normalized();break;}
        }
    }
    // A native visual approach can rock/rotate all the way into the well.
    // Its final translation still defines a straight hand-input direction;
    // buildMagazineAnimation retains the complete independent visual curve.
    if(finalApproach&&rail.outward.isNull())rail.outward=approach;
    if(rail.outward.lengthSquared()<.9f){error="No aligned magazine extraction segment was found.";return false;}
    float magazineMin=std::numeric_limits<float>::max(),weaponMax=-magazineMin;
    float radius=0;
    for(const auto& p:body){magazineMin=std::min(magazineMin,QVector3D::dotProduct(p,rail.outward));const auto d=p-rail.handle;radius=std::max(radius,(d-rail.outward*QVector3D::dotProduct(d,rail.outward)).length());}
    for(const auto& p:weapon){if(!finite(p)){error="Invalid native weapon vertex.";return false;}weaponMax=std::max(weaponMax,QVector3D::dotProduct(p,rail.outward));}
    const float clearance=size*.035f;
    rail.magazineMinProjection=magazineMin;rail.weaponMaxProjection=weaponMax;rail.clearance=clearance;
    rail.travel=weaponMax-magazineMin+clearance;
    rail.captureRadius=std::max(size*.035f,radius*.55f);rail.releaseMargin=rail.captureRadius*.6f;
    if(!std::isfinite(rail.travel)||rail.travel<size*.3f||rail.travel>size*8){error="Magazine clearance is inconsistent with this assembly.";return false;}
    out=rail;return true;
}
Transformf magazineAnimationDelta(const MagazineRail& rail,float distance) {
    if(rail.motion.isEmpty())return Transformf{{rail.outward.x()*distance,rail.outward.y()*distance,rail.outward.z()*distance}};
    if(distance>=rail.travel) {
        auto result=rail.motion.back();const auto extra=rail.outward*(distance-rail.travel);
        result.translation.x+=extra.x();result.translation.y+=extra.y();result.translation.z+=extra.z();return result;
    }
    // The user's straight stroke drives normalized visual progress. Its
    // length and angle do not require tracing the animation's spatial bends.
    const float sample=std::clamp(distance/rail.travel,0.f,1.f)*rail.motionDistances.back();
    const auto next=std::upper_bound(rail.motionDistances.cbegin(),rail.motionDistances.cend(),sample);
    const int last=std::min(int(next-rail.motionDistances.cbegin()),int(rail.motion.size()-1)),first=std::max(0,last-1);
    const float span=rail.motionDistances[last]-rail.motionDistances[first];
    return interpolatedTransform(rail.motion[first],rail.motion[last],span>0?(sample-rail.motionDistances[first])/span:0);
}
QVector3D magazineInputOffset(const MagazineRail& rail,float distance){return rail.outward*distance;}
QVector3D magazineAnimationOffset(const MagazineRail& rail,float distance) {
    return v(composed(magazineAnimationDelta(rail,distance),Transformf{{rail.handle.x(),rail.handle.y(),rail.handle.z()}}).translation)-rail.handle;
}
bool buildMagazineAnimation(const ReloadRecipe& recipe,float entrySeconds,const QVector<QVector3D>& body,MagazineRail& rail,QString& error) {
    error.clear();
    if(rail.fullTrack<0||rail.fullTrack>=recipe.tracks.size()||entrySeconds<=0||entrySeconds>=recipe.duration||body.isEmpty()) {error="Native magazine insertion interval is unavailable.";return false;}
    const auto& poses=recipe.tracks[rail.fullTrack].poses;
    Transformf endInverse,startInverse;
    if(!invertUniformTransform(poses.back(),endInverse)||!invertUniformTransform(poses.front(),startInverse)){error="Invalid magazine insertion reference.";return false;}
    const auto correction=composed(poses.front(),endInverse);
    MagazineRail candidate=rail;candidate.motion={Transformf{}};candidate.motionDistances={0};
    const int first=std::clamp(qRound(entrySeconds/recipe.duration*(recipe.frames-1)),0,recipe.frames-2);
    float length=0;
    for(int i=recipe.frames-2;i>=first;--i) {
        if(!reloadPoseVisible(poses[i])){error="Magazine disappears within its insertion approach.";return false;}
        const auto delta=composed(composed(correction,poses[i]),startInverse);
        const float step=reloadPoseDistance(candidate.motion.back(),delta);
        if(step<1e-5f)continue;
        length+=step;candidate.motion.append(delta);candidate.motionDistances.append(length);
    }
    if(candidate.motion.size()<3||length<.1f){error="Native magazine insertion approach has no motion.";return false;}
    float minimum=std::numeric_limits<float>::max();
    for(const auto& point:body)minimum=std::min(minimum,QVector3D::dotProduct(v(composed(candidate.motion.back(),Transformf{{point.x(),point.y(),point.z()}}).translation),rail.outward));
    const float extension=std::max(rail.captureRadius,rail.weaponMaxProjection-minimum+rail.clearance);
    if(!std::isfinite(extension)||extension>rail.travel*4){error="Curved magazine clearance is inconsistent with its geometry.";return false;}
    auto clear=candidate.motion.back();const auto extra=rail.outward*extension;
    clear.translation.x+=extra.x();clear.translation.y+=extra.y();clear.translation.z+=extra.z();length+=extension;
    candidate.motion.append(clear);candidate.motionDistances.append(length);
    rail=std::move(candidate);return true;
}
bool buildManualMagazineAnimation(const ReloadRecipe& recipe,const PartMechanism& movement,
    const QVector<QVector3D>& body,MagazineRail& rail,QString& error) {
    error.clear();
    if(rail.fullTrack<0||rail.fullTrack>=recipe.tracks.size()||body.isEmpty()||
       !sameMechanismPart(movement.anchor,recipe.tracks[rail.fullTrack].anchor)||!validateMechanism(movement,error)) {
        if(error.isEmpty())error="Choose a saved movement for the exact full magazine carrier";return false;
    }
    const auto& rest=recipe.tracks[rail.fullTrack].poses.front();Transformf inverse,startInverse;
    if(!invertUniformTransform(rest,inverse)||!invertUniformTransform(mechanismDelta(movement,0),startInverse)) {error="Invalid authored magazine rest frame";return false;}
    MagazineRail candidate=rail;candidate.motion={Transformf{}};candidate.motionDistances={0};float length=0;
    // Ordered Move/Rotate stages are the visible path. The independent input
    // guide continues to drive progress without imposing these wrist turns.
    const int samples=movement.stages.size()*16;
    for(int i=1;i<=samples;++i) {
        const auto local=composed(mechanismDelta(movement,float(i)/16),startInverse);
        const auto delta=composed(composed(rest,local),inverse);const float step=reloadPoseDistance(candidate.motion.back(),delta);
        if(step<1e-5f)continue;length+=step;candidate.motion.append(delta);candidate.motionDistances.append(length);
    }
    if(candidate.motion.size()<2||length<.1f){error="The magazine movement has no measurable extraction";return false;}
    float minimum=std::numeric_limits<float>::max();
    for(const auto& point:body)minimum=std::min(minimum,QVector3D::dotProduct(v(composed(candidate.motion.back(),Transformf{{point.x(),point.y(),point.z()}}).translation),rail.outward));
    if(!std::isfinite(minimum)||minimum<rail.weaponMaxProjection+rail.clearance) {error="Extend the saved magazine movement until the complete body clears the weapon";return false;}
    rail=std::move(candidate);return true;
}
bool MagazineMotion::animateOut(){if(!seated())return false;phase=MagazinePhase::Outgoing;elapsed=0;distance=0;offset={};return true;}
bool MagazineMotion::animateIn(const MagazineRail& rail){if(phase!=MagazinePhase::Absent&&phase!=MagazinePhase::OutsideEmpty)return false;phase=MagazinePhase::Incoming;elapsed=0;distance=rail.travel;offset=magazineInputOffset(rail,distance);return true;}
bool MagazineMotion::grab(const MagazineRail& rail){if(phase!=MagazinePhase::Absent&&phase!=MagazinePhase::OutsideEmpty)return false;phase=MagazinePhase::Free;distance=rail.travel+rail.captureRadius*2;offset=magazineAnimationOffset(rail,distance);return true;}
void MagazineMotion::drop(){phase=MagazinePhase::Absent;offset={};distance=0;elapsed=0;}
void MagazineMotion::cancel(){if(carried())drop();}
void MagazineMotion::reset(){*this={};}
void MagazineMotion::moveFree(const MagazineRail& rail,const QVector3D& p) {
    if(phase!=MagazinePhase::Free||!finite(p))return;
    const auto step=p-offset;const float squared=step.lengthSquared();
    // Swept capture against the whole finite guide, keeping the entry depth.
    const float along=QVector3D::dotProduct(step,rail.outward),start=QVector3D::dotProduct(offset,rail.outward);
    const float denominator=squared-along*along;
    float fraction=denominator>std::max(1e-8f,squared*1e-6f)?std::clamp((along*start-QVector3D::dotProduct(step,offset))/denominator,0.f,1.f):0;
    const float depth=std::clamp(start+fraction*along,0.f,rail.travel);
    if(squared>1e-10f)fraction=std::clamp((along*depth-QVector3D::dotProduct(step,offset))/squared,0.f,1.f);
    const auto nearest=offset+step*fraction,guide=rail.outward*depth;
    offset=p;
    if((nearest-guide).length()<=rail.captureRadius){phase=MagazinePhase::Rail;distance=depth;offset=guide;}
}
void MagazineMotion::moveRail(const MagazineRail& rail,float d) {
    if(phase!=MagazinePhase::Rail||!std::isfinite(d))return;
    distance=std::max(0.f,d);offset=magazineInputOffset(rail,distance);
    if(distance>rail.travel+rail.releaseMargin){phase=MagazinePhase::Free;offset=magazineAnimationOffset(rail,distance);}
    else if(distance<=rail.travel*.005f){reset();}
}
void MagazineMotion::tick(const MagazineRail& rail,float delta) {
    if(!animated()||!std::isfinite(delta)||delta<0)return;
    elapsed=std::min(.6f,elapsed+delta);const float t=elapsed/.6f;const float smooth=t*t*(3-2*t);
    distance=rail.travel*(phase==MagazinePhase::Outgoing?smooth:1-smooth);offset=magazineInputOffset(rail,distance);
    if(elapsed>=.6f){if(phase==MagazinePhase::Incoming)reset();else phase=MagazinePhase::OutsideEmpty;}
}
QString magazinePhaseName(MagazinePhase p) {
    switch(p){case MagazinePhase::Seated:return "Seated full";case MagazinePhase::Absent:return "Absent";case MagazinePhase::Outgoing:return "Empty magazine out";
    case MagazinePhase::OutsideEmpty:return "Empty magazine clear";case MagazinePhase::Incoming:return "Full magazine in";case MagazinePhase::Free:return "Full magazine at cursor";case MagazinePhase::Rail:return "Full magazine on rail";}
    return {};
}
QJsonObject magazineRailJson(const MagazineRail& r) {
    QJsonObject connection;QString error;if(!encodePoseTransform(r.connection,connection,error))return {};
    const auto array=[](QVector3D p){return QJsonArray{p.x(),p.y(),p.z()};};
    Transformf inverse;QVector3D localAxis=r.outward;
    if(invertUniformTransform(r.connection,inverse))localAxis=v(rotated(inverse.rotation,{r.outward.x(),r.outward.y(),r.outward.z()}));
    QJsonObject result{{"version",r.motion.isEmpty()?1:3},{"connectionName",r.connectionName},{"connection",connection},{"outwardInWeapon",array(r.outward)},
        {"outwardInConnection",array(localAxis)},{"handleInWeapon",array(r.handle)},{"travel",r.travel},{"captureRadius",r.captureRadius},
        {"releaseMargin",r.releaseMargin},{"fullTrack",r.fullTrack},{"emptyTrack",r.emptyTrack},{"sharedBody",r.sharedBody()},{"animationSeconds",.6},
        {"magazineMinProjection",r.magazineMinProjection},{"weaponMaxProjection",r.weaponMaxProjection},{"clearance",r.clearance}};
    if(!r.motion.isEmpty()) {
        QJsonArray samples;
        for(int i=0;i<r.motion.size();++i){QJsonObject delta;if(!encodePoseTransform(r.motion[i],delta,error))return {};samples.append(QJsonObject{{"progress",r.motionDistances[i]/r.motionDistances.back()},{"deltaInWeapon",delta}});}
        result["inputPath"]="straight";result["motionSamples"]=samples;
    }
    if(!r.inputSetup.isEmpty())result["inputSetup"]=r.inputSetup;
    return result;
}
}
