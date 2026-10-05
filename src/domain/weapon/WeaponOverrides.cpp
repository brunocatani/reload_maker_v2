#include "domain/weapon/WeaponOverrides.h"

namespace reloadmaker
{
    bool readWeaponOverrides(const QJsonObject& json, WeaponOverrides& overrides, QString& error)
    {
        WeaponOverrides result;
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (it.key() == QLatin1String("insertion")) {
                result.insertion = insertionPathFromName(it.value().toString());
                if (!result.insertion) {
                    error = QStringLiteral("Unknown magazine insertion path: ") + it.value().toString();
                    return false;
                }
            } else {
                error = QStringLiteral("Unknown weapon property: ") + it.key();
                return false;
            }
        }
        overrides = result;
        return true;
    }

    QJsonObject weaponOverridesJson(const WeaponOverrides& overrides)
    {
        QJsonObject json;
        if (overrides.insertion) {
            json["insertion"] = insertionPathName(*overrides.insertion);
        }
        return json;
    }
}
