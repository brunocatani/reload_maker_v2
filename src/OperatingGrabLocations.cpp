#include "OperatingGrabLocations.h"
#include <QJsonArray>
#include <QSet>
#include <QRegularExpression>
#include <QUuid>
#include <cmath>
namespace reloadmaker {
QString grabLocationsKey(const OperatingGrabLocations& g){auto key=g.weapon+"|"+g.group+"|"+g.sourceModel+"|"+g.sourcePath;return key.replace('\\','/').toLower();}
QJsonObject grabLocationsJson(const OperatingGrabLocations& g){
    QJsonArray points;for(const auto& p:g.points)points.append(QJsonObject{{"id",p.id},{"name",p.name},{"pointInSource",QJsonArray{p.point.x,p.point.y,p.point.z}},{"radiusGame",p.radius},{"handMask",p.hands}});
    return {{"format","reload-maker-grab-locations"},{"version",1},{"weapon",g.weapon},{"group",g.group},
        {"sourceModel",g.sourceModel},{"sourcePath",g.sourcePath},{"sourceName",g.sourceName},{"selection","points-only"},{"points",points}};
}
bool readGrabLocations(const QJsonObject& j,OperatingGrabLocations& result,QString& error){
    error.clear();const auto fail=[&](const char* why){error=why;return false;};OperatingGrabLocations g;
    if(j["format"]!="reload-maker-grab-locations"||j["version"]!=1||j["selection"]!="points-only")return fail("Unsupported grab locations.");
    g.weapon=j["weapon"].toString();g.group=j["group"].toString();g.sourceModel=j["sourceModel"].toString();g.sourcePath=j["sourcePath"].toString();g.sourceName=j["sourceName"].toString();
    static const QRegularExpression form("^[^:/\\\\]+\\.(?:esm|esp|esl):[0-9a-f]{6}$",QRegularExpression::CaseInsensitiveOption);
    if(!form.match(g.weapon).hasMatch()||!QStringList{"slide","charging_handle","operating_handle","bolt","cell_lever"}.contains(g.group)||g.sourceModel.isEmpty()||g.sourceModel.size()>1024||g.sourcePath.isEmpty()||g.sourcePath.size()>1024||g.sourceName.isEmpty()||g.sourceName.size()>256)return fail("Invalid grab-location source.");
    if(!j["points"].isArray()||j["points"].toArray().size()>32)return fail("A group supports up to 32 grab locations.");
    QSet<QString> ids;
    for(const auto& v:j["points"].toArray()) {
        if(!v.isObject())return fail("Invalid grab location.");const auto o=v.toObject();OperatingGrabLocation p;p.id=o["id"].toString();p.name=o["name"].toString();
        const auto coords=o["pointInSource"].toArray();
        if(QUuid(p.id).isNull()||ids.contains(p.id)||p.name.trimmed().isEmpty()||p.name.size()>128||coords.size()!=3||!o["radiusGame"].isDouble()||!o["handMask"].isDouble())return fail("Invalid grab-location fields.");
        for(const auto& n:coords)if(!n.isDouble()||!std::isfinite(n.toDouble())||std::abs(n.toDouble())>10000)return fail("Invalid grab-location coordinate.");
        const double radius=o["radiusGame"].toDouble(),hands=o["handMask"].toDouble();
        if(!std::isfinite(radius)||radius<=0||radius>64||hands<1||hands>3||hands!=std::floor(hands))return fail("Invalid grab radius or hand selection.");
        p.point={float(coords[0].toDouble()),float(coords[1].toDouble()),float(coords[2].toDouble())};p.radius=float(radius);p.hands=int(hands);ids.insert(p.id);g.points.append(p);
    }
    result=std::move(g);return true;
}
}
