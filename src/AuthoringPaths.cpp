#include "AuthoringPaths.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
namespace reloadmaker {
QString authoringProjectPath(const QString& weapon,const QJsonObject& sourceContext) {
    const auto match=QRegularExpression(QStringLiteral(R"(^([^\x00-\x1f<>:"/\\|?*]+\.(?:esp|esm|esl)):([0-9a-f]{6})$)"),QRegularExpression::CaseInsensitiveOption).match(weapon);
    if(!match.hasMatch()||sourceContext.isEmpty())return {};
    const auto context=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(sourceContext).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    return QString("workspace/projects/%1/%2/%3/authoring.project.json").arg(match.captured(1).toLower(),match.captured(2).toLower(),context.left(16));
}
}
