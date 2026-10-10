// The general weapon model must describe every weapon of the model probe
// (rework 12 §1) plus two existing families, and reject broken descriptions.
// The bundled presets must load and each be one DIAMOND behavior; any other
// combination must map to a preset's behavior or be refused with a reason.
#include "domain/presets/WeaponPresets.h"
#include "domain/weapon/WeaponModel.h"
#include "domain/weapon/WeaponOverrides.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
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
        weapons.append({ "Benelli M2 (every shell into the tube, the bolt chambers)",
            { { ContainerKind::Fixed, { point("loading-port", UnitKind::Round, LoadDestination::Container, 7) } },
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
            { { ContainerKind::Fixed, { point("chambers", UnitKind::Round, LoadDestination::Container, 2, "hinge") } },
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
        rejects(".44 single", [](WeaponModel& m) { m.mechanisms[0].roles = CycleRole::Extract | CycleRole::Eject;
                                                     m.mechanisms[0].returnKind = MechanismReturn::Spring; },
                "never stays open");
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
        rejects("MG42", [](WeaponModel& m) { m.feed.loadingPoints[1].destination = LoadDestination::Container; },
                "mixes a belt lead and a feed tray");
        rejects("MG42", [](WeaponModel& m) { m.feed.loadingPoints[1].unit = UnitKind::Clip; }, "mixes a belt lead and a feed tray");
        rejects("Lee Enfield", [](WeaponModel& m) { m.feed.loadingPoints[0].unit = UnitKind::Clip;
                                                      m.feed.loadingPoints[0].destination = LoadDestination::FeedTray; },
                "clip without a container");
    }

    // DIAMOND accepts exactly these template ids (ContentCatalog.cpp mechanism()).
    const QSet<QString> diamondTemplates{ "pistol.slide-detachable.v1", "rifle.ar15-detachable.v1",
                                          "rifle.operating-handle-rocking.v1", "rifle.reciprocating-detachable.v1",
                                          "sniper.manual-bolt-detachable.v1", "rifle.ar15-stroke-detachable.v1",
                                          "rifle.aug-locking-handle.v1", "rifle.hk-slap-catch-detachable.v1",
                                          "rifle.hk-slap-detachable.v1", "rifle.independent-handle-rocking.v1",
                                          "rifle.open-bolt-handle-detachable.v1", "rifle.reciprocating-rocking.v1",
                                          "rifle.rotary-handle-detachable.v1", "shotgun.self-loading-detachable.v1" };

    QVector<WeaponPreset> bundledPresets(const QString& directory)
    {
        QVector<WeaponPreset> presets;
        QString error;
        check(loadWeaponPresets(directory, presets, error), error);
        return presets;
    }

    void loadsBundledPresets(const QString& directory)
    {
        const QVector<WeaponPreset> presets = bundledPresets(directory);
        QSet<QString> ids;
        for (const WeaponPreset& preset : presets) {
            ids.insert(preset.id);
            check(diamondSupport(preset.model).action.has_value(), preset.id + " is not a DIAMOND behavior");
        }
        check(ids == diamondTemplates, "Bundled presets differ from the template ids DIAMOND accepts");
    }

    // "Fully custom": every combination either maps to the behavior of one
    // bundled preset, so a template id exists, or is refused with a reason.
    void mapsOrRefusesEveryCombination(const QString& directory)
    {
        QSet<int> presetActions;
        for (const WeaponPreset& preset : bundledPresets(directory)) {
            presetActions.insert(static_cast<int>(*diamondSupport(preset.model).action));
        }
        int supported = 0;
        for (const ContainerKind container : { ContainerKind::None, ContainerKind::Detachable, ContainerKind::Fixed }) {
            for (int kind = 0; kind <= static_cast<int>(MechanismKind::Latch); ++kind) {
                for (const AfterShot afterShot : { AfterShot::AutomaticCycle, AfterShot::ManualCycle, AfterShot::OpenBolt }) {
                    WeaponModel model;
                    model.feed.container = container;
                    if (container == ContainerKind::Detachable) {
                        model.feed.loadingPoints = { magwell() };
                    } else if (container == ContainerKind::Fixed) {
                        model.feed.loadingPoints = { point("port", UnitKind::Round, LoadDestination::Container, 5) };
                    }
                    model.mechanisms = { byHand("action", static_cast<MechanismKind>(kind), fullCycle, MechanismReturn::Stays) };
                    model.fire = afterShot == AfterShot::ManualCycle ? manualFire("action")
                               : Fire{ FireMotion::NativeClip, afterShot, CaseEjection::OnFire, {}, {} };
                    const DiamondSupport support = diamondSupport(model);
                    check(support.action.has_value() != !support.refusal.isEmpty(), "A combination was both mapped and refused");
                    if (support.action) {
                        check(presetActions.contains(static_cast<int>(*support.action)), "A combination mapped to no bundled preset");
                        ++supported;
                    }
                }
            }
        }
        check(supported == 7, QStringLiteral("Expected seven supported single-mechanism combinations, got %1").arg(supported));
    }

    void probeWeaponsMapToDiamondOrWait()
    {
        const QSet<QString> runnable{ "Glock", "M1A", "P90", "MP5", "AUG" };
        for (const Described& described : probeWeapons()) {
            const QString name = QString::fromLatin1(described.weapon).section(' ', 0, 0);
            const DiamondSupport support = diamondSupport(described.model);
            check(support.action.has_value() == runnable.contains(name),
                  QStringLiteral("%1: DIAMOND support %2 (%3)").arg(described.weapon).arg(support.action.has_value()).arg(support.refusal));
        }
    }

    void refusesBrokenPresetFiles(const QString& directory)
    {
        QFile source(QDir(directory).filePath("pistol.slide-detachable.v1.json"));
        check(source.open(QIODevice::ReadOnly), "Missing pistol preset");
        const QByteArray pistol = source.readAll();
        WeaponPreset preset;
        QString error;
        check(decodeWeaponPreset(pistol, preset, error), error);
        QByteArray future = pistol;
        future.replace("\"format\": 1", "\"format\": 2");
        check(!decodeWeaponPreset(future, preset, error), "Unknown preset format accepted");
        QByteArray unknownKind = pistol;
        unknownKind.replace("\"kind\": \"slide\"", "\"kind\": \"laser\"");
        check(!decodeWeaponPreset(unknownKind, preset, error), "Unknown mechanism kind accepted");
        QByteArray inconsistent = pistol;
        inconsistent.replace("\"lockOpen\": \"slide\"", "\"lockOpen\": \"magwell\"");
        check(!decodeWeaponPreset(inconsistent, preset, error) && error.contains("cannot be held open"),
              "Inconsistent preset accepted: " + error);

        QTemporaryDir twice;
        check(twice.isValid(), "No temporary folder");
        for (const char* name : { "a.json", "b.json" }) {
            QFile copy(twice.filePath(name));
            check(copy.open(QIODevice::WriteOnly) && copy.write(pistol) == pistol.size(), "Cannot write preset copy");
        }
        QVector<WeaponPreset> presets;
        check(!loadWeaponPresets(twice.path(), presets, error), "Two presets with one id and behavior accepted");

        QByteArray unknownInsertion = pistol;
        unknownInsertion.replace("\"container\": \"detachable\",", "\"container\": \"detachable\", \"insertion\": \"sideways\",");
        check(unknownInsertion != pistol && !decodeWeaponPreset(unknownInsertion, preset, error), "Unknown insertion path accepted");
    }

    // Presets retain their explicit visual insertion choice; controller
    // input remains independent. Weapons may override it.
    void insertionPathsAndOverrides(const QString& directory)
    {
        QVector<WeaponPreset> presets;
        QString error;
        check(loadWeaponPresets(directory, presets, error), error);
        const QSet<QString> curved{ "rifle.operating-handle-rocking.v1", "sniper.manual-bolt-detachable.v1",
            "rifle.ar15-stroke-detachable.v1", "rifle.aug-locking-handle.v1", "rifle.hk-slap-catch-detachable.v1",
            "rifle.hk-slap-detachable.v1", "rifle.independent-handle-rocking.v1", "rifle.open-bolt-handle-detachable.v1",
            "rifle.reciprocating-rocking.v1", "rifle.rotary-handle-detachable.v1", "shotgun.self-loading-detachable.v1" };
        for (const WeaponPreset& preset : presets) {
            check((preset.model.feed.insertion == InsertionPath::NativeCurve) == curved.contains(preset.id), preset.id + ": insertion path");
        }
        // Either physical hand may load a magazine; the rocking-handle rifle defaults right.
        for (const WeaponPreset& preset : presets) {
            const QVector<Hand> expected = preset.id == QLatin1String("rifle.operating-handle-rocking.v1") ? QVector<Hand>{ Hand::Right, Hand::Left }
                                                                                                              : QVector<Hand>{ Hand::Left, Hand::Right };
            check(preset.model.feed.hands == expected, preset.id + ": loading hands");
        }
        WeaponModel twice = presets.first().model;
        twice.feed.hands = { Hand::Left, Hand::Left };
        check(validateWeaponModel(twice).join(' ').contains("each once"), "A repeated loading hand accepted");
        twice.feed.hands.clear();
        check(!validateWeaponModel(twice).isEmpty(), "A feed without hands accepted");

        WeaponModel fixed = presets.first().model;
        fixed.feed.container = ContainerKind::Fixed;
        fixed.feed.insertion = InsertionPath::NativeCurve;
        check(validateWeaponModel(fixed).join(' ').contains("insertion curve"), "A fixed feed accepted a clip insertion curve");

        WeaponOverrides overrides;
        check(readWeaponOverrides({ { "insertion", "native-curve" } }, overrides, error) && overrides.insertion == InsertionPath::NativeCurve, error);
        check(weaponOverridesJson(overrides) == QJsonObject{ { "insertion", "native-curve" } }, "Overrides did not round-trip");
        check(!readWeaponOverrides({ { "insertion", "diagonal" } }, overrides, error), "Unknown insertion override accepted");
        check(!readWeaponOverrides({ { "magazineColor", "red" } }, overrides, error), "Unknown weapon property accepted");
        check(readWeaponOverrides({}, overrides, error) && overrides.empty() && weaponOverridesJson(overrides).isEmpty(), "Empty overrides");
    }
}

int main(int argc, char** argv)
{
    try {
        if (argc < 2) {
            throw std::runtime_error("usage: WeaponModelTests <assets/presets>");
        }
        const QString presets = QString::fromLocal8Bit(argv[1]);
        describesProbeWeapons();
        refusesBrokenModels();
        loadsBundledPresets(presets);
        mapsOrRefusesEveryCombination(presets);
        probeWeaponsMapToDiamondOrWait();
        refusesBrokenPresetFiles(presets);
        insertionPathsAndOverrides(presets);
        QTextStream(stdout) << "WeaponModel tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        QTextStream(stderr) << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
