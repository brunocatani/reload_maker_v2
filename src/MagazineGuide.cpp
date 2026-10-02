#include "MagazineGuide.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>

namespace reloadmaker {
bool readMagazineGuide(const QJsonObject& json,MagazineGuide& out,QString& error) {
    const auto fail=[&](const char* why){error=why;return false;};MagazineGuide value;
    if(json["version"]!=1||!QRegularExpression("^[a-f0-9]{64}$").match(json["recipeId"].toString()).hasMatch())return fail("The magazine guide needs a captured source configuration");
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
    return {{"version",1},{"recipeId",value.recipeId},{"outwardInConnection",QJsonArray{a.x(),a.y(),a.z()}},
        {"travel",value.travel},{"captureRadius",value.captureRadius},{"releaseMargin",value.releaseMargin}};
}
bool applyMagazineGuide(const MagazineGuide& guide,MagazineRail& rail,QString& error) {
    MagazineGuide checked;if(!readMagazineGuide(magazineGuideJson(guide),checked,error))return false;
    Transformf inverse;if(!invertUniformTransform(rail.connection,inverse)){error="Invalid magazine connection frame";return false;}
    // Preserve the full visible extraction before changing the player's input.
    // Even an originally straight visual path is now independent of this guide.
    if(rail.motion.isEmpty()) {
        const auto delta=rail.outward*rail.travel;
        rail.motion={Transformf{},Transformf{{delta.x(),delta.y(),delta.z()}}};rail.motionDistances={0,rail.travel};
    }
    const auto axis=rotated(rail.connection.rotation,{guide.outwardInConnection.x(),guide.outwardInConnection.y(),guide.outwardInConnection.z()});
    rail.outward=QVector3D(axis.x,axis.y,axis.z).normalized();rail.travel=guide.travel;rail.captureRadius=guide.captureRadius;rail.releaseMargin=guide.releaseMargin;
    rail.inputSetup=magazineGuideJson(guide);error.clear();return true;
}
}
