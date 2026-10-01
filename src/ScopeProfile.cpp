#include "ScopeProfile.h"
#include <QJsonArray>
#include <cmath>

namespace reloadmaker {
QString scopeModelKey(QString model) {
    model.replace('\\','/');model=model.toLower();
    if(model.startsWith("meshes/"))model.remove(0,7);
    if(model.endsWith("_1.nif"))model.chop(6),model+=".nif";
    return model;
}
QJsonObject scopeProfileJson(const ScopeProfile& p) {
    const auto& t=p.lens;
    return {{"format","true-scopes-profile"},{"version",1},{"name",p.name},
        {"model",p.model},{"shape",p.shape},{"source",QJsonObject{{"weapon",p.weapon},{"attachment",p.attachment}}},
        {"lens",QJsonObject{{"position",QJsonArray{t.translation.x,t.translation.y,t.translation.z}},
        {"rotation",QJsonArray{t.rotation.x,t.rotation.y,t.rotation.z,t.rotation.w}},
        {"radius",p.radius},{"aspect",p.aspect},{"eyeRelief",p.eyeRelief},{"cameraForward",p.cameraForward}}}};
}
bool readScopeProfile(const QJsonObject& json,ScopeProfile& out,QString& error) {
    error.clear();const auto fail=[&](const char* why){error=why;return false;};
    if(json["format"]!="true-scopes-profile"||json["version"]!=1)return fail("Unsupported scope profile.");
    ScopeProfile p;p.name=json["name"].toString();p.model=scopeModelKey(json["model"].toString());p.shape=json["shape"].toString();
    const auto printable=[](const QString& s,int max){if(s.isEmpty()||s.size()>max)return false;for(auto c:s)if(c.unicode()<32||c.unicode()>126)return false;return true;};
    if(p.name.trimmed().isEmpty()||p.name.size()>128||!printable(p.model,127)||!p.model.endsWith(".nif")||
        p.model.startsWith('/')||p.model.contains(':')||p.model.split('/').contains("..")||p.model.split('/').contains(".")||p.model.contains("//")||
        !printable(p.shape,63))return fail("Invalid optic model, shape or name.");
    const auto source=json["source"].toObject();p.weapon=source["weapon"].toString();p.attachment=source["attachment"].toString();
    if(p.weapon.size()>256||p.attachment.size()>256)return fail("Invalid source identity.");
    const auto lens=json["lens"].toObject();const auto pos=lens["position"].toArray(),rot=lens["rotation"].toArray();
    if(pos.size()!=3||rot.size()!=4)return fail("A lens needs a local position and XYZW quaternion.");
    for(auto n:pos)if(!n.isDouble()||!std::isfinite(n.toDouble())||std::abs(n.toDouble())>10000)return fail("Invalid lens position.");
    double length=0;for(auto n:rot){if(!n.isDouble()||!std::isfinite(n.toDouble()))return fail("Invalid lens rotation.");length+=n.toDouble()*n.toDouble();}
    if(std::abs(length-1)>0.001)return fail("Lens rotation must be a unit quaternion.");
    const auto number=[&](const char* key,double lo,double hi,float& value){const auto n=lens[key];if(!n.isDouble()||!std::isfinite(n.toDouble())||n.toDouble()<lo||n.toDouble()>hi)return false;value=float(n.toDouble());return true;};
    if(!number("radius",.02,32,p.radius)||!number("aspect",.1,1,p.aspect)||!number("eyeRelief",1,99,p.eyeRelief)||!number("cameraForward",0,256,p.cameraForward))return fail("Invalid aperture, eye relief or camera distance.");
    p.lens.translation={float(pos[0].toDouble()),float(pos[1].toDouble()),float(pos[2].toDouble())};
    p.lens.rotation={float(rot[0].toDouble()),float(rot[1].toDouble()),float(rot[2].toDouble()),float(rot[3].toDouble())};
    out=std::move(p);return true;
}
}
