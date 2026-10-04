// The general weapon model must describe every weapon of the model probe
// (rework 12 §1) plus two existing families, and reject broken descriptions.
#include "domain/weapon/WeaponModel.h"

#include <QTextStream>
#include <functional>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void check(bool ok, const QString& why)
    {
        if (!ok) {
            throw std::runtime_error(why.toStdString());
        }
    }

    constexpr CycleRoles fullCycle = CycleRole::Extract | CycleRole::Eject | CycleRole::Cock | CycleRole::Chamber;

    LoadingPoint point(const char* id, UnitKind unit, LoadDestination destination, int capacity = 1,
                       const char* requiresOpen = "")
    {
        return { id, unit, destination, requiresOpen, capacity };
    }

    LoadingPoint magwell()
    {
        return point("magwell", UnitKind::Container, LoadDestination::Container);
    }

    Mechanism byHand(const char* id, MechanismKind kind, CycleRoles roles, MechanismReturn returnKind,
                     bool locksOpen = false)
    {
        return { id, kind, roles, returnKind, MechanismDriver::Hand, {}, locksOpen };
    }

    Mechanism driven(const char* id, MechanismKind kind, MechanismDriver driver, const char* by)
    {
        return { id, kind, {}, MechanismReturn::Spring, driver, by, false };
    }

    Fire manualFire(const char* mechanism, CaseEjection ejection = CaseEjection::OnManualCycle)
    {
        return { FireMotion::NativeClip, AfterShot::ManualCycle, ejection, mechanism, {} };
    }

    Fire selfLoading(CaseEjection ejection = CaseEjection::OnFire, const char* lockOpen = "")
    {
        return { FireMotion::NativeClip, AfterShot::AutomaticCycle, ejection, {}, lockOpen };
    }

    struct Described
    {
        const char* weapon;
        WeaponModel model;
    };

    QVector<Described> probeWeapons()
    {
        const auto stays = MechanismReturn::Stays;
        const auto spring = MechanismReturn::Spring;
        QVector<Described> weapons;
        weapons.append({ "Glock (slide, existing)",
            { { ContainerKind::Detachable, { magwell() } },
              { byHand("slide", MechanismKind::Slide, fullCycle, spring, true) },
              selfLoading(CaseEjection::OnFire, "slide") } });
        weapons.append({ "M1A (operating handle, existing)",
            { { ContainerKind::Detachable, { magwell() } },
              { byHand("handle", MechanismKind::OperatingHandle, fullCycle, spring, true) },
              selfLoading(CaseEjection::OnFire, "handle") } });
        weapons.append({ "Remington 870 (pump, shells into the tube)",
            { { ContainerKind::Fixed, { point("tube-port", UnitKind::Round, LoadDestination::Container, 6) } },
              { byHand("pump", MechanismKind::Pump, fullCycle, stays) },
              manualFire("pump") } });
        weapons.append({ "Benelli M2 (port load into the chamber, then the tube)",
            { { ContainerKind::Fixed,
                { point("ejection-port", UnitKind::Round, LoadDestination::Chamber, 1, "bolt"),
                  point("loading-port", UnitKind::Round, LoadDestination::Container, 7) } },
              { byHand("bolt", MechanismKind::ReciprocatingBolt, fullCycle, spring, true),
                driven("lifter", MechanismKind::Latch, MechanismDriver::Unit, "loading-port") },
              selfLoading(CaseEjection::OnFire, "bolt") } });
        weapons.append({ ".44 single action (crane, hammer indexes the cylinder)",
            { { ContainerKind::Fixed, { point("cylinder", UnitKind::Round, LoadDestination::Container, 6, "crane") } },
              { byHand("crane", MechanismKind::Crane, CycleRole::OpenAccess | CycleRole::Extract | CycleRole::Eject, stays),
                byHand("hammer", MechanismKind::Hammer, CycleRole::Cock | CycleRole::Index, stays),
                driven("cylinder", MechanismKind::Latch, MechanismDriver::Mechanism, "hammer") },
              manualFire("hammer", CaseEjection::OnOpening) } });
        weapons.append({ ".44 double action (no manual cock)",
            { { ContainerKind::Fixed, { point("cylinder", UnitKind::Round, LoadDestination::Container, 6, "crane") } },
              { byHand("crane", MechanismKind::Crane, CycleRole::OpenAccess | CycleRole::Extract | CycleRole::Eject, stays) },
              selfLoading(CaseEjection::OnOpening) } });
        weapons.append({ "Double-barrel (hinge, two chambers)",
            { { ContainerKind::Fixed, { point("chambers", UnitKind::Round, LoadDestination::Chamber, 2, "hinge") } },
              { byHand("hinge", MechanismKind::Hinge, CycleRole::OpenAccess | CycleRole::Extract, stays) },
              selfLoading(CaseEjection::OnOpening) } });
        weapons.append({ "Lever-action (loading gate moved by each round)",
            { { ContainerKind::Fixed, { point("loading-gate", UnitKind::Round, LoadDestination::Container, 10) } },
              { byHand("lever", MechanismKind::Lever, fullCycle, stays),
                driven("hammer", MechanismKind::Hammer, MechanismDriver::Mechanism, "lever"),
                driven("gate", MechanismKind::Latch, MechanismDriver::Unit, "loading-gate") },
              manualFire("lever") } });
        weapons.append({ "Lee Enfield (single rounds through the open action, owner rule)",
            { { ContainerKind::Fixed, { point("action", UnitKind::Round, LoadDestination::Container, 10, "bolt") } },
              { byHand("bolt", MechanismKind::TurnBolt, fullCycle, stays) },
              manualFire("bolt") } });
        weapons.append({ "MP5 (HK slap, notch holds the handle)",
            { { ContainerKind::Detachable, { magwell() } },
              { byHand("handle", MechanismKind::HkSlap, fullCycle, spring, true),
                driven("bolt", MechanismKind::ReciprocatingBolt, MechanismDriver::Mechanism, "handle") },
              selfLoading() } });
        weapons.append({ "AUG (bullpup magazine, HK slap by owner)",
            { { ContainerKind::Detachable, { point("behind-grip", UnitKind::Container, LoadDestination::Container) } },
              { byHand("handle", MechanismKind::HkSlap, fullCycle, spring, true) },
              selfLoading() } });
        weapons.append({ "AEP-7 (energy cell behind a latch, no cases)",
            { { ContainerKind::Detachable, { point("cell-slot", UnitKind::Container, LoadDestination::Container, 1, "latch") } },
              { byHand("latch", MechanismKind::Latch, CycleRole::OpenAccess, stays) },
              selfLoading(CaseEjection::None) } });
        weapons.append({ "PPSh-41 (open bolt, drum or stick)",
            { { ContainerKind::Detachable, { magwell() } },
              { byHand("bolt", MechanismKind::ReciprocatingBolt, CycleRole::Cock, spring, true) },
              { FireMotion::NativeClip, AfterShot::OpenBolt, CaseEjection::OnFire, {}, {} } } });
        weapons.append({ "P90 (top magazine)",
            { { ContainerKind::Detachable, { point("top-magwell", UnitKind::Container, LoadDestination::Container) } },
              { byHand("handle", MechanismKind::ChargingHandle, fullCycle, spring) },
              selfLoading() } });
        weapons.append({ "MG42 (belt box, lead under the top cover)",
            { { ContainerKind::Detachable,
                { point("box", UnitKind::Container, LoadDestination::Container),
                  point("tray", UnitKind::BeltLead, LoadDestination::FeedTray, 1, "cover") } },
              { byHand("cover", MechanismKind::Cover, CycleRole::OpenAccess, stays),
                byHand("handle", MechanismKind::ChargingHandle, CycleRole::Cock, spring, true) },
              { FireMotion::NativeClip, AfterShot::OpenBolt, CaseEjection::OnFire, {}, {} } } });
        return weapons;
    }

    void describesProbeWeapons()
    {
        for (const Described& described : probeWeapons()) {
            const QStringList problems = validateWeaponModel(described.model);
            check(problems.isEmpty(), QStringLiteral("%1: %2").arg(described.weapon, problems.join("; ")));
        }
    }

    // Each broken description must be refused with a problem naming the cause.
    void rejects(const char* weapon, const std::function<void(WeaponModel&)>& breakIt, const char* expected)
    {
        for (const Described& described : probeWeapons()) {
            if (QLatin1String(described.weapon).startsWith(QLatin1String(weapon))) {
                WeaponModel model = described.model;
                breakIt(model);
                const QStringList problems = validateWeaponModel(model);
                check(problems.join('\n').contains(QLatin1String(expected)),
                      QStringLiteral("%1 was not refused for \"%2\": %3").arg(weapon, expected, problems.join("; ")));
                return;
            }
        }
        throw std::runtime_error(std::string("No probe weapon named ") + weapon);
    }

    void refusesBrokenModels()
    {
        rejects("Glock", [](WeaponModel& m) { m.feed.loadingPoints.append(magwell()); }, "is empty or repeated");
        rejects("Glock", [](WeaponModel& m) { m.feed.loadingPoints[0].id = "second"; m.feed.loadingPoints.append(magwell()); },
                "exactly one container loading point");
        rejects("Remington", [](WeaponModel& m) { m.feed.loadingPoints.append(magwell()); }, "fixed or no feed");
        rejects("Remington", [](WeaponModel& m) { m.feed.loadingPoints[0].capacity = 0; }, "has no capacity");
        rejects("Remington", [](WeaponModel& m) { m.fire.manualCycleMechanism.clear(); }, "must be set together");
        rejects("Remington", [](WeaponModel& m) { m.mechanisms[0].roles = CycleRole::OpenAccess; }, "neither cocks nor chambers");
        rejects("Benelli", [](WeaponModel& m) { m.mechanisms[0].locksOpen = false; }, "never stays open");
        rejects("Benelli", [](WeaponModel& m) { m.mechanisms[1].drivenBy = "missing-port"; }, "missing loading point");
        rejects("Benelli", [](WeaponModel& m) { m.fire.lockOpenMechanism = "lifter"; }, "cannot be held open");
        rejects(".44 single", [](WeaponModel& m) { m.feed.container = ContainerKind::Detachable; }, "indexes a container that is not fixed");
        rejects(".44 single", [](WeaponModel& m) { m.feed.loadingPoints[0].requiresOpen = "missing"; }, "missing mechanism");
        rejects("Double-barrel", [](WeaponModel& m) { m.mechanisms[0].roles = CycleRole::OpenAccess; }, "nothing opens and extracts");
        rejects("Lever", [](WeaponModel& m) { m.mechanisms[0].driver = MechanismDriver::Mechanism; m.mechanisms[0].drivenBy = "hammer"; },
                "driven in a loop");
        rejects("Lever", [](WeaponModel& m) { m.mechanisms[1].drivenBy = "hammer"; }, "follows missing mechanism");
        rejects("MP5", [](WeaponModel& m) { m.mechanisms[0].roles = {}; }, "does nothing in the cycle");
        rejects("MP5", [](WeaponModel& m) { m.fire.ejection = CaseEjection::OnManualCycle; }, "manual cycle the weapon does not have");
        rejects("AEP-7", [](WeaponModel& m) { m.feed.container = ContainerKind::None; }, "without a feed has loading points");
        rejects("MG42", [](WeaponModel& m) { m.feed.loadingPoints[1].destination = LoadDestination::Chamber; },
                "mixes a belt lead and a feed tray");
        rejects("MG42", [](WeaponModel& m) { m.feed.loadingPoints[1].unit = UnitKind::Clip; }, "mixes a belt lead and a feed tray");
        rejects("Lee Enfield", [](WeaponModel& m) { m.feed.loadingPoints[0].unit = UnitKind::Clip;
                                                      m.feed.loadingPoints[0].destination = LoadDestination::Chamber; },
                "clip without a container");
    }
}

int main()
{
    try {
        describesProbeWeapons();
        refusesBrokenModels();
        QTextStream(stdout) << "WeaponModel tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
