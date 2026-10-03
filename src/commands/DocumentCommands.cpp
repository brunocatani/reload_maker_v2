#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "ReloadBundle.h"
#include "commands/CommandSupport.h"
#include "glview.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QThread>
#include <QTimer>

#include <algorithm>

namespace reloadmaker
{
    using commands::require;

    void ReloadMakerController::registerDocumentCommands()
    {
        _commands.add({ "state", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return studioState(); } });

        _commands.add({ "focus", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            if (_window->isMinimized()) {
                                _window->showNormal();
                            }
                            _window->raise();
                            _window->activateWindow();
                            return studioState();
                        } });

        // A second editor launched through MO2 hands its verified source over to
        // this one; this editor saves and closes so the new one owns the workspace.
        _commands.add({ "accept_source_session", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(!_profileHandoff.isEmpty() && params["token"].toString() == _profileHandoff,
                                    "No matching MO2 handoff is pending");
                            require(!_loadThread && !_reloadCapturing && !_packProcess && saveSession(),
                                    "The current project cannot close safely; source handoff cancelled");
                            _profileHandoff.clear();
                            QTimer::singleShot(0, _window, [this] { _window->close(); });
                            return QJsonObject{ { "accepted", true } };
                        } });

        _commands.add({ "diagnostics", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return diagnosticsState(); } });

        _commands.add({ "pose", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return poseState(); } });

        _commands.add({ "cancel", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            if (_loadThread) {
                                _loadThread->requestInterruption();
                            }
                            if (_reloadCapturing) {
                                invalidateReloadCapture();
                            }
                            return studioState();
                        } });

        _commands.add({ "home", CommandAccess::Edit, CommandGate::Always,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return params.contains("role") ? chooseHomeRole(params["role"].toString()) : homeState();
                        } });

        _commands.add({ "workspace", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const int index = params["index"].toInt(-1);
                            require(validWorkspace(index), "Unknown workspace");
                            setWorkspace(index);
                            return std::nullopt;
                        } });

        _commands.add({ "save_project", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(saveSession(params["path"].toString()), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "open_project", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            openSession(params["path"].toString());
                            return std::nullopt;
                        } });

        _commands.add({ "import_records", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return importAuthoredRecords(params["paths"].toArray());
                        } });

        _commands.add({ "export", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return exportRecord(params["kind"].toString());
                        } });
    }

    QJsonObject ReloadMakerController::diagnosticsState() const
    {
        QJsonArray remaps;
        for (const auto& remap : _remappedAttachments) {
            Transformf world;
            Transformf parent;
            Transformf root;
            QString error;
            if (!sceneNodeWorld(remap.node, world, error) || !sceneNodeWorld(remap.parent, parent, error) ||
                !sceneNodeWorld(_partRoots.value(remap.slot), root, error)) {
                continue;
            }
            remaps.append(QJsonObject{ { "keyword", remap.keyword }, { "point", remap.point },
                                       { "slot", QString::number(remap.slot) },
                                       { "world", commands::transformJson(world) },
                                       { "parentWorld", commands::transformJson(parent) },
                                       { "attachmentWorld", commands::transformJson(root) } });
        }
        return { { "text", _details->toPlainText() }, { "viewport", _window->getGLView()->studioDiagnostics() },
                 { "attachmentRemaps", remaps }, { "weaponLibrary", _weaponLibraryReport } };
    }

    QJsonObject ReloadMakerController::poseState() const
    {
        QJsonArray bones;
        for (int i = 0; i < _localPose.size(); ++i) {
            bones.append(QJsonObject{ { "index", i }, { "name", _skeleton.bones[i].name },
                                      { "local", commands::transformJson(_localPose[i]) },
                                      { "world", commands::transformJson(_globalPose[i]) } });
        }
        QJsonArray parts;
        // Anchor 0 is the weapon itself; parts start at 1.
        for (int i = 1; i < _handPoseAnchors.size(); ++i) {
            const auto& anchor = _handPoseAnchors[i];
            Transformf world;
            QString error;
            if (!sceneNodeWorld(anchor.node, world, error)) {
                continue;
            }
            parts.append(QJsonObject{ { "index", i }, { "name", anchor.description.anchorName },
                                      { "kind", anchor.description.anchorKind },
                                      { "ownerRecord", anchor.description.ownerRecord },
                                      { "ownerModel", anchor.description.ownerModel },
                                      { "world", commands::transformJson(world) } });
        }
        return { { "source", currentPoseSource() }, { "bones", bones }, { "parts", parts } };
    }

    // Imports validate each record against its domain contract but store the
    // original bytes: re-normalizing float matrices would turn an identical
    // re-import into a false ID conflict. Nothing is written unless every record
    // in the batch is valid and conflict-free.
    QJsonObject ReloadMakerController::importAuthoredRecords(const QJsonArray& paths)
    {
        require(!paths.isEmpty() && paths.size() <= 1000, "Choose 1–1000 authored JSON records");
        struct Pending
        {
            QString path;
            QByteArray bytes;
        };
        QVector<Pending> pending;
        QJsonArray destinations;
        for (const auto& entry : paths) {
            QFile input(entry.toString());
            require(input.open(QIODevice::ReadOnly) && input.size() <= 64 * 1024 * 1024,
                    "Cannot read authored record: " + entry.toString());
            const auto bytes = input.readAll();
            QString error;
            QString id;
            QString directory;
            QByteArray validated;
            const auto kind = QJsonDocument::fromJson(bytes).object()["format"].toString();
            if (kind == "reload-maker-hand-pose") {
                HandPose value;
                require(decodeHandPose(bytes, value, error), error);
                id = value.id;
                directory = HandPoseLibrary::defaultDirectory();
                validated = encodeHandPose(value, error);
            } else if (kind == "reload-maker-part-mechanism") {
                PartMechanism value;
                require(decodeMechanism(bytes, value, error), error);
                id = value.id;
                directory = MechanismLibrary::defaultDirectory();
                validated = encodeMechanism(value, error);
            } else {
                PartGrip value;
                require(decodePartGrip(bytes, value, error), error);
                id = value.pose.id;
                directory = PartGripLibrary::defaultDirectory();
                validated = encodePartGrip(value, error);
            }
            require(error.isEmpty() && !validated.isEmpty(), error);
            validated = bytes;
            const QString destination = QDir(directory).filePath(id + ".json");
            QFile existing(destination);
            if (existing.exists()) {
                const bool same = existing.open(QIODevice::ReadOnly) &&
                                  QJsonDocument::fromJson(existing.readAll()) == QJsonDocument::fromJson(validated);
                require(same, "A different record already owns this ID: " + destination);
            } else {
                const auto staged = std::find_if(pending.begin(), pending.end(),
                                                 [&](const auto& value) { return value.path == destination; });
                if (staged != pending.end()) {
                    require(QJsonDocument::fromJson(staged->bytes) == QJsonDocument::fromJson(validated),
                            "Conflicting IDs within the import batch: " + destination);
                } else {
                    pending.append({ destination, validated });
                }
            }
            destinations.append(destination);
        }
        for (const auto& file : pending) {
            commands::writeFileAtomically(file.path, file.bytes);
        }
        refreshHandPoseLibrary();
        ++_revision;
        return { { "files", destinations }, { "imported", pending.size() } };
    }

    // Exports stay in the workspace's exports folder; they never deploy.
    QJsonObject ReloadMakerController::exportRecord(const QString& kind)
    {
        if (kind == "scope") {
            return exportScopeProfile();
        }
        QString error;
        QByteArray bytes;
        QString id;
        if (kind == "slide") {
            const auto record = exportSlideProfile();
            require(!record.isEmpty(), "Prepare a pistol simulation with native rig bindings before exporting");
            bytes = QJsonDocument(record).toJson();
            id = _reloadRecipe->id;
        } else if (kind == "grab_locations") {
            const auto record = exportGrabLocations();
            require(!record.isEmpty(), "Configure grab locations first");
            bytes = QJsonDocument(record).toJson();
            id = _reloadRecipe->id;
        } else if (kind == "sounds") {
            const auto record = exportSounds();
            require(!record.isEmpty(), "Prepare a weapon in Operate before exporting sounds");
            bytes = QJsonDocument(record).toJson();
            id = _reloadRecipe->id;
        } else if (kind == "simulation") {
            const auto record = exportSimulation();
            require(!record.isEmpty(), "Prepare a simulation before exporting");
            bytes = QJsonDocument(record).toJson();
            id = _reloadRecipe->id;
        } else if (kind == "runtime_configuration") {
            const auto record = exportRuntimeConfiguration();
            bytes = QJsonDocument(record).toJson();
            id = _reloadRecipe->id;
        } else if (kind == "reload_bundle") {
            require(_reloadRecipe.has_value(), "Learn the configured reload first");
            bytes = exportReloadBundle(error);
            id = _reloadRecipe->id;
        } else if (kind == "reload") {
            require(_reloadRecipe.has_value(), "No reload recipe");
            bytes = encodeReloadRecipe(*_reloadRecipe, error);
            id = _reloadRecipe->id;
        } else if (kind == "grip") {
            require(_partGripDraft.has_value(), "No grip draft");
            bytes = encodePartGrip(*_partGripDraft, error);
            id = _partGripDraft->pose.id;
        } else if (kind == "mechanism") {
            require(_mechanismDraft.has_value(), "No mechanism draft");
            bytes = encodeMechanism(*_mechanismDraft, error);
            id = _mechanismDraft->id;
        } else if (kind == "hand") {
            const auto* pose = selectedHandPose();
            require(pose, "Choose a saved hand pose");
            bytes = encodeHandPose(*pose, error);
            id = pose->id;
        } else {
            require(false, "Export kind must be hand, grip, mechanism, reload, reload_bundle, simulation, slide, "
                           "sounds, grab_locations, scope or runtime_configuration");
        }
        require(error.isEmpty() && !bytes.isEmpty(), error);
        const QString path = studioDataPath("exports/" + kind + "-" + id + ".json");
        commands::writeFileAtomically(path, bytes);
        return { { "path", path }, { "kind", kind }, { "deployed", false } };
    }

    // Version 2 bundles carry the authored operating grips and grab locations.
    QByteArray ReloadMakerController::exportReloadBundle(QString& error)
    {
        const auto magazine = simulationMagazineAnchor();
        QByteArray bytes = encodeReloadBundle(*_reloadRecipe, _partGrips, _mechanisms, _magazineGrabGrips, error,
                                              _magazineGrabDefaults,
                                              magazine.ownerRecord.isEmpty() ? nullptr : &magazine);
        if (bytes.isEmpty()) {
            return bytes;
        }
        auto bundle = QJsonDocument::fromJson(bytes).object();
        const auto interactions = exportInteractionGrips();
        require(!interactions.isEmpty(), "Operating grip assignments could not be resolved for export");
        if (!interactions["operatingGroups"].toArray().isEmpty() || interactions.contains("grabLocations")) {
            bundle["version"] = 2;
            bundle["authoredInteractions"] = interactions;
        }
        return QJsonDocument(bundle).toJson();
    }
}
