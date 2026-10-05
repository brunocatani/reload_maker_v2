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
    // no grab points. Detectors observe values; the author confirms them.

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

    // Who normally completes the item. Anyone may in the UI; a value a person
    // authored (a hand fit, grab points) counts as confirmed by its author.
    enum class ChecklistOwner
    {
        Auto,
        Preset,
        You,
    };

    enum class ChecklistState
    {
        NeedsYou,
        Detected,
        Confirmed,
        NotApplicable,
    };

    struct ChecklistItem
    {
        QString id; // stable: confirmations are saved under it
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
        QJsonObject inputs;      // what the value depends on; hashed into the fingerprint
    };

    struct ChecklistItemStatus
    {
        ChecklistItem item;
        ChecklistState state{ ChecklistState::NeedsYou };
        QString value;
        QString evidence;
        QString message;
        QString fingerprint;
        bool stale{ false };      // confirmed earlier; the inputs changed since
        bool canConfirm{ false }; // a detected value awaits confirmation
        bool canSkip{ false };    // an optional item may be marked not applicable
        bool byYou{ false };      // confirmed, authored or skipped by a person
        bool rechecked{ true };   // false: shown from its saved confirmation
        QString confirmedAt;      // ISO time of the saved confirmation
    };

    // A saved confirmation: {"fingerprint": the confirmed fingerprint, or
    // "n/a" for a skipped optional item; "value": the value confirmed;
    // "at": ISO time}. The value shows while the detector cannot run.
    inline constexpr char kChecklistNotApplicable[] = "n/a";
    [[nodiscard]] QJsonObject checklistConfirmation(const QString& fingerprint, const QString& value, const QString& at);

    [[nodiscard]] QString checklistFingerprint(const QString& item, const QJsonObject& inputs);

    // `confirmations` maps item ids to saved confirmations. A confirmation
    // whose fingerprint no longer matches returns the item to Detected, marked
    // stale; "n/a" is honored for optional items only. An item whose detector
    // cannot run shows its saved confirmation, marked not rechecked.
    [[nodiscard]] QVector<ChecklistItemStatus> evaluateChecklist(const QVector<ChecklistItem>& items,
                                                                 const QHash<QString, ChecklistObservation>& observations,
                                                                 const QJsonObject& confirmations);

    struct ChecklistSummary
    {
        int total{ 0 };
        int done{ 0 }; // confirmed or not applicable
        int mandatory{ 0 };
        int mandatoryDone{ 0 };
        int needsYou{ 0 };
        int toConfirm{ 0 };
        bool started{ false };  // something was confirmed, authored or skipped
        bool complete{ false }; // every mandatory item is confirmed or not applicable
    };

    [[nodiscard]] ChecklistSummary summarizeChecklist(const QVector<ChecklistItemStatus>& items);

    [[nodiscard]] bool validChecklistConfirmations(const QJsonObject& confirmations);

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
    [[nodiscard]] QString weaponProgressName(WeaponProgress progress);
    [[nodiscard]] bool parseWeaponProgress(const QString& name, WeaponProgress& progress);
}
