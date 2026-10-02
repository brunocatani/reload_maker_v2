#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QCheckBox>
#include <QTimer>

namespace reloadmaker
{
    using commands::number;
    using commands::require;

    namespace
    {
        // Pull progress above 1 releases a locked-back slide or bolt catch.
        constexpr float kMaximumPullProgress = 1.08f;
        constexpr float kMaximumStepSeconds = 30.0f;

        bool isActionOperation(const QString& operation)
        {
            return operation == "fire" || operation == "reload" || operation == "drop_magazine" ||
                   operation == "reset" || operation == "mag_out" || operation == "mag_in" ||
                   operation == "grab_magazine" || operation == "slide_release" || operation == "bolt_release";
        }
    }

    void ReloadMakerController::registerSimulationCommands()
    {
        _commands.add({ "simulation_state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return simulationState(); } });

        _commands.add({ "simulate", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return simulateOperation(params);
                        } });
    }

    QJsonObject ReloadMakerController::simulateOperation(const QJsonObject& params)
    {
        require(simulationWorkspace(_workspace), "Open Operate or Sounds first");
        // Same rule as the Operate buttons: an action leaves native clip preview.
        if (!_simulationActive) {
            prepareSimulation();
        }
        require(_simulationActive,
                _simulationError.isEmpty() ? "Wait until the Operate workspace is ready" : _simulationError);
        const auto operation = params["operation"].toString();
        if (operation == "trigger_down" || operation == "trigger_up") {
            setSimulationTrigger(operation == "trigger_down");
        } else if (operation == "step") {
            const float delta = number(params["seconds"]);
            require(delta >= 0 && delta <= kMaximumStepSeconds, "Simulation step must be between 0 and 30 seconds");
            _simulationTimer->stop();
            advanceSimulation(delta);
        } else if (operation == "pull") {
            require(operatingGrabAvailable(), "The operating grip assignment is invalid for this assembly.");
            const float progress = number(params["progress"]);
            require(progress >= 0 && progress <= kMaximumPullProgress,
                    "Slide pull must be between 0 and 1.08; above 1 releases the catch");
            require(_simulation.action == SimulationAction::Held || _simulation.grab(),
                    "Finish the native action before pulling");
            _simulationTimer->stop();
            dragSimulation(progress);
        } else if (operation == "release") {
            releaseSimulation(params["cancel"].toBool());
        } else if (operation == "move_magazine") {
            moveSimulatedMagazine(params);
        } else if (operation == "cancel_magazine") {
            require(_simulation.magazine.carried(), "No cursor-held magazine to cancel");
            cancelMagazine();
        } else if (operation == "spring") {
            _simulationSpring->setChecked(params["enabled"].toBool(true));
        } else {
            require(isActionOperation(operation), "Unknown simulation operation");
            runSimulationAction(operation);
        }
        require(_lastError.isEmpty(), _lastError);
        ++_revision;
        return simulationState();
    }

    // A railed magazine moves by distance along its insertion line; a free one by
    // a weapon-local offset.
    void ReloadMakerController::moveSimulatedMagazine(const QJsonObject& params)
    {
        require(_magazineRail.has_value(), "Magazine rail is unavailable");
        if (params.contains("distance")) {
            require(_simulation.magazine.phase == MagazinePhase::Rail, "The magazine is not on the rail");
            moveMagazineRail(number(params["distance"]));
            return;
        }
        require(_simulation.magazine.phase == MagazinePhase::Free, "Grab a magazine before moving it freely");
        const auto position = params["position"].toArray();
        require(position.size() == 3, "Position needs three weapon-local offset components");
        moveFreeMagazine({ number(position[0]), number(position[1]), number(position[2]) });
    }
}
