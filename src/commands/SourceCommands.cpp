#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QFileInfo>

namespace reloadmaker
{
    using commands::require;

    void ReloadMakerController::registerSourceCommands()
    {
        _commands.add({ "installation", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return installationState(); } });

        _commands.add({ "configure_installation", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(configureInstallation(params), _lastError);
                            return installationState();
                        } });

        _commands.add({ "connect_profile", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(connectInstalledProfile(), _lastError);
                            return studioState();
                        } });

        _commands.add({ "base_game", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(openBaseGame(), _lastError);
                            return studioState();
                        } });

        _commands.add({ "open_source", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const QString path = params["path"].toString();
                            require(QFileInfo(path).exists(), "Source folder or ZIP does not exist");
                            startPackageLoad(path, params["gameData"].toString(_gameDataText), true);
                            return std::nullopt;
                        } });

        _commands.add({ "open_library", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            if (params.contains("instance")) {
                                _mo2Instance = params["instance"].toString();
                            }
                            if (params.contains("profile")) {
                                _requestedMo2Profile = params["profile"].toString();
                            }
                            startLibraryLoad();
                            return std::nullopt;
                        } });

        // Reading the browser is allowed while busy; changing it is an idle-only edit.
        _commands.add({ "source_browser", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto operation = params["operation"].toString("state");
                            if (operation == "state") {
                                return sourceBrowserState();
                            }
                            requireIdle();
                            _lastError.clear();
                            if (operation == "plugin") {
                                require(selectSourcePlugin(params["plugin"].toString()), _lastError);
                            } else if (operation == "weapon") {
                                require(selectSourceWeapon(params["index"].toInt(-1)), _lastError);
                            } else if (operation == "mode") {
                                require(selectSourceMode(params["mode"].toString()), _lastError);
                            } else {
                                require(false, "Unknown source browser operation");
                            }
                            ++_revision;
                            return sourceBrowserState();
                        } });

        _commands.add({ "testbed", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(selectTestbed(params["name"].toString()), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "source_policy_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return sourcePolicyState(); } });

        _commands.add({ "source_policy", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return configureSourcePolicy(params);
                        } });
    }
}
