// Operating-part motion for the simulation: the rig built from the captured
// reload, and the firing and bolt-cycle motion sampled from native clips.
// Sampled poses are weapon-local: each frame is divided by the weapon bone.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "areas/operate/WeaponLocalSampler.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        // Native clips are sampled at this rate when no recipe frame count applies.
        constexpr float kSamplesPerSecond = 120.0f;
        int sampleCount(float seconds)
        {
            return int(std::ceil(seconds * kSamplesPerSecond)) + 1;
        }
    }

    // A saved operating setup decides the rig; without one the source reload does.
    bool ReloadMakerController::buildCurrentSimulationRig(SimulationRig& rig, QString& error)
    {
        _operatingFallbackNote.clear();
        if (!_assignedMechanism) {
            error = "Confirm this weapon's behavior preset in the Reload checklist first.";
            return false;
        }
        OperatingSetup setup;
        const auto field = authoringPlanField(QStringLiteral("operating"));
        bool authored = field.source != PlanSource::None;
        if (authored) {
            QString inherited;
            if (!savedOperatingSetup(setup, inherited) || !buildConfiguredSimulationRig(setup, rig, inherited)) {
                // The part's own setup must work; an anchor or sibling setup
                // that does not fit this reload falls back to the source rig.
                if (field.source == PlanSource::Own) {
                    error = inherited;
                    return false;
                }
                _operatingFallbackNote = "The anchor setup (" + field.from + ") does not fit this reload (" + inherited + "); the preset's source rig is used.";
                authored = false;
                rig = {};
                setup = {};
            }
        }
        if (!authored) {
            if (!buildSourceSimulationRig(rig, error)) {
                return false;
            }
        }
        // Explicit per-configuration fire choices override the family default.
        // Replaying the measured stroke never substitutes a generic distance.
        const auto* preset = checklistPreset();
        if(preset) {
            rig.hkSlap=std::any_of(preset->model.mechanisms.cbegin(),preset->model.mechanisms.cend(),[](const auto& part){return part.kind==MechanismKind::HkSlap;});
            rig.lastShotHoldOpen=!preset->model.fire.lockOpenMechanism.isEmpty();
        }
        if ((!authored || setup.fireMode == "preset") && preset && preset->model.fire.motion == FireMotion::ReplayActionStroke) {
            rig.fireFromStroke = true;
            rig.presetFire = false;
        }
        if(authored&&setup.catchMode!="preset")rig.lastShotHoldOpen=setup.catchMode=="hold-open";
        rig.openBolt=authored&&setup.cycleMode!="preset"?setup.cycleMode=="open-bolt":preset&&preset->model.fire.afterShot==AfterShot::OpenBolt;
        if(rig.openBolt&&rig.hkSlap){error="Open-bolt sear and HK handle notch are distinct actions";return false;}
        if(rig.openBolt&&rig.mechanism!=WeaponMechanism::Ar15){error="Open-bolt firing requires an independent handle and bolt";return false;}
        return true;
    }

    // The saved operating setup with its track indices mapped onto this
    // configuration's recipe by node, so other parts (a different
    // underbarrel, grip or sight) keep it. Fails only when a part it names
    // is not in this reload.
    bool ReloadMakerController::savedOperatingSetup(OperatingSetup& setup, QString& error) const
    {
        const auto saved = authoringPlan().value("operating").toObject();
        if (!_reloadRecipe || !readOperatingSetup(saved, setup, error)) {
            return false;
        }
        return matchOperatingSetup(setup, error);
    }

    // A saved setup matched onto this configuration's reload by node. The
    // magazine track belongs to the magazine: when the saved one's path is
    // not in this reload, the track the reload itself detects carries it.
    bool ReloadMakerController::matchOperatingSetup(OperatingSetup& setup, QString& error) const
    {
        if (!remapOperatingSetup(setup, *_reloadRecipe, error)) {
            return false;
        }
        if (setup.parts.magazine < 0) {
            setup.parts.magazine = detectMagazineTrack(*_reloadRecipe);
            if (setup.parts.magazine < 0) {
                error = QStringLiteral("The magazine track is missing or ambiguous in this reload");
                return false;
            }
        }
        return true;
    }

    // The rig from the captured reload (rework 14, items 9-13). An AR-15
    // reload that never moves the charging handle derives the handle from the
    // bolt's stroke. Parts that move only with the bolt follow it.
    bool ReloadMakerController::buildSourceSimulationRig(SimulationRig& rig, QString& error)
    {
        const auto& recipe = *_reloadRecipe;
        const auto mechanism = simulationMechanism();
        const int handle = surfaceTrack(recipe, QStringLiteral("WeaponExtra1"));
        const bool staticHandle = mechanism == WeaponMechanism::Ar15 && handle >= 0 && reloadTrackStroke(recipe, handle) <= .01f;
        if (!(staticHandle ? buildBoltDrivenRig(rig, error) : buildSimulationRig(recipe, rig, error, mechanism))) {
            return false;
        }
        rig.slideTracks += boltDrivenTracks(recipe, rig);
        return true;
    }

    // An AR-15 whose reload never moves the charging handle: the handle
    // follows the bolt's stroke. A reload that never strokes the bolt either
    // (a tactical reload) takes the stroke from the weapon's standard reload;
    // its magazine and hands stay native.
    bool ReloadMakerController::buildBoltDrivenRig(SimulationRig& rig, QString& error)
    {
        const auto& recipe = *_reloadRecipe;
        const int bolt = surfaceTrack(recipe, QStringLiteral("WeaponBolt"));
        if (bolt < 0) {
            error = "The reload has no native bolt binding.";
            return false;
        }
        if (reloadTrackStroke(recipe, bolt) >= kCompleteStroke) {
            return buildSimulationRig(recipe, rig, error, WeaponMechanism::Ar15, true);
        }
        // A tactical reload in its own folder (G3 QuickReload/WPNReload.hkx)
        // has the same file name as the standard reload one folder up.
        const QString path = resolveAnimationPath(QStringLiteral("WPNReload.hkx"), recipe.source["animation"].toString());
        if (path.isEmpty()) {
            error = "This reload moves neither the charging handle nor the bolt, and the weapon has no standard reload to take the bolt stroke from.";
            return false;
        }
        const auto bytes = readAnimationResource(path, error);
        const auto clip = HkxReader::readClip(bytes, error);
        const auto base = HkxReader::readClip(readAnimationResource(recipe.source["baseline"].toString(), error), error);
        if (!clip.valid() || !base.valid()) {
            return false;
        }
        WeaponLocalSampler sampler(_skeleton, _nodeBindings, _reloadScenePlan, _weaponBoneIndex);
        QVector<Transformf> poses;
        if (!sampler.setBaselineClip(base, _readyLocalPose, error)) {
            return false;
        }
        for (int frame = 0; frame < recipe.frames; ++frame) {
            const float seconds = clip.durationSeconds * frame / (recipe.frames - 1);
            if (!sampler.sample(clip, seconds, error)) {
                return false;
            }
            if (!sampler.weaponFrameValid()) {
                error = "Invalid native bolt source frame.";
                return false;
            }
            poses.append(sampler.nodePose(_reloadTrackNodes[bolt]));
        }
        if (!buildSimulationRig(recipe, rig, error, WeaponMechanism::Ar15, true, poses)) {
            return false;
        }
        rig.mechanicalSource = {
            { "animation", path },
            { "duration", clip.durationSeconds },
        };
        return true;
    }

    // An AR-15 bolt worked by hand follows its reload track rather than the
    // fire cycle: it drives a derived handle and the fire clip has its own
    // stroke (with a replayed reload stroke the two are the same path).
    bool ReloadMakerController::manualBoltFromReload() const
    {
        return _simulationRig && _simulationRig->mechanism == WeaponMechanism::Ar15 && !_simulationRig->derivedHandle.isEmpty() &&
               !_rifleBoltFromReloadStroke;
    }

    // Rifle firing: the cadence from the native fire clips, and the bolt cycle
    // the firing pose replays. A weapon whose fire clips are missing,
    // unreadable or strokeless still reloads (owner, 2026-10-08: the fire
    // clip is not a reload gate): its bolt cycle replays the reload's own
    // measured stroke at a nominal cycle, and the note says so.
    bool ReloadMakerController::prepareRifleMotion()
    {
        _fireFallbackNote.clear();
        if (prepareRifleMotionFromClips()) {
            return true;
        }
        const auto failure = _simulationError;
        _simulationError.clear();
        constexpr float kNominalCycleSeconds = .1f; // 600 rounds per minute
        _rifleFireInterval = kNominalCycleSeconds;
        HkxClip nominal;
        nominal.durationSeconds = kNominalCycleSeconds;
        if (_simulationRig && _simulationRig->bolt >= 0 && useReloadBoltStroke(nominal)) {
            _fireFallbackNote = failure + " The reload's bolt stroke is replayed for firing; the game fires natively.";
            return true;
        }
        _simulationError = failure;
        return false;
    }

    bool ReloadMakerController::prepareRifleMotionFromClips()
    {
        _rifleBoltCycle.clear();
        _rifleBoltOpen = 0;
        _rifleFireInterval = 0;
        _rifleBoltSource.clear();
        _rifleBoltFromReloadStroke = false;
        const auto single = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& action) {
            return action.family == ActionFamily::Fire && !action.automatic;
        });
        const auto automatic = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& action) {
            return action.family == ActionFamily::Fire && action.automatic;
        });
        const bool hasAutomatic = automatic != _animationActions.cend();
        if (single == _animationActions.cend() || (_simulationAmmo.automatic && !hasAutomatic)) {
            _simulationError = "Rifle needs its native firing motion and applicable cadence.";
            return false;
        }
        QString error;
        const auto bytes = readAnimationResource(single->resourcePath, error);
        const auto clip = HkxReader::readClip(bytes, error);
        const auto base = HkxReader::readClip(readAnimationResource(single->baselineResourcePath, error), error);
        if (!clip.valid() || !base.valid()) {
            _simulationError = "Cannot read rifle fire source: " + error;
            return false;
        }
        if (!measureRifleFireInterval(clip, hasAutomatic ? &*automatic : nullptr, error)) {
            return false;
        }
        if (!sampleRifleFireBolt(base, clip, error)) {
            return false;
        }
        const int frames = int(_rifleBoltCycle.size());
        float travel = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const float distance = reloadPoseDistance(_rifleBoltCycle.front(), _rifleBoltCycle[frame]);
            if (distance > travel) {
                travel = distance;
                _rifleBoltOpen = frame;
            }
        }
        int closed = -1;
        for (int frame = _rifleBoltOpen + 1; frame < frames; ++frame) {
            if (reloadPoseDistance(_rifleBoltCycle.front(), _rifleBoltCycle[frame]) <= travel * .002f) {
                closed = frame;
                break;
            }
        }
        // A fire clip that never strokes the bolt replays the reload's stroke
        // (rework 14, item 14), as an authored "stroke" fire mode does.
        const auto& rig = *_simulationRig;
        bool reloadStrokes = false;
        if (rig.bolt >= 0) {
            const auto& reloadBolt = simulationTrackCurve(*_reloadRecipe, rig, rig.bolt);
            reloadStrokes = reloadPoseDistance(reloadBolt[rig.closedFrame], reloadBolt[rig.openFrame]) >= kCompleteStroke;
        }
        if ((rig.presetFire && travel < kCompleteStroke && reloadStrokes) || rig.fireFromStroke) {
            return useReloadBoltStroke(clip);
        }
        // An explicit native choice preserves a source that leaves its manual
        // charging part still on fire (ARR's rotating charging lever). Keep
        // every sampled pose and the source duration; do not invent recoil
        // by replaying that part's manual cocking stroke on each shot.
        if (rig.customSetup && !rig.presetFire && !rig.fireFromStroke && travel <= .01f && frames >= 3) {
            _rifleBoltOpen = frames / 2; // phase marker; the visible curve is static
            _rifleBoltDuration = clip.durationSeconds;
            _rifleSingleDuration = clip.durationSeconds;
            _rifleBoltSource = single->resourcePath;
            return true;
        }
        if (travel < kCompleteStroke || closed < 0) {
            _simulationError = "Single-fire source has no complete bolt stroke.";
            return false;
        }
        _rifleBoltDuration = clip.durationSeconds * closed / (frames - 1);
        _rifleSingleDuration = clip.durationSeconds;
        _rifleBoltCycle.resize(closed + 1);
        _rifleBoltSource = single->resourcePath;
        return true;
    }

    // Seconds between automatic shots, including authored variable spacing
    // and the loop boundary. A receiver the records make semi-automatic fires
    // once per press at the single clip's length; it does not need the
    // graph's automatic clip, whose cadence may vary (rework 14, item 16).
    bool ReloadMakerController::measureRifleFireInterval(const HkxClip& single, const AnimationAction* automatic, QString& error)
    {
        const auto singleFire = [&] {
            _rifleFireInterval = single.durationSeconds;
            return true;
        };
        if (_simulationAmmo.repeatableSingleFire && !_simulationAmmo.automatic) {
            return singleFire();
        }
        const auto fail = [&](const QString& message) {
            if (!_simulationAmmo.automatic) {
                return singleFire();
            }
            _simulationError = message;
            return false;
        };
        if (!automatic) {
            return fail("Automatic-fire source is missing.");
        }
        const auto automaticClip = HkxReader::readClip(readAnimationResource(automatic->resourcePath, error), error);
        if (!automaticClip.valid()) {
            return fail("Cannot read rifle fire source: " + error);
        }
        QVector<float> shots;
        for (const auto& event : automaticClip.annotations) {
            if (event.text.compare("weaponfire", Qt::CaseInsensitive) == 0) {
                shots.append(event.timeSeconds);
            }
        }
        std::sort(shots.begin(), shots.end());
        if (shots.size() < 2 || shots.size() > 1024) {
            return fail("Automatic-fire source has no repeated weaponfire cadence.");
        }
        const float interval = shots[1] - shots[0];
        if (interval < .01f || interval > 2) {
            return fail("Invalid automatic-fire cadence.");
        }
        QVector<float> schedule;
        bool variable = false;
        for (int i = 1; i < shots.size(); ++i) {
            const float gap = shots[i] - shots[i - 1];
            if (gap < .01f || gap > 2 || !std::isfinite(gap)) {
                return fail("Invalid automatic-fire event spacing.");
            }
            variable |= std::abs(gap - interval) > .002f;
            schedule.append(gap);
        }
        if (variable && _simulationAmmo.automatic) {
            const float loopGap = automaticClip.durationSeconds + shots.front() - shots.back();
            if (loopGap < .01f || loopGap > 2 || !std::isfinite(loopGap)) {
                return fail("Automatic-fire loop has no usable next-shot boundary.");
            }
            schedule.append(loopGap);
            _simulation.fireCycleSchedule = std::move(schedule);
        }
        _rifleFireInterval = interval;
        return true;
    }

    // Fills _rifleBoltCycle with the rig's bolt over the whole single-fire clip.
    bool ReloadMakerController::sampleRifleFireBolt(const HkxClip& base, const HkxClip& clip, QString& error)
    {
        const int frames = sampleCount(clip.durationSeconds);
        if (frames < 3 || frames > maximumReloadFrames) {
            _simulationError = "Rifle firing motion exceeds the sampling limit.";
            return false;
        }
        WeaponLocalSampler sampler(_skeleton, _nodeBindings, _reloadScenePlan, _weaponBoneIndex);
        if (!sampler.setBaselineClip(base, _readyLocalPose, error)) {
            _simulationError = error;
            return false;
        }
        for (int frame = 0; frame < frames; ++frame) {
            const float seconds = clip.durationSeconds * frame / (frames - 1);
            if (!sampler.sample(clip, seconds, error)) {
                _simulationError = error;
                return false;
            }
            if (!sampler.weaponFrameValid()) {
                _simulationError = "Invalid firing reference frame.";
                return false;
            }
            _rifleBoltCycle.append(sampler.nodePose(_reloadTrackNodes[_simulationRig->bolt]));
        }
        return true;
    }

    // A bolt the fire clip leaves static: the native reload supplies the
    // complete stroke; replay that measured path, closed to open and back, at
    // the firing cadence.
    bool ReloadMakerController::useReloadBoltStroke(const HkxClip& singleFire)
    {
        const auto& rig = *_simulationRig;
        const auto& source = simulationTrackCurve(*_reloadRecipe, rig, rig.bolt);
        _rifleBoltCycle.clear();
        for (int frame = rig.closedFrame; frame >= rig.openFrame; --frame) {
            _rifleBoltCycle.append(source[frame]);
        }
        _rifleBoltOpen = int(_rifleBoltCycle.size()) - 1;
        for (int frame = rig.openFrame + 1; frame <= rig.closedFrame; ++frame) {
            _rifleBoltCycle.append(source[frame]);
        }
        if (_rifleBoltOpen < 1 || reloadPoseDistance(_rifleBoltCycle.front(), _rifleBoltCycle[_rifleBoltOpen]) < kCompleteStroke) {
            _simulationError = "The native reload has no complete bolt stroke.";
            return false;
        }
        _rifleBoltDuration = _rifleFireInterval;
        _rifleSingleDuration = singleFire.durationSeconds;
        _rifleBoltSource = rig.derivedBolt.isEmpty()
            ? _reloadRecipe->source["animation"].toString()
            : rig.mechanicalSource["animation"].toString();
        _rifleBoltFromReloadStroke = true;
        return true;
    }

    // Bolt pose at a manual stroke: 0 is closed (the cycle's last sample), 1 is open.
    Transformf ReloadMakerController::rifleBoltPose(float progress) const
    {
        const int last = int(_rifleBoltCycle.size()) - 1;
        const float index = last + (_rifleBoltOpen - last) * std::clamp(progress, 0.f, 1.f);
        const int first = int(index);
        const int next = std::min(first + 1, last);
        return interpolatedTransform(_rifleBoltCycle[first], _rifleBoltCycle[next], index - first);
    }

    // Bolt pose during a shot. A shot that empties a locking magazine holds
    // the bolt at its open sample.
    Transformf ReloadMakerController::rifleFireBoltPose() const
    {
        const float duration = _simulationAmmo.automatic ? _simulation.currentShotInterval() : _rifleBoltDuration;
        const float elapsed = isM1AConfiguration() ? _simulation.seconds : _simulation.shotSeconds;
        const int last = int(_rifleBoltCycle.size()) - 1;
        float index = std::clamp(elapsed / duration, 0.f, 1.f) * last;
        if(_simulation.openBolt)index=std::fmod(index+float(_rifleBoltOpen),float(last));
        if (_simulation.willLockAfterShot() && index >= _rifleBoltOpen) {
            index = float(_rifleBoltOpen);
        }
        const int first = int(index);
        const int next = std::min(first + 1, last);
        return interpolatedTransform(_rifleBoltCycle[first], _rifleBoltCycle[next], index - first);
    }

    // Manual bolt action: the bolt and its followers through one lift, pull,
    // push and lock cycle, plus the rest pose after a shot. The cycle comes
    // from the native cycle clip, or from the authored curves when the rig
    // has a manual cycle. Errors go to _simulationError.
    bool ReloadMakerController::prepareBoltActionMotion()
    {
        _boltCycle = {};
        _boltFiredPose.clear();
        _boltCycleSource = {};
        const auto cycle = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& action) {
            return action.family == ActionFamily::Cycle;
        });
        const auto fire = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& action) {
            return action.family == ActionFamily::Fire && !action.automatic;
        });
        const auto& rig = *_simulationRig;
        const bool authoredCycle = rig.manualCycle;
        if ((!authoredCycle && cycle == _animationActions.cend()) || fire == _animationActions.cend()) {
            _simulationError = "A bolt-cycle source and native firing clip are required.";
            return false;
        }
        QString error;
        const auto bytes = authoredCycle ? QByteArray() : readAnimationResource(cycle->resourcePath, error);
        const auto baseBytes = readAnimationResource(authoredCycle ? fire->baselineResourcePath : cycle->baselineResourcePath, error);
        const auto clip = authoredCycle ? HkxClip{} : HkxReader::readClip(bytes, error);
        const auto base = HkxReader::readClip(baseBytes, error);
        const auto fireBytes = readAnimationResource(fire->resourcePath, error);
        const auto firing = HkxReader::readClip(fireBytes, error);
        if ((!authoredCycle && !clip.valid()) || !base.valid() || !firing.valid()) {
            _simulationError = "Cannot read bolt-cycle sources: " + error;
            return false;
        }
        if (!resolveBoltCycleTracks()) {
            return false;
        }
        _boltCycle.duration = authoredCycle ? rig.motionDuration : clip.durationSeconds;
        const int frames = authoredCycle ? simulationFrameCount(*_reloadRecipe, rig) : sampleCount(clip.durationSeconds);
        if (frames < 3 || frames > maximumReloadFrames) {
            _simulationError = "Native bolt cycle exceeds the sampling limit.";
            return false;
        }
        WeaponLocalSampler sampler(_skeleton, _nodeBindings, _reloadScenePlan, _weaponBoneIndex);
        if (!sampler.setBaselineClip(base, _readyLocalPose, error)) {
            _simulationError = error;
            return false;
        }
        // Weapon-local poses of every cycle track at one moment of a clip.
        const auto sample = [&](const HkxClip& source, float seconds, QVector<Transformf>& result) {
            if (!sampler.sample(source, seconds, error)) {
                return false;
            }
            if (!sampler.weaponFrameValid()) {
                error = "Invalid bolt reference frame.";
                return false;
            }
            result.clear();
            for (const int track : _boltCycle.tracks) {
                result.append(sampler.nodePose(_reloadTrackNodes[track]));
            }
            return true;
        };
        if (authoredCycle) {
            for (int i = 0; i < _boltCycle.tracks.size(); ++i) {
                _boltCycle.poses[i] = simulationTrackCurve(*_reloadRecipe, rig, _boltCycle.tracks[i]);
            }
        } else {
            QVector<Transformf> poses;
            for (int frame = 0; frame < frames; ++frame) {
                if (!sample(clip, clip.durationSeconds * frame / (frames - 1), poses)) {
                    _simulationError = error;
                    return false;
                }
                for (int i = 0; i < poses.size(); ++i) {
                    _boltCycle.poses[i].append(poses[i]);
                }
            }
        }
        float eject = authoredCycle ? rig.manualEjectSeconds : -1;
        if (!authoredCycle) {
            for (const auto& event : clip.annotations) {
                if (event.text.compare("EjectShellCasing", Qt::CaseInsensitive) == 0) {
                    eject = event.timeSeconds;
                    break;
                }
            }
        }
        // A manual setup without an authored ejection ejects at full rearward travel.
        const bool ejectAtRear = authoredCycle && rig.authoring["mode"] == "manual" && eject < 0;
        if (!analyzeBoltCycle(_boltCycle, eject, error, ejectAtRear) || !sample(firing, firing.durationSeconds, _boltFiredPose)) {
            _simulationError = error;
            return false;
        }
        if (authoredCycle) {
            _boltCycleSource = rig.mechanicalSource;
            _boltCycleSource["fireAnimation"] = fire->resourcePath;
        } else {
            _boltCycleSource = {
                { "animation", cycle->resourcePath },
                { "baseline", cycle->baselineResourcePath },
                { "fireAnimation", fire->resourcePath },
                { "ejectSeconds", eject },
            };
        }
        _simulationRig->slideTracks = _boltCycle.tracks;
        return true;
    }

    // The bolt cycle's tracks, input track first. A custom setup names them;
    // the native bolt actions use the bolt and its two named followers.
    bool ReloadMakerController::resolveBoltCycleTracks()
    {
        const auto& rig = *_simulationRig;
        if (rig.customSetup) {
            _boltCycle.tracks = rig.slideTracks;
            _boltCycle.tracks.removeAll(rig.slide);
            _boltCycle.tracks.prepend(rig.slide);
            _boltCycle.poses.resize(_boltCycle.tracks.size());
            return true;
        }
        const auto& tracks = _reloadRecipe->tracks;
        for (const char* name : { "WeaponBolt", "WeaponExtra1", "WeaponOptics2" }) {
            int track = -1;
            for (int i = 0; i < tracks.size(); ++i) {
                if (tracks[i].surface && tracks[i].anchor.anchorName == QLatin1String(name)) {
                    if (track >= 0) {
                        _simulationError = "Ambiguous bolt follower: " + QString(name);
                        return false;
                    }
                    track = i;
                }
            }
            if (track < 0) {
                _simulationError = "Missing bolt follower: " + QString(name);
                return false;
            }
            _boltCycle.tracks.append(track);
            _boltCycle.poses.append(QVector<Transformf>{});
        }
        return true;
    }

    // Bolt-cycle targets outside native fire and reload clips. A closed bolt
    // rests on its fired pose while a spent case is in the chamber.
    void ReloadMakerController::applyBoltActionTargets(QHash<int, Transformf>& targets) const
    {
        if (_simulation.action == SimulationAction::Fire || _simulation.action == SimulationAction::Reload) {
            return;
        }
        for (int i = 0; i < _boltCycle.tracks.size(); ++i) {
            const int track = _boltCycle.tracks[i];
            Transformf pose;
            if (_simulation.slide <= .001f) {
                pose = _simulation.spentCase ? _boltFiredPose[i] : _boltCycle.poses[i].back();
            } else {
                pose = boltCyclePose(_boltCycle, track, _simulation.slide, _simulation.boltClosing);
            }
            targets.insert(_reloadTrackNodes[track], pose);
        }
    }
}
