#pragma once

#include "WeaponSimulation.h"
#include "domain/presets/WeaponPresets.h"

#include <optional>

namespace reloadmaker
{
    // The bundled presets (assets/presets beside the executable), read once on
    // first use. When the folder is unreadable or invalid the list is empty,
    // reloadPresetsError() names the problem, and every template lookup fails,
    // so nothing is authored or exported against a half-loaded preset set.
    [[nodiscard]] const QVector<WeaponPreset>& reloadPresets();
    [[nodiscard]] QString reloadPresetsError();

    // Template id <-> the coded simulation mechanism. The simulation still runs
    // one of five coded mechanisms until P2-08 retires WeaponMechanism.
    [[nodiscard]] std::optional<WeaponMechanism> templateMechanism(const QString& id);
    [[nodiscard]] QString templateIdentity(WeaponMechanism mechanism);
}
