#pragma once

#include "WeaponSimulation.h"

#include <QString>

namespace reloadmaker
{
    // Display name of a simulation action. The held and returning part is
    // named for the action type: slide, charging handle or bolt.
    [[nodiscard]] inline QString simulationActionName(SimulationAction action, bool rifle = false, bool boltAction = false)
    {
        switch (action) {
        case SimulationAction::Fire:
            return QStringLiteral("Firing");
        case SimulationAction::Reload:
            return QStringLiteral("Automatic reload");
        case SimulationAction::Held:
            if (boltAction) {
                return QStringLiteral("Bolt held");
            }
            return rifle ? QStringLiteral("Charging handle held") : QStringLiteral("Slide held");
        case SimulationAction::Returning:
            return rifle ? QStringLiteral("Bolt / handle returning") : QStringLiteral("Slide returning");
        default:
            return QStringLiteral("Ready");
        }
    }
}
