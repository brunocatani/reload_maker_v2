#include "ReloadMakerController.h"

// Commands of the self-contained workspace tools: grab locations, collisions,
// scopes and pack building. Each tool owns its own state and operations.

namespace reloadmaker
{
    void ReloadMakerController::registerGrabLocationCommands()
    {
        _commands.add({ "grab_locations_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return grabLocationsState(); } });

        _commands.add({ "grab_locations", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            _lastError.clear();
                            return configureGrabLocation(params);
                        } });
    }

    void ReloadMakerController::registerCollisionCommands()
    {
        _commands.add({ "collision_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            refreshCollisions();
                            return _collisionWorkshop.state();
                        } });

        _commands.add({ "collisions", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            refreshCollisions();
                            return _collisionWorkshop.command(params);
                        } });
    }

    void ReloadMakerController::registerScopeCommands()
    {
        _commands.add({ "scope_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return scopeState(); } });

        _commands.add({ "scope_profile", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> { return configureScope(params); } });
    }

    void ReloadMakerController::registerPackCommands()
    {
        _commands.add({ "pack_coverage", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return packCoverageState(params["refresh"].toBool());
                        } });

        _commands.add({ "pack_job", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> { return packCommand(params); } });
    }
}
