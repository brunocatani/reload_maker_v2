// Pack and asset jobs: one bundled product-tool process at a time builds,
// validates, inspects or exports packs, saves a configured reload into a
// pack project, lists pack coverage, or prepares and cooks world models.
// Every dialog and command shares the one job state.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "commands/CommandSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <memory>

namespace reloadmaker
{
    using commands::require;

    // state; cancel; or start build, validate, inventory, export,
    // inspect-project, save-configuration, cook or prepare.
    QJsonObject ReloadMakerController::packCommand(const QJsonObject& request)
    {
        const auto operation = request.value("operation").toString("state");
        if (operation == "state") {
            return _packJob.isEmpty() ? QJsonObject{ { "phase", "idle" } } : _packJob;
        }
        if (operation == "cancel") {
            cancelPackJob();
            return _packJob;
        }
        require(!_packProcess && !_saveAll, "A pack job is already running");
        const bool saving = operation == "save-configuration";
        const bool inventory = operation == "inventory";
        const bool authoring = operation == "export" || operation == "inspect-project" || saving;
        const bool cooking = operation == "cook" || operation == "prepare";
        require(operation == "build" || operation == "validate" || authoring || cooking || inventory, "Unknown pack operation");
        const auto packs = request["packs"].toArray();
        require(authoring || cooking || inventory || (!packs.isEmpty() && packs.size() <= 256), "Select between one and 256 finished pack folders");
        // Export leaves out weapons whose checklist is not complete (owner,
        // 2026-10-05: warn and export the rest, never block); Check project
        // lists them in advance.
        const auto skipped = operation == "export" || operation == "inspect-project" ? exportSkippedWeapons(request["project"].toString()) : QJsonArray{};
        QString python;
        QString id;
        auto arguments = packToolArguments(python, id);
        if (cooking) {
            require(QFileInfo(request["job"].toString()).isFile() && !request["output"].toString().isEmpty(), "Select a frozen asset job and new finished NIF path");
            arguments << (operation == "cook" ? "cook-object" : "prepare-object") << "--job" << request["job"].toString() << "--output" << request["output"].toString();
        } else {
            arguments << "packs" << operation;
        }
        if (inventory) {
            appendInventoryArguments(arguments, request);
        } else if (authoring) {
            appendAuthoringArguments(arguments, request, operation);
            if (operation == "export") {
                for (const auto& weapon : skipped) {
                    arguments << "--skip" << weapon.toObject()["record"].toString();
                }
            }
        } else if (!cooking) {
            for (const auto& pack : packs) {
                require(pack.isString() && QFileInfo(pack.toString()).isDir(), "A selected pack folder is missing");
                arguments << "--pack" << pack.toString();
            }
        }
        if (operation == "build") {
            const auto installation = request["installation"].toString();
            require(!installation.isEmpty(), "Select the installation ledger folder");
            arguments << "--installation" << installation;
            if (request["initialize"].toBool()) {
                arguments << "--initialize";
            }
        }
        QJsonObject job{ { "id", id }, { "phase", "running" }, { "operation", operation }, { "packs", packs } };
        if (!skipped.isEmpty()) {
            job["skipped"] = skipped;
        }
        startPackProcess(python, arguments, job);
        return _packJob;
    }

    QJsonArray ReloadMakerController::exportSkippedWeapons(const QString& project) const
    {
        QFile file(project);
        require(file.size() <= 64 * 1024 * 1024 && file.open(QIODevice::ReadOnly), "Select an authored pack project");
        const auto progress = weaponProgressIndex();
        QJsonArray skipped;
        for (const auto& value : QJsonDocument::fromJson(file.readAll()).object()["weapons"].toArray()) {
            const auto weapon = value.toObject();
            const auto record = weapon["record"].toString().toLower();
            const auto state = progress.constFind(record);
            if (state != progress.cend() && *state == WeaponProgress::Complete) {
                continue;
            }
            skipped.append(QJsonObject{ { "record", record },
                                        { "name", weapon["name"].toString(record) },
                                        { "reason", state == progress.cend() ? QStringLiteral("no checklist for it in the loaded source")
                                                                             : QStringLiteral("checklist not complete") } });
        }
        return skipped;
    }

    // The bundled tool's interpreter and its leading arguments, with a new
    // job ID (unless one is given) and cancel file.
    QStringList ReloadMakerController::packToolArguments(QString& python, QString& id)
    {
        const QDir application(QCoreApplication::applicationDirPath());
        python = application.filePath("runtime/python/python.exe");
        const auto entry = application.filePath("tools/product_cli.py");
        require(QFileInfo::exists(python) && QFileInfo::exists(entry), "The bundled object builder is missing. Install the complete Reload Maker tool package.");
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        _packCancelFile = studioDataPath("local/tmp/pack-" + id + ".cancel");
        return { "-B", "-I", entry, "--workspace", studioRoot(), "--cancel-file", _packCancelFile };
    }

    // The running tool stops at its next safe point once the cancel file
    // exists; a capturing save-all job stops after its current step.
    void ReloadMakerController::cancelPackJob()
    {
        if (_saveAll) {
            _saveAll->cancelled = true;
            _packJob["phase"] = "cancelling";
            emit packJobChanged();
            return;
        }
        if (!_packProcess) {
            return;
        }
        QFile cancel(_packCancelFile);
        require(cancel.open(QIODevice::WriteOnly), "Cannot request cancellation");
        cancel.close();
        _packJob["phase"] = "cancelling";
        emit packJobChanged();
    }

    // At most 512 explicit pack projects.
    void ReloadMakerController::appendInventoryArguments(QStringList& arguments, const QJsonObject& request) const
    {
        const auto projects = request["projects"].toArray();
        require(projects.size() <= 512, "Too many explicit pack projects");
        for (const auto& project : projects) {
            require(project.isString() && !project.toString().isEmpty(), "Choose a pack project path");
            arguments << "--project" << project.toString();
        }
    }

    // The pack project (a new one only when saving), the saved capture's
    // inputs, and the export folder.
    void ReloadMakerController::appendAuthoringArguments(QStringList& arguments, const QJsonObject& request, const QString& operation) const
    {
        const bool saving = operation == "save-configuration";
        const auto project = request["project"].toString();
        require(!project.isEmpty() && (saving || QFileInfo(project).isFile()), "Select an authored pack project");
        arguments << "--project" << project;
        if (saving) {
            for (const auto* key : { "capture", "expected", "id", "full", "empty" }) {
                const auto value = request[key].toString();
                if (!value.isEmpty()) {
                    arguments << ("--" + QString(key)) << value;
                }
            }
        }
        if (operation == "export") {
            const auto output = request["output"].toString();
            require(!output.isEmpty(), "Choose a new local export folder");
            arguments << "--output" << output;
        }
    }

    // The tool's last output line is its JSON result. Output is kept to the
    // last 2 MiB. A finished inventory refreshes the pack coverage.
    void ReloadMakerController::startPackProcess(const QString& python, const QStringList& arguments, const QJsonObject& job)
    {
        auto* process = new QProcess(this);
        _packProcess = process;
        process->setProgram(python);
        process->setArguments(arguments);
        process->setWorkingDirectory(studioRoot());
        _packJob = job;
        auto output = std::make_shared<QByteArray>();
        const auto collect = [process, output] {
            output->append(process->readAllStandardOutput());
            if (output->size() > 2 * 1024 * 1024) {
                output->remove(0, output->size() - 2 * 1024 * 1024);
            }
        };
        connect(process, &QProcess::readyReadStandardOutput, this, collect);
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) {
                return;
            }
            _packJob["phase"] = "failed";
            _packJob["error"] = process->errorString();
            _packProcess = nullptr;
            process->deleteLater();
            emit packJobChanged();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, output, collect](int code, QProcess::ExitStatus status) {
            collect();
            const auto lines = output->trimmed().split('\n');
            const auto result = QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.back()).object();
            const bool success = code == 0 && status == QProcess::NormalExit && result["ok"].toBool();
            _packJob["phase"] = success ? "complete" : "failed";
            _packJob["result"] = result;
            if (!success) {
                _packJob["error"] = result.value("error").toString(QString::fromUtf8(process->readAllStandardError()).right(8192));
            }
            QFile::remove(_packCancelFile);
            _packProcess = nullptr;
            process->deleteLater();
            if (success && _packJob["operation"] == "inventory") {
                updatePackCoverage(result);
            }
            // A save-all job's captures only carry the configurations to the
            // save; the pack project holds the result.
            if (_packJob["operation"] == "save-all-configurations") {
                QDir(_packJob["captureFolder"].toString()).removeRecursively();
            }
            emit packJobChanged();
        });
        process->start();
        emit packJobChanged();
    }

    // Freezes the configuration prepared in Operate and saves it into the
    // requested pack project.
    QJsonObject ReloadMakerController::saveConfiguredReload(const QJsonObject& request)
    {
        require(!_packProcess, "Wait for the current asset or pack job");
        const auto capture = exportRuntimeConfiguration();
        const auto folder = studioDataPath("local/authoring-captures/" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QDir().mkpath(folder);
        const auto path = folder + "/configuration.json";
        QSaveFile file(path);
        const auto bytes = QJsonDocument(capture).toJson();
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot freeze the configured reload");
        auto args = request;
        args["operation"] = "save-configuration";
        args["capture"] = path;
        return packCommand(args);
    }

    // Checks the loaded configuration's source bytes against an inspected
    // pack project: exactly one of its configurations must use the loaded
    // weapon, receiver, magazine and operating part.
    QString ReloadMakerController::loadedConfigurationSourceReport(const QJsonObject& inspection) const
    {
        require(currentWeapon() && configuration(), "Load the weapon and configuration to check");
        const auto weapon = poseRecordKey(currentWeapon()->formId).toLower();
        QSet<QString> forms{ weapon };
        for (const auto* mod : configuredObjectMods()) {
            forms.insert(poseRecordKey(mod->formId).toLower());
        }
        QMap<QString, QJsonObject> selected;
        int matches = 0;
        for (const auto& row : inspection["sourceRequirements"].toArray()) {
            const auto entry = row.toObject();
            if (entry["weapon"].toString().toLower() != weapon) {
                continue;
            }
            for (const auto& value : entry["configurations"].toArray()) {
                const auto config = value.toObject();
                if (!forms.contains(config["receiver"].toString().toLower()) || !forms.contains(config["magazine"].toString().toLower())
                    || (config.contains("operatingPart") && !forms.contains(config["operatingPart"].toString().toLower()))) {
                    continue;
                }
                ++matches;
                for (const auto& item : config["resources"].toArray()) {
                    auto resource = item.toObject();
                    const auto path = resource["path"].toString();
                    resource["kind"] = path.endsWith(".hkx") ? "animation" : "model";
                    selected[path] = resource;
                }
            }
        }
        require(matches == 1, "Select one configuration included in this pack project");
        QJsonArray resources;
        for (const auto& resource : selected) {
            resources.append(resource);
        }
        const auto audit = auditSources({ { "version", 1 }, { "resources", resources } });
        QString text = "\n\nSource view: " + audit["sourceMode"].toString() + " " + audit["profile"].toString();
        text += audit["compatible"].toBool() ? "\nSelected configuration source bytes match." : "\nSelected configuration needs source review.";
        for (const auto& value : audit["resources"].toArray()) {
            const auto item = value.toObject();
            if (item["status"] != "unchanged") {
                text += "\n" + item["status"].toString() + " · " + item["path"].toString() + "\n" + item["provider"].toString();
            }
        }
        return text;
    }
}
