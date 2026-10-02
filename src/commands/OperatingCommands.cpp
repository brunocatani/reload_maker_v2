#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QJsonDocument>

#include <algorithm>

namespace reloadmaker
{
    using commands::number;
    using commands::require;

    namespace
    {
        constexpr int kMaximumMechanismStages = 32;

        ReleaseRule releaseRule(const QString& name)
        {
            require(name == "hold" || name == "start" || name == "rest", "Unknown release rule");
            return name == "hold" ? ReleaseRule::Hold : name == "start" ? ReleaseRule::StageStart : ReleaseRule::Rest;
        }
    }

    void ReloadMakerController::registerOperatingCommands()
    {
        _commands.add({ "operating_setup_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return operatingSetupState(); } });

        _commands.add({ "operating_setup", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return configureOperatingSetup(params);
                        } });

        _commands.add({ "capture_prepared_part", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return capturePreparedPart(params["output"].toString());
                        } });

        _commands.add({ "mechanism", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return mechanismOperation(params);
                        } });
    }

    // Edits the mechanism draft of the part selected in the workshop and answers
    // with the encoded draft. A supplied record replaces the draft first.
    QJsonObject ReloadMakerController::mechanismOperation(const QJsonObject& params)
    {
        require(_mechanismDraft.has_value(), "Select a part in Mechanisms first");
        if (params.contains("record")) {
            PartMechanism candidate;
            QString error;
            require(decodeMechanism(QJsonDocument(params["record"].toObject()).toJson(), candidate, error), error);
            require(sameMechanismPart(candidate.anchor, _mechanismDraft->anchor), "Mechanism targets another part");
            _mechanismDraft = candidate;
            displayMechanism();
        }
        const QString operation = params["operation"].toString("read");
        if (operation == "set_stage") {
            setMechanismStage(params);
        } else if (operation == "preview") {
            beginMechanismPreview();
        } else if (operation == "stop") {
            stopMechanismPreview();
            if (_clip.valid()) {
                (void)applyPose(_playbackTime);
            }
        } else if (operation == "seek") {
            require(_mechanismPreview, "Start mechanism preview first");
            const float path = number(params["path"]);
            require(path >= 0 && path <= _mechanismDraft->stages.size(), "Path outside mechanism stages");
            _mechanismState.path = path;
            _mechanismState.activeStage = std::min(int(path), int(_mechanismDraft->stages.size()) - 1);
            _mechanismState.returning = false;
            require(applyPose(_playbackTime), _poseError);
        } else if (operation == "release") {
            releaseMechanismGrab();
        } else if (operation == "save") {
            saveWorkshop();
        } else {
            require(operation == "read", "Unknown mechanism operation");
        }
        QString error;
        const auto bytes = encodeMechanism(*_mechanismDraft, error);
        require(error.isEmpty(), error);
        ++_revision;
        return QJsonDocument::fromJson(bytes).object();
    }

    // Stage index equal to the stage count appends a new stage.
    void ReloadMakerController::setMechanismStage(const QJsonObject& params)
    {
        auto candidate = *_mechanismDraft;
        const int index = params["stage"].toInt(0);
        require(index >= 0 && index <= candidate.stages.size() && index < kMaximumMechanismStages,
                "Unknown mechanism stage");
        if (index == candidate.stages.size()) {
            candidate.stages.append(MechanismStage{});
        }
        auto& stage = candidate.stages[index];
        if (params.contains("axis")) {
            const int axis = params["axis"].toInt(-1);
            require(axis >= 0 && axis <= 2, "Axis must be 0, 1 or 2");
            stage.axis = axis;
        }
        if (params.contains("rotation")) {
            stage.rotation = params["rotation"].toBool();
        }
        if (params.contains("from")) {
            stage.from = number(params["from"]);
        }
        if (params.contains("to")) {
            stage.to = number(params["to"]);
        }
        if (params.contains("returnRate")) {
            stage.returnRate = number(params["returnRate"]);
        }
        if (params.contains("release")) {
            stage.release = releaseRule(params["release"].toString());
        }
        if (params.contains("name")) {
            stage.forwardName = params["name"].toString();
        }
        if (params.contains("pivot")) {
            const auto pivot = params["pivot"].toArray();
            require(pivot.size() == 3, "Pivot needs three components");
            stage.pivot = { number(pivot[0]), number(pivot[1]), number(pivot[2]) };
        }
        QString error;
        require(validateMechanism(candidate, error), error);
        _mechanismDraft = candidate;
        displayMechanism();
        if (_mechanismPreview) {
            require(applyPose(_playbackTime), _poseError);
        }
    }
}
