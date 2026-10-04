#include "WeaponPresets.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <utility>

namespace reloadmaker
{
    namespace
    {
        constexpr int kPresetFormat = 1;
        constexpr qint64 kPresetSizeLimit = 64 * 1024;
        constexpr int kPresetCountLimit = 64;
        constexpr int kListLimit = 32;

        template <class Enum, std::size_t Count>
        bool parseName(const QJsonValue& value, const std::pair<const char*, Enum> (&names)[Count], Enum& result)
        {
            const QString text = value.toString();
            for (const auto& [name, candidate] : names) {
                if (text == QLatin1String(name)) {
                    result = candidate;
                    return true;
                }
            }
            return false;
        }

        constexpr std::pair<const char*, ContainerKind> containerNames[] = {
            { "none", ContainerKind::None }, { "detachable", ContainerKind::Detachable }, { "fixed", ContainerKind::Fixed } };
        constexpr std::pair<const char*, UnitKind> unitNames[] = {
            { "container", UnitKind::Container }, { "round", UnitKind::Round }, { "clip", UnitKind::Clip },
            { "projectile", UnitKind::Projectile }, { "belt-lead", UnitKind::BeltLead } };
        constexpr std::pair<const char*, LoadDestination> destinationNames[] = {
            { "container", LoadDestination::Container }, { "chamber", LoadDestination::Chamber },
            { "feed-tray", LoadDestination::FeedTray } };
        constexpr std::pair<const char*, MechanismKind> kindNames[] = {
            { "slide", MechanismKind::Slide }, { "charging-handle", MechanismKind::ChargingHandle },
            { "operating-handle", MechanismKind::OperatingHandle }, { "reciprocating-bolt", MechanismKind::ReciprocatingBolt },
            { "turn-bolt", MechanismKind::TurnBolt }, { "hk-slap", MechanismKind::HkSlap }, { "pump", MechanismKind::Pump },
            { "lever", MechanismKind::Lever }, { "hinge", MechanismKind::Hinge }, { "hammer", MechanismKind::Hammer },
            { "crane", MechanismKind::Crane }, { "cover", MechanismKind::Cover }, { "latch", MechanismKind::Latch } };
        constexpr std::pair<const char*, CycleRole> roleNames[] = {
            { "extract", CycleRole::Extract }, { "eject", CycleRole::Eject }, { "cock", CycleRole::Cock },
            { "chamber", CycleRole::Chamber }, { "open-access", CycleRole::OpenAccess },
            { "release-catch", CycleRole::ReleaseCatch }, { "index", CycleRole::Index } };
        constexpr std::pair<const char*, MechanismReturn> returnNames[] = {
            { "spring", MechanismReturn::Spring }, { "stays", MechanismReturn::Stays } };
        constexpr std::pair<const char*, MechanismDriver> driverNames[] = {
            { "hand", MechanismDriver::Hand }, { "mechanism", MechanismDriver::Mechanism }, { "unit", MechanismDriver::Unit } };
        constexpr std::pair<const char*, FireMotion> motionNames[] = {
            { "native-clip", FireMotion::NativeClip }, { "replay-action-stroke", FireMotion::ReplayActionStroke },
            { "nothing", FireMotion::Nothing } };
        constexpr std::pair<const char*, AfterShot> afterShotNames[] = {
            { "automatic-cycle", AfterShot::AutomaticCycle }, { "manual-cycle", AfterShot::ManualCycle },
            { "open-bolt", AfterShot::OpenBolt } };
        constexpr std::pair<const char*, CaseEjection> ejectionNames[] = {
            { "on-fire", CaseEjection::OnFire }, { "on-manual-cycle", CaseEjection::OnManualCycle },
            { "on-opening", CaseEjection::OnOpening }, { "none", CaseEjection::None } };

        bool decodeLoadingPoint(const QJsonObject& json, LoadingPoint& point)
        {
            point.id = json["id"].toString();
            point.requiresOpen = json["requiresOpen"].toString();
            point.capacity = json["capacity"].toInt(1);
            return parseName(json["unit"], unitNames, point.unit) &&
                   parseName(json["destination"], destinationNames, point.destination);
        }

        bool decodeMechanism(const QJsonObject& json, Mechanism& mechanism)
        {
            mechanism.id = json["id"].toString();
            mechanism.drivenBy = json["drivenBy"].toString();
            mechanism.locksOpen = json["locksOpen"].toBool(false);
            const QJsonArray roles = json["roles"].toArray();
            if (roles.size() > kListLimit) {
                return false;
            }
            for (const QJsonValue& value : roles) {
                CycleRole role{};
                if (!parseName(value, roleNames, role)) {
                    return false;
                }
                mechanism.roles |= role;
            }
            return parseName(json["kind"], kindNames, mechanism.kind) &&
                   parseName(json["return"], returnNames, mechanism.returnKind) &&
                   parseName(json["driver"].isUndefined() ? QJsonValue("hand") : json["driver"], driverNames, mechanism.driver);
        }

        bool decodeFire(const QJsonObject& json, Fire& fire)
        {
            fire.manualCycleMechanism = json["manualCycle"].toString();
            fire.lockOpenMechanism = json["lockOpen"].toString();
            return parseName(json["motion"], motionNames, fire.motion) &&
                   parseName(json["afterShot"], afterShotNames, fire.afterShot) &&
                   parseName(json["ejection"], ejectionNames, fire.ejection);
        }

        bool decodeModel(const QJsonObject& json, WeaponModel& model)
        {
            const QJsonObject feed = json["feed"].toObject();
            const QJsonArray points = feed["loadingPoints"].toArray();
            const QJsonArray mechanisms = json["mechanisms"].toArray();
            if (!parseName(feed["container"], containerNames, model.feed.container) || points.size() > kListLimit ||
                mechanisms.size() > kListLimit || !decodeFire(json["fire"].toObject(), model.fire)) {
                return false;
            }
            for (const QJsonValue& value : points) {
                LoadingPoint point;
                if (!decodeLoadingPoint(value.toObject(), point)) {
                    return false;
                }
                model.feed.loadingPoints.append(point);
            }
            for (const QJsonValue& value : mechanisms) {
                Mechanism mechanism;
                if (!decodeMechanism(value.toObject(), mechanism)) {
                    return false;
                }
                model.mechanisms.append(mechanism);
            }
            return true;
        }

        bool diamondAction(MechanismKind kind)
        {
            switch (kind) {
            case MechanismKind::Slide:
            case MechanismKind::ChargingHandle:
            case MechanismKind::OperatingHandle:
            case MechanismKind::ReciprocatingBolt:
            case MechanismKind::TurnBolt:
                return true;
            default:
                return false;
            }
        }

        DiamondSupport refused(const QString& reason)
        {
            return { std::nullopt, reason };
        }
    }

    DiamondSupport diamondSupport(const WeaponModel& model)
    {
        if (!validateWeaponModel(model).isEmpty()) {
            return refused(QStringLiteral("The weapon description is inconsistent"));
        }
        const Feed& feed = model.feed;
        if (feed.container != ContainerKind::Detachable) {
            return refused(QStringLiteral("DIAMOND has no runtime yet for fixed feeds (single rounds, clips, cylinders)"));
        }
        if (feed.loadingPoints.size() != 1 || !feed.loadingPoints[0].requiresOpen.isEmpty()) {
            return refused(QStringLiteral("DIAMOND supports one magazine well that is always reachable"));
        }
        const Mechanism* action = nullptr;
        for (const Mechanism& mechanism : model.mechanisms) {
            if (mechanism.driver != MechanismDriver::Hand) {
                continue;
            }
            if (action || !diamondAction(mechanism.kind)) {
                return refused(QStringLiteral("DIAMOND has no runtime yet for \"%1\" with this action").arg(mechanism.id));
            }
            action = &mechanism;
        }
        if (!action) {
            return refused(QStringLiteral("DIAMOND needs a hand-operated action"));
        }
        if (model.fire.afterShot == AfterShot::OpenBolt) {
            return refused(QStringLiteral("DIAMOND has no runtime yet for open-bolt firing"));
        }
        const bool manual = model.fire.afterShot == AfterShot::ManualCycle;
        if (manual != (action->kind == MechanismKind::TurnBolt)) {
            return refused(QStringLiteral("DIAMOND cycles by hand after each shot only with a turn bolt"));
        }
        return { action->kind, {} };
    }

    bool decodeWeaponPreset(const QByteArray& bytes, WeaponPreset& preset, QString& error)
    {
        QJsonParseError parse;
        const QJsonObject json = QJsonDocument::fromJson(bytes, &parse).object();
        if (parse.error != QJsonParseError::NoError || json["format"].toInt() != kPresetFormat) {
            error = QStringLiteral("Not a weapon preset of format %1").arg(kPresetFormat);
            return false;
        }
        preset = {};
        preset.id = json["id"].toString();
        preset.label = json["label"].toString();
        preset.category = json["category"].toString();
        if (preset.id.isEmpty() || preset.label.isEmpty() || preset.category.isEmpty() ||
            !decodeModel(json["model"].toObject(), preset.model)) {
            error = QStringLiteral("Weapon preset \"%1\" has missing or unknown fields").arg(preset.id);
            return false;
        }
        const QStringList problems = validateWeaponModel(preset.model);
        if (!problems.isEmpty()) {
            error = QStringLiteral("Weapon preset \"%1\": %2").arg(preset.id, problems.join("; "));
            return false;
        }
        return true;
    }

    bool loadWeaponPresets(const QString& directory, QVector<WeaponPreset>& presets, QString& error)
    {
        presets.clear();
        const QStringList files = QDir(directory).entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
        if (files.isEmpty() || files.size() > kPresetCountLimit) {
            error = QStringLiteral("No weapon presets in %1").arg(directory);
            return false;
        }
        QSet<QString> ids;
        QSet<int> actions; // MechanismKind values
        for (const QString& name : files) {
            QFile file(QDir(directory).filePath(name));
            if (!file.open(QIODevice::ReadOnly) || file.size() > kPresetSizeLimit) {
                error = QStringLiteral("Cannot read weapon preset %1").arg(name);
                return false;
            }
            WeaponPreset preset;
            if (!decodeWeaponPreset(file.readAll(), preset, error)) {
                return false;
            }
            const DiamondSupport support = diamondSupport(preset.model);
            if (ids.contains(preset.id) || !support.action || actions.contains(static_cast<int>(*support.action))) {
                error = QStringLiteral("Weapon preset \"%1\" repeats an id or is not one DIAMOND behavior: %2")
                            .arg(preset.id, support.refusal);
                return false;
            }
            ids.insert(preset.id);
            actions.insert(static_cast<int>(*support.action));
            presets.append(preset);
        }
        return true;
    }
}
