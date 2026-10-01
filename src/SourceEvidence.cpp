#include "SourceEvidence.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <stdexcept>

namespace reloadmaker {
bool validSourceEvidence(const QJsonObject& source,QString& error) {
    const auto fail=[&](const QString& message){error=message;return false;};
    if(source["version"]!=1||!source["resources"].isArray())return fail("Unsupported source evidence");
    const auto resources=source["resources"].toArray();if(resources.isEmpty()||resources.size()>512)return fail("Source evidence needs 1–512 relevant inputs");
    const QRegularExpression hash("^[a-f0-9]{64}$");QSet<QString> keys;
    for(const auto& value:resources) {
        const auto item=value.toObject();const auto path=item["path"].toString(),kind=item["kind"].toString();
        if(path.isEmpty()||path.size()>512||path.contains(':')||path.contains('\\')||path.startsWith('/')||path.split('/').contains("..")||path.split('/').contains("."))return fail("Invalid source resource path");
        if(kind!="animation"&&kind!="model")return fail("Unsupported source resource kind");
        if(!hash.match(item["sha256"].toString()).hasMatch()||keys.contains(path.toLower()))return fail("Missing digest or duplicate source resource");
        keys.insert(path.toLower());
    }
    return true;
}
QJsonObject inspectSourceEvidence(const QJsonObject& expected,const SourceReader& read) {
    QString error;if(!validSourceEvidence(expected,error))throw std::runtime_error(error.toStdString());
    QJsonArray results;int changed=0,missing=0;qsizetype total=0;
    for(const auto& value:expected["resources"].toArray()) {
        const auto item=value.toObject();const auto path=item["path"].toString();const auto bytes=read(item["kind"].toString(),path);
        total+=bytes.bytes.size();if(total>256*1024*1024)throw std::runtime_error("Source check exceeds the 256 MiB input budget");
        QJsonObject result{{"path",path},{"kind",item["kind"]},{"expectedSha256",item["sha256"]},{"provider",bytes.provider}};
        if(!bytes.error.isEmpty()||bytes.bytes.isEmpty()){result["status"]="missing";result["error"]=bytes.error;++missing;}
        else {
            const auto hash=QString::fromLatin1(QCryptographicHash::hash(bytes.bytes,QCryptographicHash::Sha256).toHex());
            result["actualSha256"]=hash;const bool same=hash==item["sha256"].toString();result["status"]=same?"unchanged":"changed";if(!same)++changed;
        }
        results.append(result);
    }
    return {{"compatible",changed==0&&missing==0},{"changed",changed},{"missing",missing},{"resources",results}};
}
}
