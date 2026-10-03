#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QJsonDocument>


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

        _commands.add({ "roadmap", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> { return roadmapCommand(params); } });

        _commands.add({ "save_configured_reload", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return saveConfiguredReload(params);
                        } });
    }
}
