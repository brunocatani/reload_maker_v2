// Save all configurations (rework 06). Each of the weapon's receiver ×
// operating part × magazine choices is selected with the loadout's own code,
// prepared in Operate and captured with the single-save export. The bundled
// tool then saves every passing capture into the pack project in one write;
// a configuration saved from identical inputs is reported up to date. A
// configuration that cannot be prepared or captured becomes an exception
// that names its parts and the reason. It never stops the others.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "commands/CommandSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        // How long the job waits before looking again while the assembly is busy.
        constexpr int kPollMilliseconds = 15;
    }

    bool ReloadMakerController::assemblyBusy() const
    {
        return _loadThread || _reloadCapturing || _reloadCaptureQueued || _rebuildTimer->isActive();
    }

    // Receiver outermost, so it changes least: every choice of the receiver's
    // slot, then of the operating part's slot when that part is separate, then
    // of the magazine's slot. Parts the source policy blocks are left out. A
    // fixed receiver or operating part, or a magazine built into the receiver,
    // adds no choices.
    ReloadMakerController::ConfigurationPlan ReloadMakerController::saveAllPlan() const
    {
        const auto* config = configuration();
        QSet<QString> blocked;
        for (const auto& value : sourcePolicyState()["policy"].toObject()["blockedMods"].toArray()) {
            blocked.insert(value.toString().toLower());
        }
        const auto slotOf = [this](const QString& record) -> SlotKey {
            for (const auto* mod : configuredObjectMods()) {
                if (poseRecordKey(mod->formId).compare(record, Qt::CaseInsensitive) == 0) {
                    return configurationSlotKey(*mod);
                }
            }
            return 0;
        };
        QVector<QPair<ConfigurationRole, SlotKey>> axes;
        const auto add = [&](ConfigurationRole role, SlotKey slot) {
            if (slot && std::none_of(axes.cbegin(), axes.cend(), [&](const auto& axis) { return axis.second == slot; })) {
                axes.append({ role, slot });
            }
        };
        if (const auto* receiver = resolvedReceiverObjectMod()) {
            add(ConfigurationRole::Receiver, configurationSlotKey(*receiver));
        }
        if (const int anchor = operatingAnchor(); anchor >= 0) {
            add(ConfigurationRole::OperatingPart, slotOf(_handPoseAnchors[anchor].description.ownerRecord));
        }
        add(ConfigurationRole::Magazine, slotOf(simulationMagazineAnchor().ownerRecord));
        ConfigurationPlan plan{ {} };
        for (const auto& [role, slot] : axes) {
            const auto* entry = config->slot(slot);
            ConfigurationPlan expanded;
            for (const auto& partial : plan) {
                for (const auto id : entry ? entry->choices : QVector<std::uint32_t>{}) {
                    if (!config->mod(id) || blocked.contains(poseRecordKey(id).toLower())) {
                        continue;
                    }
                    auto combination = partial;
                    combination.append({ role, slot, id });
                    expanded.append(combination);
                }
            }
            plan = expanded;
        }
        if (plan.size() == 1 && plan.front().isEmpty()) {
            plan.clear();
        }
        return plan;
    }

    // The project must exist: a magazine's first save brings its cooked
    // world models, which only the single save takes.
    QJsonObject ReloadMakerController::startSaveAllConfigurations(const QJsonObject& request)
    {
        require(!_packProcess && !_saveAll, "Wait for the current asset or pack job");
        require(currentWeapon() && configuration() && !assemblyBusy(), "Load the weapon and wait until its assembly is ready");
        const auto project = request["project"].toString();
        require(!project.isEmpty() && QFileInfo(project).isFile(),
                "Choose the weapon's pack project. Save one configuration of each new magazine with its cooked models first.");
        require(!request["expected"].toString().isEmpty(), "Inspect the pack project before saving");
        setWorkspace(OperateWorkspace);
        auto plan = saveAllPlan();
        require(!plan.isEmpty(), "This weapon offers no receiver, operating part or magazine choices to save");
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        SaveAllJob job;
        job.request = request;
        job.plan = std::move(plan);
        job.folder = studioDataPath("local/authoring-captures/" + id);
        require(QDir().mkpath(job.folder), "Cannot create the capture folder");
        for (const auto& slot : configuration()->entries()) {
            job.original[slot.key] = configuration()->selected(slot.key);
        }
        _packJob = { { "id", id },
                     { "phase", "running" },
                     { "operation", "save-all-configurations" },
                     { "stage", "capturing" },
                     { "total", job.plan.size() },
                     { "done", 0 },
                     { "exceptions", QJsonArray{} } };
        _saveAll = std::move(job);
        emit packJobChanged();
        QTimer::singleShot(0, this, [this] { continueSaveAll(); });
        return _packJob;
    }

    // One step: wait for the assembly, select the next combination, or
    // capture the selected one.
    void ReloadMakerController::continueSaveAll()
    {
        if (!_saveAll) {
            return;
        }
        auto& job = *_saveAll;
        if (assemblyBusy()) {
            QTimer::singleShot(kPollMilliseconds, this, [this] { continueSaveAll(); });
            return;
        }
        if (job.cancelled || job.next >= job.plan.size()) {
            finishSaveAll();
            return;
        }
        const auto combination = job.plan[job.next];
        if (!job.assembled) {
            job.assembled = true;
            try {
                bool changed = false;
                for (const auto& choice : combination) {
                    if (configuration()->selected(choice.slot) != choice.mod) {
                        changed |= configuration()->select(choice.slot, choice.mod);
                    }
                }
                if (changed) {
                    refreshAttachments();
                    rebuildWeapon();
                }
            } catch (const std::exception& error) {
                // The capture step reports the configuration as an exception.
                setStatus(QString::fromUtf8(error.what()), true);
            }
        } else {
            captureSaveAllConfiguration(combination);
            job.assembled = false;
            ++job.next;
            _packJob["done"] = job.next;
            _packJob["exceptions"] = job.exceptions;
            emit packJobChanged();
        }
        QTimer::singleShot(0, this, [this] { continueSaveAll(); });
    }

    // Prepares the selected combination in Operate, checks that the assembly
    // uses the planned parts and writes the capture. Choices that assemble
    // the same configuration (every magazine of a receiver with a built-in
    // one) are captured once.
    void ReloadMakerController::captureSaveAllConfiguration(const QVector<ConfigurationChoice>& combination)
    {
        auto& job = *_saveAll;
        const auto roleName = [](ConfigurationRole role) {
            switch (role) {
            case ConfigurationRole::Receiver: return QStringLiteral("receiver");
            case ConfigurationRole::OperatingPart: return QStringLiteral("operatingPart");
            case ConfigurationRole::Magazine: break;
            }
            return QStringLiteral("magazine");
        };
        QJsonArray parts;
        for (const auto& choice : combination) {
            const auto* mod = configuration()->mod(choice.mod);
            parts.append(QJsonObject{ { "role", roleName(choice.role) },
                                      { "slot", QString::number(choice.slot) },
                                      { "record", poseRecordKey(choice.mod) },
                                      { "name", mod ? mod->name : QString() } });
        }
        try {
            runSimulationAction(QStringLiteral("reset"));
            require(_simulationActive, _simulationError.isEmpty() ? QStringLiteral("Operate could not prepare this configuration") : _simulationError);
            const auto capture = exportRuntimeConfiguration();
            const auto selection = capture["selection"].toObject();
            QStringList key;
            for (const auto* role : { "receiver", "magazine", "operatingPart" }) {
                key.append(selection.value(QLatin1String(role)).toString().toLower());
            }
            for (const auto& choice : combination) {
                const auto record = poseRecordKey(choice.mod).toLower();
                const bool used = std::any_of(selection.begin(), selection.end(), [&](const QJsonValue& value) { return value.toString().toLower() == record; });
                const bool builtIn = choice.role == ConfigurationRole::Magazine && magazineBuiltIn();
                require(used || builtIn, "This assembly does not take " + configuration()->mod(choice.mod)->name);
            }
            if (job.captured.contains(key.join('|'))) {
                return;
            }
            QSaveFile file(job.folder + QString("/%1.json").arg(job.captures, 4, 10, QChar('0')));
            const auto bytes = QJsonDocument(capture).toJson();
            require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot write the capture");
            job.captured.insert(key.join('|'));
            ++job.captures;
        } catch (const std::exception& error) {
            job.exceptions.append(QJsonObject{ { "status", "failed" }, { "parts", parts }, { "error", QString::fromUtf8(error.what()) } });
        }
    }

    // Restores the author's selection, then hands the captures to the bundled
    // tool. A cancelled job saves nothing.
    void ReloadMakerController::finishSaveAll()
    {
        auto job = std::move(*_saveAll);
        _saveAll.reset();
        bool changed = false;
        for (auto it = job.original.cbegin(); it != job.original.cend(); ++it) {
            if (configuration() && configuration()->selected(it.key()) != it.value()) {
                changed |= configuration()->select(it.key(), it.value());
            }
        }
        if (changed) {
            refreshAttachments();
            rebuildWeapon();
        }
        _packJob["done"] = job.next;
        _packJob["exceptions"] = job.exceptions;
        // Step errors belong to the job's exceptions, not to the editor's status.
        setStatus(job.cancelled ? QStringLiteral("Save all configurations cancelled; the pack project is unchanged.")
                                : QStringLiteral("Captured %1 of %2 configurations; %3 could not be prepared.")
                                      .arg(job.captures).arg(job.plan.size()).arg(job.exceptions.size()));
        if (job.cancelled || job.captures == 0) {
            QDir(job.folder).removeRecursively();
            _packJob["phase"] = job.cancelled ? "cancelled" : "complete";
            _packJob["result"] = QJsonObject{ { "ok", !job.cancelled }, { "saved", 0 }, { "upToDate", 0 }, { "failed", 0 }, { "rows", QJsonArray{} } };
            emit packJobChanged();
            return;
        }
        try {
            QString python;
            QString id = _packJob["id"].toString();
            auto arguments = packToolArguments(python, id);
            arguments << "packs" << "save-configurations" << "--project" << job.request["project"].toString() << "--captures" << job.folder
                      << "--expected" << job.request["expected"].toString();
            auto state = _packJob;
            state["stage"] = "saving";
            state["captureFolder"] = job.folder;
            startPackProcess(python, arguments, state);
        } catch (const std::exception& error) {
            _packJob["phase"] = "failed";
            _packJob["error"] = QString::fromUtf8(error.what());
            emit packJobChanged();
        }
    }

    // Selects the parts named by records, for opening a reported
    // configuration. Records not offered by any slot are left out.
    bool ReloadMakerController::selectConfigurationRecords(const QStringList& records)
    {
        auto* config = configuration();
        if (!config || assemblyBusy() || _saveAll) {
            return false;
        }
        bool changed = false;
        for (const auto& record : records) {
            for (const auto& slot : config->entries()) {
                for (const auto id : slot.choices) {
                    if (poseRecordKey(id).compare(record, Qt::CaseInsensitive) == 0 && config->selected(slot.key) != id) {
                        changed |= config->select(slot.key, id);
                    }
                }
            }
        }
        if (changed) {
            refreshAttachments();
            rebuildWeapon();
        }
        return changed;
    }
}
