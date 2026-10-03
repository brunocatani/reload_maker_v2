#pragma once
#include "ReloadRecipe.h"
#include "MagazineInteraction.h"
#include <QHash>

namespace reloadmaker {
    enum class WeaponMechanism { PistolSlide, Ar15, M1A, BoltAction, ReciprocatingBolt };
    struct SimulationParts {
        int operating{-1},bolt{-1},magazine{-1};
        QVector<int> followers;
        int openFrame{-1},closedFrame{-1};
        QHash<int,QVector<Transformf>> curves;
    };
    // Indices belong to this exact captured recipe, never to scene blocks.
    struct SimulationRig {
        QString recipeId;
        int slide{-1}, magazine{-1}, openFrame{0}, closedFrame{0};
        QVector<int> slideTracks;
        QVector<int> magazineTracks;
        WeaponMechanism mechanism{WeaponMechanism::PistolSlide};
        int bolt{-1}; // AR bolt is independent of the input (charging-handle) track.
        QVector<Transformf> derivedHandle; // Explicit static-handle repair from native bolt translation.
        QVector<Transformf> derivedBolt;
        QJsonObject mechanicalSource;
        QHash<int,QVector<Transformf>> authoredCurves;
        bool customSetup{false},manualCycle{false},fireFromStroke{false},presetFire{true};
        bool magazineBranchOnly{false}; // Explicit carrier inside a shared receiver model.
        float motionDuration{0},manualEjectSeconds{-1};
        QJsonObject authoring;
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
        bool spentCase{false},boltClosing{false};
        int casesEjected{0};
        float boltEjectProgress{1};
        float inputProgress()const{return mechanism==WeaponMechanism::Ar15?handle:slide;}
        int capacity{0}, magazineRounds{0}, shotsFired{0}, roundsEjected{0};
        bool chambered{false}, slideLocked{false}, pistolLock{false};
        bool reloadStartedLoaded{false}, reloadStartedOpen{false}, reloadCycled{false}, reloadFed{false};
        float reloadRackAt{-1}, reloadFeedAt{-1};
        bool ammoKnown()const{return capacity>0;}
        bool canFire()const{return ammoKnown()&&chambered&&!slideLocked&&action==SimulationAction::Idle&&slide<=.001f&&handle<=.001f&&!magazine.busy();}
        // Empty chamber with the action closed and at rest: the trigger only clicks.
        bool canDryFire()const{return !chambered&&action==SimulationAction::Idle&&inputProgress()<=.001f&&slide<=.001f&&!magazine.busy();}
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
        struct PullSnapshot {bool chambered{false},locked{false},cycle{false};int rounds{0},ejected{0};float bolt{0},handle{0};bool spent{false},closing{false};int cases{0};} _pullSnapshot;
    };
    bool buildSimulationRig(const ReloadRecipe& recipe, SimulationRig& rig, QString& error,WeaponMechanism profile=WeaponMechanism::PistolSlide,bool deriveHandleFromBolt=false,const QVector<Transformf>& boltOverride={},const SimulationParts* selected=nullptr);
    const QVector<Transformf>& simulationTrackCurve(const ReloadRecipe&,const SimulationRig&,int track);
    Transformf simulationTrackPose(const ReloadRecipe& recipe, const SimulationRig& rig, int track, float progress);
    QJsonObject simulationRigJson(const ReloadRecipe& recipe, const SimulationRig& rig, bool springReturn);
}
