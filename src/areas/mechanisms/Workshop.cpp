// The part workshop: choosing a weapon part (in 3D or from the list), its
// optional hand (a saved grip or a finger pose placed at the picked palm
// point), editing its movement steps, and saving the part with its hand.
#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioWindow.h"
#include "glview.h"

#include <QAction>
#include <QMenu>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace reloadmaker
{
    // Opens a part in the workshop: its saved definition, or a new one at
    // the part's current rest. A picked point becomes the palm contact. The
    // hand choice follows the active grip, else the part's first saved grip.
    void ReloadMakerController::selectWorkshopPart(int index, std::optional<Vec3f> point, const QString& surface)
    {
        if (!_clip.valid() || _loadThread || _rebuildTimer->isActive() || index <= 0 || index >= _handPoseAnchors.size()) {
            return;
        }
        const auto anchor = _handPoseAnchors[index];
        const QString weapon = poseRecordKey(currentWeapon()->formId);
        const bool same = _mechanismDraft && _mechanismDraft->weaponRecord.compare(weapon, Qt::CaseInsensitive) == 0 &&
                          sameMechanismPart(_mechanismDraft->anchor, anchor.description);
        stopMechanismPreview();
        _workshopHandEditing = false;
        _workshopPicking = false;
        _gripViewport->setPicking(false);
        if (!same) {
            _workshopContactPoint.reset();
            _workshopSurface.clear();
            _workshopGripDirty = false;
        }
        if (_partGripDraft && !sameMechanismPart(_partGripDraft->pose.placement, anchor.description)) {
            _partGripDraft.reset();
        }
        if (!applyPose(_playbackTime)) {
            return;
        }
        QString error;
        Transformf world;
        Transformf inverse;
        if (!sceneNodeWorld(anchor.node, world, error) || !invertUniformTransform(world, inverse)) {
            setStatus("Cannot select part: " + error, true);
            return;
        }
        if (point) {
            _workshopContactPoint = composed(inverse, Transformf{ *point }).translation;
            _workshopSurface = surface;
        }
        if (!same) {
            const PartMechanism* existing = nullptr;
            for (const auto& saved : _mechanisms) {
                if (saved.weaponRecord.compare(weapon, Qt::CaseInsensitive) != 0 || !sameMechanismPart(saved.anchor, anchor.description)) {
                    continue;
                }
                if (existing) {
                    setStatus("Duplicate definitions describe this part; resolve the duplicate files first.", true);
                    return;
                }
                existing = &saved;
            }
            if (existing) {
                _mechanismDraft = *existing;
            } else {
                if (!invertUniformTransform(_globalPose[_weaponBoneIndex], inverse)) {
                    setStatus("Cannot resolve Weapon frame", true);
                    return;
                }
                PartMechanism part;
                part.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                part.name = anchor.description.anchorName;
                part.weaponRecord = weapon;
                part.anchor = anchor.description;
                part.partInWeaponAtRest = composed(inverse, world);
                _mechanismDraft = part;
            }
            // A new part starts without a hand; not a user choice.
            _workshopPoseChoice.clear();
            showWorkshopPoseChoice();
        }
        _gripAnchor = index;
        showGripAnchor();
        displayMechanism();
        refreshWorkshopChoices();
        const auto choices = workshopPoseChoices();
        if (_partGripDraft) {
            QString choice = "grip:" + _partGripDraft->pose.id;
            if (!workshopPoseListed(choice)) {
                choice = "pose:" + _partGripDraft->sourcePoseId;
            }
            if (workshopPoseListed(choice)) {
                setWorkshopPoseChoice(choice);
            }
        } else if (!same && choices.size() > 1 && choices[1].key.startsWith("grip:")) {
            setWorkshopPoseChoice(choices[1].key);
        }
        setHandEditorPage(HandEditorPage::Mechanism);
        applyWorkshopPose();
    }

    // Applies the hand choice to the part: none, a saved grip as it is, or a
    // finger pose. A finger pose keeps the current hand's placement on this
    // part when it is the same hand, else its palm goes to the contact point
    // with the pose's wrist rotation. Teaching starts afterwards.
    void ReloadMakerController::applyWorkshopPose()
    {
        if (!_mechanismDraft || !_clip.valid() || _loadThread) {
            return;
        }
        const QString choice = _workshopPoseChoice;
        stopMechanismPreview();
        _workshopHandEditing = false;
        if (!applyPose(_playbackTime)) {
            return;
        }
        QString error;
        const int anchor = resolveMechanismAnchor(_mechanismDraft->anchor, error);
        if (anchor < 0 || !preparePartGripFrame(anchor, error)) {
            setStatus(error, true);
            return;
        }
        if (choice.isEmpty()) {
            _partGripDraft.reset();
            _workshopGripDirty = false;
        } else if (choice.startsWith("grip:")) {
            const QString id = choice.mid(5);
            const auto saved = std::find_if(_partGrips.cbegin(), _partGrips.cend(), [&](const auto& grip) { return grip.pose.id == id; });
            if (saved == _partGrips.cend()) {
                setStatus("Saved grip is no longer available.", true);
                return;
            }
            _partGripDraft = *saved;
            _workshopGripDirty = saved->mechanismId != _mechanismDraft->id;
        } else if (!placeWorkshopFingerPose(choice.mid(5))) {
            return;
        }
        if (_partGripDraft) {
            _partGripDraft->mechanismId = _mechanismDraft->id;
            followGripInForm(_partGripDraft->sourcePoseId, anchor);
            setGripDraftName(_partGripDraft->pose.name + " · " + _mechanismDraft->name);
            setArmsShown(true);
            showPartGripValues();
        }
        if (!applyPose(_playbackTime)) {
            return;
        }
        beginMechanismTeaching();
    }

    bool ReloadMakerController::placeWorkshopFingerPose(const QString& id)
    {
        const auto saved = std::find_if(_savedHandPoses.cbegin(), _savedHandPoses.cend(), [&](const auto& pose) { return pose.id == id; });
        if (saved == _savedHandPoses.cend()) {
            setStatus("Finger pose is no longer available.", true);
            return false;
        }
        const bool keep = _partGripDraft && _partGripDraft->pose.hand == saved->hand && sameMechanismPart(_partGripDraft->pose.placement, _mechanismDraft->anchor);
        PartGrip draft{ *saved, _mechanismDraft->weaponRecord, saved->id };
        draft.pose.placement = _mechanismDraft->anchor;
        if (keep) {
            draft.pose.placement = _partGripDraft->pose.placement;
            draft.surfaceContact = _partGripDraft->surfaceContact;
        } else {
            const auto reference = gripReferenceWorld();
            Transformf inverse;
            if (!invertUniformTransform(reference, inverse)) {
                setStatus("Invalid part frame", true);
                return false;
            }
            draft.pose.placement.handInAnchor.rotation = saved->placement.handInAnchor.rotation;
            const auto palm = genericPalmPoint(draft.pose);
            const Vec3f contact = _workshopContactPoint.value_or(Vec3f{});
            const auto placed = placePalmAt(composed(reference, draft.pose.placement.handInAnchor), palm, composed(reference, Transformf{ contact }).translation);
            draft.pose.placement.handInAnchor = composed(inverse, placed);
            if (_workshopContactPoint) {
                draft.surfaceContact = PartGrip::SurfaceContact{ contact, palm, _workshopSurface };
            }
        }
        _partGripDraft = draft;
        _workshopGripDirty = true;
        return true;
    }

    // Picks the part to drive in 3D, or with contactOnly the palm point on
    // the selected part.
    void ReloadMakerController::beginWorkshopPick(bool contactOnly)
    {
        if (!_clip.valid() || _loadThread || (contactOnly && !_partGripDraft)) {
            return;
        }
        stopMechanismPreview();
        _workshopHandEditing = false;
        stopPlayback();
        if (!applyPose(_playbackTime)) {
            return;
        }
        _workshopPicking = true;
        _workshopContactOnly = contactOnly;
        _workshopPivotPicking = false;
        setHandEditorPage(HandEditorPage::Mechanism);
        updateGripViewport();
        _gripViewport->setPicking(true);
        showWorkshopHint(contactOnly ? "Click the contact point on the selected part. Then ADJUST HAND shows its move/rotate handles."
                                     : "Click the part you want to drive. No hand pose is required. If several bones drive the surface, choose the intended part from the menu.");
    }

    // A workshop pick: the rotation pivot of the selected step, the palm
    // contact on the selected part, or the part to drive (a menu when several
    // parts drive the surface).
    void ReloadMakerController::pickWorkshopPart(QPointF point)
    {
        if (!_workshopPicking) {
            return;
        }
        int automatic = -1;
        Vec3f world;
        QString surface;
        QString error;
        QVector<int> candidates;
        if (!pickWeaponPart(point, automatic, world, surface, candidates, error)) {
            showWorkshopHint(error);
            return;
        }
        if (_workshopPivotPicking && _mechanismDraft) {
            pickWorkshopPivot(world);
            return;
        }
        if (_workshopContactOnly && _mechanismDraft && _partGripDraft) {
            pickWorkshopContact(world, surface, candidates);
            return;
        }
        if (automatic >= 0) {
            selectWorkshopPart(automatic, world, surface);
            return;
        }
        if (candidates.isEmpty()) {
            showWorkshopHint("No unique moving frame was found. Choose the part in the part dropdown.");
            return;
        }
        _workshopPicking = false;
        _gripViewport->setPicking(false);
        auto* menu = new QMenu(_window);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addSection("Which part should this point drive?");
        for (const int index : candidates) {
            auto* action = menu->addAction(_handPoseAnchors[index].label);
            connect(action, &QAction::triggered, this, [this, index, world, surface]() { selectWorkshopPart(index, world, surface); });
        }
        menu->popup(_window->getGLView()->mapToGlobal(point.toPoint()));
    }

    // The selected step's pivot, relative to the part at the step's start.
    void ReloadMakerController::pickWorkshopPivot(const Vec3f& world)
    {
        const int i = _mechanismStage;
        if (i < 0) {
            return;
        }
        const auto rest = composed(_globalPose[_weaponBoneIndex], _mechanismDraft->partInWeaponAtRest);
        const auto start = composed(rest, mechanismDelta(*_mechanismDraft, float(i)));
        Transformf inverse;
        if (!invertUniformTransform(start, inverse)) {
            return;
        }
        _mechanismDraft->stages[i].pivot = composed(inverse, Transformf{ world }).translation;
        _workshopPicking = false;
        _workshopPivotPicking = false;
        _gripViewport->setPicking(false);
        _mechanismState.path = float(i + 1);
        displayMechanism();
        (void)applyPose(_playbackTime);
        updateGripViewport();
    }

    // Places the palm at the clicked point, which must be on the selected
    // part, then shows the hand handles.
    void ReloadMakerController::pickWorkshopContact(const Vec3f& world, const QString& surface, const QVector<int>& candidates)
    {
        QString error;
        const int anchor = resolveMechanismAnchor(_mechanismDraft->anchor, error);
        const bool onPart = std::any_of(candidates.cbegin(), candidates.cend(), [&](int i) { return sameMechanismPart(_handPoseAnchors[i].description, _mechanismDraft->anchor); });
        if (anchor < 0 || !onPart) {
            showWorkshopHint("That surface is not owned by the selected part. Pick the selected part, or change the part first.");
            return;
        }
        const auto reference = gripReferenceWorld();
        Transformf inverse;
        if (!invertUniformTransform(reference, inverse)) {
            return;
        }
        _workshopContactPoint = composed(inverse, Transformf{ world }).translation;
        _workshopSurface = surface;
        const auto palm = genericPalmPoint(_partGripDraft->pose);
        const auto hand = placePalmAt(composed(reference, _partGripDraft->pose.placement.handInAnchor), palm, world);
        _partGripDraft->pose.placement.handInAnchor = composed(inverse, hand);
        _partGripDraft->surfaceContact = PartGrip::SurfaceContact{ *_workshopContactPoint, palm, surface };
        _workshopPicking = false;
        _workshopGripDirty = true;
        _gripViewport->setPicking(false);
        editWorkshopHand(true);
    }

    // Hand handles instead of part handles; leaving returns to teaching.
    void ReloadMakerController::editWorkshopHand(bool enabled)
    {
        if (enabled && !_partGripDraft) {
            updateMechanismControls();
            return;
        }
        stopMechanismPreview();
        _workshopPicking = false;
        _gripViewport->setPicking(false);
        _workshopHandEditing = enabled;
        if (_clip.valid()) {
            (void)applyPose(_playbackTime);
        }
        if (!enabled && _mechanismDraft) {
            beginMechanismTeaching();
        }
        updatePartGripControls();
    }

    // Picks the selected step's rotation pivot, adding a rotation step when
    // none is selected. The part is shown at the step's start.
    void ReloadMakerController::beginWorkshopPivotPick()
    {
        if (!_mechanismDraft || !_clip.valid() || _loadThread) {
            return;
        }
        if (!_mechanismTeaching) {
            beginMechanismTeaching();
        }
        if (!_mechanismTeaching) {
            return;
        }
        int i = _mechanismStage;
        if (i < 0) {
            MechanismStage stage;
            stage.rotation = true;
            stage.to = 0;
            stage.forwardName = "Rotate";
            _mechanismDraft->stages.append(stage);
            displayMechanism();
            i = _mechanismDraft->stages.size() - 1;
            selectMechanismStage(i);
        }
        _mechanismState.path = float(i);
        _workshopPicking = true;
        _workshopPivotPicking = true;
        _workshopContactOnly = false;
        (void)applyPose(_playbackTime);
        updateGripViewport();
        _gripViewport->setPicking(true);
        showWorkshopHint("Click the rotation center on the weapon. The part is shown at this step's start; its ring will turn around the point you choose. Esc cancels.");
    }

    // Uses the current animation frame as the part's rest; followers keep
    // their offsets measured at that frame.
    void ReloadMakerController::captureMechanismRest()
    {
        if (!_mechanismDraft || !_clip.valid() || _loadThread) {
            return;
        }
        stopMechanismPreview();
        if (!applyPose(_playbackTime)) {
            return;
        }
        QString error;
        const int i = resolveMechanismAnchor(_mechanismDraft->anchor, error);
        Transformf master;
        Transformf inverse;
        if (i < 0 || !sceneNodeWorld(_handPoseAnchors[i].node, master, error) || !invertUniformTransform(_globalPose[_weaponBoneIndex], inverse)) {
            setStatus("Cannot capture rest: " + error, true);
            return;
        }
        auto captured = *_mechanismDraft;
        captured.partInWeaponAtRest = composed(inverse, master);
        if (!invertUniformTransform(master, inverse)) {
            setStatus("Rest part is hidden / non-invertible", true);
            return;
        }
        for (auto& f : captured.followers) {
            const int j = resolveMechanismAnchor(f.anchor, error);
            Transformf w;
            if (j < 0 || !sceneNodeWorld(_handPoseAnchors[j].node, w, error)) {
                setStatus(error, true);
                return;
            }
            f.inPartAtRest = composed(inverse, w);
        }
        _mechanismDraft = std::move(captured);
        beginMechanismTeaching();
        setStatus("Part rest captured from this animation frame.");
    }

    // Saves the part definition, then its hand grip when the hand changed.
    void ReloadMakerController::saveWorkshop()
    {
        if (!_mechanismDraft) {
            return;
        }
        const bool resume = _mechanismTeaching;
        stopMechanismPreview();
        if (_clip.valid() && !applyPose(_playbackTime)) {
            return;
        }
        const int revision = _mechanismDraft->revision;
        saveMechanism();
        if (_mechanismDraft->revision == revision) {
            if (resume) {
                beginMechanismTeaching();
            }
            return;
        }
        if (_partGripDraft && _workshopGripDirty) {
            _partGripDraft->mechanismId = _mechanismDraft->id;
            if (!savePartGrip()) {
                setStatus("Part saved, but the hand grip was not saved. Check its reach/placement before saving again.", true);
                return;
            }
            _workshopGripDirty = false;
        }
        refreshWorkshopChoices();
        if (_partGripDraft && workshopPoseListed("grip:" + _partGripDraft->pose.id)) {
            setWorkshopPoseChoice("grip:" + _partGripDraft->pose.id);
        }
        setStatus(_partGripDraft ? "Saved this part's movement and its selected hand grip. Source finger poses are unchanged."
                                 : "Saved this part's movement. No hand pose is attached.");
        if (resume) {
            beginMechanismTeaching();
        }
    }

    // Adds a step after the current one is complete (at most 32) and edits it.
    void ReloadMakerController::addMechanismStage()
    {
        if (!_mechanismDraft || _mechanismDraft->stages.size() >= 32) {
            return;
        }
        QString error;
        if (!validateMechanism(*_mechanismDraft, error)) {
            setStatus("Finish this step's travel before adding another.", true);
            return;
        }
        MechanismStage stage;
        stage.forwardName = QStringLiteral("Step %1").arg(_mechanismDraft->stages.size() + 1);
        stage.to = 0;
        _mechanismDraft->stages.append(stage);
        displayMechanism();
        selectMechanismStage(_mechanismDraft->stages.size() - 1);
        beginMechanismTeaching();
    }

    void ReloadMakerController::removeMechanismStage()
    {
        const int i = _mechanismStage;
        if (!_mechanismDraft || i < 0) {
            return;
        }
        stopMechanismPreview();
        _mechanismDraft->stages.removeAt(i);
        displayMechanism();
        beginMechanismTeaching();
    }

    // Moves the selected step earlier (-1) or later (+1).
    void ReloadMakerController::moveMechanismStage(int delta)
    {
        const int i = _mechanismStage;
        if (!_mechanismDraft || i < 0 || i + delta < 0 || i + delta >= _mechanismDraft->stages.size()) {
            return;
        }
        stopMechanismPreview();
        _mechanismDraft->stages.swapItemsAt(i, i + delta);
        displayMechanism();
        selectMechanismStage(i + delta);
        beginMechanismTeaching();
    }

    // Move or rotate handles for the part, entering teaching if needed.
    void ReloadMakerController::chooseTeachingTool(bool rotation)
    {
        if (!_mechanismTeaching) {
            beginMechanismTeaching();
        }
        _teachingRotation = rotation;
        _gripViewport->setRotationTool(rotation);
        updateMechanismControls();
        updateGripViewport();
    }

    // Follower checkboxes changed: apply the form outside any preview, then
    // resume teaching.
    void ReloadMakerController::applyFollowerChange(const MechanismForm& form)
    {
        const bool resume = _mechanismTeaching;
        stopMechanismPreview();
        if (_clip.valid()) {
            (void)applyPose(_playbackTime);
        }
        applyMechanismForm(form);
        if (resume) {
            beginMechanismTeaching();
        }
    }

    void ReloadMakerController::suggestMechanismFollowers()
    {
        stopMechanismPreview();
        if (_clip.valid()) {
            (void)applyPose(_playbackTime);
        }
        analyzeMechanismAnimation();
    }
}
