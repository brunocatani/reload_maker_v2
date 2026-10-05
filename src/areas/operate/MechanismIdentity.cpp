// Which operating mechanism the simulation runs for the open weapon: the
// assigned or proposed preset's. The remaining weapon-name checks are
// replaced by weapon properties (rework 14).
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"

#include <algorithm>

namespace reloadmaker
{
    namespace
    {
        bool hasEditorId(const WeaponRecord* weapon, QLatin1String editorId)
        {
            return weapon && weapon->editorId.compare(editorId, Qt::CaseInsensitive) == 0;
        }
    }

    bool ReloadMakerController::isGlockConfiguration() const
    {
        return hasEditorId(currentWeapon(), QLatin1String("WPNGlock19x"));
    }

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

    bool ReloadMakerController::isSrepConfiguration() const
    {
        const auto* weapon = currentWeapon();
        return hasEditorId(weapon, QLatin1String("SREP_ServiceRifle")) ||
               hasEditorId(weapon, QLatin1String("SREP_ServiceRifle_Unique_HoneyBadger")) ||
               hasEditorId(weapon, QLatin1String("SREP_AR10")) ||
               hasEditorId(weapon, QLatin1String("SREP_XM16E1"));
    }

    // An SREP SMG receiver carries its magazine inside the receiver model.
    bool ReloadMakerController::isSrepEmbeddedMagazine() const
    {
        if (!isSrepConfiguration()) {
            return false;
        }
        const auto* config = configuration();
        if (!config) {
            return false;
        }
        for (const auto id : config->activeMods()) {
            const auto* mod = config->mod(id);
            if (mod &&
                mod->editorId.contains(QStringLiteral("receiver_"), Qt::CaseInsensitive) &&
                mod->editorId.contains(QStringLiteral("SMG"), Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }

    bool ReloadMakerController::isMk18Configuration() const
    {
        const auto* weapon = currentWeapon();
        return hasEditorId(weapon, QLatin1String("WPNMK18")) || hasEditorId(weapon, QLatin1String("WPNMK18UNIQUE"));
    }

    bool ReloadMakerController::isMk18OverhaulConfiguration() const
    {
        if (!isMk18Configuration()) {
            return false;
        }
        const QString winner = currentPlugin()->origins.value(currentWeapon()->formId).winner;
        return winner.compare(QStringLiteral("MK18AttachmentOverhaul.esp"), Qt::CaseInsensitive) == 0;
    }

    bool ReloadMakerController::isM1AConfiguration() const
    {
        return simulationMechanism() == WeaponMechanism::M1A;
    }

    bool ReloadMakerController::isThanatosConfiguration() const
    {
        return hasEditorId(currentWeapon(), QLatin1String("DiaM107"));
    }

    bool ReloadMakerController::isL118Configuration() const
    {
        return hasEditorId(currentWeapon(), QLatin1String("L118A2"));
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
