#pragma once
#include "ReloadRecipe.h"
#include "PartGrip.h"
#include "PartMechanism.h"
#include <QHash>

namespace reloadmaker {
// A project assignment belongs to a weapon and one concrete magazine owner.
QString magazineGrabKey(const QString& weapon,const QString& magazine);
bool magazineGrabSupported(const HandPosePlacement& anchor);
// Explicit magazine fits win over a weapon default. Defaults are instantiated
// against the actual magazine identity; they never change the magazine frame.
std::optional<PartGrip> magazineGripFor(const QString& weapon,const HandPosePlacement& anchor,
    const QVector<PartGrip>& library,const QHash<QString,QString>& assignments,
    const QHash<QString,QString>& defaults,QString& error);
QByteArray encodeReloadBundle(const ReloadRecipe& recipe,const QVector<PartGrip>& grips,
    const QVector<PartMechanism>& mechanisms,const QHash<QString,QString>& magazineGrabs,QString& error,
    const QHash<QString,QString>& defaults={});
}
