// The checklist follows the weapon model: each bundled preset yields the
// items its behavior needs (a manual-cycle bolt gets "cycle between shots",
// a driven bolt gets no grab points), and the item states, staleness,
// skips and completion follow rework 02 §4.
#include "domain/checklist/Checklist.h"
#include "domain/presets/WeaponPresets.h"

#include <QCoreApplication>
#include <QSet>
#include <QTextStream>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void check(bool ok, const QString& why)
    {
        if (!ok) {
            throw std::runtime_error(why.toStdString());
        }
    }

    QSet<QString> ids(const QVector<ChecklistItem>& items)
    {
        QSet<QString> result;
        for (const auto& item : items) {
            check(!result.contains(item.id), "Repeated checklist item " + item.id);
            result.insert(item.id);
        }
        return result;
    }

    const ChecklistItemStatus& find(const QVector<ChecklistItemStatus>& items, const QString& id)
    {
        for (const auto& item : items) {
            if (item.item.id == id) {
                return item;
            }
        }
        throw std::runtime_error(("Missing checklist item " + id).toStdString());
    }

    void presetItems(const QString& directory)
    {
        QVector<WeaponPreset> presets;
        QString error;
        check(loadWeaponPresets(directory, presets, error), error);
        QHash<QString, QSet<QString>> byPreset;
        for (const auto& preset : presets) {
            byPreset.insert(preset.id, ids(checklistItems(preset.model)));
        }
        const auto& pistol = byPreset.value("pistol.slide-detachable.v1");
        const auto& ar15 = byPreset.value("rifle.ar15-detachable.v1");
        const auto& bolt = byPreset.value("sniper.manual-bolt-detachable.v1");
        const auto& m1a = byPreset.value("rifle.operating-handle-rocking.v1");
        check(pistol.contains("action.slide.grab") && pistol.contains("action.slide.catch") && !pistol.contains("action.slide.cycle"),
              "Pistol items: slide grab points and catch, no manual cycle");
        check(bolt.contains("action.bolt.cycle") && !bolt.contains("action.bolt.catch"), "Bolt action needs the manual cycle between shots");
        check(ar15.contains("action.handle.grab") && ar15.contains("action.bolt.follows") && ar15.contains("action.bolt.catch") &&
                  !ar15.contains("action.bolt.grab") && !ar15.contains("action.bolt.cycle"),
              "AR-15: the handle is grabbed, the bolt follows it and locks open");
        check(!m1a.contains("action.handle.cycle") && !m1a.contains("action.handle.catch"), "M1A has no manual cycle and no catch");
        for (const auto& set : byPreset) {
            check(set.contains("feed.fits") && set.contains("feed.guide") && set.contains("fire.afterShot") && set.contains("weapon.preset"),
                  "Every detachable-magazine preset needs fits, a guide and fire behavior");
        }
    }

    void singleUnitFeed()
    {
        WeaponModel shotgun;
        shotgun.feed.container = ContainerKind::Fixed;
        shotgun.feed.loadingPoints = { { "tube-port", UnitKind::Round, LoadDestination::Container, {}, 6 } };
        shotgun.mechanisms = { { "pump", MechanismKind::Pump, CycleRole::Extract | CycleRole::Eject | CycleRole::Chamber, MechanismReturn::Stays } };
        shotgun.fire = { FireMotion::Nothing, AfterShot::ManualCycle, CaseEjection::OnManualCycle, "pump", {} };
        const auto items = ids(checklistItems(shotgun));
        check(items.contains("feed.tube-port.fit") && !items.contains("feed.fits") && !items.contains("feed.guide"),
              "A tube feed needs a round fit, not magazine items");
        check(items.contains("action.pump.cycle"), "A pump needs its cycle between shots");
    }

    void states()
    {
        WeaponModel model;
        model.feed.container = ContainerKind::Detachable;
        model.feed.loadingPoints = { { "magwell", UnitKind::Container, LoadDestination::Container, {}, 1 } };
        model.mechanisms = { { "slide", MechanismKind::Slide, CycleRole::Extract | CycleRole::Chamber, MechanismReturn::Spring, MechanismDriver::Hand, {}, true } };
        model.fire.lockOpenMechanism = "slide";
        const auto items = checklistItems(model);

        QHash<QString, ChecklistObservation> observed;
        for (const auto& item : items) {
            observed.insert(item.id, { true, true, true, "value", "evidence", {}, { { "value", item.id } } });
        }
        observed["feed.fits"] = { true, false, true, {}, {}, "Fit the left hand", {} };
        observed["feed.world"] = { true, false, false, {}, {}, "No loose magazine model", {} };
        auto evaluated = evaluateChecklist(items, observed, {});
        auto summary = summarizeChecklist(evaluated);
        check(find(evaluated, "feed.fits").state == ChecklistState::NeedsYou && find(evaluated, "feed.fits").message == "Fit the left hand",
              "Missing authored value needs you");
        check(find(evaluated, "action.slide.grab").state == ChecklistState::Confirmed, "Authored value counts as confirmed by its author");
        check(find(evaluated, "feed.world").state == ChecklistState::NotApplicable && !find(evaluated, "feed.world").byYou, "Detector skip");
        check(find(evaluated, "weapon.preset").state == ChecklistState::Detected && find(evaluated, "weapon.preset").canConfirm, "Detected value awaits confirmation");
        check(summary.started && !summary.complete && summary.needsYou == 1, "Partial checklist summary");

        QJsonObject confirmations;
        for (const auto& status : evaluated) {
            if (status.canConfirm) {
                confirmations[status.item.id] = checklistConfirmation(status.fingerprint, status.value, "2026-10-04T14:02:00Z");
            }
        }
        observed["feed.fits"] = { true, true, true, "Left and right", {}, {}, {} };
        evaluated = evaluateChecklist(items, observed, confirmations);
        summary = summarizeChecklist(evaluated);
        check(summary.complete && weaponProgress(summary) == WeaponProgress::Complete, "Every mandatory item confirmed completes the weapon");

        // A changed input returns the item to Detected, marked stale.
        observed["feed.guide"].inputs = { { "angle", 4 } };
        evaluated = evaluateChecklist(items, observed, confirmations);
        check(find(evaluated, "feed.guide").state == ChecklistState::Detected && find(evaluated, "feed.guide").stale, "Stale confirmation");
        check(!summarizeChecklist(evaluated).complete, "A stale mandatory item blocks completion");

        // A detector that cannot run keeps the saved confirmation, not rechecked.
        auto away = observed;
        away["feed.presentation"] = { false, false, true, {}, {}, "Open Operate to recheck", {} };
        evaluated = evaluateChecklist(items, away, confirmations);
        const auto kept = find(evaluated, "feed.presentation");
        check(kept.state == ChecklistState::Confirmed && !kept.rechecked && kept.value == "value" && kept.confirmedAt.startsWith("2026"),
              "Unavailable detector lost the saved confirmation");
        away["weapon.preset"] = { false, false, true, {}, {}, "Not loaded", {} };
        check(find(evaluateChecklist(items, away, {}), "weapon.preset").state == ChecklistState::NeedsYou, "Unknown item reported done");

        // Optional items may be skipped; mandatory ones may not.
        const auto skip = checklistConfirmation(kChecklistNotApplicable, {}, "2026-10-04T14:03:00Z");
        confirmations["fire.sound"] = skip;
        confirmations["feed.guide"] = skip;
        evaluated = evaluateChecklist(items, observed, confirmations);
        check(find(evaluated, "fire.sound").state == ChecklistState::NotApplicable && find(evaluated, "fire.sound").byYou, "Optional skip");
        check(find(evaluated, "feed.guide").state == ChecklistState::Detected, "Mandatory item skipped");

        check(validChecklistConfirmations(confirmations), "Saved confirmations rejected");
        check(!validChecklistConfirmations({ { "feed.guide", checklistConfirmation("maybe", {}, {}) } }) &&
                  !validChecklistConfirmations({ { "../x", skip } }) && !validChecklistConfirmations({ { "feed.guide", "n/a" } }),
              "Invalid confirmations accepted");
        check(weaponProgress(summarizeChecklist(evaluateChecklist(items, {}, {}))) == WeaponProgress::NotStarted, "Nothing done is not started");
        using P = WeaponProgress;
        check(combineWeaponProgress({ P::Complete, P::NotStarted }) == P::Complete && combineWeaponProgress({ P::Complete, P::Draft }) == P::Draft &&
                  combineWeaponProgress({}) == P::NotStarted,
              "Weapon progress over configurations");
        WeaponProgress parsed{};
        check(parseWeaponProgress("draft", parsed) && parsed == WeaponProgress::Draft && !parseWeaponProgress("done", parsed), "Progress names");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        check(app.arguments().size() == 2, "Usage: ChecklistTests <preset directory>");
        presetItems(app.arguments()[1]);
        singleUnitFeed();
        states();
        QTextStream(stdout) << "PASS checklist items per preset and feed, states, staleness, skips, completion and weapon progress\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << "FAIL " << e.what() << '\n';
        return 1;
    }
}
