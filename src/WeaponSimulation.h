#pragma once
#include "ReloadRecipe.h"
#include "MagazineInteraction.h"

namespace reloadmaker {
    // Indices belong to this exact captured recipe, never to scene blocks.
    struct SimulationRig {
        QString recipeId;
        int slide{-1}, magazine{-1}, openFrame{0}, closedFrame{0};
        QVector<int> slideTracks;
        QVector<int> magazineTracks;
    };
    enum class SimulationAction { Idle, Fire, Reload, Held, Returning };
    struct WeaponSimulation {
        SimulationAction action{SimulationAction::Idle};
        MagazineMotion magazine;
        bool magazinePresent()const{return magazine.seated();}
        float slide{0}, seconds{0}, grabStart{0};
        bool fire();
        bool reload();
        bool dropMagazine();
        bool grab();
        void drag(float progress);
        void release(bool cancel, bool springReturn);
        // Returns true when a native action has finished and needs idle pose.
        bool tick(float delta, float duration);
        void reset();
    };
    bool buildSimulationRig(const ReloadRecipe& recipe, SimulationRig& rig, QString& error);
    Transformf simulationTrackPose(const ReloadRecipe& recipe, const SimulationRig& rig, int track, float progress);
    QJsonObject simulationRigJson(const ReloadRecipe& recipe, const SimulationRig& rig, bool springReturn);
}
