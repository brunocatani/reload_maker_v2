// The checklist follows the weapon model: each bundled preset yields the
// items its behavior needs (a manual-cycle bolt gets "cycle between shots",
// a driven bolt gets no grab points), and the item states, skips and
// completion follow rework 02 §4. An item is done when its data exists,
// whoever made it; nothing waits for a confirmation.
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
            observed.insert(item.id, { true, true, true, "value", "evidence", {} });
        }
        observed["feed.fits"] = { true, false, true, {}, {}, "Fit the left hand" };
        observed["feed.world"] = { true, false, false, {}, {}, "No loose magazine model" };
        auto evaluated = evaluateChecklist(items, observed, {});
        auto summary = summarizeChecklist(evaluated);
        check(find(evaluated, "feed.fits").state == ChecklistState::Missing && find(evaluated, "feed.fits").message == "Fit the left hand",
              "Missing authored value is missing");
        check(find(evaluated, "action.slide.grab").state == ChecklistState::Done && find(evaluated, "action.slide.grab").byYou,
              "Authored value is done");
        check(find(evaluated, "feed.guide").state == ChecklistState::Done && !find(evaluated, "feed.guide").byYou,
              "Value read from the weapon is done without a confirmation");
        check(find(evaluated, "feed.world").state == ChecklistState::NotApplicable && !find(evaluated, "feed.world").byYou, "Detector skip");
        check(summary.started && !summary.complete && summary.missing == 1, "Partial checklist summary");

        observed["feed.fits"] = { true, true, true, "Left and right", {}, {} };
        evaluated = evaluateChecklist(items, observed, {});
        summary = summarizeChecklist(evaluated);
        check(summary.complete && summary.missing == 0 && weaponProgress(summary) == WeaponProgress::Complete,
              "Every required item existing completes the weapon");

        // A changed value stays done and shows the new value.
        auto changed = observed;
        changed["feed.guide"].value = "4 degrees steeper";
        evaluated = evaluateChecklist(items, changed, {});
        check(find(evaluated, "feed.guide").state == ChecklistState::Done && find(evaluated, "feed.guide").value == "4 degrees steeper",
              "A changed value is not done");

        // Retired confirmations in older records are valid and ignored.
        QJsonObject saved{ { "feed.fits", QJsonObject{ { "state", "confirmed" }, { "value", "old" }, { "at", "2026-10-04T14:02:00Z" } } } };
        auto missingFit = observed;
        missingFit["feed.fits"] = { true, false, true, {}, {}, "Fit the left hand" };
        check(find(evaluateChecklist(items, missingFit, saved), "feed.fits").state == ChecklistState::Missing,
              "A retired confirmation hid missing data");

        // While the weapon loads nothing is reported missing or done.
        auto loading = observed;
        loading["feed.presentation"] = { false, false, true, {}, {}, "Wait for the weapon" };
        evaluated = evaluateChecklist(items, loading, {});
        summary = summarizeChecklist(evaluated);
        check(find(evaluated, "feed.presentation").state == ChecklistState::NotChecked && summary.notChecked == 1 && summary.missing == 0 &&
                  !summary.complete,
              "Loading item reported as missing or done");

        // Missing optional items may be skipped; required ones may not.
        const auto skip = checklistSkip("2026-10-04T14:03:00Z");
        auto optional = observed;
        optional["fire.sound"] = { true, false, true, {}, {}, "No fire sound" };
        check(find(evaluateChecklist(items, optional, {}), "fire.sound").canSkip, "Missing optional item cannot be skipped");
        saved = QJsonObject{ { "fire.sound", skip }, { "feed.guide", skip } };
        evaluated = evaluateChecklist(items, optional, saved);
        check(find(evaluated, "fire.sound").state == ChecklistState::NotApplicable && find(evaluated, "fire.sound").byYou, "Optional skip");
        check(find(evaluated, "feed.guide").state == ChecklistState::Done, "Required item skipped");

        saved["feed.fits"] = QJsonObject{ { "state", "confirmed" }, { "value", "old" }, { "at", "2026-10-04T14:02:00Z" } };
        check(validChecklistEntries(saved), "Saved checklist entries rejected");
        check(!validChecklistEntries({ { "feed.guide", QJsonObject{ { "state", "maybe" }, { "at", "" } } } }) &&
                  !validChecklistEntries({ { "../x", skip } }) && !validChecklistEntries({ { "feed.guide", "n/a" } }),
              "Invalid checklist entries accepted");
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
        QTextStream(stdout) << "PASS checklist items per preset and feed, done/missing states without confirmations, skips, completion and weapon progress\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << "FAIL " << e.what() << '\n';
        return 1;
    }
}
