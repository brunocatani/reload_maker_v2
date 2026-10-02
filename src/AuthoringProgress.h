#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>

namespace reloadmaker {
struct AuthoringStepInput {
    bool available{},ready{};
    QString message;
    QJsonObject evidence;
};
bool validAuthoringReviews(const QJsonObject&);
bool validCollisionDrafts(const QJsonObject&);
QString collisionDraftPolicy(const QString& recipe,const QJsonObject& sources,const QJsonObject& presentation);
QJsonArray authoringProgress(const std::array<AuthoringStepInput,12>&,const QJsonObject& reviews);
}
