#include "MagazineReference.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace reloadmaker {
QString magazineReferenceKey(const HandPosePlacement& p) {
    const auto normalize=[](QString s){return s.replace('\\','/').toLower();};
    const auto bytes=QJsonDocument(QJsonArray{p.anchorKind,normalize(p.ownerRecord),normalize(p.ownerModel),
        normalize(p.anchorName),normalize(p.anchorPath)}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
bool readMagazineReference(const QJsonObject& json,MagazineReference& out,QString& error) {
    const auto hash=[](const QJsonValue& v){return v.isString()&&QRegularExpression("^[a-f0-9]{64}$").match(v.toString()).hasMatch();};
    if(json["version"]!=1||!hash(json["recipeId"])||!hash(json["anchorKey"])) {error="Select a magazine reference from the current captured assembly";return false;}
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
