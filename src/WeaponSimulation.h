#pragma once
#include "ReloadRecipe.h"
#include "MagazineInteraction.h"

namespace reloadmaker {
    enum class WeaponMechanism { PistolSlide, Ar15 };
    // Indices belong to this exact captured recipe, never to scene blocks.
    struct SimulationRig {
        QString recipeId;
        int slide{-1}, magazine{-1}, openFrame{0}, closedFrame{0};
        QVector<int> slideTracks;
        QVector<int> magazineTracks;
        WeaponMechanism mechanism{WeaponMechanism::PistolSlide};
        int bolt{-1}; // AR bolt is independent of the input (charging-handle) track.
    };
    enum class SimulationAction { Idle, Fire, Reload, Held, Returning };
    struct WeaponSimulation {
        SimulationAction action{SimulationAction::Idle};
        MagazineMotion magazine;
        bool magazinePresent()const{return magazine.seated();}
        float slide{0}, seconds{0}, grabStart{0};
        WeaponMechanism mechanism{WeaponMechanism::PistolSlide};
        float handle{0}, shotSeconds{0}, fireCycleSeconds{0};
        bool triggerHeld{false};
        float inputProgress()const{return mechanism==WeaponMechanism::Ar15?handle:slide;}
        int capacity{0}, magazineRounds{0}, shotsFired{0}, roundsEjected{0};
        bool chambered{false}, slideLocked{false}, pistolLock{false};
        bool reloadStartedLoaded{false}, reloadStartedOpen{false}, reloadCycled{false}, reloadFed{false};
        float reloadRackAt{-1}, reloadFeedAt{-1};
        bool ammoKnown()const{return capacity>0;}
        bool canFire()const{return ammoKnown()&&chambered&&!slideLocked&&action==SimulationAction::Idle&&slide<=.001f&&handle<=.001f&&!magazine.busy();}
        bool willLockAfterShot()const{return action==SimulationAction::Fire&&pistolLock&&magazine.seated()&&magazineRounds==0&&!chambered;}
        bool feedCycleActive()const{return _cycleArmed;}
        void configureAmmunition(int magazineCapacity,bool lockPistol,WeaponMechanism profile=WeaponMechanism::PistolSlide);
        bool slideRelease();
        bool magazineOut();
        bool magazineIn(const MagazineRail& rail);
        bool grabMagazine(const MagazineRail& rail);
        void cancelMagazine();
        QString magazineDescription()const;
        bool fire();
        bool reload(float rackAt=-1, float feedAt=-1);
        bool dropMagazine();
        bool grab();
        void drag(float progress);
        void release(bool cancel, bool springReturn);
        // Returns true when a native action has finished and needs idle pose.
        bool tick(float delta, float duration);
        void reset();
    private:
        void finishClosing();
        bool _cycleArmed{false},_pullFromLock{false},_pulledRearward{false};
        struct PullSnapshot {bool chambered{false},locked{false},cycle{false};int rounds{0},ejected{0};float bolt{0},handle{0};} _pullSnapshot;
    };
    bool buildSimulationRig(const ReloadRecipe& recipe, SimulationRig& rig, QString& error,WeaponMechanism profile=WeaponMechanism::PistolSlide);
    Transformf simulationTrackPose(const ReloadRecipe& recipe, const SimulationRig& rig, int track, float progress);
    QJsonObject simulationRigJson(const ReloadRecipe& recipe, const SimulationRig& rig, bool springReturn);
}
