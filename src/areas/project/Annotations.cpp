// Point-and-note pins (rework 03 §4). A person right-clicks a part and writes
// what a point means; a command client (the AI) pins the same way by viewport
// point. Pins are saved with the project, follow their part through animation
// and stay listed when their part is not installed. UI and commands share the
// mutations below; commands add the parameter checks.
#include "LoadoutView.h"
#include "PoseEvaluator.h"
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "commands/CommandSupport.h"
#include "glview.h"
#include "gl/glnode.h"
#include "gl/glscene.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include "viewport/PinOverlay.h"

#include <QInputDialog>
#include <QJsonArray>
#include <QMenu>
#include <QUuid>

namespace reloadmaker
{
    using commands::number;
    using commands::require;

    // The part surface under a viewport point and the point in its shape's
    // frame: everything a pin needs except its note.
    std::optional<Pin> ReloadMakerController::pinAnchorAt(QPointF point, QString& error) const
    {
        auto* view = _window->getGLView();
        auto* model = activeModel();
        auto* scene = view->getScene();
        const auto* config = configuration();
        if (!model || !scene || !config) {
            error = QStringLiteral("No weapon is shown.");
            return std::nullopt;
        }
        QVector<int> visible;
        for (const auto* shape : scene->shapes) {
            if (shape && !shape->isHidden()) {
                visible.append(shape->id());
            }
        }
        int block = -1;
        Vector3 world;
        if (!view->reloadMakerSurfaceAt(point, visible, block, world)) {
            error = QStringLiteral("Point at a part of the weapon.");
            return std::nullopt;
        }
        const auto index = model->getBlockIndex(block);
        const auto name = model->get<QString>(index, "Name");
        for (auto it = _partSurfaces.cbegin(); it != _partSurfaces.cend(); ++it) {
            int ordinal = 0;
            bool found = false;
            for (const auto& surface : it.value()) {
                if (!surface.isValid() || surface.model() != model) {
                    continue;
                }
                if (model->getBlockNumber(surface) == block) {
                    found = true;
                    break;
                }
                ordinal += model->get<QString>(surface, "Name") == name ? 1 : 0;
            }
            if (!found) {
                continue;
            }
            const auto* node = scene->getNode(model, index);
            if (!node || name.isEmpty()) {
                break;
            }
            const auto local = node->worldTrans().inverted() * world;
            Pin pin;
            pin.slot = it.key();
            pin.part = config->selected(it.key());
            pin.shape = name;
            pin.shapeIndex = ordinal;
            pin.offset = { local[0], local[1], local[2] };
            return pin;
        }
        error = QStringLiteral("Pins attach to installed parts; point at one.");
        return std::nullopt;
    }

    // The shape a pin is anchored to in the current scene; invalid when its
    // part is not installed or the shape is gone.
    QPersistentModelIndex ReloadMakerController::pinShape(const Pin& pin) const
    {
        const auto* config = configuration();
        auto* model = activeModel();
        if (!config || !model || config->selected(pin.slot) != pin.part) {
            return {};
        }
        int ordinal = 0;
        for (const auto& surface : _partSurfaces.value(pin.slot)) {
            if (!surface.isValid() || surface.model() != model || model->get<QString>(surface, "Name") != pin.shape) {
                continue;
            }
            if (ordinal++ == pin.shapeIndex) {
                return surface;
            }
        }
        return {};
    }

    PinMarker ReloadMakerController::pinMarker(int index) const
    {
        const auto& pin = _pins[index];
        return { pin.id, index + 1, pin.text, pin.resolved, pinShape(pin), QVector3D(pin.offset.x, pin.offset.y, pin.offset.z) };
    }

    void ReloadMakerController::refreshPins()
    {
        if (!_pinOverlay) {
            return;
        }
        QVector<PinMarker> markers;
        for (int i = 0; i < _pins.size(); ++i) {
            auto marker = pinMarker(i);
            if (marker.shape.isValid()) {
                markers.append(std::move(marker));
            }
        }
        _pinOverlay->setMarkers(std::move(markers));
        _pinOverlay->setSelected(_selectedPin);
    }

    int ReloadMakerController::pinIndex(const QString& id) const
    {
        for (int i = 0; i < _pins.size(); ++i) {
            if (_pins[i].id == id) {
                return i;
            }
        }
        return -1;
    }

    bool ReloadMakerController::addPin(Pin anchor, const QString& text, QString& error)
    {
        const auto note = pinText(text);
        if (note.isEmpty()) {
            error = QStringLiteral("Write a note of up to %1 characters.").arg(kMaximumPinText);
            return false;
        }
        if (_pins.size() >= kMaximumPins) {
            error = QStringLiteral("This weapon already has %1 pins.").arg(kMaximumPins);
            return false;
        }
        anchor.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        anchor.text = note;
        anchor.author = _executingCommand ? PinAuthor::Ai : PinAuthor::Human;
        anchor.resolved = false;
        anchor.resolution.clear();
        _pins.append(std::move(anchor));
        _selectedPin = _pins.back().id;
        noteDocumentChange(QStringLiteral("pin added"));
        refreshPins();
        return true;
    }

    bool ReloadMakerController::editPin(const QString& id, const QString& text, QString& error)
    {
        const int index = pinIndex(id);
        const auto note = pinText(text);
        if (index < 0 || note.isEmpty()) {
            error = index < 0 ? QStringLiteral("Unknown pin.") : QStringLiteral("Write a note of up to %1 characters.").arg(kMaximumPinText);
            return false;
        }
        if (_pins[index].text != note) {
            _pins[index].text = note;
            noteDocumentChange(QStringLiteral("pin note"));
            refreshPins();
        }
        return true;
    }

    // A resolution says what resolved the pin, for example the property a
    // command changed; resolving from the editor leaves it empty.
    bool ReloadMakerController::setPinResolved(const QString& id, bool resolved, const QString& resolution, QString& error)
    {
        const int index = pinIndex(id);
        if (index < 0 || resolution.size() > kMaximumPinText) {
            error = index < 0 ? QStringLiteral("Unknown pin.") : QStringLiteral("The resolution is too long.");
            return false;
        }
        auto& pin = _pins[index];
        pin.resolved = resolved;
        pin.resolution = resolved ? resolution.trimmed() : QString();
        noteDocumentChange(resolved ? QStringLiteral("pin resolved") : QStringLiteral("pin reopened"));
        refreshPins();
        return true;
    }

    bool ReloadMakerController::removePin(const QString& id, QString& error)
    {
        const int index = pinIndex(id);
        if (index < 0) {
            error = QStringLiteral("Unknown pin.");
            return false;
        }
        _pins.removeAt(index);
        if (_selectedPin == id) {
            _selectedPin.clear();
        }
        noteDocumentChange(QStringLiteral("pin removed"));
        refreshPins();
        return true;
    }

    // Selecting is a view state: it shows the whole note, not a document edit.
    void ReloadMakerController::selectPin(const QString& id)
    {
        const int index = pinIndex(id);
        if (index < 0) {
            return;
        }
        _selectedPin = id;
        if (_pinOverlay) {
            _pinOverlay->setSelected(id);
        }
        const auto& pin = _pins[index];
        setStatus(QStringLiteral("Pin %1%2 · %3").arg(index + 1).arg(pin.resolved ? QStringLiteral(" (resolved)") : QString(), pin.text));
    }

    void ReloadMakerController::showPinMenu(const QString& id, QPoint globalPosition)
    {
        const int index = pinIndex(id);
        if (index < 0) {
            return;
        }
        selectPin(id);
        const bool resolved = _pins[index].resolved;
        QMenu menu(_window);
        auto* title = menu.addAction(QStringLiteral("Pin %1").arg(index + 1));
        title->setEnabled(false);
        menu.addSeparator();
        auto* edit = menu.addAction(QStringLiteral("Edit note…"));
        auto* resolve = menu.addAction(resolved ? QStringLiteral("Reopen") : QStringLiteral("Resolve"));
        auto* remove = menu.addAction(QStringLiteral("Remove"));
        const auto* chosen = menu.exec(globalPosition);
        QString error;
        bool done = true;
        if (chosen == edit) {
            bool accepted = false;
            const auto text = QInputDialog::getMultiLineText(_window, QStringLiteral("Edit pin"), QStringLiteral("Note:"),
                                                              _pins[index].text, &accepted);
            done = !accepted || editPin(id, text, error);
        } else if (chosen == resolve) {
            done = setPinResolved(id, !resolved, {}, error);
        } else if (chosen == remove) {
            done = removePin(id, error);
        }
        if (!done) {
            setStatus(error, true);
        }
    }

    // Asks for the note of a pin at an anchor picked before any dialog opened.
    void ReloadMakerController::askNewPin(const Pin& anchor)
    {
        bool accepted = false;
        const auto text = QInputDialog::getMultiLineText(_window, QStringLiteral("Add pin"),
                                                          QStringLiteral("What does this point mean?"), {}, &accepted);
        QString error;
        if (accepted && !addPin(anchor, text, error)) {
            setStatus(error, true);
        }
    }

    QJsonObject ReloadMakerController::annotationsState() const
    {
        const auto* config = configuration();
        QJsonArray list;
        for (int i = 0; i < _pins.size(); ++i) {
            const auto& pin = _pins[i];
            auto json = pinJson(pin);
            json["number"] = i + 1;
            const auto* slot = config ? config->slot(pin.slot) : nullptr;
            const auto* mod = config ? config->mod(pin.part) : nullptr;
            json["slotLabel"] = slot ? slot->label : QString();
            json["partName"] = mod ? cleanPartName(mod->name.isEmpty() ? mod->editorId : mod->name) : QString();
            const auto marker = pinMarker(i);
            json["placed"] = marker.shape.isValid();
            if (const auto point = marker.shape.isValid() && _pinOverlay ? _pinOverlay->screenPosition(marker) : std::nullopt) {
                json["screen"] = QJsonArray{ point->x(), point->y() };
            }
            // Where the pinned point is now in weapon space, independent of the
            // camera: it moves when the part does.
            Transformf weaponInverse;
            const auto world = marker.shape.isValid() && _pinOverlay ? _pinOverlay->worldPosition(marker) : std::nullopt;
            if (world && _weaponBoneIndex >= 0 && _weaponBoneIndex < _globalPose.size() &&
                invertUniformTransform(_globalPose[_weaponBoneIndex], weaponInverse)) {
                const auto local = composed(weaponInverse, Transformf{ { world->x(), world->y(), world->z() } }).translation;
                json["position"] = QJsonArray{ local.x, local.y, local.z };
            }
            list.append(json);
        }
        return { { "annotations", list }, { "selected", _selectedPin } };
    }

    void ReloadMakerController::registerAnnotationCommands()
    {
        _commands.add({ "annotations", CommandAccess::Read, CommandGate::Always,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> { return annotationsState(); } });

        _commands.add({ "annotation", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto operation = params["operation"].toString();
                            const auto id = params["id"].toString();
                            QString error;
                            if (operation == "add") {
                                const auto* viewport = _window->getGLView();
                                require(params["x"].isDouble() && params["y"].isDouble(), "x and y viewport pixels are required");
                                const float x = number(params["x"]), y = number(params["y"]);
                                require(x >= 0 && y >= 0 && x < viewport->width() && y < viewport->height(), "Point is outside the viewport");
                                const auto anchor = pinAnchorAt(QPointF(x, y), error);
                                require(anchor.has_value(), error);
                                require(addPin(*anchor, params["text"].toString(), error), error);
                                auto state = annotationsState();
                                state["id"] = _pins.back().id;
                                return state;
                            }
                            if (operation == "edit") {
                                require(editPin(id, params["text"].toString(), error), error);
                            } else if (operation == "resolve") {
                                require(setPinResolved(id, true, params["reference"].toString(), error), error);
                            } else if (operation == "reopen") {
                                require(setPinResolved(id, false, {}, error), error);
                            } else if (operation == "remove") {
                                require(removePin(id, error), error);
                            } else {
                                require(false, "Unknown annotation operation; use add, edit, resolve, reopen or remove");
                            }
                            return annotationsState();
                        } });
    }

    // ---- Project file ----

    QJsonArray ReloadMakerController::pinsJson() const
    {
        QJsonArray pins;
        for (const auto& pin : _pins) {
            pins.append(pinJson(pin));
        }
        return pins;
    }

    // The record was validated when it was opened (readPins in validWeaponRecord).
    void ReloadMakerController::restorePins(const QJsonObject& record)
    {
        QString error;
        if (!readPins(record["annotations"], _pins, error)) {
            appendDiagnostic(QStringLiteral("PROJECT: saved pins dropped: ") + error);
        }
        _selectedPin.clear();
        refreshPins();
    }
}
