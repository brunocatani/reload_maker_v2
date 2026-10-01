#pragma once
#include "WeaponSimulation.h"
#include <array>
#include <optional>

namespace reloadmaker {
struct ReloadTemplate {const char* id;const char* label;const char* category;WeaponMechanism mechanism;};
inline constexpr std::array<ReloadTemplate,5> reloadTemplates{{
    {"pistol.slide-detachable.v1","Pistol — slide and magazine","Pistol",WeaponMechanism::PistolSlide},
    {"rifle.ar15-detachable.v1","AR-15 — independent charging handle","Rifle",WeaponMechanism::Ar15},
    {"rifle.operating-handle-rocking.v1","Rifle — operating handle and rocking magazine","Rifle",WeaponMechanism::M1A},
    {"rifle.reciprocating-detachable.v1","Semi-auto rifle / sniper — reciprocating bolt","Rifle / Sniper",WeaponMechanism::ReciprocatingBolt},
    {"sniper.manual-bolt-detachable.v1","Bolt-action sniper — detachable magazine","Sniper",WeaponMechanism::BoltAction}
}};
inline std::optional<WeaponMechanism> templateMechanism(const QString& id) {
    for(const auto& value:reloadTemplates)if(id==QLatin1String(value.id))return value.mechanism;return {};
}
inline QString templateIdentity(WeaponMechanism mechanism) {
    for(const auto& value:reloadTemplates)if(value.mechanism==mechanism)return QLatin1String(value.id);return {};
}
}
