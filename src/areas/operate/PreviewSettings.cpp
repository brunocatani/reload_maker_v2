// Preview and Operate settings saved with the project: slide spring return,
// weapon-fixed motion, approximate clip transitions, arms shown and the
// QuickHands NoExtender reload. They are
// document fields; their controls only show them and report user changes.
// Each setter does what the control's change used to trigger, and only when
// the value changes.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "gl/glscene.h"
#include "glview.h"
#include "model/nifmodel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>

namespace reloadmaker
{
    namespace
    {
        void showChecked(QCheckBox* box, bool checked)
        {
            if (box) {
                const QSignalBlocker block(box);
                box->setChecked(checked);
            }
        }
    }

    QString ReloadMakerController::motionModeName() const
    {
        return _weaponFixedMotion ? QStringLiteral("Weapon fixed") : QStringLiteral("Full native motion");
    }

    void ReloadMakerController::setSlideSpringReturn(bool enabled)
    {
        if (enabled == _slideSpringReturn) {
            return;
        }
        _slideSpringReturn = enabled;
        showChecked(_simulationSpring, enabled);
        noteDocumentChange("slide spring return");
    }

    // A view option: not saved, and it changes no document state. Re-posing
    // records the native poses (or clears the ghosts) for the current state.
    void ReloadMakerController::setNativeGhost(bool enabled)
    {
        if (enabled == _nativeGhost) {
            return;
        }
        _nativeGhost = enabled;
        showChecked(_simulationGhost, enabled);
        if (_simulationActive) {
            const bool handHeld = _simulation.action == SimulationAction::Held || _simulation.grab();
            (void)applyPose(handHeld ? 0.0f : simulationPoseSeconds());
        }
    }

    void ReloadMakerController::setWeaponFixedMotion(bool fixed)
    {
        if (fixed == _weaponFixedMotion) {
            return;
        }
        _weaponFixedMotion = fixed;
        showWeaponFixedMotion();
        refreshPoseMode();
        noteDocumentChange("motion mode");
    }

    void ReloadMakerController::showWeaponFixedMotion()
    {
        if (_motionMode) {
            const QSignalBlocker block(_motionMode);
            _motionMode->setCurrentIndex(_weaponFixedMotion ? 0 : 1);
        }
    }

    void ReloadMakerController::setApproximateTransitions(bool enabled)
    {
        if (enabled == _approximateTransitions) {
            return;
        }
        _approximateTransitions = enabled;
        showChecked(_transitionCheck, enabled);
        refreshPoseMode();
        noteDocumentChange("clip transitions");
    }

    void ReloadMakerController::setArmsShown(bool shown)
    {
        if (shown == _armsShown) {
            return;
        }
        _armsShown = shown;
        showChecked(_showArmsCheck, shown);
        setArmsVisible(shown);
        noteDocumentChange("arms shown");
    }

    // The caller reassembles the weapon, which picks the reload clip.
    void ReloadMakerController::setGlockNoExtender(bool enabled)
    {
        if (enabled == _glockNoExtender) {
            return;
        }
        _glockNoExtender = enabled;
        showChecked(_simulationNoExtender, enabled);
        noteDocumentChange("NoExtender reload");
    }

    // Re-poses the current clip after a motion or transition change.
    void ReloadMakerController::refreshPoseMode()
    {
        if (!_clip.valid()) {
            return;
        }
        _playbackTime = std::min(_playbackTime, playbackDuration());
        if (applyPose(_playbackTime)) {
            updateTimelineText();
            showClipDetails();
            setStatus(QStringLiteral("%1 — %2 — %3 s; offline clip preview")
                          .arg(_activeAction, motionModeName())
                          .arg(playbackDuration(), 0, 'f', 3));
        }
    }

    // Shows or hides the first-person body and hands roots; the weapon
    // animation keeps running either way.
    void ReloadMakerController::setArmsVisible(const bool visible)
    {
        NifModel* model = activeModel();
        Scene* scene = _window ? _window->getGLView()->getScene() : nullptr;
        if (!model || !scene) {
            return;
        }
        const auto updateRoot = [model, scene, visible](const QPersistentModelIndex& root, const int originalFlags) {
            if (!root.isValid()) {
                return;
            }
            const int flags = visible ? originalFlags : originalFlags | 1;
            if (!model->set<int>(root, "Flags", flags)) {
                return;
            }
            if (Node* node = scene->getNode(model, root)) {
                node->update(model, root);
            }
        };
        const QSignalBlocker signalBlocker(model);
        updateRoot(_bodyRoot, _bodyRootFlags);
        updateRoot(_handsRoot, _handsRootFlags);
        _window->getGLView()->update();
        appendDiagnostic(visible ? QStringLiteral("VIEW  arms and hands visible")
                                 : QStringLiteral("VIEW  arms and hands hidden; weapon animation remains active"));
    }
}
