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
        bool singleHandle{false}; // Internal bolt has no separate visible mesh.
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
        QHash<int,QVector<Transformf>> grabCurves; // Closed-to-open, distance-driven, only while grabbed.
        QHash<int,QVector<Transformf>> nativeCurves; // Unchanged native tracks resampled to the independent operating clock.
        int motionFrames{0};
        float strokeSeconds{-1};
        QVector<float> nativeProgress; // Original recipe progress at each operating sample, including fractional endpoints.
        bool customSetup{false},manualCycle{false},fireFromStroke{false},presetFire{true};
        bool magazineBranchOnly{false}; // Explicit carrier inside a shared receiver model.
        bool hkSlap{false},lastShotHoldOpen{true},singleHandle{false},openBolt{false};
        float motionDuration{0},manualEjectSeconds{-1};
        QJsonObject authoring;
    };
    bool buildGrabDeployment(const ReloadRecipe&,SimulationRig&,double sourceSeconds,QString& error);
    Transformf simulationGrabPose(const SimulationRig&,int track,float progress);
    enum class SimulationAction { Idle, Fire, Reload, Held, Returning };
    struct WeaponSimulation {
        SimulationAction action{SimulationAction::Idle};
        MagazineMotion magazine;
        bool magazinePresent()const{return magazine.seated();}
        float slide{0}, seconds{0}, grabStart{0};
        WeaponMechanism mechanism{WeaponMechanism::PistolSlide};
        float handle{0}, shotSeconds{0}, fireCycleSeconds{0};
        QVector<float> fireCycleSchedule; // source event gaps, including the loop boundary
        float currentShotInterval() const { return fireCycleSchedule.isEmpty() ? fireCycleSeconds : fireCycleSchedule[_fireCycleIndex]; }
        bool triggerHeld{false};
        bool spentCase{false},boltClosing{false};
        int casesEjected{0};
        float boltEjectProgress{1};
        float inputProgress()const{return mechanism==WeaponMechanism::Ar15?handle:slide;}
        int capacity{0}, magazineRounds{0}, shotsFired{0}, roundsEjected{0};
        bool chambered{false}, slideLocked{false}, pistolLock{false};
        bool hkSlap{false},notchLatched{false},openBolt{false};
        bool reloadStartedLoaded{false}, reloadStartedOpen{false}, reloadCycled{false}, reloadFed{false};
        float reloadRackAt{-1}, reloadFeedAt{-1};
        bool ammoKnown()const{return capacity>0;}
        bool canFire()const{return ammoKnown()&&(openBolt?(magazine.seated()&&magazineRounds>0&&slideLocked):(chambered&&!slideLocked&&slide<=.001f))&&action==SimulationAction::Idle&&handle<=.001f&&!magazine.busy();}
        // Empty chamber with the action closed and at rest: the trigger only clicks.
        bool canDryFire()const{return action==SimulationAction::Idle&&inputProgress()<=.001f&&!magazine.busy()&&(openBolt?(!magazine.seated()||magazineRounds==0):(!chambered&&slide<=.001f));}
        bool willLockAfterShot()const{return !openBolt&&action==SimulationAction::Fire&&pistolLock&&magazine.seated()&&magazineRounds==0&&!chambered;}
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
        int _fireCycleIndex{0};
        struct PullSnapshot {bool chambered{false},locked{false},cycle{false};int rounds{0},ejected{0};float bolt{0},handle{0};bool spent{false},closing{false};int cases{0};bool notch{false};} _pullSnapshot;
    };
    bool buildSimulationRig(const ReloadRecipe& recipe, SimulationRig& rig, QString& error,WeaponMechanism profile=WeaponMechanism::PistolSlide,bool deriveHandleFromBolt=false,const QVector<Transformf>& boltOverride={},const SimulationParts* selected=nullptr);
    const QVector<Transformf>& simulationTrackCurve(const ReloadRecipe&,const SimulationRig&,int track);
    int simulationFrameCount(const ReloadRecipe&,const SimulationRig&);
    float simulationStrokeDuration(const ReloadRecipe&,const SimulationRig&);
    float simulationNativeFrame(const ReloadRecipe&,const SimulationRig&,float frame);
    Transformf simulationTrackPose(const ReloadRecipe& recipe, const SimulationRig& rig, int track, float progress);
    // Straight controller stroke in weapon space; visual curves remain intact.
    Vec3f simulationControllerStroke(const ReloadRecipe&,const SimulationRig&,const Vec3f& contact);
    // Whether the rig replaces the track's native curve (an authored stage, a
    // derived handle or bolt).
    [[nodiscard]] bool simulationTrackAuthored(const SimulationRig& rig, int track);
    // The track's native pose at the recipe frame simulationTrackPose uses
    // for the same progress.
    [[nodiscard]] Transformf simulationNativeTrackPose(const ReloadRecipe& recipe, const SimulationRig& rig, int track, float progress);
    QJsonObject simulationRigJson(const ReloadRecipe& recipe, const SimulationRig& rig, bool springReturn);
    // A full bolt or slide stroke moves at least this far (game units plus 3 x radians).
    constexpr float kCompleteStroke = .15f;
    // The largest distance of a track from its last pose over the recipe.
    float reloadTrackStroke(const ReloadRecipe& recipe, int track);
    // The magazine node that carries a WeaponMagazine track in the reload: the
    // topmost WeaponMagazine surface ancestor it stays fixed to for every
    // frame, else the track itself (any other track is returned unchanged). A magazine model mounted on the receiver's animated
    // magazine node (1911 drum and extended magazines) only rides that node.
    int magazineCarrier(const ReloadRecipe& recipe, int track);
    // The reload's one magazine carrier among its WeaponMagazine surfaces,
    // or -1 when it has none or several.
    int detectMagazineTrack(const ReloadRecipe& recipe);
    // The surface track bound to a native animation bone, or -1 when there is
    // none or more than one. The rig's default roles use these names.
    int surfaceTrack(const ReloadRecipe& recipe, const QString& anchorName);
    // Parts the bolt drives (rework 14, item 9): surface tracks outside the
    // magazine and the operating tracks that move only while the bolt is
    // away from rest (at least 80% of their displaced frames; for an AR-15,
    // at most 20% with the charging handle displaced) and whose distance from
    // rest follows the bolt's (correlation at least 0.7). A spare magazine
    // carried while the bolt is locked back moves with it but not like it.
    // Empty unless the rig is an AR-15 or a reciprocating bolt with a
    // complete bolt stroke.
    QVector<int> boltDrivenTracks(const ReloadRecipe& recipe, const SimulationRig& rig);
}
