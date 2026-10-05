#include "domain/checklist/Checklist.h"

#include <QRegularExpression>

namespace reloadmaker
{
    namespace
    {
        QString partName(MechanismKind kind)
        {
            switch (kind) {
            case MechanismKind::Slide: return QStringLiteral("slide");
            case MechanismKind::ChargingHandle: return QStringLiteral("charging handle");
            case MechanismKind::OperatingHandle: return QStringLiteral("operating handle");
            case MechanismKind::ReciprocatingBolt:
            case MechanismKind::TurnBolt: return QStringLiteral("bolt");
            case MechanismKind::HkSlap: return QStringLiteral("cocking handle");
            case MechanismKind::Pump: return QStringLiteral("pump");
            case MechanismKind::Lever: return QStringLiteral("lever");
            case MechanismKind::Hinge: return QStringLiteral("barrels");
            case MechanismKind::Hammer: return QStringLiteral("hammer");
            case MechanismKind::Crane: return QStringLiteral("cylinder");
            case MechanismKind::Cover: return QStringLiteral("top cover");
            case MechanismKind::Latch: return QStringLiteral("latch");
            }
            return QStringLiteral("part");
        }

        QString capitalized(QString text)
        {
            if (!text.isEmpty()) {
                text[0] = text[0].toUpper();
            }
            return text;
        }

        QString unitName(UnitKind unit)
        {
            switch (unit) {
            case UnitKind::Container: return QStringLiteral("magazine");
            case UnitKind::Round: return QStringLiteral("round");
            case UnitKind::Clip: return QStringLiteral("clip");
            case UnitKind::Projectile: return QStringLiteral("projectile");
            case UnitKind::BeltLead: return QStringLiteral("belt");
            }
            return QStringLiteral("unit");
        }
    }

    QVector<ChecklistItem> checklistItems(const WeaponModel& model)
    {
        using S = ChecklistSection;
        using Sub = ChecklistSubsystem;
        using O = ChecklistOwner;
        QVector<ChecklistItem> items;
        const auto add = [&](const QString& id, S section, Sub subsystem, O owner, bool mandatory, const QString& title, const QString& mechanism = {}) {
            items.append({ id, section, subsystem, owner, mandatory, title, mechanism });
        };

        items.append(presetChecklistItem());
        add("weapon.reload", S::LearnedReload, Sub::Weapon, O::Auto, true, "Reload learned");

        const bool detachable = model.feed.container == ContainerKind::Detachable;
        if (detachable) {
            add("feed.fits", S::PartGrips, Sub::Feed, O::You, true, "Magazine hand fits");
        }
        for (const auto& point : model.feed.loadingPoints) {
            if (point.unit != UnitKind::Container) {
                add("feed." + point.id + ".fit", S::PartGrips, Sub::Feed, O::You, true, "Hand fit for the " + unitName(point.unit));
            }
        }

        for (const auto& m : model.mechanisms) {
            const QString part = partName(m.kind);
            const QString prefix = "action." + m.id;
            if (m.driver == MechanismDriver::Hand) {
                add(prefix + ".part", S::Mechanisms, Sub::Action, O::Auto, true, "Moving " + part + " and followers", m.id);
                add(prefix + ".motion", S::Mechanisms, Sub::Action, O::Auto, true, capitalized(part) + " motion and endpoints", m.id);
            } else {
                const auto* driver = model.mechanism(m.drivenBy);
                add(prefix + ".follows", S::Mechanisms, Sub::Action, O::Auto, true,
                    capitalized(part) + " follows the " + (driver ? partName(driver->kind) : m.drivenBy), m.id);
            }
            if (model.fire.afterShot == AfterShot::ManualCycle && model.fire.manualCycleMechanism == m.id) {
                add(prefix + ".cycle", S::Mechanisms, Sub::Action, O::Auto, true, "Cycle the " + part + " between shots", m.id);
            }
            if (model.fire.lockOpenMechanism == m.id) {
                add(prefix + ".catch", S::Mechanisms, Sub::Action, O::Preset, true, capitalized(part) + " locks open on empty", m.id);
            }
        }

        bool handOperated = false;
        for (const auto& m : model.mechanisms) {
            if (m.driver == MechanismDriver::Hand) {
                handOperated = true;
                add("action." + m.id + ".grab", S::GrabPoints, Sub::Action, O::You, true, "Grab points on the " + partName(m.kind), m.id);
            }
        }
        if (handOperated) {
            add("action.fingers", S::GrabPoints, Sub::Action, O::You, false, "Operating fingers");
        }

        if (detachable) {
            add("feed.presentation", S::Magazines, Sub::Feed, O::Auto, true, "Full and empty magazines");
            add("feed.guide", S::Magazines, Sub::Feed, O::Auto, true, "Insertion guide");
            add("feed.world", S::Magazines, Sub::Feed, O::Auto, false, "World models and collision");
        }

        add("fire.motion", S::Fire, Sub::Fire, O::Auto, true, "What moves on fire");
        add("fire.afterShot", S::Fire, Sub::Fire, O::Preset, true, "After each shot");
        add("fire.ejection", S::Fire, Sub::Fire, O::Preset, true, "Case ejection");
        add("fire.modes", S::Fire, Sub::Fire, O::Auto, true, "Fire modes");
        add("fire.dry", S::Fire, Sub::Fire, O::Auto, false, "Dry fire");
        add("fire.sound", S::Fire, Sub::Fire, O::Auto, false, "Fire sound");
        add("sounds.reload", S::Fire, Sub::Feed, O::Auto, false, "Reload sounds");
        return items;
    }

    ChecklistItem presetChecklistItem()
    {
        return { "weapon.preset", ChecklistSection::LearnedReload, ChecklistSubsystem::Weapon, ChecklistOwner::Auto, true, "Behavior preset", {} };
    }

    QJsonObject checklistConfirmation(const QString& value, const QString& at)
    {
        return { { "state", "confirmed" }, { "value", value.left(200) }, { "at", at } };
    }

    QJsonObject checklistSkip(const QString& at)
    {
        return { { "state", kChecklistNotApplicable }, { "at", at } };
    }

    QVector<ChecklistItemStatus> evaluateChecklist(const QVector<ChecklistItem>& items,
                                                   const QHash<QString, ChecklistObservation>& observations,
                                                   const QJsonObject& confirmations)
    {
        QVector<ChecklistItemStatus> result;
        result.reserve(items.size());
        for (const auto& item : items) {
            ChecklistItemStatus status;
            status.item = item;
            const auto observation = observations.value(item.id);
            status.value = observation.value;
            status.evidence = observation.evidence;
            const auto saved = confirmations.value(item.id).toObject();
            const bool confirmed = saved.value("state").toString() == QLatin1String("confirmed");
            const bool skipped = saved.value("state").toString() == QLatin1String(kChecklistNotApplicable) && !item.mandatory;
            status.confirmedAt = saved.value("at").toString();
            status.canSkip = !item.mandatory;
            if (!observation.available) {
                status.rechecked = false;
                status.canSkip = false;
                status.message = observation.problem;
                if (skipped) {
                    status.state = ChecklistState::NotApplicable;
                    status.byYou = true;
                } else if (confirmed) {
                    status.state = ChecklistState::Confirmed;
                    status.value = saved.value("value").toString();
                    status.byYou = true;
                } else {
                    status.state = ChecklistState::NeedsYou;
                }
            } else if (!observation.applicable) {
                status.state = ChecklistState::NotApplicable;
                status.message = observation.problem;
                status.canSkip = false;
            } else if (skipped) {
                status.state = ChecklistState::NotApplicable;
                status.message = QStringLiteral("Marked not applicable by you");
                status.canSkip = false;
                status.byYou = true;
            } else if (!observation.detected) {
                status.state = ChecklistState::NeedsYou;
                status.message = observation.problem;
            } else if (item.owner == ChecklistOwner::You || confirmed) {
                status.state = ChecklistState::Confirmed;
                status.byYou = true;
            } else {
                status.state = ChecklistState::Detected;
                status.canConfirm = true;
            }
            result.append(status);
        }
        return result;
    }

    ChecklistSummary summarizeChecklist(const QVector<ChecklistItemStatus>& items)
    {
        ChecklistSummary summary;
        for (const auto& status : items) {
            const bool done = status.state == ChecklistState::Confirmed || status.state == ChecklistState::NotApplicable;
            ++summary.total;
            summary.done += done;
            summary.needsYou += status.state == ChecklistState::NeedsYou;
            summary.toConfirm += status.state == ChecklistState::Detected;
            if (status.item.mandatory) {
                ++summary.mandatory;
                summary.mandatoryDone += done;
            }
            // A detector's "not applicable" is not work; a confirmation,
            // an authored value or a skip is.
            summary.started = summary.started || status.byYou;
        }
        summary.complete = summary.mandatory > 0 && summary.mandatoryDone == summary.mandatory;
        return summary;
    }

    bool validChecklistConfirmations(const QJsonObject& confirmations)
    {
        static const QRegularExpression id(QStringLiteral("^[a-z0-9][a-z0-9.\\-]{0,127}$"), QRegularExpression::CaseInsensitiveOption);
        if (confirmations.size() > 256) {
            return false;
        }
        for (auto it = confirmations.begin(); it != confirmations.end(); ++it) {
            const auto saved = it.value().toObject();
            const auto state = saved.value("state").toString();
            if (!id.match(it.key()).hasMatch() || !it.value().isObject() || saved.size() > 3 ||
                (state != QLatin1String("confirmed") && state != QLatin1String(kChecklistNotApplicable)) ||
                (saved.contains("value") && (!saved.value("value").isString() || saved.value("value").toString().size() > 200)) ||
                !saved.value("at").isString() || saved.value("at").toString().size() > 40) {
                return false;
            }
        }
        return true;
    }

    WeaponProgress weaponProgress(const ChecklistSummary& summary)
    {
        return summary.complete ? WeaponProgress::Complete : summary.started ? WeaponProgress::Draft : WeaponProgress::NotStarted;
    }

    WeaponProgress combineWeaponProgress(const QVector<WeaponProgress>& configurations)
    {
        if (configurations.contains(WeaponProgress::Draft)) {
            return WeaponProgress::Draft;
        }
        return configurations.contains(WeaponProgress::Complete) ? WeaponProgress::Complete : WeaponProgress::NotStarted;
    }

    QString weaponProgressName(WeaponProgress progress)
    {
        switch (progress) {
        case WeaponProgress::Complete: return QStringLiteral("complete");
        case WeaponProgress::Draft: return QStringLiteral("draft");
        case WeaponProgress::NotStarted: break;
        }
        return QStringLiteral("not-started");
    }

    bool parseWeaponProgress(const QString& name, WeaponProgress& progress)
    {
        for (const auto value : { WeaponProgress::NotStarted, WeaponProgress::Draft, WeaponProgress::Complete }) {
            if (weaponProgressName(value) == name) {
                progress = value;
                return true;
            }
        }
        return false;
    }
}
