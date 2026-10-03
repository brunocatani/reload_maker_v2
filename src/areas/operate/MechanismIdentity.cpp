// Which operating mechanism the simulation runs for the open weapon. The
// named weapons are the testbeds built before behavior templates existed; an
// assigned template (_assignedMechanism) overrides the checks it covers. P2
// replaces these name checks with the weapon's configured subsystems.
#include "ReloadMakerController.h"

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

    // The optional QuickHands reload that holds the unextended standard
    // magazine. Empty unless that magazine and the QuickHands set are active.
    QString ReloadMakerController::glockNoExtenderPath() const
    {
        if (!isGlockConfiguration()) {
            return {};
        }
        const auto mods = configuredObjectMods();
        const bool standardMagazine = std::any_of(mods.cbegin(), mods.cend(), [this](const ObjectModRecord* mod) {
            return normalizeNifPath(mod->modelPath).endsWith(QStringLiteral("/glock19x/glock19x_mag.nif"), Qt::CaseInsensitive);
        });
        if (!standardMagazine) {
            return {};
        }
        const QString reload = resolveAnimationPath(QStringLiteral("WPNReload.hkx"));
        if (!reload.endsWith(QStringLiteral("/glock19xanims/quickhands/wpnreload.hkx"), Qt::CaseInsensitive)) {
            return {};
        }
        return resolveAnimationPath(QStringLiteral("WPNReloadNoExtender.hkx"));
    }

    bool ReloadMakerController::isAr15Configuration() const
    {
        if (_assignedMechanism) {
            return *_assignedMechanism == WeaponMechanism::Ar15;
        }
        const auto* weapon = currentWeapon();
        if (!weapon) {
            return false;
        }
        // The SREP AR-10 is a reciprocating bolt; the other SREP rifles are AR-15s.
        const bool srepAr15 = isSrepConfiguration() && !hasEditorId(weapon, QLatin1String("SREP_AR10"));
        return srepAr15 || isMk18Configuration() || hasEditorId(weapon, QLatin1String("WPNRU556"));
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
        if (_assignedMechanism) {
            return *_assignedMechanism == WeaponMechanism::M1A;
        }
        return hasEditorId(currentWeapon(), QLatin1String("WPNM1A"));
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
        if (_assignedMechanism) {
            return *_assignedMechanism == WeaponMechanism::BoltAction;
        }
        return hasEditorId(currentWeapon(), QLatin1String("C14Timberwolf")) || isL118Configuration();
    }

    WeaponMechanism ReloadMakerController::simulationMechanism() const
    {
        if (_assignedMechanism) {
            return *_assignedMechanism;
        }
        if (isBoltActionConfiguration()) {
            return WeaponMechanism::BoltAction;
        }
        if (isM1AConfiguration()) {
            return WeaponMechanism::M1A;
        }
        if (isAr15Configuration()) {
            return WeaponMechanism::Ar15;
        }
        if (isThanatosConfiguration() || isSrepConfiguration()) {
            return WeaponMechanism::ReciprocatingBolt;
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
