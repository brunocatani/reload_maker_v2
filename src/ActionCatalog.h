#pragma once
#include <QString>
#include <optional>

namespace reloadmaker {
// Only weapon actions that can author a physical reload or a useful grab pose
// are exposed: the Ready idle, Equip, Fire, the Fire cycle (bolt/charge) and
// every Reload variant. Locomotion, sighted duplicates, melee, throws and
// Unequip are intentionally excluded.
enum class ActionFamily { Idle, Reload, Fire, Cycle, Equip };

struct ActionClass {
    ActionFamily family{ActionFamily::Idle};
    QString label;
    bool automatic{false};
    bool baseReload{false};
    int order{0};
};

// Classifies a first-person clip by its HKX base name (no directory/extension).
// Returns nullopt for clips outside the authored action families.
[[nodiscard]] std::optional<ActionClass> classifyWeaponClip(const QString& baseName);
[[nodiscard]] QString actionFamilyName(ActionFamily family);
}
