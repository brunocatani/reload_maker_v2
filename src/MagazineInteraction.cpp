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
    const QVector<QVector3D>& body,const QVector<QVector3D>& weapon,MagazineRail& out,QString& error,bool finalApproach) {
    error.clear();MagazineRail rail;rail.fullTrack=full;rail.connection=connection;rail.connectionName=name;
    if(full<0||full>=r.tracks.size()||body.size()<3||weapon.isEmpty()||name.isEmpty()){error="Magazine connection or native geometry is missing.";return false;}
    const auto& main=r.tracks[full];
    for(int i=0;i<r.tracks.size();++i) {
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
        if(length>=size*(rail.sharedBody()?.035f:.12f)&&length<=size*.4f&&dot>.9998f&&reloadPoseVisible(pose)){rail.outward=delta.normalized();break;}
    }
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
Transformf magazineGuideDelta(const MagazineRail& rail,float distance) {
    if(rail.guide.isEmpty())return Transformf{{rail.outward.x()*distance,rail.outward.y()*distance,rail.outward.z()*distance}};
    if(distance>=rail.travel) {
        auto result=rail.guide.back();const auto extra=rail.outward*(distance-rail.travel);
        result.translation.x+=extra.x();result.translation.y+=extra.y();result.translation.z+=extra.z();return result;
    }
    const auto next=std::upper_bound(rail.guideDistances.cbegin(),rail.guideDistances.cend(),std::max(0.f,distance));
    const int last=std::min(int(next-rail.guideDistances.cbegin()),int(rail.guide.size()-1)),first=std::max(0,last-1);
    const float span=rail.guideDistances[last]-rail.guideDistances[first];
    return interpolatedTransform(rail.guide[first],rail.guide[last],span>0?(distance-rail.guideDistances[first])/span:0);
}
QVector3D magazineGuideOffset(const MagazineRail& rail,float distance) {
    return v(composed(magazineGuideDelta(rail,distance),Transformf{{rail.handle.x(),rail.handle.y(),rail.handle.z()}}).translation)-rail.handle;
}
bool buildMagazineGuide(const ReloadRecipe& recipe,float entrySeconds,const QVector<QVector3D>& body,MagazineRail& rail,QString& error) {
    error.clear();
    if(rail.fullTrack<0||rail.fullTrack>=recipe.tracks.size()||entrySeconds<=0||entrySeconds>=recipe.duration||body.isEmpty()) {error="Native magazine insertion interval is unavailable.";return false;}
    const auto& poses=recipe.tracks[rail.fullTrack].poses;
    Transformf endInverse,startInverse;
    if(!invertUniformTransform(poses.back(),endInverse)||!invertUniformTransform(poses.front(),startInverse)){error="Invalid magazine insertion reference.";return false;}
    const auto correction=composed(poses.front(),endInverse);
    MagazineRail candidate=rail;candidate.guide={Transformf{}};candidate.guideDistances={0};
    const int first=std::clamp(qRound(entrySeconds/recipe.duration*(recipe.frames-1)),0,recipe.frames-2);
    float length=0;
    for(int i=recipe.frames-2;i>=first;--i) {
        if(!reloadPoseVisible(poses[i])){error="Magazine disappears within its insertion approach.";return false;}
        const auto delta=composed(composed(correction,poses[i]),startInverse);
        const float step=reloadPoseDistance(candidate.guide.back(),delta);
        if(step<1e-5f)continue;
        length+=step;candidate.guide.append(delta);candidate.guideDistances.append(length);
    }
    if(candidate.guide.size()<3||length<.1f){error="Native magazine insertion approach has no motion.";return false;}
    float minimum=std::numeric_limits<float>::max();
    for(const auto& point:body)minimum=std::min(minimum,QVector3D::dotProduct(v(composed(candidate.guide.back(),Transformf{{point.x(),point.y(),point.z()}}).translation),rail.outward));
    const float extension=std::max(rail.captureRadius,rail.weaponMaxProjection-minimum+rail.clearance);
    if(!std::isfinite(extension)||extension>rail.travel*4){error="Curved magazine clearance is inconsistent with its geometry.";return false;}
    auto clear=candidate.guide.back();const auto extra=rail.outward*extension;
    clear.translation.x+=extra.x();clear.translation.y+=extra.y();clear.translation.z+=extra.z();length+=extension;
    candidate.guide.append(clear);candidate.guideDistances.append(length);candidate.travel=length;
    rail=std::move(candidate);return true;
}
bool MagazineMotion::animateOut(){if(!seated())return false;phase=MagazinePhase::Outgoing;elapsed=0;distance=0;offset={};return true;}
bool MagazineMotion::animateIn(const MagazineRail& rail){if(phase!=MagazinePhase::Absent&&phase!=MagazinePhase::OutsideEmpty)return false;phase=MagazinePhase::Incoming;elapsed=0;distance=rail.travel;offset=magazineGuideOffset(rail,distance);return true;}
bool MagazineMotion::grab(const MagazineRail& rail){if(phase!=MagazinePhase::Absent&&phase!=MagazinePhase::OutsideEmpty)return false;phase=MagazinePhase::Free;distance=rail.travel+rail.captureRadius*2;offset=magazineGuideOffset(rail,distance);return true;}
void MagazineMotion::drop(){phase=MagazinePhase::Absent;offset={};distance=0;elapsed=0;}
void MagazineMotion::cancel(){if(carried())drop();}
void MagazineMotion::reset(){*this={};}
void MagazineMotion::moveFree(const MagazineRail& rail,const QVector3D& p) {
    if(phase!=MagazinePhase::Free||!finite(p))return;
    if(!rail.guide.isEmpty()) {
        const auto u=p-offset;const float a=u.lengthSquared();float nearest=std::numeric_limits<float>::max(),captured=0;
        for(int i=1;i<rail.guide.size();++i) {
            const auto begin=magazineGuideOffset(rail,rail.guideDistances[i-1]),end=magazineGuideOffset(rail,rail.guideDistances[i]);
            const auto v=end-begin,w=offset-begin;const float b=QVector3D::dotProduct(u,v),c=v.lengthSquared(),d=QVector3D::dotProduct(u,w),e=QVector3D::dotProduct(v,w);
            if(c<1e-10f)continue;
            const float denom=a*c-b*b;
            float s=denom>1e-8f?std::clamp((b*e-c*d)/denom,0.f,1.f):0;
            float t=std::clamp((b*s+e)/c,0.f,1.f);
            if(a>1e-10f)s=std::clamp((b*t-d)/a,0.f,1.f);
            t=std::clamp((b*s+e)/c,0.f,1.f);
            const float squared=(w+u*s-v*t).lengthSquared();
            if(squared<nearest){nearest=squared;captured=rail.guideDistances[i-1]+t*(rail.guideDistances[i]-rail.guideDistances[i-1]);}
        }
        offset=p;
        if(nearest<=rail.captureRadius*rail.captureRadius){phase=MagazinePhase::Rail;distance=captured;offset=magazineGuideOffset(rail,distance);}
        return;
    }
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
    distance=std::max(0.f,d);offset=magazineGuideOffset(rail,distance);
    if(distance>rail.travel+rail.releaseMargin){phase=MagazinePhase::Free;}
    else if(distance<=rail.travel*.005f){reset();}
}
void MagazineMotion::tick(const MagazineRail& rail,float delta) {
    if(!animated()||!std::isfinite(delta)||delta<0)return;
    elapsed=std::min(.6f,elapsed+delta);const float t=elapsed/.6f;const float smooth=t*t*(3-2*t);
    distance=rail.travel*(phase==MagazinePhase::Outgoing?smooth:1-smooth);offset=magazineGuideOffset(rail,distance);
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
    QJsonObject result{{"version",r.guide.isEmpty()?1:2},{"connectionName",r.connectionName},{"connection",connection},{"outwardInWeapon",array(r.outward)},
        {"outwardInConnection",array(localAxis)},{"handleInWeapon",array(r.handle)},{"travel",r.travel},{"captureRadius",r.captureRadius},
        {"releaseMargin",r.releaseMargin},{"fullTrack",r.fullTrack},{"emptyTrack",r.emptyTrack},{"sharedBody",r.sharedBody()},{"animationSeconds",.6},
        {"magazineMinProjection",r.magazineMinProjection},{"weaponMaxProjection",r.weaponMaxProjection},{"clearance",r.clearance}};
    if(!r.guide.isEmpty()) {
        QJsonArray samples;
        for(int i=0;i<r.guide.size();++i){QJsonObject delta;if(!encodePoseTransform(r.guide[i],delta,error))return {};samples.append(QJsonObject{{"distance",r.guideDistances[i]},{"deltaInWeapon",delta},{"offset",array(magazineGuideOffset(r,r.guideDistances[i]))}});}
        result["guide"]=samples;
    }
    return result;
}
}
