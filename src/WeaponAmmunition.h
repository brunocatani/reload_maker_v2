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
    QString ammoName,ammoRecord,error;
    QJsonArray sources;
    bool valid()const{return error.isEmpty()&&capacity>0&&ammo!=0;}
    QJsonObject json()const;
};
WeaponAmmunition resolveWeaponAmmunition(const PluginContent& plugin,const WeaponRecord& weapon,
    const WeaponConfiguration& configuration,const QHash<QString,QString>& referenceNames={});
}
