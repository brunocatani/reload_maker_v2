#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QJsonDocument>
#include <QTimer>


namespace reloadmaker
{
    using commands::require;

    void ReloadMakerController::registerReloadCommands()
    {
        _commands.add({ "reload", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(_reloadRecipe && !_reloadCapturing, "No captured reload");
                            QString error;
                            const auto bytes = encodeReloadRecipe(*_reloadRecipe, error);
                            require(error.isEmpty(), error);
                            return QJsonDocument::fromJson(bytes).object();
                        } });

        _commands.add({ "learn_reload", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            setWorkspace(PhasesWorkspace);
                            if (!_reloadCapturing && !_reloadRecipe) {
                                startReloadCapture();
                            }
                            return std::nullopt;
                        } });

        _commands.add({ "author_reload_grips", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(authorReloadGrips(), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "revision", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            // A cheap idle check for clients that would otherwise poll the full state.
                            return QJsonObject{ { "busy", bool(_loadThread) || _reloadCapturing || _reloadCaptureQueued || _rebuildTimer->isActive() || espBusy() },
                                                { "lastError", _lastError } };
                        } });

        _commands.add({ "load_timings", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            auto result = _loadTimings.json(qint64(params["since"].toDouble(0)));
                            const auto resident = residentState();
                            for (auto it = resident.begin(); it != resident.end(); ++it) {
                                result.insert(it.key(), it.value());
                            }
                            return result;
                        } });

        _commands.add({ "weapon_properties_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return weaponPropertiesState(); } });

        _commands.add({ "weapon_property", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            setWeaponProperty(params["name"].toString(), params["value"]);
                            return weaponPropertiesState();
                        } });

        _commands.add({ "checklist_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return checklistState(); } });

        _commands.add({ "checklist", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            auto result = checklistCommand(params);
                            refreshChecklistUi();
                            return result;
                        } });

        // Finished full/empty world models for a magazine; Save copies them
        // into the ESP project with the weapon's captured configurations.
        _commands.add({ "assign_magazine_models", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return assignMagazineModels(params);
                        } });
    }
}
