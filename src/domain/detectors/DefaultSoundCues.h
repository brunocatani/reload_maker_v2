#pragma once

#include "domain/detectors/ClipEvents.h"

#include <QHash>
#include <QString>

#include <functional>

namespace reloadmaker
{
    // How the weapon is worked by hand, for the operating sounds' names.
    enum class CueAction { Slide, ChargingHandle, ReciprocatingBolt, OperatingHandle, BoltAction };

    // The default sound of each cue (rework 14, item 22), from the sounds the
    // native clips play, matched by the end of their names:
    //   pull / forward   ...BoltBack, ...Boltopen, ...SlideBack /
    //                    ...BoltForward, ...Boltclose, ...SlideForward; a
    //                    reciprocating bolt's forward may be ...Boltrelease.
    //                    Names with "Reload" or "Empty" rank below plain ones.
    //   unlock / lock    ...BoltUnlatch / ...BoltLatch (bolt actions)
    //   mag-release      ...MagRelease
    //   mag-out          ...MagOut (cloth and "ReloadMagOut" rank lower)
    //   mag-grab         a grab of the magazine (...GrabMag), a plain ...Grab,
    //                    then a cloth ...MagIn
    //   mag-entry        the magazine entry event (detectMagazineEvents)
    //   mag-seat         the next ...MagIn after the entry, else a ...MagSlap
    //                    or ...Magslam; a lone plain ...MagIn is the seat
    //   mag-insert       ...MagIns<n+1> after an entry ...MagIns<n>
    //   bolt-release     ...MagCatch or ...BoltCatch..., else ...BoltRelease
    //                    (charging handles and reciprocating bolts)
    // A trailing number is ignored except in MagIns<n>. Bolt actions read
    // their operating sounds from the bolt cycle clip first. A cue no clip
    // plays takes the catalog's conventional name, "<prefix><name>", where the
    // prefix is the magazine-out sound's name before "MagOut" (Glock19x clips
    // play no slide sounds; the catalog has WPNGlock19xSlideBack).
    struct DefaultSoundCues
    {
        QHash<QString, QString> sounds; // cue id -> sound editor ID
        QString prefix;
    };

    [[nodiscard]] DefaultSoundCues detectSoundCues(const QVector<ClipEvent>& reload, const QVector<ClipEvent>& cycle, CueAction action,
                                                   const std::function<bool(const QString&)>& catalogHas);
}
