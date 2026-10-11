// The Operate simulation's lifecycle and actions: start and stop, the button
// and MCP actions, the live clock, and dragging operating parts. Poses come
// from SimulationPose.cpp; the mechanism motion from MechanismMotion.cpp.
#include "ReloadMakerController.h"
#include "domain/presets/WeaponPresets.h"
#include "MagazineViewport.h"
#include "ReloadDriveViewport.h"
#include "areas/loadout/SceneTree.h"
#include "areas/operate/SimulationNames.h"
#include "model/nifmodel.h"

#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        // Fraction of the operating part's travel at which a reload counts as racked.
        constexpr float kRackTravelFraction = 0.95f;
        // Below this fraction of its travel the slide is at rest.
        constexpr float kRestTravelFraction = 0.05f;
    }

    // Selects the native clip for the simulation state: the first catalog
    // clip at rest, the reload, or the fire clip matching the receiver's
    // cadence. The catalog lists each canonical single clip before its
    // alternates.
    bool ReloadMakerController::selectSimulationClip(bool fire, bool reload)
    {
        int index = 0;
        if (fire || reload) {
            index = -1;
            for (int i = 0; i < _animationActions.size(); ++i) {
                const auto& action = _animationActions[i];
                const bool reloadClip = reload && action.baseReload;
                const bool fireClip = fire && action.family == ActionFamily::Fire && action.automatic == _simulationAmmo.automatic;
                if (reloadClip || fireClip) {
                    index = i;
                    break;
                }
            }
        }
        if (index < 0 || index >= _animationActions.size()) {
            if (!fire) {
                _simulationError = "No resolved reload animation.";
            } else if (_simulationAmmo.automatic) {
                _simulationError = "No resolved automatic-fire animation for this receiver.";
            } else {
                _simulationError = "No resolved single-fire animation.";
            }
            return false;
        }
        selectAction(index);
        return _clip.valid();
    }

    // Starts the simulation from the open weapon's captured reload. Leaves it
    // inactive, with the reason in _simulationError, when the rig, magazine
    // assembly or mechanism motion cannot be resolved.
    void ReloadMakerController::prepareSimulation()
    {
        if (!simulationWorkspace(_workspace) || _loadThread || _reloadCapturing || !_reloadRecipe) {
            updateSimulationControls();
            return;
        }
        if (_simulationActive) {
            return;
        }
        StageClock total(_loadTimings, "operate.prepare");
        SimulationRig rig;
        _simulationError.clear();
        bool built = false;
        {
            StageClock clock(_loadTimings, "operate.rig");
            built = buildCurrentSimulationRig(rig, _simulationError);
        }
        if (!built) {
            updateSimulationControls();
            return;
        }
        _simulationRig = rig;
        if (!resolveSimulationMagazineRoots()) {
            _simulationError = "The complete magazine assembly could not be resolved; simulation is unavailable.";
            _simulationRig.reset();
            updateSimulationControls();
            return;
        }
        recordReloadAxes();
        _simulationAmmo = resolveWeaponAmmunition(*currentPlugin(), *currentWeapon(), *configuration(), _referenceNames);
        _simulation.fireCycleSchedule.clear();
        bool motion = false;
        {
            StageClock clock(_loadTimings, "operate.mechanismMotion");
            motion = prepareMechanismMotion();
        }
        if (!motion) {
            updateSimulationControls();
            return;
        }
        _simulation.configureAmmunition(_simulationAmmo.valid() ? _simulationAmmo.capacity : 0, _simulationAmmo.pistol, rig.mechanism);
        _simulation.hkSlap=rig.hkSlap;
        _simulation.slapClose=rig.slapClose;_simulation.magazineEjectionSpeed=rig.magazineEjectionSpeed;
        _simulation.openBolt=rig.openBolt;
        _simulation.reset();
        _simulation.pistolLock=rig.lastShotHoldOpen;
        if(const auto* preset=checklistPreset())_simulation.magazineRelease=preset->model.feed.magazineRelease;
        _simulation.fireCycleSeconds = 0;
        if (isBoltActionConfiguration()) {
            _simulation.boltEjectProgress = _boltCycle.ejectProgress;
        }
        if (isRifleConfiguration()) {
            _simulation.fireCycleSeconds = _simulationAmmo.automatic ? _rifleFireInterval : _rifleSingleDuration;
        } else if ((rig.mechanism == WeaponMechanism::PistolSlide || rig.mechanism == WeaponMechanism::CellLever) && _simulationAmmo.automatic) {
            const auto single = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& action) {
                return action.family == ActionFamily::Fire && !action.automatic;
            });
            const auto automatic = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& action) {
                return action.family == ActionFamily::Fire && action.automatic;
            });
            QString error;
            const auto singleClip = single == _animationActions.cend() ? HkxClip{} :
                HkxReader::readClip(readAnimationResource(single->resourcePath, error), error);
            if (!singleClip.valid() || !measureRifleFireInterval(singleClip, automatic == _animationActions.cend() ? nullptr : &*automatic, error)) {
                if (_simulationError.isEmpty()) {
                    _simulationError = "Cannot resolve automatic pistol firing cadence: " + error;
                }
                updateSimulationControls();
                return;
            }
            _simulation.fireCycleSeconds = _rifleFireInterval;
        }
        _simulationActive = true;
        _reloadPreviewTimer->stop();
        _reloadPlaying = false;
        bool clip = false;
        {
            StageClock clock(_loadTimings, "operate.clip");
            clip = selectSimulationClip(false, false);
        }
        if (!clip) {
            stopSimulation();
            updateSimulationControls();
            return;
        }
        {
            StageClock clock(_loadTimings, "operate.magazine");
            prepareMagazineInteraction();
        }
        if (!magazineHandAllowed(_interactionHand)) {
            _interactionHand = defaultMagazineHand();
        }
        resolveMagazineGrabGrip();
        resolveOperatingFingers();
        {
            StageClock clock(_loadTimings, "operate.sounds");
            prepareSounds();
        }
        (void)applyPose(0);
        // Magazines shares the simulation without Operate's drag controls.
        const bool drive = _workspace != MagazinesWorkspace;
        _magazineViewport->setActive(drive);
        _simulationDrive->clearCurve();
        _simulationDrive->setActive(drive, drive);
        updateSimulationControls();
        presentMagazineStep();
        ++_revision;
    }

    // The scene roots hidden while the magazine is out, with their original
    // flags: the magazine branch of a shared receiver model, or the attached
    // parts holding the magazine. Also records the magazine's slot.
    bool ReloadMakerController::resolveSimulationMagazineRoots()
    {
        const auto& rig = *_simulationRig;
        _simulationVisibility.clear();
        const auto& owner = _reloadRecipe->tracks[rig.magazine].anchor;
        const auto* config = configuration();
        auto* model = activeModel();
        _simulationMagazineSlot = 0;
        // An installed magazine part is found by its hand anchor; a magazine
        // model that brings its carrier bone, by the carrier's record; one
        // built into the receiver (or a pistol's magazine branch) is the
        // carrier itself.
        const int magazineAnchor = currentMagazineAnchor();
        const bool installedMagazine = magazineAnchor >= 0 && _handPoseAnchors[magazineAnchor].description.anchorKind == "installed-part";
        if ((rig.magazineBranchOnly || magazineBuiltIn()) && model) {
            const auto node = _sceneNodes[_reloadTrackNodes[rig.magazine]];
            _simulationVisibility.append({ node, model->get<int>(node, "Flags") });
        } else if (config && model) {
            for (auto it = _partRoots.cbegin(); it != _partRoots.cend(); ++it) {
                bool match = false;
                if (installedMagazine) {
                    match = it.value() == _handPoseAnchors[magazineAnchor].node;
                } else {
                    match = poseRecordKey(config->selected(it.key())).compare(owner.ownerRecord, Qt::CaseInsensitive) == 0;
                }
                if (match && it.value().isValid()) {
                    _simulationMagazineSlot = it.key();
                    // Installed bodies can share the receiver's carrier with
                    // rounds and a follower. The whole carrier is the loose
                    // assembly; the installed root remains the hand reference.
                    const auto carrier = _sceneNodes[_reloadTrackNodes[rig.magazine]];
                    const auto root = installedMagazine && isWithinBlock(*model, it.value(), carrier) ? carrier : it.value();
                    _simulationVisibility.append({ root, model->get<int>(root, "Flags") });
                }
            }
        }
        return !_simulationVisibility.isEmpty();
    }

    // Samples the mechanism's motion and finds the reload's rack and feed
    // moments, in seconds into the reload clip (-1 when the mechanism has
    // none). Errors go to _simulationError.
    bool ReloadMakerController::prepareMechanismMotion()
    {
        const auto& rig = *_simulationRig;
        const auto frameSeconds = [this](float frame, int frames) {
            return _reloadRecipe->duration * frame / (frames - 1);
        };
        _glockReloadRackAt = -1;
        _glockReloadFeedAt = -1;
        if (simulationMechanism() == WeaponMechanism::PistolSlide) {
            // Racked when the slide nears full travel; fed at the rack's
            // reloadComplete marker. Animators may place it while the slide is
            // still travelling back (the 1911 marks it a frame early), so a
            // marker counts from the moment that stroke leaves rest, and the
            // feed never comes before the rack: fed first, the rack would
            // eject the round it just chambered.
            const auto& motion = _reloadRecipe->tracks[rig.slide].poses;
            const auto rest = simulationNativeTrackPose(*_reloadRecipe, rig, rig.slide, 0);
            const float travel = reloadPoseDistance(rest, simulationNativeTrackPose(*_reloadRecipe, rig, rig.slide, 1));
            float strokeStart = -1;
            for (int frame = 0; frame < motion.size(); ++frame) {
                if (reloadPoseDistance(rest, motion[frame]) >= travel * kRackTravelFraction) {
                    _glockReloadRackAt = frameSeconds(frame, motion.size());
                    int start = frame;
                    while (start > 0 && reloadPoseDistance(rest, motion[start - 1]) > travel * kRestTravelFraction) {
                        --start;
                    }
                    strokeStart = frameSeconds(start, motion.size());
                    break;
                }
            }
            for (const auto& event : _reloadClip.annotations) {
                if (event.text.compare("reloadComplete", Qt::CaseInsensitive) == 0 && event.timeSeconds >= strokeStart) {
                    _glockReloadFeedAt = std::max(event.timeSeconds, _glockReloadRackAt);
                    break;
                }
            }
            if (_glockReloadFeedAt < 0) {
                _simulationError = "The pistol reload has no reloadComplete marker after its slide cycle.";
                return false;
            }
        }
        if (isRifleConfiguration()) {
            if (!prepareRifleMotion()) {
                return false;
            }
            // Racked when the handle nears full travel; fed when the action closes.
            const auto& handle = rig.derivedHandle.isEmpty() ? _reloadRecipe->tracks[rig.slide].poses : rig.derivedHandle;
            const auto rest = rig.derivedHandle.isEmpty() ? simulationNativeTrackPose(*_reloadRecipe, rig, rig.slide, 0) : simulationTrackPose(*_reloadRecipe, rig, rig.slide, 0);
            const auto open = rig.derivedHandle.isEmpty() ? simulationNativeTrackPose(*_reloadRecipe, rig, rig.slide, 1) : simulationTrackPose(*_reloadRecipe, rig, rig.slide, 1);
            const float travel = reloadPoseDistance(rest, open);
            for (int frame = 0; frame < handle.size(); ++frame) {
                if (reloadPoseDistance(rest, handle[frame]) >= travel * kRackTravelFraction) {
                    _glockReloadRackAt = rig.derivedHandle.isEmpty() ? frameSeconds(frame, handle.size())
                        : frameSeconds(simulationNativeFrame(*_reloadRecipe, rig, float(frame)), _reloadRecipe->frames);
                    break;
                }
            }
            _glockReloadFeedAt = frameSeconds(simulationNativeFrame(*_reloadRecipe, rig, float(rig.closedFrame)), _reloadRecipe->frames);
        }
        if (isBoltActionConfiguration()) {
            if (!prepareBoltActionMotion()) {
                return false;
            }
            _glockReloadRackAt = frameSeconds(simulationNativeFrame(*_reloadRecipe, rig, float(rig.openFrame)), _reloadRecipe->frames);
            _glockReloadFeedAt = frameSeconds(simulationNativeFrame(*_reloadRecipe, rig, float(rig.closedFrame)), _reloadRecipe->frames);
        }
        return true;
    }

    // Ends the simulation. The hidden magazine roots get their flags back
    // before they are forgotten.
    void ReloadMakerController::stopSimulation()
    {
        _simulationStaticLocalPose.clear();
        _simulationTriggerDown = false;
        _audio.stop();
        _soundTracking = false;
        _magazineFitPreview = false;
        if (_simulationTimer) {
            _simulationTimer->stop();
        }
        _simulationActive = false;
        _simulation.reset();
        _magazineGrabGrip.reset();
        _magazineGrabBinding = -1;
        _magazineGrabError.clear();
        if (_magazineViewport) {
            _magazineViewport->setActive(false);
        }
        if (_simulationDrive) {
            _simulationDrive->setActive(false, false);
            _simulationDrive->clearCurve();
        }
        applySimulationVisibility();
        _simulationVisibility.clear();
        _magazineVisuals.clear();
        _magazineRail.reset();
        _ghostNatives.clear();
        refreshNativeGhost({});
        updateMagazineAuthoring();
        if (_workspace == MagazinesWorkspace) {
            refreshMagazinesUi();
        }
    }

    // Runs one Operate action by name (the buttons and MCP use the same names).
    // Starts the simulation first when the Operate workspace is open.
    void ReloadMakerController::runSimulationAction(const QString& action)
    {
        if (!_simulationActive && simulationWorkspace(_workspace)) {
            prepareSimulation();
        }
        if (!_simulationActive) {
            setStatus(_simulationError.isEmpty() ? "Wait for the simulation source to be ready." : _simulationError, true);
            return;
        }
        _simulationError.clear();
        bool accepted = false;
        if (action == "reset") {
            // Reset starts clean: a press left held (an MCP trigger_down with
            // no trigger_up) would otherwise swallow the next press.
            _simulationTriggerDown = false;
            _simulation.reset();
            resetSoundTracking();
            _simulationTimer->stop();
            _simulationDrive->clearCurve();
            accepted = selectSimulationClip(false, false);
        } else if (action == "drop_magazine") {
            accepted = _simulation.dropMagazine();
            if(accepted&&_simulation.magazine.extracting)_magazineViewport->beginCarry();
            if(accepted&&_simulation.magazine.ejecting)startSimulationClock();
        } else if (action == "slide_release" || action == "bolt_release" || action == "slap") {
            accepted = (action=="slap"||(_simulation.cellFed()&&_simulation.slapClose)) ? _simulation.slap() : _simulation.slideRelease();
            if (accepted) {
                startSimulationClock();
            }
        } else if (action == "mag_out" || action == "mag_in" || action == "grab_magazine") {
            if (!_magazineRail) {
                setStatus(_magazineError, true);
                return;
            }
            accepted = runMagazineAction(action);
        } else if (action == "fire" || action == "reload") {
            const bool fire = action == "fire";
            if (fire && _simulation.canDryFire()) {
                if(_simulation.openBolt&&_simulation.slideRelease())startSimulationClock();
                emitSoundEvent("dry_fire");
                refreshSoundsUi();
                setStatus("Dry fire · chamber empty");
                ++_revision;
                return;
            }
            accepted = startNativeAction(fire);
        }
        if (!accepted) {
            setStatus(simulationRejection(action), true);
            return;
        }
        if (!applyPose(simulationPoseSeconds())) {
            _simulationError = _lastError;
            stopSimulation();
            updateSimulationControls();
            return;
        }
        updateSimulationSounds();
        applySimulationVisibility();
        updateSimulationControls();
        ++_revision;
        if (action == "drop_magazine") {
            setStatus(_simulation.magazine.extracting?"Magazine unlocked. Pull it out, then release to drop it.":"Magazine removed. Grab a fresh magazine and guide it into the well.");
        } else {
            setStatus(simulationActionName(_simulation.action, isRifleConfiguration(), isBoltActionConfiguration()));
        }
    }

    // MAG OUT, MAG IN and GRAB. They act only while the weapon is idle.
    bool ReloadMakerController::runMagazineAction(const QString& action)
    {
        bool accepted = false;
        if (_simulation.action == SimulationAction::Idle) {
            if (action == "mag_out") {
                accepted = _simulation.magazineOut();
            } else if (action == "mag_in") {
                accepted = _simulation.magazineIn(*_magazineRail);
            } else {
                accepted = _simulation.grabMagazine(*_magazineRail);
                if (accepted) {
                    if (_magazineGrabGrip) {
                        setArmsShown(true);
                    }
                    _magazineViewport->beginCarry();
                }
            }
        }
        if (accepted && _simulation.magazine.animated()) {
            startSimulationClock();
        }
        return accepted;
    }

    // Starts the native fire or reload clip. The simulation state is restored
    // when the weapon has no matching clip.
    bool ReloadMakerController::startNativeAction(bool fire)
    {
        const auto previous = _simulation;
        const bool accepted = fire ? _simulation.fire() : _simulation.reload(_glockReloadRackAt, _glockReloadFeedAt);
        if (!accepted) {
            return false;
        }
        if (!selectSimulationClip(fire, !fire)) {
            _simulation = previous;
            return false;
        }
        _simulation.triggerHeld = false;
        _simulationDrive->clearCurve();
        resetSoundTracking(false);
        playSourceSounds(_clip, -.0001f, 0);
        if (fire) {
            playShotSound();
        }
        startSimulationClock();
        return true;
    }

    QString ReloadMakerController::simulationRejection(const QString& action) const
    {
        if((action=="drop_magazine"||action=="mag_out")&&_simulation.magazineRelease!=MagazineRelease::Button){
            return _simulation.magazineRelease==MagazineRelease::Manual?"Grab the magazine and pull it out by hand.":"Operate the ejection lever to release the cell.";
        }
        if (!_simulationAmmo.valid()) {
            return _simulationAmmo.error;
        }
        if (isBoltActionConfiguration()) {
            return "Cycle the bolt fully back and forward to load and lock the next round.";
        }
        if (action == "fire" && !_simulation.chambered) {
            return "Chamber empty. Insert a magazine and cycle or release the slide.";
        }
        return "Finish the current interaction; the slide must be closed and chamber loaded to fire.";
    }

    // Repeatable Single Fire (OMOD 86) repeats the single-shot clip while
    // held. It must not select the Automatic (OMOD 25) animation or cadence.
    void ReloadMakerController::setSimulationTrigger(bool pressed)
    {
        if (pressed && _simulationTriggerDown) {
            return;
        }
        _simulationTriggerDown = pressed;
        if (!pressed) {
            _simulation.triggerHeld = false;
            return;
        }
        if (_simulation.action == SimulationAction::Fire) {
            _simulation.triggerHeld = _simulationAmmo.repeatsWhileHeld();
            return;
        }
        runSimulationAction("fire");
        if (_simulation.action == SimulationAction::Fire) {
            _simulation.triggerHeld = _simulationAmmo.repeatsWhileHeld();
        }
    }

    // The live clock runs while a clip, a returning part or the magazine moves.
    void ReloadMakerController::startSimulationClock()
    {
        _simulationClock.restart();
        _simulationTimer->start();
    }

    // Live clock tick. It only refreshes the view, so the revision stays.
    void ReloadMakerController::tickSimulation()
    {
        const QScopedValueRollback<bool> viewOnly(_viewRefresh, true);
        if (!_simulationActive || !simulationWorkspace(_workspace)) {
            _simulationTimer->stop();
            return;
        }
        const float delta = float(_simulationClock.nsecsElapsed()) / 1e9f;
        _simulationClock.restart();
        advanceSimulation(delta);
    }

    // Advances the simulation by `delta` seconds: the native clip and its
    // sounds, the magazine motion and the pose. The clock stops once the
    // weapon and magazine are at rest.
    void ReloadMakerController::advanceSimulation(float delta)
    {
        if (_simulation.action == SimulationAction::Reload) {
            const float seconds = _simulation.seconds;
            playSourceSounds(_clip, seconds, std::min(seconds + delta, _clip.durationSeconds));
        }
        const bool finished = _simulation.tick(delta, _clip.durationSeconds);
        if (_magazineRail) {
            _simulation.magazine.tick(*_magazineRail, delta);
        }
        if (finished && !selectSimulationClip(false, false)) {
            stopSimulation();
            updateSimulationControls();
            return;
        }
        if (!applyPose(simulationPoseSeconds())) {
            stopSimulation();
            updateSimulationControls();
            return;
        }
        _playbackTime = simulationPoseSeconds();
        applySimulationVisibility();
        _simulationDrive->setTime(_simulation.inputProgress());
        updateSimulationSounds();
        if (_simulation.action == SimulationAction::Idle && !_simulation.magazine.animated()) {
            _simulationTimer->stop();
        }
        updateSimulationControls();
    }

    // Press in the viewport. Returns false when the press missed the weapon.
    // A press on the operating part, while it can be operated, grabs it and
    // fills `curve` with the hand path along its stroke.
    bool ReloadMakerController::beginSimulationGrab(QPointF point, ReloadDriveCurve& curve)
    {
        if (!_simulationActive || !_simulationRig || !_reloadRecipe) {
            return false;
        }
        int automatic = -1;
        Vec3f hit;
        QString surface;
        QString error;
        QVector<int> candidates;
        if (!pickWeaponPart(point, automatic, hit, surface, candidates, error)) {
            return false;
        }
        const bool onInput = std::any_of(candidates.cbegin(), candidates.cend(), [&](int anchor) {
            return operatingSurfaceCandidate(anchor);
        });
        curve.onDrivenPart = onInput;
        if (!operatingGrabAvailable() || !onInput || !_simulation.grab()) {
            return true;
        }
        _simulationTimer->stop();
        const bool boltAction = isBoltActionConfiguration();
        if (boltAction) {
            setBoltDriveCurve(hit, curve);
        } else if (!setStrokeDriveCurve(hit, curve, error)) {
            return true;
        }
        (void)applyPose(0);
        updateSimulationControls();
        if (_simulation.cellFed()) {
            setStatus("Cell lever grabbed � pull to eject � close after inserting a new cell");
        } else if (boltAction) {
            setStatus("Bolt grabbed · pull back to open, push forward to lock · release holds position");
        } else if (isRifleConfiguration()) {
            setStatus("Handle grabbed · pull fully to operate the bolt · Esc cancels");
        } else {
            setStatus("Slide grabbed · drag along its travel · Esc cancels");
        }
        ++_revision;
        return true;
    }

    // The bolt's hand path: a straight stroke along the cycle's input axis,
    // placed so the grabbed point sits at the bolt's current progress.
    void ReloadMakerController::setBoltDriveCurve(const Vec3f& hit, ReloadDriveCurve& curve) const
    {
        const auto axis = rotated(_globalPose[_weaponBoneIndex].rotation, _boltCycle.inputAxis);
        const float travel = _boltCycle.inputTravel;
        const QVector3D stroke(axis.x * travel, axis.y * travel, axis.z * travel);
        const QVector3D start = QVector3D(hit.x, hit.y, hit.z) - stroke * _simulation.slide;
        curve.points = { start, start + stroke };
        curve.times = { 0, 1 };
        curve.current = _simulation.slide;
        curve.endpointSnapFraction = .025f;
    }

    // A straight controller guide drives the complete visual animation.
    // It follows measured bolt travel, or the contact's endpoint chord for
    // a purely rotating lever. Both endpoints remain reachable in one pull.
    bool ReloadMakerController::setStrokeDriveCurve(const Vec3f& hit, ReloadDriveCurve& curve, QString& error)
    {
        const auto& rig = *_simulationRig;
        Transformf world;
        Transformf inverse;
        if (!sceneNodeWorld(_sceneNodes[_reloadTrackNodes[rig.slide]], world, error) || !invertUniformTransform(world, inverse)) {
            _simulation.release(true, false);
            setStatus(error, true);
            return false;
        }
        const auto contact = composed(inverse, Transformf{ hit }).translation;
        curve.current = _simulation.inputProgress();
        const auto locations = currentGrabLocations();
        const auto local = locations.points.isEmpty() ? simulationControllerStroke(*_reloadRecipe, rig, contact) : operatingControllerStroke();
        const auto& weapon = _globalPose[_weaponBoneIndex];
        const auto delta = rotated(weapon.rotation, { local.x * weapon.scale.x, local.y * weapon.scale.y, local.z * weapon.scale.z });
        const QVector3D stroke(delta.x, delta.y, delta.z);
        if (!std::isfinite(stroke.lengthSquared()) || stroke.lengthSquared() < .0001f) {
            _simulation.release(true, false);
            error = "The operating contact has no measurable controller stroke. Pick a point away from its pivot.";
            setStatus(error, true);
            return false;
        }
        const QVector3D start = QVector3D(hit.x, hit.y, hit.z) - stroke * curve.current;
        // Gesture travel beyond the catch lets a locked-open slide be pulled and
        // released. Geometry remains clamped to the verified source stroke.
        curve.points = { start, start + stroke, start + stroke * 1.08f };
        curve.times = { 0, 1, 1.08f };
        return true;
    }

    Vec3f ReloadMakerController::operatingControllerStroke() const
    {
        if (!_simulationRig || !_reloadRecipe) return {};
        if (isBoltActionConfiguration()) {
            const auto& a = _boltCycle.inputAxis;
            return { a.x * _boltCycle.inputTravel, a.y * _boltCycle.inputTravel, a.z * _boltCycle.inputTravel };
        }
        const auto locations = currentGrabLocations();
        Vec3f contact{};
        // A guide belongs to the mechanism. Picking another acquisition point
        // or switching physical hands must not change its direction or travel.
        if (!locations.points.isEmpty()) contact = locations.points.first().point;
        return simulationControllerStroke(*_reloadRecipe, *_simulationRig, contact);
    }

    void ReloadMakerController::dragSimulation(float progress)
    {
        if (!_simulationActive) {
            return;
        }
        _simulation.drag(progress);
        if(_simulation.magazine.ejecting)startSimulationClock();
        if (!applyPose(0)) {
            _simulationError = _lastError;
            stopSimulation();
        }
        updateSimulationSounds();
        applySimulationVisibility();
        updateSimulationControls();
    }

    // Releases the grabbed part. With spring return on, the part starts
    // returning and the clock runs.
    void ReloadMakerController::releaseSimulation(bool cancel)
    {
        if (!_simulationActive) {
            return;
        }
        _simulation.release(cancel, _slideSpringReturn);
        if (cancel) {
            resetSoundTracking();
        }
        if (_simulation.action == SimulationAction::Returning || _simulation.magazine.animated()) {
            startSimulationClock();
        }
        if (!applyPose(0)) {
            _simulationError = _lastError;
            stopSimulation();
        }
        updateSimulationControls();
        ++_revision;
    }
}
