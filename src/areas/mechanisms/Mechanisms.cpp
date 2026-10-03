// Shared part definitions (mechanisms): a weapon part's movement steps and
// the parts that follow it. Loading and saving definitions, and testing a
// definition: teaching steps by dragging, grabbing and releasing the part,
// and the return motion. Also suggests followers from the animation.
#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioWindow.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"

#include <QScopedValueRollback>
#include <QSet>
#include <QVector3D>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    // The one installed reference part matching a saved anchor.
    int ReloadMakerController::resolveMechanismAnchor(const HandPosePlacement& anchor, QString& error) const
    {
        int found = -1;
        for (int i = 1; i < _handPoseAnchors.size(); ++i) {
            if (!sameGripAnchor(anchor, _handPoseAnchors[i].description)) {
                continue;
            }
            if (found >= 0) {
                error = "Part frame is ambiguous";
                return -1;
            }
            found = i;
        }
        if (found < 0) {
            error = "Part frame is not installed";
        }
        return found;
    }

    void ReloadMakerController::refreshMechanisms()
    {
        QStringList errors;
        _mechanisms = _mechanismLibrary.load(errors);
        for (const auto& e : errors) {
            appendDiagnostic("MECHANISM FILE " + e);
        }
        refreshMechanismChoices();
    }

    // This weapon's saved definitions; the selection stays on the selected
    // one while it is listed, else the first. A draft for another weapon is
    // dropped.
    void ReloadMakerController::refreshMechanismChoices()
    {
        if (!_mechanismList) {
            return;
        }
        const auto listed = weaponMechanisms();
        const bool kept = std::any_of(listed.cbegin(), listed.cend(), [&](const auto* p) { return p->id == _selectedMechanismId; });
        if (!kept) {
            _selectedMechanismId = listed.isEmpty() ? QString() : listed.first()->id;
        }
        showMechanismChoices();
        const auto* weapon = currentWeapon();
        const QString record = weapon ? poseRecordKey(weapon->formId) : QString();
        if (_mechanismDraft && _mechanismDraft->weaponRecord.compare(record, Qt::CaseInsensitive) != 0) {
            stopMechanismPreview();
            _mechanismDraft.reset();
            displayMechanism();
        } else if (!_mechanismPreview) {
            displayMechanism();
        }
        refreshWorkshopChoices();
        updateMechanismControls();
    }

    QVector<const PartMechanism*> ReloadMakerController::weaponMechanisms() const
    {
        const auto* weapon = currentWeapon();
        const QString record = weapon ? poseRecordKey(weapon->formId) : QString();
        QVector<const PartMechanism*> listed;
        for (const auto& p : _mechanisms) {
            if (p.weaponRecord.compare(record, Qt::CaseInsensitive) == 0) {
                listed.append(&p);
            }
        }
        return listed;
    }

    // The part grip form's part; the first part without one.
    void ReloadMakerController::openMechanismForPart()
    {
        selectWorkshopPart(std::max(_gripAnchor, 0));
    }

    void ReloadMakerController::loadMechanism()
    {
        for (const auto& p : _mechanisms) {
            if (p.id != _selectedMechanismId) {
                continue;
            }
            QString error;
            const int index = resolveMechanismAnchor(p.anchor, error);
            if (index < 0) {
                setStatus(error, true);
                return;
            }
            stopMechanismPreview();
            _mechanismDraft.reset();
            selectWorkshopPart(index);
            return;
        }
    }

    // Saves the draft after checking its part and followers form a valid
    // scene group. A grip draft on the same part links to it.
    void ReloadMakerController::saveMechanism()
    {
        readMechanismForm();
        if (!_mechanismDraft || !_clip.valid() || _loadThread || _mechanismPreview) {
            return;
        }
        QString error;
        QVector<int> roots;
        const int master = resolveMechanismAnchor(_mechanismDraft->anchor, error);
        if (master < 0) {
            setStatus(error, true);
            return;
        }
        roots.append(_sceneNodes.indexOf(_handPoseAnchors[master].node));
        for (const auto& f : _mechanismDraft->followers) {
            const int i = resolveMechanismAnchor(f.anchor, error);
            if (i < 0) {
                setStatus(error, true);
                return;
            }
            roots.append(_sceneNodes.indexOf(_handPoseAnchors[i].node));
        }
        MechanismScenePlan plan;
        if (!compileMechanismScene(_assemblyNodes, _nodeBindings, roots, plan, error)) {
            setStatus(error, true);
            return;
        }
        if (!_mechanismLibrary.save(*_mechanismDraft, error)) {
            setStatus(error, true);
            return;
        }
        if (_partGripDraft && _partGripDraft->weaponRecord.compare(_mechanismDraft->weaponRecord, Qt::CaseInsensitive) == 0 &&
            sameMechanismPart(_partGripDraft->pose.placement, _mechanismDraft->anchor)) {
            _partGripDraft->mechanismId = _mechanismDraft->id;
        }
        refreshMechanisms();
        if (mechanismListed(_mechanismDraft->id)) {
            selectMechanism(_mechanismDraft->id);
        } else {
            selectMechanism(QString());
        }
        setStatus(QStringLiteral("Saved shared part definition, revision %1. Save the hand grip to persist its definition link.").arg(_mechanismDraft->revision));
        updatePartGripControls();
    }

    bool ReloadMakerController::mechanismListed(const QString& id) const
    {
        const auto listed = weaponMechanisms();
        return std::any_of(listed.cbegin(), listed.cend(), [&](const auto* p) { return p->id == id; });
    }

    // The grip's reference frame in world space; while a mechanism is
    // previewed, its part moves with the previewed scene.
    Transformf ReloadMakerController::gripReferenceWorld() const
    {
        if (_partGripFrame.weapon) {
            return _globalPose[_weaponBoneIndex];
        }
        if (_mechanismPreview && _partGripFrame.node >= 0 && _partGripFrame.node < _mechanismWorld.size()) {
            return _mechanismWorld[_partGripFrame.node];
        }
        return composed(_boundGlobalPose.at(_partGripFrame.binding), _partGripFrame.bridge);
    }

    void ReloadMakerController::beginMechanismPreview()
    {
        startMechanismPreview(false);
    }

    void ReloadMakerController::beginMechanismTeaching()
    {
        startMechanismPreview(true);
    }

    // Starts testing (grab and release) or teaching (drag a step's endpoint)
    // the draft. Teaching shows the selected step's end; testing needs at
    // least one valid step.
    void ReloadMakerController::startMechanismPreview(bool teaching)
    {
        if (!_mechanismDraft || !_clip.valid() || _loadThread) {
            return;
        }
        QString error;
        if (!teaching && (!validateMechanism(*_mechanismDraft, error) || _mechanismDraft->stages.isEmpty())) {
            setStatus("Set a movement endpoint before testing. " + error, true);
            return;
        }
        _workshopHandEditing = false;
        _workshopPicking = false;
        _gripViewport->setPicking(false);
        stopPlayback();
        stopMechanismPreview();
        if (!applyPose(_playbackTime)) {
            return;
        }
        if (!compileMechanismPreview(error)) {
            setStatus(error, true);
            return;
        }
        if (!mechanismSkinOwned()) {
            setStatus("This group contains skin driven by an external bone. Choose its actual animated part frame before simulating.", true);
            return;
        }
        _mechanismState = {};
        _mechanismDirection = 1;
        _mechanismHeld = teaching;
        _mechanismPreview = true;
        _mechanismTeaching = teaching;
        if (teaching) {
            const int selected = _mechanismStage;
            _mechanismState.path = float(selected >= 0 ? selected + 1 : _mechanismDraft->stages.size());
            if (selected >= 0) {
                _teachingRotation = _mechanismDraft->stages[selected].rotation;
                _teachingAxis = _mechanismDraft->stages[selected].axis;
            }
        }
        if (!applyPose(_playbackTime)) {
            stopMechanismPreview();
            return;
        }
        updatePartGripControls();
    }

    // The scene group moved by the draft: its part, then its followers.
    bool ReloadMakerController::compileMechanismPreview(QString& error)
    {
        QVector<int> roots;
        const int master = resolveMechanismAnchor(_mechanismDraft->anchor, error);
        if (master < 0) {
            return false;
        }
        roots.append(_sceneNodes.indexOf(_handPoseAnchors[master].node));
        for (const auto& f : _mechanismDraft->followers) {
            const int i = resolveMechanismAnchor(f.anchor, error);
            if (i < 0) {
                return false;
            }
            roots.append(_sceneNodes.indexOf(_handPoseAnchors[i].node));
        }
        return compileMechanismScene(_assemblyNodes, _nodeBindings, roots, _mechanismScene, error);
    }

    // A selected node may not control skinned geometry bound to external
    // bones. Refuse that preview rather than moving an invisible parent and
    // claiming it works: every skin bone of a shape in a moved group must be
    // in the same group.
    bool ReloadMakerController::mechanismSkinOwned() const
    {
        auto* scene = _window->getGLView()->getScene();
        auto* model = activeModel();
        for (auto* shape : scene->shapes) {
            if (!shape) {
                continue;
            }
            int owner = -1;
            auto parent = model->getBlockIndex(shape->id());
            for (int depth = 0; parent.isValid() && depth < model->getBlockCount(); ++depth) {
                const int n = _sceneNodes.indexOf(parent);
                if (n >= 0) {
                    owner = _mechanismScene.group[n];
                    break;
                }
                parent = model->getBlockIndex(model->getParent(parent));
            }
            if (owner < 0) {
                continue;
            }
            for (const int bone : shape->reloadMakerSkinBones()) {
                const int n = _sceneNodes.indexOf(model->getBlockIndex(bone));
                if (n < 0 || _mechanismScene.group[n] != owner) {
                    return false;
                }
            }
        }
        return true;
    }

    // Poses the previewed group: the part at the current path along its
    // steps, followers at their rest offsets from it.
    bool ReloadMakerController::applyMechanismPreview()
    {
        if (!_mechanismDraft) {
            return false;
        }
        if (!nativeMechanismScene(_mechanismScene, _nodeBindings, _boundLocalPose, _mechanismLocal, _mechanismWorld, _poseError)) {
            return false;
        }
        const auto rest = composed(_globalPose[_weaponBoneIndex], _mechanismDraft->partInWeaponAtRest);
        const auto target = composed(rest, mechanismDelta(*_mechanismDraft, _mechanismState.path));
        QVector<Transformf> targets{ target };
        for (const auto& follower : _mechanismDraft->followers) {
            targets.append(composed(target, follower.inPartAtRest));
        }
        if (!moveMechanismScene(_mechanismScene, targets, _mechanismLocal, _mechanismWorld, _poseError)) {
            return false;
        }
        for (int i = 0; i < _nodeBindings.size(); ++i) {
            _boundLocalPose[i] = _mechanismLocal[_nodeBindings[i].nodeIndex];
            _boundGlobalPose[i] = _mechanismWorld[_nodeBindings[i].nodeIndex];
        }
        return true;
    }

    void ReloadMakerController::stopMechanismPreview()
    {
        if (_mechanismTimer) {
            _mechanismTimer->stop();
        }
        if (_mechanismPreview) {
            _mechanismRestorePending = true;
        }
        _mechanismPreview = false;
        _mechanismTeaching = false;
        _mechanismHeld = false;
        _mechanismState.returning = false;
    }

    // Releasing applies the step's release rule; a return animates.
    void ReloadMakerController::releaseMechanismGrab()
    {
        if (!_mechanismPreview || !_mechanismDraft || _mechanismTeaching) {
            return;
        }
        _mechanismHeld = false;
        releaseMechanism(*_mechanismDraft, _mechanismState);
        if (_mechanismState.returning) {
            _mechanismClock.restart();
            _mechanismTimer->start();
        }
        (void)applyPose(_playbackTime);
        updateMechanismControls();
        updateGripViewport();
    }

    // Timer tick of the return motion; a view refresh, not a document change.
    void ReloadMakerController::advanceMechanismPreview()
    {
        const QScopedValueRollback<bool> viewOnly(_viewRefresh, true);
        if (!_mechanismPreview || !_mechanismDraft) {
            _mechanismTimer->stop();
            return;
        }
        const float seconds = float(_mechanismClock.nsecsElapsed()) / 1e9f;
        _mechanismClock.restart();
        advanceMechanism(*_mechanismDraft, _mechanismState, seconds);
        if (!_mechanismState.returning) {
            _mechanismTimer->stop();
        }
        if (!applyPose(_playbackTime)) {
            stopMechanismPreview();
            return;
        }
        updateMechanismControls();
        updateGripViewport();
    }

    // Esc during a drag: teaching restores the step as it was before the
    // drag, testing returns the part to where it was grabbed.
    void ReloadMakerController::cancelMechanismGrab()
    {
        if (!_mechanismPreview) {
            return;
        }
        if (_mechanismTeaching) {
            const int i = _mechanismStage;
            if (_mechanismDraft && i >= 0) {
                _mechanismDraft->stages[i] = _teachingDragStart;
                displayMechanism();
            }
        } else {
            _mechanismState.path = _mechanismGrabPath;
            _mechanismHeld = false;
        }
        (void)applyPose(_playbackTime);
        updateMechanismControls();
        updateGripViewport();
    }

    // Starts dragging a teaching gizmo handle; without a selected step a new
    // step is added for the drag.
    void ReloadMakerController::beginTeachingDrag(int axis, bool rotation)
    {
        if (!_mechanismPreview || !_mechanismTeaching || !_mechanismDraft) {
            return;
        }
        int i = _mechanismStage;
        if (i < 0) {
            MechanismStage stage;
            stage.forwardName = QStringLiteral("Step %1").arg(_mechanismDraft->stages.size() + 1);
            stage.to = 0;
            _mechanismDraft->stages.append(stage);
            displayMechanism();
            i = _mechanismDraft->stages.size() - 1;
            selectMechanismStage(i);
        }
        _teachingDragStart = _mechanismDraft->stages[i];
        _teachingAxis = axis;
        _teachingRotation = rotation;
        _mechanismDraft->stages[i] = mechanismStageFromDrag(_teachingDragStart, axis, rotation, 0);
        _mechanismState.path = float(i + 1);
        _mechanismHeld = true;
        const auto base = composed(_globalPose[_weaponBoneIndex], _mechanismDraft->partInWeaponAtRest);
        _mechanismGrabScale = rotation ? 1.f : base.scale.x;
        displayMechanism();
        (void)applyPose(_playbackTime);
        updateGripViewport();
    }

    // A gizmo drag: teaching sets the step's endpoint from the drag; testing
    // moves the part along the active step.
    void ReloadMakerController::dragMechanismBy(float delta)
    {
        if (!_mechanismPreview || !_mechanismDraft || !_mechanismHeld) {
            return;
        }
        if (_mechanismTeaching) {
            const int i = _mechanismStage;
            if (i < 0) {
                return;
            }
            auto& stage = _mechanismDraft->stages[i];
            stage = mechanismStageFromDrag(_teachingDragStart, _teachingAxis, _teachingRotation, delta / _mechanismGrabScale);
            showTaughtStage(i);
        } else {
            const int i = _mechanismState.activeStage;
            const auto& s = _mechanismDraft->stages[i];
            const float path = std::clamp(_mechanismGrabPath + delta / (s.to - s.from) / _mechanismGrabScale, float(i), float(i + 1));
            _mechanismDirection = path >= _mechanismState.path ? 1 : -1;
            _mechanismState.path = path;
        }
        (void)applyPose(_playbackTime);
        updateMechanismControls();
        updateGripViewport();
    }

    // Grabs the tested part when the click lands on a visible weapon shape
    // that belongs to the moved group; a skinned shape only when every one of
    // its bones does.
    bool ReloadMakerController::grabMechanismAt(QPointF point)
    {
        if (!_mechanismPreview || !_mechanismDraft || _mechanismTeaching) {
            return false;
        }
        auto* view = _window->getGLView();
        auto* model = activeModel();
        QVector<int> allowed;
        QSet<int> members;
        for (auto* shape : view->getScene()->shapes) {
            if (!shape || shape->isHidden()) {
                continue;
            }
            auto node = model->getBlockIndex(shape->id());
            bool weapon = false;
            bool member = false;
            for (int d = 0; node.isValid() && d < model->getBlockCount(); ++d) {
                const int i = _sceneNodes.indexOf(node);
                if (i >= 0 && _mechanismScene.group[i] >= 0) {
                    member = true;
                }
                if (node == _weaponRoot) {
                    weapon = true;
                }
                for (const auto& root : _partRoots) {
                    if (root == node) {
                        weapon = true;
                    }
                }
                node = model->getBlockIndex(model->getParent(node));
            }
            // Whole-shape picking cannot identify a triangle's skin ownership.
            // Only consume the click if every used skin bone belongs to this mechanism.
            const auto bones = shape->reloadMakerSkinBones();
            if (!bones.isEmpty()) {
                member = true;
                for (const int bone : bones) {
                    const int i = _sceneNodes.indexOf(model->getBlockIndex(bone));
                    if (i < 0 || _mechanismScene.group[i] < 0) {
                        member = false;
                        break;
                    }
                }
            }
            if (weapon) {
                allowed.append(shape->id());
            }
            if (member) {
                members.insert(shape->id());
            }
        }
        int block = -1;
        Vector3 world;
        if (!view->reloadMakerSurfaceAt(point, allowed, block, world) || !members.contains(block)) {
            return false;
        }
        beginMechanismGrab();
        return true;
    }

    // Holds the part; at a step boundary the active step follows the last
    // drag direction.
    void ReloadMakerController::beginMechanismGrab()
    {
        if (!_mechanismPreview || !_mechanismDraft) {
            return;
        }
        _mechanismTimer->stop();
        _mechanismState.returning = false;
        _mechanismHeld = true;
        _mechanismGrabPath = _mechanismState.path;
        const float rounded = std::round(_mechanismState.path);
        if (std::abs(rounded - _mechanismState.path) < .0001f) {
            const int boundary = int(rounded) - (_mechanismDirection < 0 ? 1 : 0);
            _mechanismState.activeStage = std::clamp(boundary, 0, int(_mechanismDraft->stages.size()) - 1);
        }
        const auto& stage = _mechanismDraft->stages[_mechanismState.activeStage];
        const auto base = composed(_globalPose[_weaponBoneIndex], _mechanismDraft->partInWeaponAtRest);
        _mechanismGrabScale = stage.rotation ? 1.f : base.scale.x;
        (void)applyPose(_playbackTime);
        updateMechanismControls();
        updateGripViewport();
    }

    // The test slider is pressed: hold the part where it is.
    void ReloadMakerController::holdMechanismSlider()
    {
        if (!_mechanismPreview || _mechanismTeaching) {
            return;
        }
        _mechanismHeld = true;
        _mechanismState.returning = false;
        _mechanismTimer->stop();
    }

    // The test slider moves the part within the active step (0 to 10000).
    void ReloadMakerController::slideMechanism(int value)
    {
        if (!_mechanismPreview || _mechanismTeaching || !_mechanismDraft || _mechanismUpdating) {
            return;
        }
        const float path = _mechanismState.activeStage + float(value) / 10000;
        _mechanismDirection = path >= _mechanismState.path ? 1 : -1;
        _mechanismState.path = path;
        _mechanismState.returning = false;
        _mechanismHeld = true;
        (void)applyPose(_playbackTime);
        updateMechanismControls();
        updateGripViewport();
    }

    // Steps to the previous or next movement step at the active step's end.
    void ReloadMakerController::changeMechanismStep(int direction)
    {
        if (!_mechanismPreview || _mechanismTeaching || !_mechanismDraft || _mechanismState.returning) {
            return;
        }
        const int next = _mechanismState.activeStage + direction;
        const float boundary = float(_mechanismState.activeStage + (direction > 0));
        if (next < 0 || next >= _mechanismDraft->stages.size() || std::abs(_mechanismState.path - boundary) > .0001f) {
            setStatus("Reach the step endpoint before changing direction.", true);
            return;
        }
        _mechanismState.activeStage = next;
        _mechanismDirection = direction;
        updateMechanismControls();
        updateGripViewport();
    }

    // Ends a test or teaching session and shows the native animation.
    void ReloadMakerController::endMechanismTest()
    {
        stopMechanismPreview();
        if (_clip.valid()) {
            (void)applyPose(_playbackTime);
        }
        updatePartGripControls();
    }

    // Samples the clip at 61 frames and marks parts that keep a fixed
    // relation to the definition's part while it articulates relative to the
    // weapon. Only an observation; followers are never checked for the user.
    void ReloadMakerController::analyzeMechanismAnimation()
    {
        if (!_mechanismDraft || !_clip.valid() || _mechanismPreview || _loadThread) {
            return;
        }
        QString error;
        const int index = resolveMechanismAnchor(_mechanismDraft->anchor, error);
        if (index < 0) {
            setStatus(error, true);
            return;
        }
        const int master = _sceneNodes.indexOf(_handPoseAnchors[index].node);
        MechanismScenePlan plan;
        if (master < 0 || !compileMechanismScene(_assemblyNodes, _nodeBindings, {}, plan, error)) {
            setStatus(error, true);
            return;
        }
        const auto distance = [](Vec3f a, Vec3f b) { return QVector3D(a.x - b.x, a.y - b.y, a.z - b.z).length(); };
        const auto angle = [](Quaternionf a, Quaternionf b) {
            return 2 * std::acos(std::clamp(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w), 0.f, 1.f)) * 180.f / 3.14159265f;
        };
        QVector<Transformf> firstRelative(_handPoseAnchors.size());
        QVector<float> distances(_handPoseAnchors.size(), 0);
        QVector<float> angles(_handPoseAnchors.size(), 0);
        QVector<bool> visible(_handPoseAnchors.size(), true);
        float movement = 0;
        float rotation = 0;
        Transformf firstMaster;
        QVector<Transformf> local, global, boundLocal, boundGlobal, nativeLocal, world;
        for (int sample = 0; sample <= 60; ++sample) {
            if (!sampleActionPose(_clip.durationSeconds * sample / 60.f, local, global)) {
                setStatus(_poseError, true);
                return;
            }
            if (!evaluateSceneBindings(_skeleton, _nodeBindings, local, _stateBaselineLocalPose, global, boundLocal, boundGlobal, error) ||
                !nativeMechanismScene(plan, _nodeBindings, boundLocal, nativeLocal, world, error)) {
                setStatus(error, true);
                return;
            }
            Transformf inverse;
            Transformf weaponInverse;
            if (!invertUniformTransform(world[master], inverse) || !invertUniformTransform(global[_weaponBoneIndex], weaponInverse)) {
                setStatus("Master is hidden in this clip; relationship analysis unavailable.", true);
                return;
            }
            // Whole-weapon movement is not evidence that a bolt/follower articulates.
            const auto relativeMaster = composed(weaponInverse, world[master]);
            if (sample == 0) {
                firstMaster = relativeMaster;
            }
            movement = std::max(movement, distance(relativeMaster.translation, firstMaster.translation));
            rotation = std::max(rotation, angle(relativeMaster.rotation, firstMaster.rotation));
            for (int i = 1; i < _handPoseAnchors.size(); ++i) {
                const int node = _sceneNodes.indexOf(_handPoseAnchors[i].node);
                if (node < 0 || std::abs(world[node].scale.x) < .0001f) {
                    visible[i] = false;
                    continue;
                }
                const auto relative = composed(inverse, world[node]);
                if (sample == 0) {
                    firstRelative[i] = relative;
                }
                distances[i] = std::max(distances[i], distance(relative.translation, firstRelative[i].translation));
                angles[i] = std::max(angles[i], angle(relative.rotation, firstRelative[i].rotation));
            }
        }
        QVector<FollowerObservation> observations(_handPoseAnchors.size());
        for (int i = 1; i < _handPoseAnchors.size(); ++i) {
            observations[i].visible = visible[i];
            observations[i].related = visible[i] && (movement > .1f || rotation > .5f) && distances[i] < .05f && angles[i] < .5f;
            observations[i].distance = distances[i];
            observations[i].angle = angles[i];
        }
        showFollowerObservations(observations);
        setStatus("Animation relationships measured at 61 frames in weapon space. Checkboxes unchanged; select followers explicitly.");
    }
}
