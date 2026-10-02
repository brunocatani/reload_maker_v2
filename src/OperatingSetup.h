#pragma once
#include "WeaponSimulation.h"
namespace reloadmaker {
struct OperatingSetup {
    QString recipeId,mode{"preset"},clip,baseline,fireMode{"preset"};
    SimulationParts parts;
    bool deriveHandle{false};
    float duration{.2f},ejectSeconds{-1};
    QHash<int,QString> movements;
};
bool readOperatingSetup(const QJsonObject&,OperatingSetup&,QString&);
QJsonObject operatingSetupJson(const OperatingSetup&);
}
