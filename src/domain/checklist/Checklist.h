#pragma once

#include "domain/weapon/WeaponModel.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace reloadmaker
{
    // The checklist engine (rework 02 §4, item lists 10 §4). The items come
    // from the weapon model, so the checklist follows the weapon's behavior:
    // a manual-cycle Fire adds "cycle between shots" to its mechanism, a
    // lock-open mechanism adds its catch, a part driven by another part needs
    // no grab points. An item is done when its data exists, whoever made it;
    // the checklist lists only what is actually missing. Nothing needs a
    // separate confirmation.

    // The Reload builder step that edits an item, plus Fire; the left
    // Checklist groups items this way.
    enum class ChecklistSection
    {
        LearnedReload,
        PartGrips,
        Mechanisms,
        GrabPoints,
        Magazines,
        Fire,
    };

    enum class ChecklistSubsystem
    {
        Weapon,
        Feed,
        Action,
        Fire,
    };

    // Where the item's data normally comes from: read from the weapon's own
    // files and reload, given by the behavior preset, or authored.
    enum class ChecklistOwner
    {
        Auto,
        Preset,
        You,
    };

    enum class ChecklistState
    {
        Missing,       // the data does not exist yet
        Done,          // the data exists
        NotChecked,    // can be checked only where it runs (Operate); not work
        NotApplicable,
    };

    struct ChecklistItem
    {
        QString id; // stable: a skipped optional item is saved under it
        ChecklistSection section{ ChecklistSection::LearnedReload };
        ChecklistSubsystem subsystem{ ChecklistSubsystem::Weapon };
        ChecklistOwner owner{ ChecklistOwner::Auto };
        bool mandatory{ true };
        QString title;
        QString mechanism; // the mechanism id of a per-mechanism item
    };

    // The items `model` needs, in display order.
    [[nodiscard]] QVector<ChecklistItem> checklistItems(const WeaponModel& model);
    // The behavior preset item, the only one before a preset is chosen.
    [[nodiscard]] ChecklistItem presetChecklistItem();

    // What the detectors found for one item.
    struct ChecklistObservation
    {
        bool available{ true };  // false: the detector cannot run on this page
        bool detected{ false };  // a value exists
        bool applicable{ true }; // false: the item does not apply to this weapon
        QString value;           // short, shown on the row
        QString evidence;        // where the value came from
        QString problem;         // why nothing was detected, or why it cannot run
    };

    struct ChecklistItemStatus
    {
        ChecklistItem item;
        ChecklistState state{ ChecklistState::Missing };
        QString value;
        QString evidence;
        QString message;
        bool canSkip{ false }; // a missing optional item may be marked not applicable
        bool byYou{ false };   // authored, assigned or skipped by a person
    };

    // A skipped optional item is saved as {"state": "n/a", "at": ISO time}.
    // Older records also hold {"state": "confirmed", ...} entries from the
    // retired confirmation step; they stay valid and are ignored.
    inline constexpr char kChecklistNotApplicable[] = "n/a";
    [[nodiscard]] QJsonObject checklistSkip(const QString& at);

    // `saved` maps item ids to saved checklist entries; only skips of
    // optional items take effect.
    [[nodiscard]] QVector<ChecklistItemStatus> evaluateChecklist(const QVector<ChecklistItem>& items,
                                                                 const QHash<QString, ChecklistObservation>& observations,
                                                                 const QJsonObject& saved);

    struct ChecklistSummary
    {
        int total{ 0 };
        int done{ 0 }; // done or not applicable
        int mandatory{ 0 };
        int mandatoryDone{ 0 };
        int missing{ 0 };      // required items whose data does not exist
        int notChecked{ 0 };   // required items checked only in Operate
        bool started{ false };  // something was authored, assigned or skipped
        bool complete{ false }; // every required item is done or not applicable
    };

    [[nodiscard]] ChecklistSummary summarizeChecklist(const QVector<ChecklistItemStatus>& items);

    [[nodiscard]] bool validChecklistEntries(const QJsonObject& saved);

    // How far a weapon's checklist got, as the weapon list shows it.
    enum class WeaponProgress
    {
        NotStarted,
        Draft,
        Complete,
    };

    [[nodiscard]] WeaponProgress weaponProgress(const ChecklistSummary& summary);
    // A weapon over its evaluated configurations: complete when one is
    // complete and none is a draft; a draft while any configuration is.
    [[nodiscard]] WeaponProgress combineWeaponProgress(const QVector<WeaponProgress>& configurations);
    // The checklist rules a stored configuration result was evaluated with.
    // Raise it whenever what an item requires changes: results from older
    // rules make the next Save evaluate that weapon's configurations again.
    // 2: items read from the weapon need no confirmation.
    inline constexpr int kChecklistRules = 2;
    [[nodiscard]] QString weaponProgressName(WeaponProgress progress);
    [[nodiscard]] bool parseWeaponProgress(const QString& name, WeaponProgress& progress);
}
