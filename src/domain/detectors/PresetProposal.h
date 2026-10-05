#pragma once

#include "domain/presets/WeaponPresets.h"

#include <QString>
#include <QVector>

namespace reloadmaker
{
    // What the weapon's records say about its action.
    struct ActionEvidence
    {
        bool boltAction{ false }; // the record's Bolt Action flag
        bool pistol{ false };     // a pistol keyword on the weapon or its parts
    };

    // The behavior preset Maker proposes before the author picks one (rework
    // 02 §5). Presets are matched by their model, not by id: a bolt action
    // takes the preset that is cycled by hand after each shot, a pistol the
    // one with a slide. Rifles are not told apart by their records, so they
    // get no proposal; the author chooses.
    struct PresetProposal
    {
        const WeaponPreset* preset{ nullptr };
        QString evidence; // why this preset
        QString problem;  // why there is none
    };

    [[nodiscard]] PresetProposal proposePreset(const ActionEvidence& evidence, const QVector<WeaponPreset>& presets);
}
