#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "commands/CommandSupport.h"
#include "viewport/PartPicker.h"
#include "glview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>

namespace reloadmaker
{
    using commands::number;
    using commands::require;

    void ReloadMakerController::registerLoadoutCommands()
    {
        _commands.add({ "select_weapon", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const int index = params["index"].toInt(-1);
                            require(index >= 0 && index < _weaponSelections.size(), "Unknown weapon index");
                            selectWeapon(index);
                            require(_selectedWeapon == index, "Weapon change failed: " + _lastError);
                            if (!_loadThread) {
                                rebuildWeapon();
                            }
                            return std::nullopt;
                        } });

        _commands.add({ "configure", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            configureLoadout(params);
                            return std::nullopt;
                        } });

        _commands.add({ "action", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const int index = params["index"].toInt(-1);
                            require(index >= 0 && index < _animationActions.size(), "Invalid action index");
                            previewNativeClip(index);
                            return std::nullopt;
                        } });

        _commands.add({ "playback", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            controlPlayback(params);
                            return std::nullopt;
                        } });

        _commands.add({ "view", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            arrangeView(params);
                            return std::nullopt;
                        } });

        _commands.add({ "render", CommandAccess::Read, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return renderView(params);
                        } });

        // Which installed part a click at this viewport point would select.
        _commands.add({ "part_at", CommandAccess::Read, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            return partAtPoint(params);
                        } });

        // The same focus change as clicking a part on the model or its label.
        _commands.add({ "focus_slot", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            bool slotOk = false;
                            const auto slot = params["slot"].toString().toULongLong(&slotOk);
                            const auto* config = configuration();
                            require(slotOk && params["slot"].isString() && config && config->slot(slot),
                                    "Slot must be a decimal-string key from the current state");
                            focusSlot(slot);
                            return std::nullopt;
                        } });
    }

    // Slot and modification IDs are decimal strings because 64-bit slot keys do
    // not survive JSON numbers.
    void ReloadMakerController::configureLoadout(const QJsonObject& params)
    {
        bool slotOk = false;
        bool modOk = false;
        const auto slot = params["slot"].toString().toULongLong(&slotOk);
        const auto mod = params["mod"].toString().toUInt(&modOk);
        if (params.contains("slot")) {
            require(slotOk && modOk && params["slot"].isString() && params["mod"].isString(),
                    "Slot and modification IDs must be decimal strings from the current state");
        }
        if (params.contains("body")) {
            require(isArmsBody(params["body"].toString()), "Unknown body");
        }
        require(!params.contains("animationSet"), "Animation sets follow the equipped OMODs; select weapon parts instead.");
        if (params.contains("reloadNoExtender")) {
            require(params["reloadNoExtender"].isBool(), "reloadNoExtender must be a boolean");
            require(!params.contains("slot"), "Select parts before choosing the optional reload clip");
            require(!params["reloadNoExtender"].toBool() || !glockNoExtenderPath().isEmpty(),
                    "NoExtender requires the standard Glock magazine and a QuickHands set");
            _glockNoExtender = params["reloadNoExtender"].toBool();
        }
        // The rebuild below assembles the new body.
        if (params.contains("body")) {
            _armsBody = params["body"].toString();
            showArmsBody();
        }
        if (params.contains("slot")) {
            auto* config = configuration();
            require(config && config->select(slot, mod), "Invalid slot or modification");
            refreshAttachments();
        }
        rebuildWeapon();
    }

    void ReloadMakerController::controlPlayback(const QJsonObject& params)
    {
        invalidateCollisions();
        require(_clip.valid(), "Load a configured animation first");
        if (params.contains("mode")) {
            const int mode = params["mode"].toInt(-1);
            require(mode == 0 || mode == 1, "Mode must be 0 fixed or 1 native");
            setWeaponFixedMotion(mode == 0);
        }
        if (params.contains("seconds")) {
            const float seconds = number(params["seconds"]);
            const float duration = _reloadWorkspaceActive && _reloadRecipe ? _reloadRecipe->duration : playbackDuration();
            require(seconds >= 0 && seconds <= duration, "Time is outside this animation");
            _playing = false;
            _playbackTimer->stop();
            if (_reloadWorkspaceActive) {
                _reloadPlaying = false;
                _reloadPreviewTimer->stop();
                seekReload(seconds);
            } else {
                _playbackTime = seconds;
                require(applyPose(seconds), _poseError);
                updateTimelineText();
            }
        }
        if (params.contains("playing")) {
            const bool playing = params["playing"].toBool();
            if (_reloadWorkspaceActive) {
                require(_reloadRecipe.has_value(), "Learn the reload first");
                setReloadPlaying(playing);
            } else if (playing != _playing) {
                togglePlayback();
            }
        }
        if (params.contains("loop")) {
            setLoopPlayback(params["loop"].toBool());
        }
    }

    void ReloadMakerController::arrangeView(const QJsonObject& params)
    {
        auto* viewport = _window->getGLView();
        if (params.contains("camera")) {
            require(viewport->studioRestoreCamera(params["camera"].toObject()), "Invalid camera state");
        }
        const QString view = params["view"].toString("current");
        const QHash<QString, GLView::ViewState> views{ { "front", GLView::ViewFront }, { "back", GLView::ViewBack },
                                                       { "side", GLView::ViewRight },  { "top", GLView::ViewTop },
                                                       { "iso", GLView::ViewDefault } };
        if (view != "current") {
            require(views.contains(view), "Unknown camera view");
            viewport->setOrientation(views[view], false);
            if (view == "iso") {
                viewport->setRotation(-65, 0, 135);
            }
        }
        if (params["fit"].toBool()) {
            require(fitWeapon(), "No weapon bounds available for Fit");
        }
        if (params.contains("zoom")) {
            const float zoom = number(params["zoom"]);
            require(zoom > .05f && zoom < 20, "Zoom outside supported range");
            viewport->setZoom(zoom);
        }
    }

    // Renders into the evidence folder. With preserveCamera the camera returns to
    // its previous state afterwards, even when arranging the view fails.
    QJsonObject ReloadMakerController::renderView(const QJsonObject& params)
    {
        auto* viewport = _window->getGLView();
        const auto previousCamera = viewport->studioCameraState();
        const bool preserve = params["preserveCamera"].toBool();
        const auto restoreCamera = qScopeGuard([&] {
            if (preserve) {
                (void)viewport->studioRestoreCamera(previousCamera);
            }
        });
        arrangeView(params);
        const QString view = params["view"].toString("current");
        const QString path = studioDataPath("evidence/view-" + QString::number(_revision) + "-" + view + ".png");
        const auto image = viewport->studioCaptureFrame();
        require(!image.isNull() && image.save(path), "Cannot capture current native frame");
        return { { "path", path }, { "seconds", _playbackTime }, { "width", image.width() }, { "height", image.height() } };
    }

    // Points are logical viewport pixels, the same space as mouse events.
    QJsonObject ReloadMakerController::partAtPoint(const QJsonObject& params)
    {
        const auto* viewport = _window->getGLView();
        require(params["x"].isDouble() && params["y"].isDouble(), "x and y viewport pixels are required");
        const float x = number(params["x"]);
        const float y = number(params["y"]);
        require(x >= 0 && y >= 0 && x < viewport->width() && y < viewport->height(), "Point is outside the viewport");
        QJsonObject result{ { "slot", QJsonValue::Null }, { "width", viewport->width() }, { "height", viewport->height() } };
        const auto* config = configuration();
        const auto slot = _partPicker->partAt(QPointF(x, y));
        const auto* entry = slot && config ? config->slot(*slot) : nullptr;
        if (!entry) {
            return result;
        }
        const auto* mod = config->mod(config->selected(*slot));
        result["slot"] = QString::number(*slot);
        result["label"] = entry->label;
        result["part"] = mod ? (mod->name.isEmpty() ? mod->editorId : mod->name) : QString();
        result["editorId"] = mod ? mod->editorId : QString();
        return result;
    }
}
