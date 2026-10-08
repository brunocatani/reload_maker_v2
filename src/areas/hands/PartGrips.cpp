// Part grips: a saved finger pose placed on a weapon part. Starting a grip
// draft from the form, placing the palm on a picked surface, moving the
// hand or its part, previewing the solved arm, saving the grip to the
// library and loading a saved one back.
#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "GripViewportMath.h"
#include "ReloadBundle.h"
#include "model/nifmodel.h"

#include <QDateTime>
#include <QQuaternion>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace reloadmaker
{
    // Opens the part grip page on the selected saved pose and the capture
    // form's reference part.
    void ReloadMakerController::openPartGripEditor()
    {
        if (const auto* pose = selectedHandPose()) {
            setGripPose(pose->id);
        }
        setGripAnchor(_handCaptureAnchor);
        setHandEditorPage(HandEditorPage::PartGrip);
    }

    void ReloadMakerController::refreshPartGripLibrary()
    {
        QStringList diagnostics;
        _partGrips = _partGripLibrary.load(diagnostics);
        for (const auto& error : diagnostics) {
            appendDiagnostic("PART GRIP FILE " + error);
        }
        if (!diagnostics.isEmpty()) {
            setStatus(QStringLiteral("Some part-grip files could not be loaded; see Diagnostics. Files were left untouched."), true);
        }
        refreshPartGripChoices();
        refreshPartGripList();
    }

    // The frame the hand is placed in: the whole weapon, or an animated
    // part's binding with the bridge from that binding to the part.
    bool ReloadMakerController::preparePartGripFrame(int anchorIndex, QString& error)
    {
        if (anchorIndex < 0 || anchorIndex >= _handPoseAnchors.size()) {
            error = "Choose a reference part";
            return false;
        }
        const auto& anchor = _handPoseAnchors[anchorIndex];
        if (anchor.description.anchorKind == "weapon") {
            _partGripFrame = { true, -1, {} };
            return true;
        }
        Transformf world;
        if (!sceneNodeWorld(anchor.node, world, error)) {
            return false;
        }
        const auto* model = activeModel();
        QModelIndex node = anchor.node;
        for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
            for (int i = 0; i < _nodeBindings.size(); ++i) {
                if (_sceneNodes[_nodeBindings[i].nodeIndex] != node) {
                    continue;
                }
                Transformf inverse;
                if (!invertUniformTransform(_boundGlobalPose[i], inverse)) {
                    error = "Reference part has a zero-scaled animated parent";
                    return false;
                }
                _partGripFrame = { false, i, composed(inverse, world), int(_sceneNodes.indexOf(anchor.node)) };
                return true;
            }
            node = model->getBlockIndex(model->getParent(node));
        }
        error = "Reference part has no animated assembly frame";
        return false;
    }

    // Starts a draft from the form's pose on its reference part, either at
    // the pose's captured offset or at the current wrist with the pose's
    // fingers. A magazine part enters magazine editing.
    void ReloadMakerController::startPartGrip(bool capturedOffset)
    {
        updatePartGripControls();
        if (!canStartPartGrip()) {
            return;
        }
        const HandPose* selected = gripPose();
        if (!selected) {
            return;
        }
        if (_editingMagazine && selected->hand != _interactionHand) {
            setStatus("Choose a saved pose for the selected reload hand.", true);
            return;
        }
        const bool sameSource = _partGripDraft && _partGripDraft->sourcePoseId == selected->id;
        const int anchor = _gripAnchor;
        _editingMagazine = anchor >= 0 && anchor < _handPoseAnchors.size() && magazineGrabAnchor(_handPoseAnchors[anchor].description) >= 0;
        restoreHandPosePreview();
        stopPlayback();
        if (!applyPose(_playbackTime)) {
            return;
        }
        QString error;
        if (!preparePartGripFrame(anchor, error)) {
            setStatus(error, true);
            return;
        }
        PartGrip draft{ *selected, poseRecordKey(currentWeapon()->formId), selected->id };
        draft.pose.placement = _handPoseAnchors[anchor].description;
        draft.pose.placement.handInAnchor = selected->placement.handInAnchor;
        if (!capturedOffset) {
            const auto reference = _partGripFrame.weapon ? _globalPose[_weaponBoneIndex] : composed(_boundGlobalPose[_partGripFrame.binding], _partGripFrame.bridge);
            if (!captureHandPose(_skeleton, _localPose, _globalPose, reference, draft.pose, error)) {
                setStatus(error, true);
                return;
            }
            draft.pose.fingers = selected->fingers;
        }
        _partGripDraft = std::move(draft);
        if (!assignGripPart(anchor, error)) {
            setStatus(error, true);
            _partGripDraft.reset();
            return;
        }
        if (!sameSource || _gripDraftName.trimmed().isEmpty()) {
            setGripDraftName(selected->name + " · " + _handPoseAnchors[anchor].description.anchorName);
        }
        setArmsShown(true);
        showPartGripValues();
        (void)applyPose(_playbackTime);
        updateHandPoseControls();
        markPartGripOpened();
    }

    // Applies the form's pose (hand, fingers, wrist rotation) to the draft,
    // starting one at the captured offset when there is none; the hand
    // keeps its position.
    void ReloadMakerController::applyGripPoseToDraft()
    {
        const auto* selected = gripPose();
        if (!selected) {
            return;
        }
        const auto pose = *selected;
        if (!_partGripDraft) {
            startPartGrip(true);
        }
        if (!_partGripDraft) {
            return;
        }
        _partGripDraft->pose.hand = pose.hand;
        _partGripDraft->pose.fingers = pose.fingers;
        _partGripDraft->sourcePoseId = pose.id;
        _partGripDraft->pose.placement.handInAnchor.rotation = pose.placement.handInAnchor.rotation;
        showPartGripValues();
        (void)applyPose(_playbackTime);
        rememberMagazineGrip();
        updatePartGripControls();
    }

    // Placement edits from the form, in the reference part's space.
    void ReloadMakerController::setGripPosition(int axis, float value)
    {
        if (!_partGripDraft) {
            return;
        }
        auto& position = _partGripDraft->pose.placement.handInAnchor.translation;
        if (axis == 0) {
            position.x = value;
        } else if (axis == 1) {
            position.y = value;
        } else {
            position.z = value;
        }
        (void)applyPose(_playbackTime);
        updatePartGripControls();
        noteDocumentChange("grip placement");
    }

    void ReloadMakerController::setGripRotation(const QVector3D& eulerDegrees)
    {
        if (!_partGripDraft) {
            return;
        }
        const auto q = QQuaternion::fromEulerAngles(eulerDegrees);
        _partGripDraft->pose.placement.handInAnchor.rotation = { q.x(), q.y(), q.z(), q.scalar() };
        (void)applyPose(_playbackTime);
        updatePartGripControls();
        noteDocumentChange("grip placement");
    }

    // The arm solved to the draft's hand target; magazine grips follow the
    // target, other grips keep the shoulder fixed. An unreachable target
    // keeps the native animation and disables saving.
    bool ReloadMakerController::applyPartGripPreview()
    {
        const auto reference = gripReferenceWorld();
        const auto target = composed(reference, _partGripDraft->pose.placement.handInAnchor);
        const auto reach = magazineGrabAnchor(_partGripDraft->pose.placement) >= 0 ? GripReach::FollowTarget : GripReach::FixedShoulder;
        _gripPreviewValid = solvePartGrip(_skeleton, _localPose, _globalPose, _partGripDraft->pose, target, _localPose, _globalPose, _gripError, reach);
        if (!_gripPreviewValid) {
            updatePartGripControls();
            return true;
        }
        return evaluateSceneBindings(_skeleton, _nodeBindings, _sourceLocalPose, _stateBaselineLocalPose, _globalPose, _boundLocalPose, _boundGlobalPose,
                                     _poseError) &&
               applySimulationPose();
    }

    // Saves the draft as a new library grip with its hand in weapon space.
    // A magazine grip is assigned to its magazine instead.
    bool ReloadMakerController::savePartGrip()
    {
        if (_partGripDraft && magazineGrabAnchor(_partGripDraft->pose.placement) >= 0) {
            return useMagazineGrabGrip();
        }
        updatePartGripControls();
        if (!canSavePartGrip()) {
            return false;
        }
        PartGrip grip = *_partGripDraft;
        grip.pose.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        grip.pose.name = _gripDraftName.trimmed();
        grip.pose.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        grip.pose.source = currentPoseSource();
        Transformf inverse;
        QString error;
        const int hand = _skeleton.boneIndex(grip.pose.hand == PoseHand::Left ? "LArm_Hand" : "RArm_Hand");
        if (!invertUniformTransform(_globalPose[_weaponBoneIndex], inverse) || hand < 0) {
            setStatus("Cannot capture grip placement", true);
            return false;
        }
        grip.pose.placement.handInWeapon = composed(inverse, _globalPose[hand]);
        if (!_partGripLibrary.save(grip, error)) {
            setStatus("Part grip was not saved: " + error, true);
            return false;
        }
        _partGripDraft = grip;
        markPartGripOpened();
        refreshPartGripLibrary();
        if (partGripListed(grip.pose.id)) {
            selectPartGrip(grip.pose.id);
        }
        appendDiagnostic(QStringLiteral("SAVED PART GRIP %1 · %2 · reference=%3").arg(grip.pose.name, grip.pose.id, grip.pose.placement.anchorName));
        setStatus(QStringLiteral("Saved part grip “%1”: fingers, position and angle. Source pose unchanged.").arg(grip.pose.name));
        return true;
    }

    void ReloadMakerController::loadPartGrip()
    {
        updatePartGripControls();
        const auto* selected = selectedPartGrip();
        if (!canLoadPartGrip() || !selected) {
            return;
        }
        (void)loadGripRecord(*selected);
    }

    // Loads a saved grip as the draft on its resolved part, with its stored
    // placement unchanged. Returns whether the arm reaches it.
    bool ReloadMakerController::loadGripRecord(const PartGrip& selected)
    {
        const PartGrip copy = selected;
        _editingMagazine = magazineGrabAnchor(copy.pose.placement) >= 0;
        QVector<HandPosePlacement> anchors;
        for (const auto& anchor : _handPoseAnchors) {
            anchors.append(anchor.description);
        }
        QString error;
        const int anchor = resolveGripAnchor(copy, poseRecordKey(currentWeapon()->formId), anchors, error);
        if (anchor < 0) {
            setStatus(error, true);
            return false;
        }
        if (!copy.mechanismId.isEmpty()) {
            const auto definition = std::find_if(_mechanisms.cbegin(), _mechanisms.cend(), [&](const auto& p) { return p.id == copy.mechanismId; });
            if (definition == _mechanisms.cend() || definition->weaponRecord.compare(copy.weaponRecord, Qt::CaseInsensitive) != 0 ||
                !sameMechanismPart(definition->anchor, copy.pose.placement)) {
                setStatus(QStringLiteral("The grip's shared part definition is missing or does not match this part."), true);
                return false;
            }
        }
        restoreHandPosePreview();
        stopPlayback();
        if (!applyPose(_playbackTime)) {
            return false;
        }
        if (!preparePartGripFrame(anchor, error)) {
            setStatus(error, true);
            return false;
        }
        followGripInForm(copy.sourcePoseId, anchor);
        _partGripDraft = copy;
        setGripDraftName(copy.pose.name);
        if (copy.mechanismId.isEmpty() && !assignGripPart(anchor, error)) {
            _partGripDraft.reset();
            setStatus(error, true);
            return false;
        }
        // Loading an already-qualified frame is not an edit. Keep exact stored
        // matrices instead of a world -> local floating-point round trip.
        _partGripDraft->pose.placement = copy.pose.placement;
        _partGripDraft->surfaceContact = copy.surfaceContact;
        setArmsShown(true);
        showPartGripValues();
        (void)applyPose(_playbackTime);
        updateHandPoseControls();
        markPartGripOpened();
        return _gripPreviewValid;
    }

    // Moves the draft to another part keeping the hand's world placement and
    // surface contact. The part's shared mechanism, when there is one,
    // becomes the grip's.
    bool ReloadMakerController::assignGripPart(int index, QString& error)
    {
        if (!_partGripDraft || index < 0 || index >= _handPoseAnchors.size()) {
            error = "No valid part selection";
            return false;
        }
        QString mechanismId;
        for (const auto& definition : _mechanisms) {
            if (definition.weaponRecord.compare(_partGripDraft->weaponRecord, Qt::CaseInsensitive) != 0 ||
                !sameMechanismPart(definition.anchor, _handPoseAnchors[index].description)) {
                continue;
            }
            if (!mechanismId.isEmpty()) {
                error = "This part has duplicate shared definitions; resolve the duplicate files first";
                return false;
            }
            mechanismId = definition.id;
        }
        if (_mechanismPreview) {
            stopMechanismPreview();
            if (!applyPose(_playbackTime)) {
                return false;
            }
        }
        const auto old = gripReferenceWorld();
        const auto hand = composed(old, _partGripDraft->pose.placement.handInAnchor);
        std::optional<Vec3f> contact;
        if (_partGripDraft->surfaceContact) {
            contact = composed(old, Transformf{ _partGripDraft->surfaceContact->pointInAnchor }).translation;
        }
        const auto previousFrame = _partGripFrame;
        if (!preparePartGripFrame(index, error)) {
            return false;
        }
        const auto frame = gripReferenceWorld();
        Transformf inverse;
        if (!invertUniformTransform(frame, inverse)) {
            _partGripFrame = previousFrame;
            error = "Invalid part frame";
            return false;
        }
        _partGripDraft->pose.placement = _handPoseAnchors[index].description;
        _partGripDraft->pose.placement.handInAnchor = composed(inverse, hand);
        if (contact) {
            _partGripDraft->surfaceContact->pointInAnchor = composed(inverse, Transformf{ *contact }).translation;
        }
        _partGripDraft->mechanismId = mechanismId;
        _gripAnchor = index;
        showGripAnchor();
        return true;
    }

    // The viewport gizmo moved the hand to a world transform.
    void ReloadMakerController::editGripWorld(const Transformf& world)
    {
        if (!_partGripDraft || _loadThread || _rebuildTimer->isActive() || !_clip.valid()) {
            return;
        }
        const auto reference = _partGripFrame.weapon ? _globalPose[_weaponBoneIndex] : composed(_boundGlobalPose.at(_partGripFrame.binding), _partGripFrame.bridge);
        Transformf inverse;
        if (!invertUniformTransform(reference, inverse)) {
            return;
        }
        _partGripDraft->pose.placement.handInAnchor = composed(inverse, world);
        if (_handEditorPage == HandEditorPage::Mechanism) {
            _workshopGripDirty = true;
        }
        showPartGripValues();
        (void)applyPose(_playbackTime);
        updateHandPoseControls();
    }

    // Uses the selected saved pose on a part picked in the viewport.
    void ReloadMakerController::applySavedPoseToPart()
    {
        _editingMagazine = false;
        const auto* pose = selectedHandPose();
        if (!pose) {
            setStatus("Select a saved hand pose first.", true);
            return;
        }
        const auto name = pose->name;
        restoreHandPosePreview();
        openPartGripEditor();
        setGripDraftName(name);
        beginGripSurfacePick();
    }

    // Starts palm picking, first starting a draft at the current wrist with
    // the form pose's wrist rotation when there is none.
    void ReloadMakerController::beginGripSurfacePick()
    {
        if (_mechanismPreview) {
            stopMechanismPreview();
            if (_clip.valid()) {
                (void)applyPose(_playbackTime);
            }
        }
        updatePartGripControls();
        if (!canPickGripSurface()) {
            return;
        }
        if (!_partGripDraft) {
            Quaternionf rotation{};
            if (const auto* pose = gripPose()) {
                rotation = pose->placement.handInAnchor.rotation;
            }
            startPartGrip(false);
            if (!_partGripDraft) {
                return;
            }
            _partGripDraft->pose.placement.handInAnchor.rotation = rotation;
            showPartGripValues();
            (void)applyPose(_playbackTime);
        }
        if (!_partGripDraft) {
            return;
        }
        setHandEditorPage(HandEditorPage::PartGrip);
        updateGripViewport();
        _gripViewport->setPicking(true);
        showGripStatus(QStringLiteral("Click a weapon surface to place the palm and identify its part. Multi-bone surfaces need an explicit Reference part choice. Esc "
                                      "cancels picking."));
    }

    // Places the palm on the clicked weapon surface. A uniquely identified
    // part becomes the reference; a magazine grip keeps its magazine.
    void ReloadMakerController::pickGripSurface(QPointF point)
    {
        if (!_partGripDraft || !_gripViewport->picking() || _loadThread || _rebuildTimer->isActive()) {
            return;
        }
        int automatic = -1;
        Vec3f world;
        QString surface;
        QString error;
        QVector<int> candidates;
        if (!pickWeaponPart(point, automatic, world, surface, candidates, error)) {
            showGripStatus(error);
            return;
        }
        // Magazine configuration has one explicit authority. A skinned triangle
        // may report its frame/parent; clicking it must not retarget the grip.
        if (_editingMagazine) {
            automatic = currentMagazineAnchor();
        }
        QString assignmentError;
        if (automatic >= 0 && !assignGripPart(automatic, assignmentError)) {
            setStatus(assignmentError, true);
            return;
        }
        const auto reference = _partGripFrame.weapon ? _globalPose[_weaponBoneIndex] : composed(_boundGlobalPose.at(_partGripFrame.binding), _partGripFrame.bridge);
        Transformf inverse;
        if (!invertUniformTransform(reference, inverse)) {
            setStatus("Reference part cannot be inverted", true);
            return;
        }
        const auto palm = genericPalmPoint(_partGripDraft->pose);
        const auto hand = placePalmAt(composed(reference, _partGripDraft->pose.placement.handInAnchor), palm, world);
        _partGripDraft->pose.placement.handInAnchor = composed(inverse, hand);
        _partGripDraft->surfaceContact = PartGrip::SurfaceContact{ composed(inverse, Transformf{ world }).translation, palm, surface };
        _gripViewport->setPicking(false);
        const QString referenceName = _partGripDraft->pose.placement.anchorName;
        showPartGripValues();
        if (!applyPose(_playbackTime)) {
            return;
        }
        updateHandPoseControls();
        if (_editingMagazine) {
            setStatus(QStringLiteral("Hand placed on the selected magazine. Fine-tune with Move / Rotate, then Save for this magazine."));
        } else if (automatic >= 0) {
            setStatus(QStringLiteral("Point attached to %1. Configure its shared mechanism to set stages and followers.").arg(referenceName));
        } else {
            setStatus(QStringLiteral("This surface has no single resolved moving-part frame. Point captured against %1; explicitly choose its Reference part before "
                                     "defining movement.")
                          .arg(referenceName));
        }
    }
}
