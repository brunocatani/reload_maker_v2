#pragma once

#include "domain/weapon/WeaponModel.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <optional>

namespace reloadmaker
{
    // A preset pre-selects a whole weapon model (rework 02 §2). Its id is the
    // DIAMOND template id written into packs, so it never changes; a new
    // behavior gets a new id. Presets ship as data in assets/presets.
    struct WeaponPreset
    {
        QString id;
        QString label;
        QString category; // "Rifle / Sniper" lists a preset under both families
        WeaponModel model;
    };

    // Whether DIAMOND can run a model today. DIAMOND accepts exactly the five
    // template ids of the bundled presets (DIAMOND ContentCatalog.cpp
    // mechanism(), refusal covered by its ContentTests); every other
    // combination is refused with the reason shown to the author.
    struct DiamondSupport
    {
        std::optional<MechanismKind> action; // the hand-operated action when supported
        QString refusal;                     // set exactly when action is empty
    };

    [[nodiscard]] DiamondSupport diamondSupport(const WeaponModel& model);

    [[nodiscard]] bool decodeWeaponPreset(const QByteArray& bytes, WeaponPreset& preset, QString& error);

    // Reads every *.json in `directory`, sorted by file name. Fails when a file
    // is invalid, an id repeats, or a preset is not the DIAMOND behavior its id
    // promises (two presets may not claim the same action).
    [[nodiscard]] bool loadWeaponPresets(const QString& directory, QVector<WeaponPreset>& presets, QString& error);
}
