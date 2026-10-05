// Which operating mechanism the simulation runs for the open weapon: the
// assigned or proposed preset's. No weapon is recognized by name (rework 14).
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"

#include <algorithm>

namespace reloadmaker
{
    // The animation set's optional NoExtender reload (QuickHands sets ship
    // one for the unextended magazine); empty when the set has none.
    QString ReloadMakerController::noExtenderReloadPath() const
    {
        return resolveAnimationPath(QStringLiteral("WPNReloadNoExtender.hkx"));
    }

    bool ReloadMakerController::isAr15Configuration() const
    {
        return simulationMechanism() == WeaponMechanism::Ar15;
    }

    bool ReloadMakerController::isM1AConfiguration() const
    {
        return simulationMechanism() == WeaponMechanism::M1A;
    }

    bool ReloadMakerController::isBoltActionConfiguration() const
    {
        return simulationMechanism() == WeaponMechanism::BoltAction;
    }

    // The assigned preset's mechanism, else the proposed one. A weapon with
    // neither reads as a pistol slide until the author picks; Operate needs
    // an assigned preset either way.
    WeaponMechanism ReloadMakerController::simulationMechanism() const
    {
        if (_assignedMechanism) {
            return *_assignedMechanism;
        }
        if (const auto* proposed = proposedPreset()) {
            if (const auto mechanism = templateMechanism(proposed->id)) {
                return *mechanism;
            }
        }
        return WeaponMechanism::PistolSlide;
    }

    bool ReloadMakerController::isRifleConfiguration() const
    {
        const auto mechanism = simulationMechanism();
        return mechanism == WeaponMechanism::Ar15 ||
               mechanism == WeaponMechanism::M1A ||
               mechanism == WeaponMechanism::ReciprocatingBolt;
    }
}
