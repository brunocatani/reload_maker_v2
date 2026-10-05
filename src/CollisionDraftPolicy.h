#pragma once
#include <QJsonObject>
#include <QString>

namespace reloadmaker {
// A magazine collision draft records the learned reload, source inputs and
// presentation it was generated from; a different policy hash marks it stale.
QString collisionDraftPolicy(const QString& recipe,const QJsonObject& sources,const QJsonObject& presentation);
bool validCollisionDrafts(const QJsonObject&);
}
