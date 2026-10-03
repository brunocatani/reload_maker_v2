// Operating-part motion for the simulation: the rig built from the captured
// reload, and the firing and bolt-cycle motion sampled from native clips.
// Sampled poses are weapon-local: each frame is divided by the weapon bone.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        // Native clips are sampled at this rate when no recipe frame count applies.
        constexpr float kSamplesPerSecond = 120.0f;
        // reloadPoseDistance (game units plus 3 x radians) below which a bolt
        // path is static or partial rather than a complete stroke.
        constexpr float kCompleteBoltStroke = 0.15f;

        int sampleCount(float seconds)
        {
            return int(std::ceil(seconds * kSamplesPerSecond)) + 1;
        }

        QString sha256Hex(const QByteArray& data)
        {
            return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        }
    }

    // A saved operating setup decides the rig; without one the source reload does.
    bool ReloadMakerController::buildCurrentSimulationRig(SimulationRig& rig, QString& error)
    {
        if (!_assignedMechanism && currentTestbed().isEmpty()) {
            error = "Choose a behavior template in Author reload before configuring this weapon.";
            return false;
        }
        const auto saved = _authoringRoadmaps.value(roadmapKey()).toObject().value("operating").toObject();
        if (saved.isEmpty()) {
            return buildSourceSimulationRig(rig, error);
        }
        OperatingSetup setup;
        if (!readOperatingSetup(saved, setup, error)) {
            return false;
        }
        return buildConfiguredSimulationRig(setup, rig, error);
    }

    bool ReloadMakerController::buildSourceSimulationRig(SimulationRig& rig, QString& error)
    {
        if (isSrepConfiguration()) {
            return buildSrepSimulationRig(rig, error);
        }
        if (isMk18Configuration()) {
            return buildMk18SimulationRig(rig, error);
        }
        return buildSimulationRig(*_reloadRecipe, rig, error, simulationMechanism());
    }

    // SREP's native bolt cycles while its separate charging handle is static.
    // Derive only the manual handle translation from that measured bolt stroke.
    bool ReloadMakerController::buildSrepSimulationRig(SimulationRig& rig, QString& error)
    {
        for (const auto id : configuration()->activeMods()) {
            const auto* mod = configuration()->mod(id);
            if (mod && (mod->editorId == QLatin1String("mod_SREP_M203Dummy") || mod->editorId == QLatin1String("mod_SREP_M26MASSDummy"))) {
                error = "The active underbarrel weapon uses its own reload. Select rifle mode for physical rifle reloads.";
                return false;
            }
        }
        if (!buildSimulationRig(*_reloadRecipe, rig, error, simulationMechanism(), isAr15Configuration())) {
            return false;
        }
        const auto& tracks = _reloadRecipe->tracks;
        for (int i = 0; i < tracks.size(); ++i) {
            if (tracks[i].surface && tracks[i].anchor.anchorName == QLatin1String("WeaponExtra2")) {
                rig.slideTracks.append(i);
            }
        }
        return true;
    }

    // The MK18 handle is derived from the reload's bolt stroke. Tactical and
    // double-magazine clips may never cycle the bolt; their magazine and hands
    // stay native and the manual action uses the standard MK18 empty reload.
    bool ReloadMakerController::buildMk18SimulationRig(SimulationRig& rig, QString& error)
    {
        const auto& recipe = *_reloadRecipe;
        int bolt = -1;
        for (int i = 0; i < recipe.tracks.size(); ++i) {
            if (recipe.tracks[i].anchor.anchorName == QLatin1String("WeaponBolt")) {
                bolt = i;
            }
        }
        if (bolt < 0) {
            error = "MK18 has no native bolt binding.";
            return false;
        }
        const auto& boltPoses = recipe.tracks[bolt].poses;
        float stroke = 0;
        for (const auto& pose : boltPoses) {
            stroke = std::max(stroke, reloadPoseDistance(pose, boltPoses.back()));
        }
        if (stroke >= kCompleteBoltStroke) {
            return buildSimulationRig(recipe, rig, error, simulationMechanism(), true);
        }
        const QString path = "meshes/actors/character/_1stperson/animations/mk18anims/wpnreload.hkx";
        const auto bytes = readAnimationResource(path, error);
        const auto clip = HkxReader::readClip(bytes, error);
        const auto base = HkxReader::readClip(readAnimationResource(recipe.source["baseline"].toString(), error), error);
        if (!clip.valid() || !base.valid()) {
            return false;
        }
        QVector<Transformf> sampled, baseline, local, global, boundLocal, boundGlobal, sceneLocal, sceneWorld, poses;
        if (!samplePose(_skeleton, base, 0, &_readyLocalPose, sampled, baseline, global, error)) {
            return false;
        }
        for (int frame = 0; frame < recipe.frames; ++frame) {
            const float seconds = clip.durationSeconds * frame / (recipe.frames - 1);
            if (!samplePose(_skeleton, clip, seconds, &baseline, sampled, local, global, error) ||
                !evaluateSceneBindings(_skeleton, _nodeBindings, local, baseline, global, boundLocal, boundGlobal, error) ||
                !nativeMechanismScene(_reloadScenePlan, _nodeBindings, boundLocal, sceneLocal, sceneWorld, error)) {
                return false;
            }
            Transformf inverse;
            if (!invertUniformTransform(global[_weaponBoneIndex], inverse)) {
                error = "Invalid native bolt source frame.";
                return false;
            }
            poses.append(composed(inverse, sceneWorld[_reloadTrackNodes[bolt]]));
        }
        if (!buildSimulationRig(recipe, rig, error, simulationMechanism(), true, poses)) {
            return false;
        }
        rig.mechanicalSource = {
            { "animation", path },
            { "animationSha256", sha256Hex(bytes) },
            { "duration", clip.durationSeconds },
        };
        return true;
    }

    // Rifle firing: the cadence from the native fire clips, and the bolt cycle
    // the firing pose replays. Errors go to _simulationError.
    bool ReloadMakerController::prepareRifleMotion()
    {
        _rifleBoltCycle.clear();
        _rifleBoltOpen = 0;
        _rifleFireInterval = 0;
        _rifleBoltSource.clear();
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
        const bool staticMk18Bolt = _simulationRig->presetFire && isMk18Configuration() && travel < kCompleteBoltStroke;
        if (staticMk18Bolt || _simulationRig->fireFromStroke) {
            return useReloadBoltStroke(clip);
        }
        if (travel < kCompleteBoltStroke || closed < 0) {
            _simulationError = "Single-fire source has no complete bolt stroke.";
            return false;
        }
        _rifleBoltDuration = clip.durationSeconds * closed / (frames - 1);
        _rifleSingleDuration = clip.durationSeconds;
        _rifleBoltCycle.resize(closed + 1);
        _rifleBoltSource = single->resourcePath;
        return true;
    }

    // Seconds between automatic shots, or the single-fire clip length when the
    // receiver fires once per press.
    bool ReloadMakerController::measureRifleFireInterval(const HkxClip& single, const AnimationAction* automatic, QString& error)
    {
        if (isThanatosConfiguration() || !automatic) {
            // The installed Thanatos receivers are semi-automatic. Its inherited
            // automatic graph clip has a variable cadence and is not their source.
            if (_simulationAmmo.automatic) {
                _simulationError = "This Thanatos profile requires a semi-automatic receiver.";
                return false;
            }
            _rifleFireInterval = single.durationSeconds;
            return true;
        }
        const auto automaticClip = HkxReader::readClip(readAnimationResource(automatic->resourcePath, error), error);
        if (!automaticClip.valid()) {
            _simulationError = "Cannot read rifle fire source: " + error;
            return false;
        }
        QVector<float> shots;
        for (const auto& event : automaticClip.annotations) {
            if (event.text.compare("weaponfire", Qt::CaseInsensitive) == 0) {
                shots.append(event.timeSeconds);
            }
        }
        std::sort(shots.begin(), shots.end());
        if (shots.size() < 2) {
            _simulationError = "Automatic-fire source has no repeated weaponfire cadence.";
            return false;
        }
        _rifleFireInterval = shots[1] - shots[0];
        if (_rifleFireInterval < .01f || _rifleFireInterval > 2) {
            _simulationError = "Invalid automatic-fire cadence.";
            return false;
        }
        for (int i = 2; i < shots.size(); ++i) {
            if (std::abs(shots[i] - shots[i - 1] - _rifleFireInterval) > .002f) {
                _simulationError = "Variable fire cadence needs an explicit event schedule.";
                return false;
            }
        }
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
        QVector<Transformf> sampled, baseline, local, global, boundLocal, boundGlobal, sceneLocal, sceneWorld;
        if (!samplePose(_skeleton, base, 0, &_readyLocalPose, sampled, baseline, global, error)) {
            _simulationError = error;
            return false;
        }
        for (int frame = 0; frame < frames; ++frame) {
            const float seconds = clip.durationSeconds * frame / (frames - 1);
            if (!samplePose(_skeleton, clip, seconds, &baseline, sampled, local, global, error) ||
                !evaluateSceneBindings(_skeleton, _nodeBindings, local, baseline, global, boundLocal, boundGlobal, error) ||
                !nativeMechanismScene(_reloadScenePlan, _nodeBindings, boundLocal, sceneLocal, sceneWorld, error)) {
                _simulationError = error;
                return false;
            }
            Transformf inverse;
            if (!invertUniformTransform(global[_weaponBoneIndex], inverse)) {
                _simulationError = "Invalid firing reference frame.";
                return false;
            }
            _rifleBoltCycle.append(composed(inverse, sceneWorld[_reloadTrackNodes[_simulationRig->bolt]]));
        }
        return true;
    }

    // The MK18 fires with a static bolt track. Its native reload supplies the
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
        if (_rifleBoltOpen < 1 || reloadPoseDistance(_rifleBoltCycle.front(), _rifleBoltCycle[_rifleBoltOpen]) < kCompleteBoltStroke) {
            _simulationError = "MK18 native reload has no complete bolt stroke.";
            return false;
        }
        _rifleBoltDuration = _rifleFireInterval;
        _rifleSingleDuration = singleFire.durationSeconds;
        _rifleBoltSource = rig.derivedBolt.isEmpty()
            ? _reloadRecipe->source["animation"].toString()
            : rig.mechanicalSource["animation"].toString();
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
        const float duration = _simulationAmmo.automatic ? _rifleFireInterval : _rifleBoltDuration;
        const float elapsed = isM1AConfiguration() ? _simulation.seconds : _simulation.shotSeconds;
        const int last = int(_rifleBoltCycle.size()) - 1;
        float index = std::clamp(elapsed / duration, 0.f, 1.f) * last;
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
        const int frames = authoredCycle ? _reloadRecipe->frames : sampleCount(clip.durationSeconds);
        if (frames < 3 || frames > maximumReloadFrames) {
            _simulationError = "Native bolt cycle exceeds the sampling limit.";
            return false;
        }
        QVector<Transformf> sampled, baseline, local, global, boundLocal, boundGlobal, sceneLocal, sceneWorld;
        if (!samplePose(_skeleton, base, 0, &_readyLocalPose, sampled, baseline, global, error)) {
            _simulationError = error;
            return false;
        }
        // Weapon-local poses of every cycle track at one moment of a clip.
        const auto sample = [&](const HkxClip& source, float seconds, QVector<Transformf>& result) {
            if (!samplePose(_skeleton, source, seconds, &baseline, sampled, local, global, error) ||
                !evaluateSceneBindings(_skeleton, _nodeBindings, local, baseline, global, boundLocal, boundGlobal, error) ||
                !nativeMechanismScene(_reloadScenePlan, _nodeBindings, boundLocal, sceneLocal, sceneWorld, error)) {
                return false;
            }
            Transformf inverse;
            if (!invertUniformTransform(global[_weaponBoneIndex], inverse)) {
                error = "Invalid bolt reference frame.";
                return false;
            }
            result.clear();
            for (const int track : _boltCycle.tracks) {
                result.append(composed(inverse, sceneWorld[_reloadTrackNodes[track]]));
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
            _boltCycleSource["fireSha256"] = sha256Hex(fireBytes);
        } else {
            _boltCycleSource = {
                { "animation", cycle->resourcePath },
                { "animationSha256", sha256Hex(bytes) },
                { "baseline", cycle->baselineResourcePath },
                { "baselineSha256", sha256Hex(baseBytes) },
                { "fireAnimation", fire->resourcePath },
                { "fireSha256", sha256Hex(fireBytes) },
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
