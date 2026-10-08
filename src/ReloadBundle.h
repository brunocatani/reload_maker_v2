#pragma once
#include "ReloadRecipe.h"
#include "PartGrip.h"
#include "PartMechanism.h"
#include <QHash>

namespace reloadmaker {
// A project assignment belongs to a weapon and one concrete magazine owner.
QString magazineGrabKey(const QString& weapon,const QString& magazine);
QString handAssignmentKey(const QString& key,PoseHand hand);
bool magazineGrabSupported(const HandPosePlacement& anchor);
// Explicit magazine fits win over a weapon default. Defaults are instantiated
// against the actual magazine identity; they never change the magazine frame.
// Fits are keyed by hand ("<weapon>|<magazine>|left", "<weapon>|left").
// Resolution (fits are optional refinements): this magazine's own fit, the
// weapon default, else the weapon's first fit for the hand on any magazine
// (rebound to this magazine), then `shared` - fits of same-ESP siblings by
// lower-case magazine|hand, their defaults by default|hand, else a sibling's
// first fit for the hand.
std::optional<PartGrip> magazineGripFor(const QString& weapon,const HandPosePlacement& anchor,
    const QVector<PartGrip>& library,const QHash<QString,QString>& assignments,
    const QHash<QString,QString>& defaults,QString& error,PoseHand hand,
    const QHash<QString,QString>& shared={});
QByteArray encodeReloadBundle(const ReloadRecipe& recipe,const QVector<PartGrip>& grips,
    const QVector<PartMechanism>& mechanisms,const QHash<QString,QString>& magazineGrabs,QString& error,
    const QHash<QString,QString>& defaults={},const HandPosePlacement* selectedMagazine=nullptr);
}
