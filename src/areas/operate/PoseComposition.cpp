// Composing the displayed pose: the clip's native pose, finger overrides,
// the preview's weapon frame, then the mechanism, simulation, magazine and
// grip overrides, written to the scene nodes.
#include "ReloadMakerController.h"
#include "PoseEvaluator.h"
#include "StudioWindow.h"
#include "data/niftypes.h"
#include "gl/glscene.h"
#include "glview.h"
#include "model/nifmodel.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>

namespace reloadmaker
{
    // Poses the scene at `timeSeconds` of the clip on the timeline. On
    // failure the clip is unloaded and _poseError names the reason.
    bool ReloadMakerController::applyPose(const float timeSeconds)
    {
        _posedSeconds = timeSeconds;
        NifModel* model = activeModel();
        Scene* scene = _window->getGLView()->getScene();
        _poseError = QStringLiteral("Missing model, skeleton, scene or binding");
        if (!model || !scene || !_skeleton.valid() || _nodeBindings.isEmpty() ||
            !sampleActionPose(timeSeconds, _sourceLocalPose, _sourceGlobalPose)) {
            return failPose();
        }
        if (!poseSkeleton() || !applyPartOverrides() || !writeScenePose(model, scene)) {
            return failPose();
        }
        return true;
    }

    // Stops playback and any mechanism preview and unloads the clip.
    bool ReloadMakerController::failPose()
    {
        stopMechanismPreview();
        stopPlayback();
        _timeline->setEnabled(false);
        _clip = {};
        _handPosePreview.reset();
        _partGripDraft.reset();
        _gripPreviewValid = false;
        updateHandPoseControls();
        setStatus(QStringLiteral("Pose evaluation stopped: %1").arg(_poseError), true);
        return false;
    }

    // The skeleton pose: native locals with finger overrides, globals in the
    // preview's weapon frame, then the bound scene poses.
    bool ReloadMakerController::poseSkeleton()
    {
        _localPose = _sourceLocalPose;
        if (!applyFingerOverrides()) {
            return false;
        }
        if (!buildPoseGlobals(_skeleton, _localPose, _globalPose, _poseError)) {
            return false;
        }
        // Fixed mode removes this frame's complete Weapon world transform from
        // every branch. Source mode removes only the Ready world frame: Weapon
        // is allowed to translate/rotate, with the very same local articulation.
        // Reload authoring must honor the fixed Weapon frame for hands and
        // internal parts alike. Firing retains its native recoil presentation.
        const bool simulationFire = _simulationActive && _simulation.action == SimulationAction::Fire;
        const Transformf frame = _weaponFixedMotion && !simulationFire ? _globalPose.at(_weaponBoneIndex) : _readyGlobalPose.at(_weaponBoneIndex);
        if (!reframePose(_skeleton, frame, Transformf{}, _localPose, _globalPose, _poseError)) {
            return false;
        }
        return evaluateSceneBindings(_skeleton, _nodeBindings, _sourceLocalPose, _stateBaselineLocalPose, _globalPose, _boundLocalPose,
                                     _boundGlobalPose, _poseError);
    }

    // Finger drafts and the previewed hand pose show while editing fingers
    // (unless a mechanism preview runs); the operating fingers show while the
    // simulation holds a part.
    bool ReloadMakerController::applyFingerOverrides()
    {
        const bool editingFingers = _workspace == 2 && _handEditorPage == HandEditorPage::FingerPoses;
        if (editingFingers && !_mechanismPreview) {
            for (const auto& draft : _fingerDrafts) {
                if (draft && !applyHandPoseFingers(_skeleton, *draft, _localPose, _poseError)) {
                    return false;
                }
            }
        }
        // Preview is a presentation override only. Preserve the native clip
        // and weapon-part inputs so restoring/scrubbing is non-destructive.
        if (editingFingers && _handPosePreview && !_mechanismPreview &&
            !applyHandPoseFingers(_skeleton, *_handPosePreview, _localPose, _poseError)) {
            return false;
        }
        if (_simulationActive && _simulation.action == SimulationAction::Held) {
            const auto& fingers = _operatingFingers[int(_interactionHand)];
            if (fingers && !applyHandPoseFingers(_skeleton, *fingers, _localPose, _poseError)) {
                return false;
            }
        }
        return true;
    }

    // Weapon-part overrides on the bound poses, in order: mechanism preview,
    // simulation, carried magazine, part grip preview.
    bool ReloadMakerController::applyPartOverrides()
    {
        if (_mechanismPreview && !applyMechanismPreview()) {
            return false;
        }
        if (!applySimulationPose()) {
            return false;
        }
        if (!applyHoldingPreview()) { return false; }
        if (!applyMagazineGrabPose()) {
            return false;
        }
        const bool gripWorkspace = (_workspace == 2 && _handEditorPage == HandEditorPage::PartGrip) || mechanismPageActive();
        bool showGrip = gripWorkspace && _partGripDraft;
        if (showGrip && _mechanismPreview) {
            // During a mechanism preview the grip shows only on the part being
            // taught, or held with pose-on-grab.
            const bool posing = _mechanismTeaching || (_mechanismHeld && _mechanismHandShownOnGrab);
            showGrip = posing && _mechanismDraft && sameMechanismPart(_partGripDraft->pose.placement, _mechanismDraft->anchor);
        }
        if (showGrip && !_reloadWorkspaceActive && !applyPartGripPreview()) {
            return false;
        }
        if (_mechanismPreview && showGrip && !applyMechanismPreview()) {
            return false;
        }
        if (_mechanismRestorePending && !_mechanismPreview) {
            MechanismScenePlan native;
            if (!compileMechanismScene(_assemblyNodes, _nodeBindings, {}, native, _poseError) ||
                !nativeMechanismScene(native, _nodeBindings, _boundLocalPose, _mechanismLocal, _mechanismWorld, _poseError)) {
                return false;
            }
        }
        return true;
    }

    // Writes the pose to the scene: every scene node from the mechanism
    // solution while a mechanism preview runs or is being undone, otherwise
    // the bound nodes and the simulation's visibility.
    bool ReloadMakerController::writeScenePose(NifModel* model, Scene* scene)
    {
        const QSignalBlocker signalBlocker(model);
        if (_mechanismPreview || _mechanismRestorePending) {
            for (int i = 0; i < _sceneNodes.size(); ++i) {
                if (!_sceneNodes[i].isValid() || !setNodeTransform(_sceneNodes[i], _mechanismLocal[i])) {
                    _poseError = QStringLiteral("Cannot write mechanism scene node %1").arg(i);
                    return false;
                }
                if (Node* node = scene->getNode(model, _sceneNodes[i])) {
                    node->update(model, _sceneNodes[i]);
                }
            }
            _mechanismRestorePending = false;
            _window->getGLView()->update();
            return true;
        }
        for (int bindingIndex = 0; bindingIndex < _nodeBindings.size(); ++bindingIndex) {
            const ScenePoseBinding& binding = _nodeBindings.at(bindingIndex);
            const auto& nodeIndex = _sceneNodes.at(binding.nodeIndex);
            if (!nodeIndex.isValid() || !setNodeTransform(nodeIndex, _boundLocalPose.at(bindingIndex))) {
                _poseError = QStringLiteral("Cannot write bound scene node for bone %1").arg(binding.boneIndex);
                return false;
            }
            if (Node* node = scene->getNode(model, nodeIndex)) {
                node->update(model, nodeIndex);
            }
        }
        // Unanimated source parts have no skeleton binding to write above.
        // Restore their original local frame whenever simulation stops or a
        // native clip owns the pose, so custom pulls cannot leak into playback.
        for (const auto& anchor : _handPoseAnchors) {
            if (anchor.description.anchorKind != "animated-part") {
                continue;
            }
            const int nodeIndex = _sceneNodes.indexOf(anchor.node);
            if (nodeIndex < 0 || _assemblyNodes[nodeIndex].boneIndex >= 0) {
                continue;
            }
            const auto pose = _simulationStaticLocalPose.constFind(nodeIndex);
            const auto& local = _simulationActive && pose != _simulationStaticLocalPose.cend() ?
                pose.value() : _assemblyNodes[nodeIndex].restLocal;
            if (!setNodeTransform(anchor.node, local)) {
                _poseError = QStringLiteral("Cannot write static weapon part %1").arg(anchor.description.anchorName);
                return false;
            }
            if (Node* node = scene->getNode(model, anchor.node)) {
                node->update(model, anchor.node);
            }
        }
        applySimulationVisibility();
        _window->getGLView()->update();
        return true;
    }

    // Logs the source, target and rendered world transform of the weapon and
    // hand nodes at the current time (after a timeline drag).
    void ReloadMakerController::capturePoseDiagnostics()
    {
        if (!_clip.valid() || _globalPose.size() != _skeleton.bones.size()) {
            return;
        }
        Scene* scene = _window->getGLView()->getScene();
        NifModel* model = activeModel();
        if (!scene || !model) {
            return;
        }
        const auto text = [](const Transformf& value) {
            return QStringLiteral("t=(%1,%2,%3) q=(%4,%5,%6,%7)")
                .arg(value.translation.x, 0, 'f', 4)
                .arg(value.translation.y, 0, 'f', 4)
                .arg(value.translation.z, 0, 'f', 4)
                .arg(value.rotation.x, 0, 'f', 5)
                .arg(value.rotation.y, 0, 'f', 5)
                .arg(value.rotation.z, 0, 'f', 5)
                .arg(value.rotation.w, 0, 'f', 5);
        };
        appendDiagnostic(QStringLiteral("\nPOSE time=%1 action=%2 mode=%3 transitions=%4\n  file=%5")
                             .arg(_playbackTime, 0, 'f', 4)
                             .arg(_activeAction, motionModeName(),
                                  _approximateTransitions ? QStringLiteral("approximate") : QStringLiteral("off"),
                                  _activeAnimationPath));
        // Read rendered-node locals/worlds, not only the desired skeleton array.
        // Flush only this view's queued render request, not arbitrary UI events.
        _window->getGLView()->update();
        QCoreApplication::sendPostedEvents(_window->getGLView(), QEvent::UpdateRequest);
        for (int bindingIndex = 0; bindingIndex < _nodeBindings.size(); ++bindingIndex) {
            const ScenePoseBinding& binding = _nodeBindings.at(bindingIndex);
            const auto& nodeIndex = _sceneNodes.at(binding.nodeIndex);
            const QString& name = _skeleton.bones.at(binding.boneIndex).name;
            if (!name.startsWith(QStringLiteral("Weapon"), Qt::CaseInsensitive) &&
                !name.endsWith(QStringLiteral("Arm_Hand"), Qt::CaseInsensitive)) {
                continue;
            }
            Node* node = scene->getNode(model, nodeIndex);
            if (!node) {
                continue;
            }
            const Transform& actual = node->worldTrans();
            const Quat q = actual.rotation.toQuat();
            const Transformf rendered{
                { actual.translation[0], actual.translation[1], actual.translation[2] },
                { q[1], q[2], q[3], q[0] },
                { actual.scale, actual.scale, actual.scale },
            };
            appendDiagnostic(QStringLiteral("  %1 block=%2 parent=%3\n    SOURCE %4\n    TARGET %5\n    RENDER %6")
                                 .arg(name)
                                 .arg(model->getBlockNumber(nodeIndex))
                                 .arg(node->parentNode() ? node->parentNode()->id() : -1)
                                 .arg(text(_sourceGlobalPose.at(binding.boneIndex)), text(_boundGlobalPose.at(bindingIndex)), text(rendered)));
        }
    }
}
