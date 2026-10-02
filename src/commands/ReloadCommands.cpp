#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QJsonDocument>

#include <algorithm>

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

        _commands.add({ "edit_reload", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            editReloadWindow(params);
                            return std::nullopt;
                        } });

        _commands.add({ "save_reload", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(_reloadRecipe.has_value(), "No reload recipe");
                            QString error;
                            require(_reloadRecipeLibrary.save(*_reloadRecipe, error), error);
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

    // Window edits apply to a copy of the recipe, which replaces the current one
    // only after it encodes successfully.
    void ReloadMakerController::editReloadWindow(const QJsonObject& params)
    {
        require(_reloadRecipe && !_reloadCapturing, "Learn a reload first");
        auto candidate = *_reloadRecipe;
        QString error;
        const int index = params["window"].toInt(-1);
        require(index >= 0 && index < candidate.windows.size(), "Invalid reload window");
        const auto operation = params["operation"].toString("update");
        if (operation == "split") {
            require(splitReloadWindow(candidate, index, params["frame"].toInt(-1), error), error);
        } else if (operation == "merge") {
            require(mergeReloadWindow(candidate, index, error), error);
        } else {
            require(operation == "update", "Unknown reload edit");
            updateReloadWindow(candidate, index, params);
        }
        require(!encodeReloadRecipe(candidate, error).isEmpty(), error);
        _reloadRecipe = candidate;
        refreshReloadWindows();
        selectReloadWindow(std::min(index, int(candidate.windows.size()) - 1));
    }

    void ReloadMakerController::updateReloadWindow(ReloadRecipe& recipe, int index, const QJsonObject& params) const
    {
        auto& window = recipe.windows[index];
        if (params.contains("name")) {
            require(!params["name"].toString().trimmed().isEmpty(), "Window needs a name");
            window.label = params["name"].toString().trimmed();
        }
        if (params.contains("returnOnRelease")) {
            window.returnOnRelease = params["returnOnRelease"].toBool();
        }
        if (!params.contains("track")) {
            return;
        }
        const int track = params["track"].toInt(-1);
        const auto member = std::find_if(window.members.begin(), window.members.end(),
                                         [track](const auto& value) { return value.track == track; });
        require(member != window.members.end(), "Track is not in this window");
        if (params.contains("grabbable")) {
            require(!params["grabbable"].toBool() || recipe.tracks[track].surface, "This track has no grabbable surface");
            member->grabbable = params["grabbable"].toBool();
        }
        if (params.contains("gripId")) {
            const auto id = params["gripId"].toString();
            const bool known = id.isEmpty() || std::any_of(_partGrips.begin(), _partGrips.end(),
                                                           [&](const auto& grip) { return grip.pose.id == id; });
            require(known, "Unknown part grip");
            member->gripId = id;
        }
    }
}
