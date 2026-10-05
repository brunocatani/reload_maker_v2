#pragma once
#include <QJsonObject>
#include <QString>
namespace reloadmaker {
QString authoringProjectPath(const QString& weapon,const QJsonObject& sourceContext);
// The folder name that separates one source's projects of a weapon from another's.
QString authoringContextKey(const QJsonObject& sourceContext);
}
