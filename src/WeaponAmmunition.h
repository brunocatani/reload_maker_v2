#pragma once
#include "WeaponConfiguration.h"
#include <QJsonArray>
#include <QJsonObject>

namespace reloadmaker {
struct WeaponAmmunition {
    int capacity{0};
    std::uint32_t ammo{0};
    bool pistol{false};
    bool automatic{false};
    bool repeatableSingleFire{false};
    bool repeatsWhileHeld()const{return automatic||repeatableSingleFire;}
    QString ammoName,ammoRecord,error;
    // Best-effort decisions where the records leave the engine's order open
    // (owner, 2026-10-08: not a gate). The game's live weapon decides in play.
    QStringList warnings;
    QJsonArray sources;
    bool valid()const{return error.isEmpty()&&capacity>0&&ammo!=0;}
    QJsonObject json()const;
};
WeaponAmmunition resolveWeaponAmmunition(const PluginContent& plugin,const WeaponRecord& weapon,
    const WeaponConfiguration& configuration,const QHash<QString,QString>& referenceNames={});
}
