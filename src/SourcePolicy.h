#pragma once
#include <QJsonObject>
#include <QString>
namespace reloadmaker {
bool validSourcePolicy(const QJsonObject&,QString& error);
}
