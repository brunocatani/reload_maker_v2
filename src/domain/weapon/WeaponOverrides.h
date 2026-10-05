#pragma once

#include "domain/weapon/WeaponModel.h"

#include <QJsonObject>
#include <QString>

#include <optional>

namespace reloadmaker
{
    // What the author chose for a whole weapon where its preset's default
    // does not fit (rework 14). Projects save one object per weapon record
    // under "weaponProperties"; an absent value keeps the preset's.
    struct WeaponOverrides
    {
        std::optional<InsertionPath> insertion;

        [[nodiscard]] bool empty() const noexcept { return !insertion; }
    };

    // Fails on unknown properties or values, so a newer project is never
    // half-read.
    [[nodiscard]] bool readWeaponOverrides(const QJsonObject& json, WeaponOverrides& overrides, QString& error);
    [[nodiscard]] QJsonObject weaponOverridesJson(const WeaponOverrides& overrides);
}
