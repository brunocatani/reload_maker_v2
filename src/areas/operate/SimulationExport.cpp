// The Operate simulation as JSON: the state MCP reads, and the exports for
// the runtime (slide profile and full simulation). An export that cannot
// encode a pose returns an empty object.
#include "ReloadMakerController.h"
#include "areas/operate/SimulationNames.h"
#include "model/nifmodel.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace reloadmaker
{
    namespace
    {
        bool encodePoses(const QVector<Transformf>& poses, QJsonArray& result)
        {
            QString error;
            for (const auto& pose : poses) {
                QJsonObject value;
                if (!encodePoseTransform(pose, value, error)) {
                    return false;
                }
                result.append(value);
            }
            return true;
        }

        QJsonArray toJsonArray(const QVector<float>& values)
        {
            QJsonArray result;
            for (const float value : values) {
                result.append(value);
            }
            return result;
        }
    }

    QJsonObject ReloadMakerController::simulationState() const
    {
        QJsonObject result{
            { "active", _simulationActive },
            { "action", simulationActionName(_simulation.action, isRifleConfiguration(), isBoltActionConfiguration()) },
            { "magazinePresent", _simulation.magazinePresent() },
            { "slide", _simulation.slide },
            { "seconds", _simulation.seconds },
            { "error", _simulationError },
            { "nativeGhost", _nativeGhost },
        };
        if (isBoltActionConfiguration()) {
            result["boltAction"] = QJsonObject{
                { "progress", _simulation.slide },
                { "closingPath", _simulation.boltClosing },
                { "spentCase", _simulation.spentCase },
                { "casesEjected", _simulation.casesEjected },
                { "ejectProgress", _simulation.boltEjectProgress },
                { "source", _boltCycleSource },
                { "manualReturn", true },
            };
        }
        if (isAr15Configuration()) {
            result["ar15"] = QJsonObject{
                { "bolt", _simulation.slide },
                { "handle", _simulation.handle },
                { "boltLocked", _simulation.slideLocked },
                { "triggerHeld", _simulation.triggerHeld },
                { "shotSeconds", _simulation.shotSeconds },
                { "fireInterval", _rifleFireInterval },
                { "boltSource", _rifleBoltSource },
                { "boltSamples", _rifleBoltCycle.size() },
                { "magazineRecord", simulationMagazineAnchor().ownerRecord },
            };
        }
        if (isM1AConfiguration()) {
            result["m1a"] = QJsonObject{
                { "bolt", _simulation.slide },
                { "operatingHandle", _simulation.slide },
                { "boltLocked", _simulation.slideLocked },
                { "boltSource", _rifleBoltSource },
                { "boltSamples", _rifleBoltCycle.size() },
                { "magazineRecord", simulationMagazineAnchor().ownerRecord },
            };
        }
        result["magazineGrip"] = QJsonObject{
            { "id", _magazineGrabGrip ? _magazineGrabGrip->pose.id : QString() },
            { "name", _magazineGrabGrip ? _magazineGrabGrip->pose.name : QString() },
            { "applied", _magazineGrabGrip && _simulation.magazine.carried() && _magazineGrabError.isEmpty() },
            { "error", _magazineGrabError },
        };
        const auto& offset = _simulation.magazine.offset;
        result["magazine"] = QJsonObject{
            { "phase", _simulation.magazineDescription() },
            { "empty", _simulation.magazineRounds == 0 },
            { "offset", QJsonArray{ offset.x(), offset.y(), offset.z() } },
            { "distance", _simulation.magazine.distance },
            { "error", _magazineError },
        };
        if (_magazineRail) {
            result["magazineRail"] = magazineRailJson(*_magazineRail);
        }
        auto ammunition = _simulationAmmo.json();
        ammunition["magazineRounds"] = _simulation.magazineRounds;
        ammunition["chambered"] = _simulation.chambered;
        ammunition["slideLocked"] = _simulation.slideLocked;
        ammunition["shotsFired"] = _simulation.shotsFired;
        ammunition["roundsEjected"] = _simulation.roundsEjected;
        ammunition["canFire"] = _simulation.canFire();
        result["ammunition"] = ammunition;
        if (simulationMechanism() == WeaponMechanism::PistolSlide) {
            result["reloadProfile"] = glockReloadProfile();
        }
        if (_simulationRig && _reloadRecipe) {
            result["setup"] = simulationRigJson(*_reloadRecipe, *_simulationRig, _slideSpringReturn);
            const auto stroke = operatingControllerStroke();
            result["controllerInput"] = QJsonObject{
                { "path", "linear" },
                { "strokeInWeapon", QJsonArray{ stroke.x, stroke.y, stroke.z } },
                { "visualMotion", "complete-authored-curves" },
            };
        }
        result["interaction"] = interactionGripState();
        result["parts"] = simulationPartsJson();
        result["magazineRoots"] = magazineRootsJson();
        result["magazineShapes"] = magazineShapesJson();
        return result;
    }

    // A pistol reload's rack and feed moments and the reload variant in use.
    // The export keeps the key the Glock testbed first gave it.
    QJsonObject ReloadMakerController::glockReloadProfile() const
    {
        return {
            { "animation", _reloadRecipe ? _reloadRecipe->source["animation"].toString() : QString() },
            { "rackSeconds", _glockReloadRackAt },
            { "feedSeconds", _glockReloadFeedAt },
            { "noExtender", _glockNoExtender },
            { "noExtenderAvailable", !noExtenderReloadPath().isEmpty() },
            { "startedLoaded", _simulation.reloadStartedLoaded },
            { "startedOpen", _simulation.reloadStartedOpen },
            { "cycled", _simulation.reloadCycled },
            { "fed", _simulation.reloadFed },
            { "fireAction", _simulationAmmo.automatic ? "Fire — automatic" : "Fire — single" },
        };
    }

    // World poses of the recipe tracks while the simulation runs.
    QJsonArray ReloadMakerController::simulationPartsJson() const
    {
        QJsonArray parts;
        if (!_simulationActive || !_reloadRecipe || !_simulationRig) {
            return parts;
        }
        for (int i = 0; i < _reloadTrackNodes.size(); ++i) {
            Transformf world;
            QString error;
            if (!sceneNodeWorld(_sceneNodes[_reloadTrackNodes[i]], world, error)) {
                continue;
            }
            QJsonObject value;
            if (!encodePoseTransform(world, value, error)) {
                continue;
            }
            parts.append(QJsonObject{
                { "track", i },
                { "name", _reloadRecipe->tracks[i].anchor.anchorName },
                { "world", value },
                { "magazineOwner", _simulationRig->magazineTracks.contains(i) },
            });
        }
        return parts;
    }

    // The magazine roots the simulation hides, with their current and original flags.
    QJsonArray ReloadMakerController::magazineRootsJson() const
    {
        QJsonArray roots;
        auto* model = activeModel();
        if (!model) {
            return roots;
        }
        for (const auto& [index, original] : _simulationVisibility) {
            if (index.isValid()) {
                roots.append(QJsonObject{
                    { "block", model->getBlockNumber(index) },
                    { "flags", model->get<int>(index, "Flags") },
                    { "originalFlags", original },
                });
            }
        }
        return roots;
    }

    QJsonArray ReloadMakerController::magazineShapesJson() const
    {
        QJsonArray shapes;
        auto* model = activeModel();
        if (!model) {
            return shapes;
        }
        for (const auto& visual : _magazineVisuals) {
            if (visual.node.isValid()) {
                shapes.append(QJsonObject{
                    { "block", model->getBlockNumber(visual.node) },
                    { "name", model->get<QString>(visual.node, "Name") },
                    { "cartridge", visual.cartridge },
                    { "emptyVersion", visual.empty },
                    { "flags", model->get<int>(visual.node, "Flags") },
                    { "originalFlags", visual.flags },
                });
            }
        }
        return shapes;
    }

    // Pistol slide profile for the runtime: each slide track's weapon-local
    // stroke from the open to the closed frame, with its HKX bone path.
    QJsonObject ReloadMakerController::exportSlideProfile() const
    {
        if (simulationMechanism() != WeaponMechanism::PistolSlide || !_simulationRig || !_reloadRecipe || _weaponBoneIndex < 0) {
            return {};
        }
        const auto& rig = *_simulationRig;
        const auto& recipe = *_reloadRecipe;
        QString error;
        const auto encoded = encodeReloadRecipe(recipe, error);
        if (!error.isEmpty()) {
            return {};
        }
        const auto encodedTracks = QJsonDocument::fromJson(encoded).object()["tracks"].toArray();
        QJsonArray tracks;
        for (const int index : rig.slideTracks) {
            // A model-relative NIF path is not a path in the game's assembled rig.
            // Keep the model identity and export the HKX bone ancestry separately.
            QString path;
            try {
                path = runtimeTrackPath(index);
            } catch (const std::exception&) {
                return {};
            }
            const auto& poses = simulationTrackCurve(recipe, rig, index);
            QJsonArray stroke;
            for (int frame = rig.openFrame; frame <= rig.closedFrame; ++frame) {
                const auto& pose = poses[frame];
                stroke.append(QJsonArray{
                    pose.translation.x, pose.translation.y, pose.translation.z,
                    pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w,
                    pose.scale.x, pose.scale.y, pose.scale.z,
                });
            }
            tracks.append(QJsonObject{
                { "anchor", encodedTracks[index].toObject().value("anchor") },
                { "runtimePath", path },
                { "poses", stroke },
            });
        }
        const int strokeFrames = rig.closedFrame - rig.openFrame;
        const float sourceDuration = rig.motionDuration > 0 ? rig.motionDuration : recipe.duration;
        QJsonObject result{
            { "format", "reload-maker-slide-profile" },
            { "version", 1 },
            { "recipeId", recipe.id },
            { "source", recipe.source },
            { "framePath", _skeleton.bones[_weaponBoneIndex].name },
            { "space", "weapon-local" },
            { "sourceFirstFrame", rig.openFrame },
            { "sourceLastFrame", rig.closedFrame },
            { "sourceFrames", recipe.frames },
            { "frames", strokeFrames + 1 },
            { "duration", sourceDuration * strokeFrames / (recipe.frames - 1) },
            { "slideTrack", rig.slideTracks.indexOf(rig.slide) },
            { "openFrame", 0 },
            { "closedFrame", strokeFrames },
            { "springReturn", _slideSpringReturn },
            { "tracks", tracks },
        };
        const auto fingers = exportInteractionGrips()["operatingFingers"].toArray();
        if (!fingers.isEmpty()) {
            result["operatingFingers"] = fingers;
        }
        if (rig.customSetup) {
            result["motionSource"] = rig.mechanicalSource;
        }
        return result;
    }

    // The complete simulation for the runtime: rig, magazine interaction,
    // ammunition, the mechanism's motion, authored grips, sounds and the
    // source recipe.
    QJsonObject ReloadMakerController::exportSimulation() const
    {
        if (!_simulationRig || !_reloadRecipe) {
            return {};
        }
        QString error;
        const auto recipe = encodeReloadRecipe(*_reloadRecipe, error);
        if (!error.isEmpty()) {
            return {};
        }
        auto result = simulationRigJson(*_reloadRecipe, *_simulationRig, _slideSpringReturn);
        if (_magazineRail) {
            result["magazineInteraction"] = magazineRailJson(*_magazineRail);
        }
        result["ammunitionConfiguration"] = _simulationAmmo.json();
        if (isRifleConfiguration() && !addRifleExport(result)) {
            return {};
        }
        if (simulationMechanism() == WeaponMechanism::PistolSlide) {
            result["glockReloadProfile"] = glockReloadProfile();
        }
        if (isBoltActionConfiguration()) {
            const auto motion = exportBoltActionMotion();
            if (motion.isEmpty()) {
                return {};
            }
            result["format"] = "reload-maker-bolt-action-simulation";
            result["mechanismProfile"] = "manual-bolt-action";
            result["springReturn"] = false;
            result["boltMotion"] = motion;
        }
        const auto grips = exportInteractionGrips();
        if (grips.isEmpty()) {
            return {};
        }
        result["authoredInteractions"] = grips;
        const auto sounds = exportSounds();
        if (sounds.isEmpty()) {
            return {};
        }
        result["sounds"] = sounds;
        result["sourceRecipe"] = QJsonDocument::fromJson(recipe).object();
        return result;
    }

    // Rifle fields of the simulation export: the mechanism profile, the
    // handle and bolt tracks, the firing bolt motion and its cadence.
    bool ReloadMakerController::addRifleExport(QJsonObject& result) const
    {
        const bool reciprocating = simulationMechanism() == WeaponMechanism::ReciprocatingBolt;
        const bool m1a = isM1AConfiguration();
        if (reciprocating) {
            result["format"] = "reload-maker-reciprocating-bolt-simulation";
        } else if (m1a) {
            result["format"] = "reload-maker-m1a-simulation";
        } else {
            result["format"] = "reload-maker-ar15-simulation";
        }
        QJsonArray poses;
        if (!encodePoses(_rifleBoltCycle, poses)) {
            return false;
        }
        if (reciprocating) {
            result["mechanismProfile"] = "reciprocating-bolt";
        } else if (m1a) {
            result["mechanismProfile"] = "m1a";
        } else {
            result["mechanismProfile"] = "ar15";
        }
        result["chargingHandleTrack"] = _simulationRig->slide;
        result["boltTrack"] = _simulationRig->bolt;
        result["magazineRecord"] = simulationMagazineAnchor().ownerRecord;
        result["magazineModel"] = simulationMagazineAnchor().ownerModel;
        result["boltMotion"] = QJsonObject{
            { "animation", _rifleBoltSource },
            { "duration", _rifleBoltDuration },
            { "openSample", _rifleBoltOpen },
            { "poses", poses },
        };
        result["automaticFireInterval"] = _rifleFireInterval;
        if (manualBoltFromReload()) {
            result["manualBoltFromReload"] = true;
        }
        if (m1a) {
            result["reciprocatingHandle"] = true;
            result["handleFireMotion"] = "native-reload-stroke-by-firing-bolt-distance";
            result["emptyMagazineFrame"] = _m1aEmptyMagazineFrame;
            result["loadedMagazineFrame"] = _m1aLoadedMagazineFrame;
        }
        return true;
    }

    // Manual bolt-action motion for the runtime: the sampled cycle of every
    // bolt track, its rest pose after a shot, and the cycle's analysis.
    QJsonObject ReloadMakerController::exportBoltActionMotion() const
    {
        if (_boltCycle.poses.isEmpty() || _boltFiredPose.size() != _boltCycle.tracks.size() ||
            _boltCycle.opening.isEmpty() || _boltCycle.closing.isEmpty()) {
            return {};
        }
        QString error;
        QJsonArray tracks;
        for (int i = 0; i < _boltCycle.tracks.size(); ++i) {
            QJsonArray poses;
            if (!encodePoses(_boltCycle.poses[i], poses)) {
                return {};
            }
            QJsonObject fired;
            if (!encodePoseTransform(_boltFiredPose[i], fired, error)) {
                return {};
            }
            tracks.append(QJsonObject{
                { "track", _boltCycle.tracks[i] },
                { "poses", poses },
                { "firedRest", fired },
            });
        }
        const auto& axis = _boltCycle.inputAxis;
        return {
            { "source", _boltCycleSource },
            { "duration", _boltCycle.duration },
            { "firstFrame", _boltCycle.first },
            { "rearFrame", _boltCycle.rear },
            { "lastFrame", _boltCycle.last },
            { "openingProgress", toJsonArray(_boltCycle.opening) },
            { "closingProgress", toJsonArray(_boltCycle.closing) },
            { "ejectProgress", _boltCycle.ejectProgress },
            { "inputAxis", QJsonArray{ axis.x, axis.y, axis.z } },
            { "inputTravel", _boltCycle.inputTravel },
            { "tracks", tracks },
            { "release", "hold" },
        };
    }
}
