#pragma once
#include "PoseEvaluator.h"
#include <QJsonObject>

namespace reloadmaker {
// True Scopes uses the named rigid shape's native local frame. +Y looks down
// the tube; +X is lens right and +Z is up. Lengths are native model units.
struct ScopeProfile {
    QString name,model,shape,weapon,attachment;
    Transformf lens;
    float radius{1},aspect{1},eyeRelief{12},cameraForward{10};
};
QString scopeModelKey(QString model);
QJsonObject scopeProfileJson(const ScopeProfile&);
bool readScopeProfile(const QJsonObject&,ScopeProfile&,QString& error);
}
