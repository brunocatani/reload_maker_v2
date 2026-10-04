#pragma once

#include <QFlags>
#include <QString>
#include <QStringList>
#include <QVector>

namespace reloadmaker
{
    // The general weapon model (rework 10 §3, amended by the model probe in
    // 12 §3): a feed with ordered loading points, mechanisms with roles in the
    // cycle, and per-shot fire behavior. Families such as "pump shotgun" or
    // "revolver" are presets over this model, never code paths.
    //
    // Part roles come from presets and detection. Nothing here maps a bone name
    // to a role: the probe found WeaponMagazine acting as a revolver crane, a
    // barrel hinge and a belt box, and WeaponOptics2 as a belt cover.

    enum class ContainerKind
    {
        None,       // recharge or no-reload weapons
        Detachable, // magazines, drums, energy cells, belt boxes
        Fixed,      // tubes, internal boxes, cylinders, chambers
    };

    enum class UnitKind
    {
        Container,  // the detachable container itself
        Round,      // one cartridge or shell
        Clip,       // a stripper or en-bloc clip carrying several rounds
        Projectile, // loaded at the muzzle or tube end
        BeltLead,   // the free end of a belt, laid into the feed tray
    };

    // Rounds always load into the weapon's regular container (tube, box,
    // cylinder, a double-barrel's chambers); the action then chambers them by
    // releasing a held-open bolt or by pulling a closed one. There is no
    // straight-to-chamber loading point even when a source clip shows one,
    // such as the Benelli M2's ejection-port load (owner, 2026-10-04).
    enum class LoadDestination
    {
        Container, // the magazine, tube, box, cylinder or barrel chambers
        FeedTray,  // belt-fed weapons, under the top cover
    };

    enum class MechanismKind
    {
        // DIAMOND runtime exists.
        Slide,
        ChargingHandle,
        OperatingHandle,
        ReciprocatingBolt,
        TurnBolt,
        // Planned families (P8), built when a roster weapon needs them.
        HkSlap,
        Pump,
        Lever,
        Hinge,
        Hammer,
        Crane,
        Cover,
        Latch,
    };

    enum class CycleRole
    {
        Extract = 0x01,
        Eject = 0x02,
        Cock = 0x04,
        Chamber = 0x08,
        OpenAccess = 0x10,   // opens the way for loading: crane, hinge, cover, cell latch
        ReleaseCatch = 0x20, // releases a held-open part
        Index = 0x40,        // advances a fixed container by one unit (revolver cylinder)
    };
    Q_DECLARE_FLAGS(CycleRoles, CycleRole)

    enum class MechanismReturn
    {
        Spring,
        Stays,
    };

    enum class MechanismDriver
    {
        Hand,
        Mechanism, // a follower or driven part of another mechanism
        Unit,      // moved by a passing unit (lever-action loading gate)
    };

    struct Mechanism
    {
        QString id; // unique within the weapon, e.g. "bolt", "crane", "cover"
        MechanismKind kind{ MechanismKind::Slide };
        CycleRoles roles;
        MechanismReturn returnKind{ MechanismReturn::Spring };
        MechanismDriver driver{ MechanismDriver::Hand };
        QString drivenBy; // a mechanism id or loading point id; empty for Hand
        bool locksOpen{ false }; // a catch or notch can hold it open
    };

    struct LoadingPoint
    {
        QString id; // unique within the weapon, e.g. "magwell", "tube-port", "ejection-port"
        UnitKind unit{ UnitKind::Container };
        LoadDestination destination{ LoadDestination::Container };
        QString requiresOpen; // mechanism that must be open; empty when always reachable
        int capacity{ 1 };    // units this point fills: rounds in a tube, chambers in a barrel set
    };

    struct Feed
    {
        ContainerKind container{ ContainerKind::None };
        QVector<LoadingPoint> loadingPoints; // in loading order
    };

    enum class FireMotion
    {
        NativeClip,
        ReplayActionStroke,
        Nothing,
    };

    // Decided from the weapon record; WPNBoltChargeReady is evidence and motion
    // source only (12 §2.8: the automatic PPSh-41 ships one).
    enum class AfterShot
    {
        AutomaticCycle,
        ManualCycle,
        OpenBolt,
    };

    enum class CaseEjection
    {
        OnFire,
        OnManualCycle,
        OnOpening, // break-action, revolver ejector
        None,      // caseless, energy
    };

    struct Fire
    {
        FireMotion motion{ FireMotion::NativeClip };
        AfterShot afterShot{ AfterShot::AutomaticCycle };
        CaseEjection ejection{ CaseEjection::OnFire };
        QString manualCycleMechanism; // required for ManualCycle
        QString lockOpenMechanism;    // held open on empty; empty when nothing locks
    };

    struct WeaponModel
    {
        Feed feed;
        QVector<Mechanism> mechanisms;
        Fire fire;

        [[nodiscard]] const Mechanism* mechanism(const QString& id) const noexcept;
        [[nodiscard]] const LoadingPoint* loadingPoint(const QString& id) const noexcept;
    };

    // Every broken invariant, one line each; empty when the model is consistent.
    [[nodiscard]] QStringList validateWeaponModel(const WeaponModel& model);
}

Q_DECLARE_OPERATORS_FOR_FLAGS(reloadmaker::CycleRoles)
