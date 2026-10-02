#pragma once
#include "ReloadRecipe.h"

namespace reloadmaker {
struct BoltCycle {
    float duration{0};
    QVector<int> tracks; // Indices in the current reload recipe's scene binding.
    QVector<QVector<Transformf>> poses;
    int first{0},rear{0},last{0};
    QVector<float> opening,closing; // Normalized source-motion progress.
    float ejectProgress{0},inputTravel{0};
    Vec3f inputAxis{};
};
bool analyzeBoltCycle(BoltCycle& cycle,float ejectSeconds,QString& error,bool ejectAtRear=false);
Transformf boltCyclePose(const BoltCycle& cycle,int track,float progress,bool closing);
}
