#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "commands/CommandSupport.h"
#include "glview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

#include <memory>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        // A single request line may not exceed this; larger input aborts the client.
        constexpr qsizetype kMaximumRequestBytes = 4 * 1024 * 1024;

        QString commandSession()
        {
            return QCoreApplication::instance()->property("commandSession").toString();
        }
    }

    void ReloadMakerController::registerCommands()
    {
        registerDocumentCommands();
        registerSourceCommands();
        registerLoadoutCommands();
        registerSimulationCommands();
        registerHandCommands();
        registerMagazineCommands();
        registerOperatingCommands();
        registerReloadCommands();
        registerSoundCommands();
        registerGrabLocationCommands();
        registerCollisionCommands();
        registerScopeCommands();
        registerPackCommands();
    }

    void ReloadMakerController::startCommandServer()
    {
        registerCommands();
        auto* server = new QLocalServer(this);
        server->setSocketOptions(QLocalServer::UserAccessOption);
        const auto pipe = QCoreApplication::instance()->property("commandPipe").toString();
        const auto session = commandSession();
        require(!pipe.isEmpty() && !session.isEmpty() && server->listen(pipe),
                "Another Reload Maker owns this workspace's command pipe");
        const QJsonObject endpoint{ { "version", 1 }, { "pipe", pipe }, { "session", session },
                                    { "workspace", studioRoot() },
                                    { "pid", double(QCoreApplication::applicationPid()) } };
        commands::writeFileAtomically(studioDataPath("local/settings/endpoint.json"), QJsonDocument(endpoint).toJson());
        connect(server, &QLocalServer::newConnection, this, [this, server] {
            while (server->hasPendingConnections()) {
                acceptCommandClient(server->nextPendingConnection());
            }
        });
        // UI edits and command edits share the same generation; clients must read
        // current state instead of reusing selections from an earlier scene.
        for (auto* widget : _window->findChildren<QComboBox*>()) {
            connect(widget, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { ++_revision; });
        }
        for (auto* widget : _window->findChildren<QDoubleSpinBox*>()) {
            connect(widget, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { ++_revision; });
        }
        for (auto* widget : _window->findChildren<QLineEdit*>()) {
            connect(widget, &QLineEdit::textEdited, this, [this] { ++_revision; });
        }
        for (auto* widget : _window->findChildren<QCheckBox*>()) {
            connect(widget, &QCheckBox::toggled, this, [this] { ++_revision; });
        }
        for (auto* widget : _window->findChildren<QListWidget*>()) {
            connect(widget, &QListWidget::currentRowChanged, this, [this] { ++_revision; });
        }
    }

    // Requests are newline-delimited JSON objects; every request gets exactly one
    // newline-delimited response in order.
    void ReloadMakerController::acceptCommandClient(QLocalSocket* socket)
    {
        socket->setParent(this);
        auto pending = std::make_shared<QByteArray>();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, pending] {
            pending->append(socket->readAll());
            if (pending->size() > kMaximumRequestBytes) {
                socket->abort();
                return;
            }
            qsizetype newline;
            while ((newline = pending->indexOf('\n')) >= 0) {
                const auto line = pending->left(newline);
                pending->remove(0, newline + 1);
                socket->write(QJsonDocument(answerCommand(line)).toJson(QJsonDocument::Compact) + '\n');
                socket->flush();
            }
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }

    QJsonObject ReloadMakerController::answerCommand(const QByteArray& line)
    {
        QJsonObject response;
        try {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid command JSON");
            response = executeCommand(document.object());
            response["ok"] = true;
        } catch (const std::exception& error) {
            response = { { "ok", false }, { "error", QString::fromUtf8(error.what()) } };
        }
        // Saved mechanisms/recipes own their revision field. Do not overwrite it
        // with the live document generation.
        response["documentRevision"] = double(_revision);
        response["session"] = commandSession();
        if (!response.contains("revision")) {
            response["revision"] = double(_revision);
        }
        return response;
    }

    QJsonObject ReloadMakerController::executeCommand(const QJsonObject& request)
    {
        const QString method = request["method"].toString();
        const auto params = request["params"].toObject();
        if (request.contains("expectedSession")) {
            require(request["expectedSession"].toString() == commandSession(),
                    "Editor session changed; read state before editing");
        }
        const auto* command = _commands.find(method);
        require(command != nullptr, "Unknown studio command: " + method);
        if (command->access == CommandAccess::Edit && request.contains("expectedRevision")) {
            require(request["expectedRevision"].toDouble() == double(_revision),
                    "Stale document revision; read state again");
        }
        if (command->gate == CommandGate::WhenIdle) {
            requireIdle();
            _lastError.clear();
        }
        if (auto response = command->run(params)) {
            return *response;
        }
        require(_lastError.isEmpty(), _lastError);
        ++_revision;
        return studioState();
    }

    void ReloadMakerController::requireIdle() const
    {
        require(!_loadThread && !_reloadCapturing && !_rebuildTimer->isActive(),
                "Source loading, assembly or reload capture is in progress; read state or cancel");
    }

    QJsonObject ReloadMakerController::studioState() const
    {
        QJsonArray weapons;
        for (int i = 0; i < _weaponSelections.size(); ++i) {
            const auto& selection = _weaponSelections[i];
            const auto& plugin = _plugins[selection.pluginIndex];
            const auto& weapon = plugin.weapons[selection.weaponIndex];
            weapons.append(QJsonObject{ { "index", i }, { "name", weapon.name }, { "editorId", weapon.editorId },
                                        { "formId", QString::number(weapon.formId, 16) },
                                        { "plugin", plugin.fileName } });
        }
        QJsonArray actions;
        for (int i = 0; i < _animationList->count(); ++i) {
            actions.append(QJsonObject{ { "index", i }, { "name", _animationList->item(i)->text() },
                                        { "path", _animationList->item(i)->data(Qt::UserRole).toString() } });
        }
        QJsonArray configurationSlots;
        if (const auto* config = configuration()) {
            for (const auto& slot : config->entries()) {
                QJsonArray choices;
                for (auto id : slot.choices) {
                    if (const auto* mod = config->mod(id)) {
                        choices.append(QJsonObject{ { "id", QString::number(id) }, { "record", poseRecordKey(id) },
                                                    { "name", mod->name }, { "editorId", mod->editorId },
                                                    { "model", mod->modelPath } });
                    }
                }
                configurationSlots.append(QJsonObject{ { "key", QString::number(slot.key) }, { "name", slot.label },
                                                       { "selected", QString::number(config->selected(slot.key)) },
                                                       { "choices", choices } });
            }
        }
        QJsonArray anchors;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            anchors.append(QJsonObject{ { "index", i }, { "name", _handPoseAnchor->itemText(i) },
                                        { "kind", _handPoseAnchors[i].description.anchorKind } });
        }
        QJsonArray poses;
        for (const auto& pose : _savedHandPoses) {
            poses.append(QJsonObject{ { "id", pose.id }, { "name", pose.name },
                                      { "hand", pose.hand == PoseHand::Left ? "left" : "right" } });
        }
        QJsonArray grips;
        for (const auto& grip : _partGrips) {
            grips.append(QJsonObject{ { "id", grip.pose.id }, { "name", grip.pose.name },
                                      { "part", grip.pose.placement.anchorName } });
        }
        QJsonObject magazineGrabs;
        for (auto it = _magazineGrabGrips.cbegin(); it != _magazineGrabGrips.cend(); ++it) {
            magazineGrabs[it.key()] = it.value();
        }
        const bool busy = bool(_loadThread) || _reloadCapturing || _reloadCaptureQueued || _rebuildTimer->isActive();
        return { { "revision", double(_revision) },
                 { "session", commandSession() },
                 { "busy", busy },
                 { "loading", bool(_loadThread) },
                 { "capturing", _reloadCapturing },
                 { "source", _package.sourcePath },
                 { "gameData", _gameData },
                 { "library", _libraryMode },
                 { "workspace", _workspace },
                 { "weaponIndex", _weaponCombo->currentIndex() },
                 { "weapons", weapons },
                 { "slots", configurationSlots },
                 { "focusedSlot", QString::number(_focusedSlot) },
                 { "body", _bodyCombo->currentData().toString() },
                 { "animationSource", animationSourceIdentity() },
                 { "actions", actions },
                 { "action", _activeAction },
                 { "animation", _activeAnimationPath },
                 { "seconds", _playbackTime },
                 { "duration", playbackDuration() },
                 { "playing", _playing || _reloadPlaying },
                 { "motionMode", _motionMode->currentIndex() },
                 { "anchors", anchors },
                 { "interaction", interactionGripState() },
                 { "handPoses", poses },
                 { "partGrips", grips },
                 { "magazineGrabGrips", magazineGrabs },
                 { "magazineEditor", magazineEditorState() },
                 { "recipeWindows", _reloadRecipe ? int(_reloadRecipe->windows.size()) : 0 },
                 { "recipeTracks", _reloadRecipe ? int(_reloadRecipe->tracks.size()) : 0 },
                 { "status", _window->statusBar()->currentMessage() },
                 { "lastError", _lastError },
                 { "project", _sessionPath },
                 { "suggestedProject", suggestedSessionPath() },
                 { "camera", _window->getGLView()->studioCameraState() },
                 { "nativeApplicationShell", false } };
    }
}
