#include "MagazineReference.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace reloadmaker {
// The reference node, readable: kind|record|model|name|path.
QString magazineReferenceKey(const HandPosePlacement& p) {
    const auto normalize=[](QString s){return s.replace('\\','/').toLower();};
    return QStringList{p.anchorKind,normalize(p.ownerRecord),normalize(p.ownerModel),normalize(p.anchorName),normalize(p.anchorPath)}.join('|');
}
bool readMagazineReference(const QJsonObject& json,MagazineReference& out,QString& error) {
    if(json["version"]!=1||json["anchorKey"].toString().isEmpty()||json["anchorKey"].toString().size()>4096) {error="Select a magazine reference from the current assembly";return false;}
    MagazineReference value;value.recipeId=json["recipeId"].toString();value.anchorKey=json["anchorKey"].toString();value.connectionMode=json["connectionMode"].toString();
    if(value.connectionMode!="source"&&value.connectionMode!="carrier"&&value.connectionMode!="custom") {error="Choose a native mount, carrier pivot or custom connection";return false;}
    if(!decodePoseTransform(json["connectionInCarrier"],value.connectionInCarrier,error))return false;
    Transformf inverse;if(!invertUniformTransform(value.connectionInCarrier,inverse)) {error="The magazine connection must have an invertible uniform frame";return false;}
    out=value;error.clear();return true;
}
QJsonObject magazineReferenceJson(const MagazineReference& value) {
    QJsonObject frame;QString error;if(!encodePoseTransform(value.connectionInCarrier,frame,error))return {};
    return {{"version",1},{"recipeId",value.recipeId},{"anchorKey",value.anchorKey},{"connectionMode",value.connectionMode},{"connectionInCarrier",frame}};
}
}
