#include "AuthoringProgress.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QVector>

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
bool validAuthoringReviews(const QJsonObject& reviews) {
    if(reviews.size()>12)return false;
    const QRegularExpression hash("^[a-f0-9]{64}$");
    for(auto it=reviews.begin();it!=reviews.end();++it){bool number=false;const int index=it.key().toInt(&number);
        if(!number||index<0||index>=12||it.key()!=QString::number(index)||!hash.match(it.value().toString()).hasMatch())return false;}
    return true;
}
QJsonArray authoringProgress(const std::array<AuthoringStepInput,12>& input,const QJsonObject& reviews) {
    // Dependency hashes express authoring authority. Hand fits do not depend
    // on an input guide's angle; grab-point motion checks do depend on curves.
    const std::array<QVector<int>,12> dependencies{{{}, {0}, {1}, {2}, {3}, {2}, {5}, {3,5}, {3,4}, {4,6}, {5}, {4,6,7,8,9,10}}};
    QJsonArray result;
    for(int i=0;i<12;++i) {
        QJsonArray parents;for(int parent:dependencies[i])parents.append(result[parent].toObject()["fingerprint"]);
        const QJsonObject value{{"version",1},{"step",i},{"evidence",input[i].evidence},{"dependencies",parents}};
        const auto hash=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        const auto reviewed=reviews[QString::number(i)].toString();
        const auto status=!input[i].available?"blocked":!input[i].ready?(reviewed.isEmpty()?"draft":"needs-review"):
            reviewed==hash?"reviewed":reviewed.isEmpty()?"ready":"needs-review";
        result.append(QJsonObject{{"index",i},{"available",input[i].available},{"canReview",input[i].available&&input[i].ready},
            {"status",status},{"message",input[i].message},{"fingerprint",hash}});
    }
    return result;
}
}
