// Magazine grips: the hand fit used when the selected reload hand grabs the
// magazine. Each magazine (per hand) has an assigned fit or uses the
// weapon's default; editing keeps unsaved fits per magazine and hand, and
// saving assigns a new library snapshot and saves the project.
#include "ReloadMakerController.h"
#include <algorithm>
#include "MagazineViewport.h"
#include "ReloadBundle.h"
#include "areas/loadout/SceneTree.h"

#include <QDateTime>
#include <QTimer>
#include <QUuid>

namespace reloadmaker
{
    // Index of the magazine anchor the placement belongs to, or -1. When the
    // anchor decides the magazine, that one anchor; otherwise (a pistol
    // magazine that brings its own bone) a grab-capable magazine of the
    // same record and model.
    int ReloadMakerController::magazineGrabAnchor(const HandPosePlacement& placement) const
    {
        if (magazineAnchorDecides()) {
            const int index = currentMagazineAnchor();
            if(index<0)return -1;
            const auto& target=_handPoseAnchors[index].description;
            if(sameGripAnchor(placement,target))return index;
            // A source reload may hold its separate incoming copy (MP5).
            // Only a classified magazine spare from the same owner/model is
            // eligible for conversion; unrelated receiver parts are not.
            if(target.ownerRecord.compare(placement.ownerRecord,Qt::CaseInsensitive)!=0||normalizeNifPath(target.ownerModel)!=normalizeNifPath(placement.ownerModel))return -1;
            for(const auto& anchor:_handPoseAnchors)if(sameMechanismPart(anchor.description,placement))
                for(const auto& visual:_magazineVisuals)if(visual.nativeOnly&&isWithinBlock(*activeModel(),visual.node,anchor.node))return index;
            return -1;
        }
        // A frame exactly on the magazine's carrier is that magazine, whatever
        // the carrier is called (the USP's WeaponMagazineChild2 helper pivot).
        if (const int current = currentMagazineAnchor(); current >= 0 && sameGripAnchor(placement, _handPoseAnchors[current].description)) {
            return current;
        }
        if (placement.anchorName != "WeaponMagazine" && placement.anchorName != "WeaponExtra3") {
            return -1;
        }
        // A frame on the installed magazine's model maps to the anchor the
        // editor and simulation use for it. The first supported anchor on the
        // model can be another one: on the Glock, the installed part's "Mag"
        // root precedes the WeaponMagazine carrier, and the editor then never
        // recognised its own draft.
        const auto sameModel = [&](const HandPosePlacement& candidate) {
            return candidate.ownerRecord.compare(placement.ownerRecord, Qt::CaseInsensitive) == 0 &&
                normalizeNifPath(candidate.ownerModel) == normalizeNifPath(placement.ownerModel);
        };
        if (const int current = currentMagazineAnchor(); current >= 0 && sameModel(_handPoseAnchors[current].description)) {
            return current;
        }
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            const auto& candidate = _handPoseAnchors[i].description;
            if (magazineGrabSupported(candidate) && sameModel(candidate)) {
                return i;
            }
        }
        return -1;
    }

    // Keeps an unsaved magazine fit for its magazine and hand, so leaving
    // and re-entering the editor resumes it; a saved one drops the draft.
    void ReloadMakerController::rememberMagazineGrip()
    {
        if (!_partGripDraft || magazineGrabAnchor(_partGripDraft->pose.placement) < 0) {
            return;
        }
        auto copy = *_partGripDraft;
        copy.pose.name = _gripDraftName.trimmed();
        const auto key = handAssignmentKey(magazineGrabKey(copy.weaponRecord, copy.pose.placement.ownerRecord), copy.pose.hand);
        if (magazineEditorState()["unsaved"].toBool()) {
            _magazineGripDrafts[key] = copy;
        } else {
            _magazineGripDrafts.remove(key);
        }
    }

    // Opens the selected magazine's fit for the reload hand: a named saved
    // fit, the unsaved draft, or the assigned / default fit. Without one the
    // form starts on the magazine with the selected saved pose. In Operate
    // the magazine is shown detached and frozen while the hand is edited.
    bool ReloadMakerController::editMagazineGrip(const QString& savedId)
    {
        const int anchor = currentMagazineAnchor();
        if (anchor < 0 || _loadThread || _reloadCapturing || _rebuildTimer->isActive()) {
            setStatus("Wait for the selected magazine to finish loading.", true);
            return false;
        }
        rememberMagazineGrip();
        const auto part = _handPoseAnchors[anchor].description;
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto key = handAssignmentKey(magazineGrabKey(weapon, part.ownerRecord), _interactionHand);
        QString error;
        std::optional<PartGrip> fit;
        bool resumed = false; // an unsaved draft stays unsaved when reopened
        if (!savedId.isEmpty()) {
            for (const auto& grip : _partGrips) {
                if (grip.pose.id == savedId) {
                    fit = grip;
                }
            }
            if (!fit || fit->weaponRecord.compare(weapon, Qt::CaseInsensitive) != 0 || magazineGrabAnchor(fit->pose.placement) != anchor) {
                setStatus("Choose a saved fit for this magazine. Use Saved pose to reuse fingers from another part.", true);
                return false;
            }
        } else if (_magazineGripDrafts.contains(key)) {
            fit = _magazineGripDrafts.value(key);
            resumed = true;
        } else {
            fit = magazineGripFor(weapon, part, _partGrips, _magazineGrabGrips, _magazineGrabDefaults, error, _interactionHand, _siblingMagazineFits);
        }
        if (!error.isEmpty()) {
            setStatus(error, true);
            return false;
        }
        // A retained draft may still reference the incoming animation copy.
        // Edit the physical magazine, using the same model-frame conversion
        // as Save; otherwise the gizmo follows a hidden spare while the real
        // magazine stays at the end of its insertion guide.
        if (fit && !sameGripAnchor(fit->pose.placement, part) && !convertToMagazineFrame(*fit, anchor)) {
            return false;
        }
        // Reuse the existing detached-magazine presentation, frozen for hand editing.
        // The magazine's pose remains the reference; gizmos never drive its geometry.
        if ((_workspace == OperateWorkspace || _workspace == MagazinesWorkspace) && _simulationActive && _magazineRail) {
            _simulationTimer->stop();
            _simulation.reset();
            if (!selectSimulationClip(false, false)) {
                setStatus(_simulationError, true);
                return false;
            }
            _simulation.magazine.drop();
            _simulation.magazine.grab(*_magazineRail);
            _magazineFitPreview = true;
        }
        _editingMagazine = true;
        setWorkspace(GripsWorkspace);
        setHandEditorPage(HandEditorPage::PartGrip);
        stopPlayback();
        if (fit) {
            if (!loadGripRecord(*fit)) {
                return false;
            }
            if (resumed) {
                _partGripOpened.clear();
            }
        } else {
            restoreHandPosePreview();
            _gripAnchor = anchor;
            showGripAnchor();
            if (const auto* pose = selectedHandPose()) {
                _gripPoseId = pose->id;
                showGripPose();
            }
            setGripDraftName("Magazine hold");
            (void)applyPose(_playbackTime);
            updatePartGripControls();
        }
        if (_magazineFitPreview) {
            _magazineViewport->setActive(false);
            _simulationDrive->setActive(false, false);
        }
        setStatus(fit ? "Edit this magazine’s hand with Move / Rotate. Save replaces its active fit." : "No fit assigned. Choose a named Saved pose, then Apply pose + hand rotation.");
        return true;
    }

    // The selected magazine's fit for the reload hand: its source (custom,
    // default or none), whether it is being edited with unsaved changes, and
    // whether the edit can be saved.
    QJsonObject ReloadMakerController::magazineEditorState() const
    {
        const int anchor = currentMagazineAnchor();
        if (anchor < 0 || !currentWeapon()) {
            return {};
        }
        const auto& part = _handPoseAnchors[anchor].description;
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto key = handAssignmentKey(magazineGrabKey(weapon, part.ownerRecord), _interactionHand);
        QString error;
        const auto fit = magazineGripFor(weapon, part, _partGrips, _magazineGrabGrips, _magazineGrabDefaults, error, _interactionHand, _siblingMagazineFits);
        QString name = part.ownerRecord;
        if (const auto* config = configuration()) {
            for (const auto& slot : config->entries()) {
                const auto* mod = config->mod(config->selected(slot.key));
                if (mod && poseRecordKey(mod->formId).compare(part.ownerRecord, Qt::CaseInsensitive) == 0) {
                    name = mod->name;
                }
            }
        }
        const bool editing = _partGripDraft && magazineGrabAnchor(_partGripDraft->pose.placement) == anchor;
        const bool dirty = editing && partGripSnapshot(*_partGripDraft) != _partGripOpened;
        const bool custom = _magazineGrabGrips.contains(key) || (fit && _magazineGrabGrips.value(magazineGrabKey(weapon, part.ownerRecord)) == fit->pose.id);
        return {
            { "magazine", name },
            { "ownerRecord", part.ownerRecord },
            { "anchor", anchor },
            { "hand", _interactionHand == PoseHand::Left ? "left" : "right" },
            { "assignment", custom ? "custom" : fit ? "default" : "none" },
            { "gripId", fit ? fit->pose.id : QString() },
            { "name", fit ? fit->pose.name : QString() },
            { "editing", editing },
            { "unsaved", dirty },
            { "canSave", editing && _gripPreviewValid && !_gripDraftName.trimmed().isEmpty() },
            { "preview", _magazineFitPreview },
            { "error", error.isEmpty() && editing && !_gripPreviewValid ? _gripError : error },
        };
    }

    // Drops this magazine's own fit for the reload hand so it uses the
    // weapon default, then reopens it and saves the project. A failure
    // restores the assignments.
    bool ReloadMakerController::useDefaultMagazineGrip()
    {
        const int anchor = currentMagazineAnchor();
        if (anchor < 0 || !currentWeapon()) {
            return false;
        }
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto& part = _handPoseAnchors[anchor].description;
        QString error;
        if (!magazineGripFor(weapon, part, _partGrips, {}, _magazineGrabDefaults, error, _interactionHand)) {
            return false;
        }
        const auto key = handAssignmentKey(magazineGrabKey(weapon, part.ownerRecord), _interactionHand);
        const auto assignments = _magazineGrabGrips;
        const auto drafts = _magazineGripDrafts;
        _magazineGrabGrips.remove(key);
        _magazineGripDrafts.remove(key);
        _partGripDraft.reset();
        if (!editMagazineGrip()) {
            _magazineGrabGrips = assignments;
            _magazineGripDrafts = drafts;
            return false;
        }
        noteDocumentChange(QStringLiteral("magazine fit"));
        resolveMagazineGrabGrip();
        refreshPartGripList();
        updatePartGripControls();
        setStatus("This magazine now uses the weapon default.");
        return true;
    }

    // Saves the edited magazine fit as a new library snapshot and assigns it
    // to the magazine (or as the weapon default for every magazine), then
    // saves the project. A fit captured on the native reload's auxiliary
    // magazine body is converted into the installed magazine's frame.
    bool ReloadMakerController::useMagazineGrabGrip(bool allMagazines)
    {
        const int anchor = _partGripDraft ? magazineGrabAnchor(_partGripDraft->pose.placement) : -1;
        if (anchor < 0) {
            setStatus("Place the hand on the selected magazine before saving.", true);
            return false;
        }
        updatePartGripControls();
        if (!canSavePartGrip()) {
            setStatus("Place the hand within reach and name the grip before assigning it.", true);
            return false;
        }
        PartGrip grip = *_partGripDraft;
        if (!magazineHandAllowed(grip.pose.hand)) {
            setStatus(magazineHandRule(), true);
            return false;
        }
        _interactionHand = grip.pose.hand;
        Transformf weaponInverse;
        if (!invertUniformTransform(_globalPose[_weaponBoneIndex], weaponInverse)) {
            setStatus("The weapon reference is not invertible.", true);
            return false;
        }
        const auto handInWeapon = composed(weaponInverse, composed(gripReferenceWorld(), grip.pose.placement.handInAnchor));
        const auto& target = _handPoseAnchors[anchor];
        if (!sameMechanismPart(grip.pose.placement, target.description) && !convertToMagazineFrame(grip, anchor)) {
            return false;
        }
        grip.pose.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        grip.pose.placement.handInWeapon = handInWeapon;
        grip.pose.name = _gripDraftName.trimmed();
        grip.pose.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        grip.pose.source = currentPoseSource();
        QString error;
        if (!_partGripLibrary.save(grip, error)) {
            setStatus(error, true);
            return false;
        }
        const auto assign = [&](QHash<QString, QString>& values, const QString& base) { values[handAssignmentKey(base, grip.pose.hand)] = grip.pose.id; };
        assign(_magazineGrabGrips, magazineGrabKey(grip.weaponRecord, grip.pose.placement.ownerRecord));
        // The first fit placed for a hand is the weapon's fallback for every
        // magazine (owner, 2026-10-08); later fits refine single magazines.
        const auto hasDefault = [&](PoseHand hand) { return _magazineGrabDefaults.contains(handAssignmentKey(grip.weaponRecord.toLower(), hand)); };
        const bool firstForHand = !hasDefault(grip.pose.hand);
        if (allMagazines || firstForHand) {
            assign(_magazineGrabDefaults, grip.weaponRecord.toLower());
        }
        if (sameGripAnchor(_partGripDraft->pose.placement, grip.pose.placement)) {
            _partGripDraft = grip;
        } else {
            _partGripDraft->pose.id = grip.pose.id;
            _partGripDraft->pose.name = grip.pose.name;
        }
        markPartGripOpened();
        rememberMagazineGrip();
        refreshPartGripLibrary();
        resolveMagazineGrabGrip();
        noteDocumentChange(QStringLiteral("magazine fit"));
        // The other hand starts as this fit's mirror when it has nothing yet.
        const PoseHand other = grip.pose.hand == PoseHand::Left ? PoseHand::Right : PoseHand::Left;
        if (!_mirroringMagazineFit && magazineHandAllowed(other) && !hasDefault(other)) {
            _mirroringMagazineFit = true;
            const auto hand = _interactionHand;
            const bool mirrored = mirrorMagazineGrip();
            _mirroringMagazineFit = false;
            _interactionHand = hand;
            if (!mirrored) {
                setStatus("Saved; the other hand could not be mirrored automatically. Fit it in Part grips.", true);
            }
            resolveMagazineGrabGrip();
        }
        updatePartGripControls();
        setStatus(allMagazines ? "Weapon default set. Other custom magazine fits are preserved."
                               : "Magazine fit replaced. Grab Magazine now uses this fit.");
        return true;
    }

    // The native Glock reload uses an auxiliary magazine body. Converts the
    // fit between the two authored model frames, never between their current
    // animated positions (the fresh copy may be parked off-weapon).
    bool ReloadMakerController::convertToMagazineFrame(PartGrip& grip, int targetAnchor)
    {
        const auto& target = _handPoseAnchors[targetAnchor];
        int source = -1;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            if (sameMechanismPart(_handPoseAnchors[i].description, grip.pose.placement)) {
                source = i;
                break;
            }
        }
        if (source < 0) {
            setStatus("The captured magazine frame is no longer available.", true);
            return false;
        }
        const auto rest = [&](int node) {
            Transformf result;
            for (int depth = 0; node >= 0 && depth < _assemblyNodes.size(); ++depth) {
                result = composed(_assemblyNodes[node].restLocal, result);
                node = _assemblyNodes[node].parent;
            }
            return result;
        };
        const int sourceNode = _sceneNodes.indexOf(_handPoseAnchors[source].node);
        const int targetNode = _sceneNodes.indexOf(target.node);
        Transformf inverse;
        if (sourceNode < 0 || targetNode < 0 || !invertUniformTransform(rest(targetNode), inverse)) {
            setStatus("Cannot convert the magazine's authored frames.", true);
            return false;
        }
        const auto bridge = composed(inverse, rest(sourceNode));
        const auto placement = composed(bridge, grip.pose.placement.handInAnchor);
        grip.pose.placement = target.description;
        grip.pose.placement.handInAnchor = placement;
        if (grip.surfaceContact) {
            grip.surfaceContact->pointInAnchor = composed(bridge, Transformf{ grip.surfaceContact->pointInAnchor }).translation;
        }
        grip.mechanismId.clear();
        return true;
    }

    // The fit Grab Magazine uses in Operate for the reload hand, with the
    // bridge from the magazine's live binding to its attachment frame.
    void ReloadMakerController::resolveMagazineGrabGrip()
    {
        _magazineGrabGrip.reset();
        _magazineGrabBinding = -1;
        _magazineGrabError.clear();
        showMagazineGrabGrip(nullptr);
        if (!_simulationRig || !_reloadRecipe) {
            return;
        }
        const auto anchor = simulationMagazineAnchor();
        if (anchor.ownerRecord.isEmpty() || magazineGrabAnchor(anchor) < 0) {
            return;
        }
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto selected = magazineGripFor(weapon, anchor, _partGrips, _magazineGrabGrips, _magazineGrabDefaults, _magazineGrabError, _interactionHand, _siblingMagazineFits);
        if (!selected) {
            return;
        }
        // The normal selector is the active assignment. Historical snapshots are
        // available explicitly in the hand editor, not mixed with the current fit.
        showMagazineGrabGrip(&*selected);
        _magazineGrabBridge = {};
        for (int i = 0; i < _nodeBindings.size(); ++i) {
            if (_nodeBindings[i].nodeIndex == _reloadTrackNodes[_simulationRig->magazine]) {
                _magazineGrabBinding = i;
                break;
            }
        }
        if (_magazineGrabBinding < 0) {
            _magazineGrabError = "The magazine grip has no live binding.";
            return;
        }
        if (isRifleConfiguration() || !sameGripAnchor(anchor, _reloadRecipe->tracks[_simulationRig->magazine].anchor)) {
            const int reference = currentMagazineAnchor();
            Transformf world;
            Transformf inverse;
            if (reference < 0 || !sceneNodeWorld(_handPoseAnchors[reference].node, world, _magazineGrabError) ||
                !invertUniformTransform(_boundGlobalPose[_magazineGrabBinding], inverse)) {
                _magazineGrabError = "The magazine attachment frame is unavailable.";
                return;
            }
            _magazineGrabBridge = composed(inverse, world);
        }
        _magazineGrabGrip = *selected;
    }

    // While the magazine is carried, solves the reload hand to its fit on the
    // moving magazine and restores the manually driven part targets.
    bool ReloadMakerController::applyMagazineGrabPose()
    {
        if (!_simulationActive || !_magazineGrabGrip || _magazineFitPreview) {
            return true;
        }
        if (!_simulation.magazine.carried()) {
            _magazineGrabError.clear();
            return true;
        }
        const auto target = composed(composed(_boundGlobalPose[_magazineGrabBinding], _magazineGrabBridge), _magazineGrabGrip->pose.placement.handInAnchor);
        if (!solvePartGrip(_skeleton, _localPose, _globalPose, _magazineGrabGrip->pose, target, _localPose, _globalPose, _magazineGrabError, GripReach::FollowTarget)) {
            return true;
        }
        // Arm composition preserves the weapon; restore its manually driven part
        // targets after evaluating the updated hand skeleton bindings.
        return evaluateSceneBindings(_skeleton, _nodeBindings, _sourceLocalPose, _stateBaselineLocalPose, _globalPose, _boundLocalPose, _boundGlobalPose,
                                     _poseError) &&
               applySimulationPose();
    }
}
