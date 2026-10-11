#include "MagazineGuide.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>

namespace reloadmaker {
bool readMagazineGuide(const QJsonObject& json,MagazineGuide& out,QString& error) {
    const auto fail=[&](const char* why){error=why;return false;};MagazineGuide value;
    if(json["version"]!=1&&json["version"]!=2)return fail("Unsupported magazine guide");
    value.recipeId=json["recipeId"].toString();const auto axis=json["outwardInConnection"].toArray();
    if(axis.size()!=3)return fail("Specify all three guide direction components");
    for(int i=0;i<3;++i){if(!axis[i].isDouble()||!std::isfinite(axis[i].toDouble()))return fail("Guide direction must be finite");value.outwardInConnection[i]=float(axis[i].toDouble());}
    if(std::abs(value.outwardInConnection.lengthSquared()-1)>.001f)return fail("Guide direction must have unit length");
    for(const auto* key:{"travel","captureRadius","releaseMargin"})if(!json[key].isDouble()||!std::isfinite(json[key].toDouble())||json[key].toDouble()<=0)return fail("Guide dimensions must be positive and finite");
    value.travel=float(json["travel"].toDouble());value.captureRadius=float(json["captureRadius"].toDouble());value.releaseMargin=float(json["releaseMargin"].toDouble());
    if(value.travel>=1000||value.captureRadius>=value.travel||value.releaseMargin>value.travel)return fail("Use a finite stroke below 1000 units, with pickup radius and release margin smaller than its travel");
    out=value;error.clear();return true;
}
QJsonObject magazineGuideJson(const MagazineGuide& value) {
    const auto a=value.outwardInConnection;
    return {{"version",2},{"recipeId",value.recipeId},{"outwardInConnection",QJsonArray{a.x(),a.y(),a.z()}},
        {"travel",value.travel},{"captureRadius",value.captureRadius},{"releaseMargin",value.releaseMargin}};
}
bool applyMagazineGuide(const MagazineGuide& guide,MagazineRail& rail,QString& error,bool linearMotion) {
    MagazineGuide checked;if(!readMagazineGuide(magazineGuideJson(guide),checked,error))return false;
    Transformf inverse;if(!invertUniformTransform(rail.connection,inverse)){error="Invalid magazine connection frame";return false;}
    const auto rotatedAxis=rotated(rail.connection.rotation,{guide.outwardInConnection.x(),guide.outwardInConnection.y(),guide.outwardInConnection.z()});
    const QVector3D axis=QVector3D(rotatedAxis.x,rotatedAxis.y,rotatedAxis.z).normalized();
    if(linearMotion||(rail.motion.isEmpty()&&rail.guideSourceMotion.isEmpty())) {
        rail.motion.clear();rail.motionDistances.clear();rail.motionSeconds.clear();
    } else {
        if(rail.guideSourceMotion.isEmpty()) {
            rail.guideSourceMotion=rail.motion;rail.guideSourceDistances=rail.motionDistances;rail.guideSourceSeconds=rail.motionSeconds;
        }
        rail.motion=rail.guideSourceMotion;rail.motionDistances=rail.guideSourceDistances;rail.motionSeconds=rail.guideSourceSeconds;
        const auto depth=[&](const Transformf& delta) {
            const auto point=composed(delta,Transformf{{rail.handle.x(),rail.handle.y(),rail.handle.z()}}).translation;
            return QVector3D::dotProduct(QVector3D(point.x,point.y,point.z)-rail.handle,axis);
        };
        bool clipped=false;
        for(int i=1;i<rail.motion.size();++i) {
            if(depth(rail.motion[i])<guide.travel)continue;
            // Keep the near-seat source curve, ending where it first crosses
            // the authored entrance. Do not squeeze the old full stroke into
            // the shortened controller travel. Interpolate the rotation too.
            const auto a=rail.motion[i-1],b=rail.motion[i];
            float low=0,high=1;
            for(int step=0;step<24;++step) {
                const float u=(low+high)*.5f;
                if(depth(interpolatedTransform(a,b,u))<guide.travel)low=u;else high=u;
            }
            const float u=(low+high)*.5f;
            const auto end=interpolatedTransform(a,b,u);
            const float distance=rail.motionDistances[i-1]+u*(rail.motionDistances[i]-rail.motionDistances[i-1]);
            if(rail.motionSeconds.size()==rail.motion.size()){const float time=rail.motionSeconds[i-1]+u*(rail.motionSeconds[i]-rail.motionSeconds[i-1]);rail.motionSeconds.resize(i);rail.motionSeconds.append(time);}
            rail.motion.resize(i);rail.motionDistances.resize(i);
            rail.motion.append(end);rail.motionDistances.append(distance);clipped=true;break;
        }
        if(!clipped) {
            // Lengthening beyond the authored curve adds straight clearance;
            // shortening again always starts from the unmodified source.
            const float extra=guide.travel-depth(rail.motion.back());
            if(extra>1e-5f) {
                auto end=rail.motion.back();end.translation.x+=axis.x()*extra;end.translation.y+=axis.y()*extra;end.translation.z+=axis.z()*extra;
                if(rail.motionSeconds.size()==rail.motion.size())rail.motionSeconds.append(rail.motionSeconds.back());
                rail.motion.append(end);rail.motionDistances.append(rail.motionDistances.back()+extra);
            }
        }
    }
    rail.outward=axis;rail.travel=guide.travel;rail.captureRadius=guide.captureRadius;rail.releaseMargin=guide.releaseMargin;
    rail.inputSetup=magazineGuideJson(guide);error.clear();return true;
}
}
