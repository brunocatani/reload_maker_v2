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
    const QVector<QVector3D>& body,const QVector<QVector3D>& weapon,MagazineRail& out,QString& error) {
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
    const auto& rest=empty.poses.front();
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
bool MagazineMotion::animateOut(){if(!seated())return false;phase=MagazinePhase::Outgoing;elapsed=0;distance=0;offset={};return true;}
bool MagazineMotion::animateIn(const MagazineRail& rail){if(phase!=MagazinePhase::Absent&&phase!=MagazinePhase::OutsideEmpty)return false;phase=MagazinePhase::Incoming;elapsed=0;distance=rail.travel;offset=rail.outward*distance;return true;}
bool MagazineMotion::grab(const MagazineRail& rail){if(phase!=MagazinePhase::Absent&&phase!=MagazinePhase::OutsideEmpty)return false;phase=MagazinePhase::Free;distance=rail.travel+rail.captureRadius*2;offset=rail.outward*distance;return true;}
void MagazineMotion::drop(){phase=MagazinePhase::Absent;offset={};distance=0;elapsed=0;}
void MagazineMotion::cancel(){if(carried())drop();}
void MagazineMotion::reset(){*this={};}
void MagazineMotion::moveFree(const MagazineRail& rail,const QVector3D& p) {
    if(phase!=MagazinePhase::Free||!finite(p))return;
    const auto entrance=rail.outward*rail.travel;
    const auto step=p-offset;const float squared=step.lengthSquared();
    const float fraction=squared>1e-10f?std::clamp(QVector3D::dotProduct(entrance-offset,step)/squared,0.f,1.f):0;
    const auto nearest=offset+step*fraction;
    offset=p;
    if((nearest-entrance).length()<=rail.captureRadius){phase=MagazinePhase::Rail;distance=rail.travel;offset=entrance;}
}
void MagazineMotion::moveRail(const MagazineRail& rail,float d) {
    if(phase!=MagazinePhase::Rail||!std::isfinite(d))return;
    distance=std::max(0.f,d);offset=rail.outward*distance;
    if(distance>rail.travel+rail.releaseMargin){phase=MagazinePhase::Free;}
    else if(distance<=rail.travel*.005f){reset();}
}
void MagazineMotion::tick(const MagazineRail& rail,float delta) {
    if(!animated()||!std::isfinite(delta)||delta<0)return;
    elapsed=std::min(.6f,elapsed+delta);const float t=elapsed/.6f;const float smooth=t*t*(3-2*t);
    distance=rail.travel*(phase==MagazinePhase::Outgoing?smooth:1-smooth);offset=rail.outward*distance;
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
    return {{"version",1},{"connectionName",r.connectionName},{"connection",connection},{"outwardInWeapon",array(r.outward)},
        {"outwardInConnection",array(localAxis)},{"handleInWeapon",array(r.handle)},{"travel",r.travel},{"captureRadius",r.captureRadius},
        {"releaseMargin",r.releaseMargin},{"fullTrack",r.fullTrack},{"emptyTrack",r.emptyTrack},{"sharedBody",r.sharedBody()},{"animationSeconds",.6},
        {"magazineMinProjection",r.magazineMinProjection},{"weaponMaxProjection",r.weaponMaxProjection},{"clearance",r.clearance}};
}
}
