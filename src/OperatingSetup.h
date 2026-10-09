#pragma once
#include "WeaponSimulation.h"
namespace reloadmaker {
struct OperatingSetup {
    QString recipeId,mode{"preset"},clip,baseline,fireMode{"preset"},cycleMode{"preset"},catchMode{"preset"};
    SimulationParts parts;
    bool deriveHandle{false};
    float duration{.2f},ejectSeconds{-1};
    bool sourceTimes{false}; // Version 2 native windows; manual movements keep their existing sampling.
    double openSeconds{-1},closedSeconds{-1};
    QHash<int,QString> movements;
    QJsonArray trackAnchors; // The node of each track index (recipeTrackAnchors).
};
bool readOperatingSetup(const QJsonObject&,OperatingSetup&,QString&);
bool remapOperatingSetup(OperatingSetup&,const ReloadRecipe&,QString&);
QJsonObject operatingSetupJson(const OperatingSetup&);
bool operatingTimeSamples(const OperatingSetup&,float duration,int frames,SimulationParts&,QVector<float>&,QString&);
}
