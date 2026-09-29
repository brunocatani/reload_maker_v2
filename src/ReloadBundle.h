#pragma once
#include "ReloadRecipe.h"
#include "PartGrip.h"
#include "PartMechanism.h"
#include <QHash>

namespace reloadmaker {
// A project assignment belongs to a weapon and one concrete magazine owner.
QString magazineGrabKey(const QString& weapon,const QString& magazine);
bool magazineGrabSupported(const HandPosePlacement& anchor);
QByteArray encodeReloadBundle(const ReloadRecipe& recipe,const QVector<PartGrip>& grips,
    const QVector<PartMechanism>& mechanisms,const QHash<QString,QString>& magazineGrabs,QString& error);
}
