#include "MagazineSetup.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>
namespace reloadmaker {
bool readMagazineSetup(const QJsonObject& json,MagazineSetup& out,QString& error) {
    const auto fail=[&](const char* why){error=why;return false;};MagazineSetup value;
    if(json["version"]!=1||!QRegularExpression("^[a-f0-9]{64}$").match(json["recipeId"].toString()).hasMatch())return fail("Magazine presentation needs the current source recipe");
    value.recipeId=json["recipeId"].toString();value.motion=json["motion"].toString();
    if(value.motion!="preset"&&value.motion!="linear"&&value.motion!="native")return fail("Choose source behavior, linear extraction or sampled native insertion");
    const auto integer=[](QJsonValue v,int high){return v.isDouble()&&v.toDouble()==std::floor(v.toDouble())&&v.toInt(-1)>=0&&v.toInt(high)<high;};
    if(!integer(json["emptyTrack"],maximumReloadTracks)||!integer(json["loadedFrame"],maximumReloadFrames)||!integer(json["emptyFrame"],maximumReloadFrames))return fail("Select the empty body and payload samples");
    value.emptyTrack=json["emptyTrack"].toInt();value.loadedFrame=json["loadedFrame"].toInt();value.emptyFrame=json["emptyFrame"].toInt();
    if(!json["sampledPayload"].isBool())return fail("Choose whether payload uses captured source frames");value.sampledPayload=json["sampledPayload"].toBool();
    if(!json["entrySeconds"].isDouble()||!std::isfinite(json["entrySeconds"].toDouble())||json["entrySeconds"].toDouble()<0||json["entrySeconds"].toDouble()>120)return fail("Invalid native magazine entry time");
    value.entrySeconds=float(json["entrySeconds"].toDouble());
    if(!json["pieces"].isArray()||json["pieces"].toArray().isEmpty()||json["pieces"].toArray().size()>256)return fail("Review the native magazine pieces before saving");
    for(const auto& item:json["pieces"].toArray()) {
        const auto row=item.toObject();const auto path=row["path"].toString(),role=row["role"].toString();
        if(path.isEmpty()||path.size()>4096||value.pieces.contains(path)||(role!="body"&&role!="cartridge"&&role!="spare"))return fail("Each native magazine piece needs a unique path and explicit role");
        value.pieces[path]=role;
    }
    out=value;error.clear();return true;
}
QJsonObject magazineSetupJson(const MagazineSetup& value) {
    QJsonArray pieces;for(auto it=value.pieces.cbegin();it!=value.pieces.cend();++it)pieces.append(QJsonObject{{"path",it.key()},{"role",it.value()}});
    return {{"version",1},{"recipeId",value.recipeId},{"motion",value.motion},{"emptyTrack",value.emptyTrack},{"loadedFrame",value.loadedFrame},
        {"emptyFrame",value.emptyFrame},{"sampledPayload",value.sampledPayload},{"entrySeconds",value.entrySeconds},{"pieces",pieces}};
}
}
