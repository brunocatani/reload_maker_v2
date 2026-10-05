#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace reloadmaker
{
    // One annotation of a native clip, as the clip names it.
    struct ClipEvent
    {
        float seconds{ 0.0f };
        QString text;
    };

    // The sound descriptor's editor ID a "SoundPlay.<EDID>" event plays;
    // empty for other events.
    [[nodiscard]] QString playedSound(const ClipEvent& event);

    // When the magazine starts into the well and when the old one comes out,
    // from the reload clip's sound events (rework 02 §5). Clips name these
    // sounds per weapon (WPNM1AMagIn, C14_MagInserting, L115A3_ReloadMagIn,
    // WPNM107Magin), so the detector reads the name's ending:
    //   entry    "...MagInserting" / "...MagInsert" / "...MagIns<n>", then the
    //            reload layer's "...ReloadMagIn", then a plain "...MagIn";
    //   removal  a plain "...MagOut", then "...ReloadMagOut".
    // Cloth and other handling layers ("...ClothMagIn") are not magazine
    // events. Within one kind the earliest event wins; more than one is
    // reported, since the author may need to pick.
    struct MagazineEvents
    {
        float entrySeconds{ -1.0f };
        QString entry; // the event text
        float removalSeconds{ -1.0f };
        QString removal;
        QStringList ambiguities; // one line per kind with several candidates
    };

    [[nodiscard]] MagazineEvents detectMagazineEvents(const QVector<ClipEvent>& events);
}
