#include "CollisionDraftPolicy.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>

namespace reloadmaker {
QString collisionDraftPolicy(const QString& recipe,const QJsonObject& sources,const QJsonObject& presentation) {
    const QJsonObject value{{"recipe",recipe},{"sources",sources},{"presentation",presentation}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
bool validCollisionDrafts(const QJsonObject& drafts) {
    const QRegularExpression hash("^[a-f0-9]{64}$"),edit("^[a-f0-9]{32}$");
    if(drafts.size()>2)return false;
    for(auto it=drafts.begin();it!=drafts.end();++it){const auto item=it.value().toObject();if((it.key()!="full"&&it.key()!="empty")||!hash.match(item["policy"].toString()).hasMatch()||!edit.match(item["edit"].toString()).hasMatch()||!item["usable"].isBool())return false;}
    return true;
}
}
