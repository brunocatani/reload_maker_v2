#include "OperatingSetup.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <algorithm>
namespace reloadmaker {
bool readOperatingSetup(const QJsonObject& json,OperatingSetup& output,QString& error) {
    const auto fail=[&](const char* why){error=why;return false;};OperatingSetup value;
    if(json["version"]!=1)return fail("Unsupported operating setup");
    if(json.contains("trackAnchors")&&(!json["trackAnchors"].isArray()||json["trackAnchors"].toArray().size()>maximumReloadTracks))return fail("Invalid operating track nodes");
    value.recipeId=json["recipeId"].toString();value.trackAnchors=json["trackAnchors"].toArray();value.mode=json["mode"].toString();
    if(value.mode!="preset"&&value.mode!="reload"&&value.mode!="clip"&&value.mode!="manual")return fail("Choose a supported movement source");
    const auto integer=[](QJsonValue v,int low,int high){return v.isDouble()&&v.toDouble()==std::floor(v.toDouble())&&v.toInt(low-1)>=low&&v.toInt(high+1)<=high;};
    for(const auto* key:{"operating","bolt","magazine"})if(!integer(json[key],0,maximumReloadTracks-1))return fail("Select the operating, bolt and magazine tracks");
    value.parts.operating=json["operating"].toInt();value.parts.bolt=json["bolt"].toInt();value.parts.magazine=json["magazine"].toInt();
    const auto followers=json["followers"].toArray();if(!json["followers"].isArray()||followers.isEmpty()||followers.size()>16)return fail("Select one to sixteen operating followers");QSet<int> seen;
    for(const auto& item:followers){if(!integer(item,0,maximumReloadTracks-1)||seen.contains(item.toInt()))return fail("Follower tracks must be valid and unique");seen.insert(item.toInt());value.parts.followers.append(item.toInt());}
    if(!seen.contains(value.parts.operating))return fail("The operating part must be in its follower group");
    for(const auto* key:{"openFrame","closedFrame"})if(!integer(json[key],-1,maximumReloadFrames-1))return fail("Invalid operating source window");
    value.parts.openFrame=json["openFrame"].toInt();value.parts.closedFrame=json["closedFrame"].toInt();
    if((value.parts.openFrame<0)!=(value.parts.closedFrame<0)||(value.parts.openFrame>=0&&value.parts.closedFrame==value.parts.openFrame))return fail("Choose automatic stroke detection or distinct open and closed samples");
    if(!json["deriveHandle"].isBool())return fail("Movement derivation choices must be explicit");
    value.deriveHandle=json["deriveHandle"].toBool();value.fireMode=json["fireMode"].toString();
    if(value.fireMode!="preset"&&value.fireMode!="native"&&value.fireMode!="stroke")return fail("Choose a firing movement source");
    value.cycleMode=json["cycleMode"].toString("preset");
    if(value.cycleMode!="preset"&&value.cycleMode!="closed-bolt"&&value.cycleMode!="open-bolt")return fail("Choose a firing action");
    value.catchMode=json["catchMode"].toString("preset");
    if(value.catchMode!="preset"&&value.catchMode!="hold-open"&&value.catchMode!="close")return fail("Choose the last-round action");
    value.clip=json["clip"].toString();value.baseline=json["baseline"].toString();
    if(value.mode=="clip"&&(value.clip.isEmpty()||value.baseline.isEmpty()||value.clip.size()>512||value.baseline.size()>512))return fail("Choose both native movement and baseline clips");
    if(!json["duration"].isDouble()||!std::isfinite(json["duration"].toDouble())||json["duration"].toDouble()<.01||json["duration"].toDouble()>120)return fail("Manual duration must be between 0.01 and 120 seconds");value.duration=float(json["duration"].toDouble());
    if(!json["ejectSeconds"].isDouble()||!std::isfinite(json["ejectSeconds"].toDouble())||json["ejectSeconds"].toDouble()< -1||json["ejectSeconds"].toDouble()>120)return fail("Choose the manual ejection time or -1 for full opening");value.ejectSeconds=float(json["ejectSeconds"].toDouble());
    if(!json["movements"].isArray()||json["movements"].toArray().size()>16)return fail("Invalid authored movement assignments");
    for(const auto& item:json["movements"].toArray()){const auto row=item.toObject();const int track=row["track"].toInt(-1);const auto id=row["id"].toString();if(!integer(row["track"],0,maximumReloadTracks-1)||value.movements.contains(track)||id.isEmpty()||id.size()>128)return fail("Choose a distinct track and saved movement for each assignment");value.movements[track]=id;}
    if(value.mode=="manual"&&!value.movements.contains(value.parts.operating))return fail("Assign the saved operating movement");
    output=std::move(value);error.clear();return true;
}
// Only the operating part and its bolt define the mechanism and must exist
// in this reload. The magazine track belongs to the magazine: when this
// reload has no track at the saved one's path (another magazine model) it is
// left -1 and the rig uses the magazine track the reload itself detects; on
// another reload a matched track that only rides a magazine node is that node.
// Followers and movement targets this assembly lacks are skipped; a different
// model of a moving part is not a different mechanism.
bool remapOperatingSetup(OperatingSetup& setup,const ReloadRecipe& recipe,QString& error) {
    const auto map=[&](int track){return track<0?-1:mapRecipeTrack(setup.trackAnchors,recipe,track);};
    const auto required=[&](int& track) {
        if(track<0)return true;
        const int mapped=map(track);
        if(mapped<0){error="An authored operating part is missing or ambiguous in this assembly";return false;}
        track=mapped;return true;
    };
    if(!required(setup.parts.operating)||!required(setup.parts.bolt))return false;
    setup.parts.magazine=map(setup.parts.magazine);
    if(setup.recipeId!=recipe.id)setup.parts.magazine=magazineCarrier(recipe,setup.parts.magazine);
    QVector<int> followers;
    for(const int track:setup.parts.followers)if(const int mapped=map(track);mapped>=0&&!followers.contains(mapped))followers.append(mapped);
    setup.parts.followers=followers;
    QHash<int,QString> movements;
    for(auto it=setup.movements.cbegin();it!=setup.movements.cend();++it)if(const int mapped=map(it.key());mapped>=0&&!movements.contains(mapped))movements.insert(mapped,it.value());
    setup.movements=movements;setup.trackAnchors=recipeTrackAnchors(recipe);setup.recipeId=recipe.id;return true;
}
QJsonObject operatingSetupJson(const OperatingSetup& value) {
    QJsonArray followers,movements;for(int track:value.parts.followers)followers.append(track);
    auto keys=value.movements.keys();std::sort(keys.begin(),keys.end());for(int track:keys)movements.append(QJsonObject{{"track",track},{"id",value.movements[track]}});
    return {{"version",1},{"recipeId",value.recipeId},{"mode",value.mode},{"operating",value.parts.operating},{"bolt",value.parts.bolt},{"magazine",value.parts.magazine},
        {"followers",followers},{"openFrame",value.parts.openFrame},{"closedFrame",value.parts.closedFrame},{"deriveHandle",value.deriveHandle},{"fireMode",value.fireMode},{"cycleMode",value.cycleMode},{"catchMode",value.catchMode},
        {"clip",value.clip},{"baseline",value.baseline},{"duration",value.duration},{"ejectSeconds",value.ejectSeconds},{"movements",movements},
        {"trackAnchors",value.trackAnchors}};
}
}
