// Pack and asset jobs: one bundled product-tool process at a time exports ESP
// projects, commits or deletes their saved versions, validates or inspects
// packs, or prepares and cooks world models. Every dialog and command shares
// the one job state.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "commands/CommandSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QDialog>
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

    // state; cancel; open/close the export window; export ESP projects; or
    // start validate, freeze, inspect-project, cook or prepare.
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
        require(!_packProcess && !_saveAll && !espBusy(), "A save or pack job is already running");
        if (operation == "close") {
            if(auto* dialog=_window->findChild<QDialog*>("ReloadExportDialog"))dialog->close();
            return {{"ok",true},{"phase","closed"}};
        }
        if (operation == "open") {
            QMetaObject::invokeMethod(this,[this]{showPackBuilder();},Qt::QueuedConnection);
            return {{"ok",true},{"phase","dialog"}};
        }
        if (operation == "export") {
            return espExport(request);
        }
        const bool authoring = operation == "freeze" || operation == "inspect-project";
        const bool cooking = operation == "cook" || operation == "prepare";
        require(operation == "validate" || authoring || cooking, "Unknown pack operation");
        const auto packs = request["packs"].toArray();
        require(authoring || cooking || (!packs.isEmpty() && packs.size() <= 256), "Select between one and 256 finished pack folders");
        QString python;
        QString id;
        auto arguments = packToolArguments(python, id);
        if (cooking) {
            require(QFileInfo(request["job"].toString()).isFile() && !request["output"].toString().isEmpty(), "Select a frozen asset job and new finished NIF path");
            arguments << (operation == "cook" ? "cook-object" : "prepare-object") << "--job" << request["job"].toString() << "--output" << request["output"].toString();
        } else {
            arguments << "packs" << operation;
        }
        if (authoring) {
            appendAuthoringArguments(arguments, request, operation);
        } else if (!cooking) {
            for (const auto& pack : packs) {
                require(pack.isString() && QFileInfo(pack.toString()).isDir(), "A selected pack folder is missing");
                arguments << "--pack" << pack.toString();
            }
        }
        startPackProcess(python, arguments, { { "id", id }, { "phase", "running" }, { "operation", operation }, { "packs", packs } });
        return _packJob;
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
    // exists; a capturing save stops after its current step.
    void ReloadMakerController::cancelPackJob()
    {
        // A save between two weapons' captures has nothing running to stop.
        if (_espSave && !_saveAll && !_packProcess) {
            finishEspSave(false, QJsonObject{ { "error", "Save cancelled; nothing was written." }, { "cancelled", true } });
            return;
        }
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

    // The pack project and, for freeze, its new interchange folder.
    void ReloadMakerController::appendAuthoringArguments(QStringList& arguments, const QJsonObject& request, const QString& operation) const
    {
        const auto project = request["project"].toString();
        require(!project.isEmpty() && QFileInfo(project).isFile(), "Select an authored pack project");
        arguments << "--project" << project;
        if (operation == "freeze") {
            const auto output = request["output"].toString();
            require(!output.isEmpty(), "Choose a new local export folder");
            arguments << "--output" << output;
        }
    }

    // The tool's last output line is its JSON result. Output is kept to the
    // last 2 MiB. Library saves, deletions and exports update the working
    // project and the library view.
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
            if (_packJob["operation"] == "library-save") {
                finishEspSave(false, QJsonObject{ { "error", _packJob["error"] } });
            }
            emit packJobChanged();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, output, collect](int code, QProcess::ExitStatus status) {
            collect();
            const auto lines = output->trimmed().split('\n');
            const auto result = QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.back()).object();
            const bool success = code == 0 && status == QProcess::NormalExit && result["ok"].toBool();
            const bool cancelled = !success && _packJob["phase"] == "cancelling";
            _packJob["phase"] = success ? "complete" : "failed";
            _packJob["result"] = result;
            if (!success) {
                _packJob["error"] = result.value("error").toString(QString::fromUtf8(process->readAllStandardError()).right(8192));
            }
            QFile::remove(_packCancelFile);
            _packProcess = nullptr;
            process->deleteLater();
            const auto operation = _packJob["operation"].toString();
            if (operation == "library-save") {
                finishEspSave(success, success ? result : QJsonObject{ { "error", _packJob["error"] }, { "cancelled", cancelled } });
            } else if (operation == "library-delete") {
                finishEspDelete(success, success ? result : QJsonObject{ { "error", _packJob["error"] } });
            }
            if (operation.startsWith("library-")) {
                _espIndexCache.clear();
                updatePackCoverage();
            }
            emit packJobChanged();
        });
        process->start();
        emit packJobChanged();
    }

}
