#include "ReloadTemplates.h"

#include <QCoreApplication>

namespace reloadmaker
{
    namespace
    {
        struct BundledPresets
        {
            QVector<WeaponPreset> presets;
            QString error;
        };

        const BundledPresets& bundled()
        {
            static const BundledPresets value = [] {
                BundledPresets result;
                const QString directory = QCoreApplication::applicationDirPath() + QStringLiteral("/assets/presets");
                if (!loadWeaponPresets(directory, result.presets, result.error)) {
                    result.presets.clear();
                }
                return result;
            }();
            return value;
        }

        std::optional<WeaponMechanism> codedMechanism(const WeaponPreset& preset)
        {
            const DiamondSupport support = diamondSupport(preset.model);
            if (!support.action) {
                return std::nullopt;
            }
            switch (*support.action) {
            case MechanismKind::Slide:
                return WeaponMechanism::PistolSlide;
            case MechanismKind::ChargingHandle:
            case MechanismKind::HkSlap:
                return WeaponMechanism::Ar15;
            case MechanismKind::OperatingHandle:
                return WeaponMechanism::M1A;
            case MechanismKind::ReciprocatingBolt:
                return WeaponMechanism::ReciprocatingBolt;
            case MechanismKind::CellLever:
                return WeaponMechanism::CellLever;
            case MechanismKind::TurnBolt:
                return WeaponMechanism::BoltAction;
            default:
                return std::nullopt;
            }
        }
    }

    const QVector<WeaponPreset>& reloadPresets()
    {
        return bundled().presets;
    }

    QString reloadPresetsError()
    {
        return bundled().error;
    }

    std::optional<WeaponMechanism> templateMechanism(const QString& id)
    {
        for (const WeaponPreset& preset : reloadPresets()) {
            if (preset.id == id) {
                return codedMechanism(preset);
            }
        }
        return std::nullopt;
    }

}
