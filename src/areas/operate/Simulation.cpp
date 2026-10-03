// The Operate simulation's lifecycle and actions: start and stop, the button
// and MCP actions, the live clock, and dragging operating parts. Poses come
// from SimulationPose.cpp; the mechanism motion from MechanismMotion.cpp.
#include "ReloadMakerController.h"
#include "MagazineViewport.h"
#include "ReloadDriveViewport.h"
#include "areas/operate/SimulationNames.h"
#include "model/nifmodel.h"

#include <QListWidget>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>

namespace reloadmaker
{
    namespace
    {
        // Fraction of the operating part's travel at which a reload counts as racked.
        constexpr float kRackTravelFraction = 0.95f;
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
        if (index < 0 || index >= _animationList->count()) {
            if (!fire) {
                _simulationError = "No resolved reload animation.";
            } else if (_simulationAmmo.automatic) {
                _simulationError = "No resolved automatic-fire animation for this receiver.";
            } else {
                _simulationError = "No resolved single-fire animation.";
            }
            return false;
        }
        const QSignalBlocker blocker(_animationList);
        _animationList->setCurrentRow(index);
        selectAnimation(_animationList->item(index));
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
        SimulationRig rig;
        _simulationError.clear();
        if (!buildCurrentSimulationRig(rig, _simulationError)) {
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
        _simulationAmmo = resolveWeaponAmmunition(*currentPlugin(), *currentWeapon(), *configuration(), _referenceNames);
        if (!prepareMechanismMotion()) {
            updateSimulationControls();
            return;
        }
        _simulation.configureAmmunition(_simulationAmmo.valid() ? _simulationAmmo.capacity : 0, _simulationAmmo.pistol, rig.mechanism);
        if (isBoltActionConfiguration()) {
            _simulation.boltEjectProgress = _boltCycle.ejectProgress;
        }
        if (isRifleConfiguration()) {
            _simulation.fireCycleSeconds = _simulationAmmo.automatic ? _rifleFireInterval : _rifleSingleDuration;
        }
        _simulationActive = true;
        _reloadPreviewTimer->stop();
        _reloadPlaying = false;
        _reloadHeld = false;
        _reloadDrive->setActive(false, false);
        if (!selectSimulationClip(false, false)) {
            stopSimulation();
            updateSimulationControls();
            return;
        }
        prepareMagazineInteraction();
        if (isGlockConfiguration()) {
            _interactionHand = PoseHand::Left;
        }
        resolveMagazineGrabGrip();
        resolveOperatingFingers();
        prepareSounds();
        (void)applyPose(0);
        _magazineViewport->setActive(true);
        _simulationDrive->clearCurve();
        _simulationDrive->setActive(true, true);
        updateSimulationControls();
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
        // Rifles other than the MK18 and embedded SREP magazines find the
        // magazine by its installed hand anchor rather than by its record.
        const bool installedMagazine = isRifleConfiguration() && !isMk18Configuration() && !isSrepEmbeddedMagazine();
        const int magazineAnchor = installedMagazine ? currentMagazineAnchor() : -1;
        if ((rig.magazineBranchOnly || isSrepEmbeddedMagazine()) && model) {
            const auto node = _sceneNodes[_reloadTrackNodes[rig.magazine]];
            _simulationVisibility.append({ node, model->get<int>(node, "Flags") });
        } else if (config && model) {
            for (auto it = _partRoots.cbegin(); it != _partRoots.cend(); ++it) {
                bool match = false;
                if (installedMagazine) {
                    match = magazineAnchor >= 0 && it.value() == _handPoseAnchors[magazineAnchor].node;
                } else {
                    match = poseRecordKey(config->selected(it.key())).compare(owner.ownerRecord, Qt::CaseInsensitive) == 0;
                }
                if (match && it.value().isValid()) {
                    _simulationMagazineSlot = it.key();
                    _simulationVisibility.append({ it.value(), model->get<int>(it.value(), "Flags") });
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
        const auto frameSeconds = [this](int frame) {
            return _reloadRecipe->duration * frame / (_reloadRecipe->frames - 1);
        };
        _glockReloadRackAt = -1;
        _glockReloadFeedAt = -1;
        if (isGlockConfiguration()) {
            // Racked when the slide nears full travel; fed at the following
            // reloadComplete marker.
            const auto& motion = _reloadRecipe->tracks[rig.slide].poses;
            const auto& rest = motion[rig.closedFrame];
            const float travel = reloadPoseDistance(rest, motion[rig.openFrame]);
            for (int frame = 0; frame < motion.size(); ++frame) {
                if (reloadPoseDistance(rest, motion[frame]) >= travel * kRackTravelFraction) {
                    _glockReloadRackAt = frameSeconds(frame);
                    break;
                }
            }
            for (const auto& event : _reloadClip.annotations) {
                if (event.text.compare("reloadComplete", Qt::CaseInsensitive) == 0 && event.timeSeconds >= _glockReloadRackAt) {
                    _glockReloadFeedAt = event.timeSeconds;
                    break;
                }
            }
            if (_glockReloadFeedAt < 0) {
                _simulationError = "The Glock reload has no completion marker after its slide cycle.";
                return false;
            }
        }
        if (isRifleConfiguration()) {
            if (!prepareRifleMotion()) {
                return false;
            }
            // Racked when the handle nears full travel; fed when the action closes.
            const auto& handle = rig.derivedHandle.isEmpty() ? _reloadRecipe->tracks[rig.slide].poses : rig.derivedHandle;
            const float travel = reloadPoseDistance(handle[rig.closedFrame], handle[rig.openFrame]);
            for (int frame = 0; frame < handle.size(); ++frame) {
                if (reloadPoseDistance(handle[rig.closedFrame], handle[frame]) >= travel * kRackTravelFraction) {
                    _glockReloadRackAt = frameSeconds(frame);
                    break;
                }
            }
            _glockReloadFeedAt = frameSeconds(rig.closedFrame);
        }
        if (isBoltActionConfiguration()) {
            if (!prepareBoltActionMotion()) {
                return false;
            }
            _glockReloadRackAt = frameSeconds(rig.openFrame);
            _glockReloadFeedAt = frameSeconds(rig.closedFrame);
        }
        return true;
    }

    // Ends the simulation. The hidden magazine roots get their flags back
    // before they are forgotten.
    void ReloadMakerController::stopSimulation()
    {
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
            _simulation.reset();
            resetSoundTracking();
            _simulationTimer->stop();
            _simulationDrive->clearCurve();
            accepted = selectSimulationClip(false, false);
        } else if (action == "drop_magazine") {
            accepted = _simulation.dropMagazine();
        } else if (action == "slide_release" || action == "bolt_release") {
            accepted = _simulation.slideRelease();
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
            setStatus("Magazine removed. Grab a fresh magazine and guide it into the well.");
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
        startSimulationClock();
        return true;
    }

    QString ReloadMakerController::simulationRejection(const QString& action) const
    {
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

    // FIRE press and release. Holding the trigger keeps an automatic receiver
    // firing; a semi-automatic receiver fires once per press.
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
            _simulation.triggerHeld = _simulationAmmo.automatic;
            return;
        }
        runSimulationAction("fire");
        if (_simulation.action == SimulationAction::Fire) {
            _simulation.triggerHeld = _simulationAmmo.automatic;
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
        const auto& input = _reloadRecipe->tracks[_simulationRig->slide];
        const bool onInput = std::any_of(candidates.cbegin(), candidates.cend(), [&](int anchor) {
            return sameGripAnchor(_handPoseAnchors[anchor].description, input.anchor);
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
        if (boltAction) {
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

    // The slide or handle's hand path: the grabbed point carried along the
    // part's sampled stroke. Releases the grab when the part has no world
    // transform.
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
        const int samples = std::max(16, rig.closedFrame - rig.openFrame);
        for (int i = 0; i <= samples; ++i) {
            const float progress = float(i) / samples;
            const auto position = composed(simulationTrackPose(*_reloadRecipe, rig, rig.slide, progress), Transformf{ contact }).translation;
            curve.points.append({ position.x, position.y, position.z });
            curve.times.append(progress);
        }
        // Gesture travel beyond the catch lets a locked-open slide be pulled and
        // released. Geometry remains clamped to the verified source stroke.
        curve.points.append(curve.points.back() + (curve.points.back() - curve.points.front()) * .08f);
        curve.times.append(1.08f);
        return true;
    }

    void ReloadMakerController::dragSimulation(float progress)
    {
        if (!_simulationActive) {
            return;
        }
        _simulation.drag(progress);
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
        if (_simulation.action == SimulationAction::Returning) {
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
