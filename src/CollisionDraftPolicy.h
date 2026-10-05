#pragma once
#include <QJsonObject>

namespace reloadmaker {
// A magazine collision draft: whether its hulls are usable, and the id of the
// edit that wrote it.
bool validCollisionDrafts(const QJsonObject&);
}
