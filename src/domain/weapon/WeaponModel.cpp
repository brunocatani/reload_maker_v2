#include "WeaponModel.h"

#include <QSet>

namespace reloadmaker
{
    namespace
    {
        bool canBeOpen(const Mechanism& mechanism)
        {
            return mechanism.roles.testFlag(CycleRole::OpenAccess) || mechanism.locksOpen ||
                   mechanism.returnKind == MechanismReturn::Stays;
        }

        void validateLoadingPoint(const WeaponModel& model, const LoadingPoint& point, QStringList& problems)
        {
            const QString name = QStringLiteral("Loading point \"%1\"").arg(point.id);
            if (point.capacity < 1) {
                problems << name + QStringLiteral(" has no capacity");
            }
            if (point.unit == UnitKind::Container &&
                (point.destination != LoadDestination::Container || point.capacity != 1)) {
                problems << name + QStringLiteral(" inserts a container somewhere other than one container slot");
            }
            if (point.unit == UnitKind::Clip && point.destination != LoadDestination::Container) {
                problems << name + QStringLiteral(" loads a clip without a container to strip it into");
            }
            if ((point.unit == UnitKind::BeltLead) != (point.destination == LoadDestination::FeedTray)) {
                problems << name + QStringLiteral(" mixes a belt lead and a feed tray with other kinds");
            }
            if (point.unit == UnitKind::BeltLead && model.feed.container != ContainerKind::Detachable) {
                problems << name + QStringLiteral(" lays a belt without a detachable belt box");
            }
            if (point.unit == UnitKind::Container && model.feed.container != ContainerKind::Detachable) {
                problems << name + QStringLiteral(" inserts a container into a weapon with a fixed or no feed");
            }
            if (!point.requiresOpen.isEmpty()) {
                const Mechanism* access = model.mechanism(point.requiresOpen);
                if (!access) {
                    problems << name + QStringLiteral(" waits for missing mechanism \"%1\"").arg(point.requiresOpen);
                } else if (!canBeOpen(*access)) {
                    problems << name + QStringLiteral(" waits for \"%1\", which never stays open").arg(access->id);
                }
            }
        }

        void validateFeed(const WeaponModel& model, QStringList& problems)
        {
            const Feed& feed = model.feed;
            if (feed.container == ContainerKind::None) {
                if (!feed.loadingPoints.isEmpty()) {
                    problems << QStringLiteral("A weapon without a feed has loading points");
                }
                return;
            }
            if (feed.loadingPoints.isEmpty()) {
                problems << QStringLiteral("The feed has no loading point");
            }
            QSet<QString> ids;
            int containerPoints = 0;
            for (const LoadingPoint& point : feed.loadingPoints) {
                if (point.id.isEmpty() || ids.contains(point.id)) {
                    problems << QStringLiteral("Loading point id \"%1\" is empty or repeated").arg(point.id);
                }
                ids.insert(point.id);
                containerPoints += point.unit == UnitKind::Container ? 1 : 0;
                validateLoadingPoint(model, point, problems);
            }
            if (feed.container == ContainerKind::Detachable && containerPoints != 1) {
                problems << QStringLiteral("A detachable feed needs exactly one container loading point");
            }
            if (feed.insertion == InsertionPath::NativeCurve && feed.container != ContainerKind::Detachable) {
                problems << QStringLiteral("Only a detachable container follows its clip's insertion curve");
            }
            if (feed.hands.isEmpty() || feed.hands.size() > 2 || (feed.hands.size() == 2 && feed.hands[0] == feed.hands[1])) {
                problems << QStringLiteral("The feed needs one or both hands, each once");
            }
        }

        // Follows drivenBy links between mechanisms; a loop means no part is
        // ever moved by the hand or a unit.
        bool drivenInLoop(const WeaponModel& model, const Mechanism& start)
        {
            QSet<QString> seen{ start.id };
            const Mechanism* current = &start;
            while (current->driver == MechanismDriver::Mechanism) {
                current = model.mechanism(current->drivenBy);
                if (!current) {
                    return false;
                }
                if (seen.contains(current->id)) {
                    return true;
                }
                seen.insert(current->id);
            }
            return false;
        }

        void validateMechanism(const WeaponModel& model, const Mechanism& mechanism, QStringList& problems)
        {
            const QString name = QStringLiteral("Mechanism \"%1\"").arg(mechanism.id);
            switch (mechanism.driver) {
            case MechanismDriver::Hand:
                if (!mechanism.drivenBy.isEmpty()) {
                    problems << name + QStringLiteral(" is moved by hand but names a driver");
                }
                if (mechanism.roles == CycleRoles()) {
                    problems << name + QStringLiteral(" is moved by hand but does nothing in the cycle");
                }
                break;
            case MechanismDriver::Mechanism:
                if (mechanism.drivenBy == mechanism.id || !model.mechanism(mechanism.drivenBy)) {
                    problems << name + QStringLiteral(" follows missing mechanism \"%1\"").arg(mechanism.drivenBy);
                } else if (drivenInLoop(model, mechanism)) {
                    problems << name + QStringLiteral(" is driven in a loop");
                }
                break;
            case MechanismDriver::Unit:
                if (!model.loadingPoint(mechanism.drivenBy)) {
                    problems << name + QStringLiteral(" follows missing loading point \"%1\"").arg(mechanism.drivenBy);
                }
                break;
            }
            if (mechanism.roles.testFlag(CycleRole::Index) && model.feed.container != ContainerKind::Fixed) {
                problems << name + QStringLiteral(" indexes a container that is not fixed");
            }
        }

        void validateMechanisms(const WeaponModel& model, QStringList& problems)
        {
            QSet<QString> ids;
            for (const Mechanism& mechanism : model.mechanisms) {
                if (mechanism.id.isEmpty() || ids.contains(mechanism.id)) {
                    problems << QStringLiteral("Mechanism id \"%1\" is empty or repeated").arg(mechanism.id);
                }
                ids.insert(mechanism.id);
                validateMechanism(model, mechanism, problems);
            }
        }

        void validateFire(const WeaponModel& model, QStringList& problems)
        {
            const Fire& fire = model.fire;
            const bool manual = fire.afterShot == AfterShot::ManualCycle;
            if (manual != !fire.manualCycleMechanism.isEmpty()) {
                problems << QStringLiteral("A manual cycle and its mechanism must be set together");
            }
            if (manual) {
                const Mechanism* cycle = model.mechanism(fire.manualCycleMechanism);
                if (!cycle) {
                    problems << QStringLiteral("The manual cycle uses missing mechanism \"%1\"").arg(fire.manualCycleMechanism);
                } else if (!cycle->roles.testAnyFlags(CycleRole::Cock | CycleRole::Chamber)) {
                    problems << QStringLiteral("The manual cycle mechanism \"%1\" neither cocks nor chambers").arg(cycle->id);
                }
            }
            if (!fire.lockOpenMechanism.isEmpty()) {
                const Mechanism* locked = model.mechanism(fire.lockOpenMechanism);
                if (!locked || !locked->locksOpen) {
                    problems << QStringLiteral("\"%1\" cannot be held open on empty").arg(fire.lockOpenMechanism);
                }
            }
            if (fire.ejection == CaseEjection::OnManualCycle && !manual) {
                problems << QStringLiteral("Cases eject on a manual cycle the weapon does not have");
            }
            if (fire.ejection == CaseEjection::OnOpening) {
                bool ejector = false;
                for (const Mechanism& mechanism : model.mechanisms) {
                    ejector |= mechanism.roles.testFlag(CycleRole::OpenAccess) &&
                               mechanism.roles.testAnyFlags(CycleRole::Extract | CycleRole::Eject);
                }
                if (!ejector) {
                    problems << QStringLiteral("Cases eject on opening but nothing opens and extracts");
                }
            }
        }
    }

    const Mechanism* WeaponModel::mechanism(const QString& id) const noexcept
    {
        for (const Mechanism& value : mechanisms) {
            if (value.id == id) {
                return &value;
            }
        }
        return nullptr;
    }

    const LoadingPoint* WeaponModel::loadingPoint(const QString& id) const noexcept
    {
        for (const LoadingPoint& value : feed.loadingPoints) {
            if (value.id == id) {
                return &value;
            }
        }
        return nullptr;
    }

    QString magazineReleaseName(MagazineRelease mode) {
        switch(mode){case MagazineRelease::Manual:return "manual";case MagazineRelease::Mechanism:return "mechanism";default:return "button";}
    }
    std::optional<MagazineRelease> magazineReleaseFromName(const QString& name) {
        for(auto mode:{MagazineRelease::Button,MagazineRelease::Manual,MagazineRelease::Mechanism})if(magazineReleaseName(mode)==name)return mode;
        return std::nullopt;
    }
    QString insertionPathName(InsertionPath path)
    {
        return path == InsertionPath::NativeCurve ? QStringLiteral("native-curve") : QStringLiteral("straight");
    }

    std::optional<InsertionPath> insertionPathFromName(const QString& name)
    {
        for (const auto path : { InsertionPath::Straight, InsertionPath::NativeCurve }) {
            if (name == insertionPathName(path)) {
                return path;
            }
        }
        return std::nullopt;
    }

    QStringList validateWeaponModel(const WeaponModel& model)
    {
        QStringList problems;
        validateFeed(model, problems);
        validateMechanisms(model, problems);
        validateFire(model, problems);
        return problems;
    }
}
