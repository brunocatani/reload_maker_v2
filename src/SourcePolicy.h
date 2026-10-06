#pragma once
#include <QJsonObject>
#include <QString>
namespace reloadmaker {
bool validSourcePolicy(const QJsonObject&,QString& error);
// Auxiliary firing-mode selectors do not change the host's regular reload.
bool auxiliaryReloadMode(const QString& record);
}
