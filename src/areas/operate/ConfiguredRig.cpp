// A saved operating setup and the simulation rig built from it. The setup
// names the operating roles and the movement window; its mode decides where
// the moving parts' curves come from:
//   preset  the verified source behavior (buildSourceSimulationRig),
//   reload  the captured reload itself,
//   clip    another native clip, sampled over the recipe's frames,
//   manual  saved Move / Rotate movements authored on the parts.
#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "areas/operate/WeaponLocalSampler.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker
{
    bool ReloadMakerController::buildConfiguredSimulationRig(const OperatingSetup& setup, SimulationRig& rig, QString& error)
    {
        if (!_reloadRecipe) {
            error = "Capture this configuration's reload first.";
            return false;
        }
        const auto& recipe = *_reloadRecipe;
        auto parts = setup.parts;
        const auto valid = [&](int track) { return track >= 0 && track < recipe.tracks.size(); };
        if (!valid(parts.operating) || !valid(parts.bolt) || !valid(parts.magazine)) {
            error = "An operating role is outside the current source tracks";
            return false;
        }
        if (parts.openFrame >= recipe.frames || parts.closedFrame >= recipe.frames) {
            error = "The selected movement window exceeds this capture";
            return false;
        }
        ConfiguredMotion motion{ recipe.duration, setup.ejectSeconds, {} };
        SimulationRig preset;
        if (setup.mode == "preset") {
            if (!buildSourceSimulationRig(preset, error)) {
                return false;
            }
            if (!preset.derivedHandle.isEmpty()) {
                parts.curves[preset.slide] = preset.derivedHandle;
            }
            if (!preset.derivedBolt.isEmpty()) {
                parts.curves[preset.bolt] = preset.derivedBolt;
            }
            motion.provenance = preset.mechanicalSource;
        } else if (setup.mode == "clip") {
            if (!sampleSetupClip(setup, parts, motion, error)) {
                return false;
            }
        } else if (setup.mode == "manual") {
            if (!composeManualMovements(setup, parts, motion, error)) {
                return false;
            }
        }
        // A window chosen closed-to-open is played backwards.
        const bool reversed = parts.openFrame >= 0 && parts.closedFrame < parts.openFrame;
        if (reversed && !reverseSetupWindow(parts, motion, error)) {
            return false;
        }
        if(const auto* preset=checklistPreset();preset&&preset->model.mechanisms.size()==1&&preset->model.mechanisms.front().kind==MechanismKind::HkSlap)parts.singleHandle=true;
        if (!buildSimulationRig(recipe, rig, error, simulationMechanism(), setup.deriveHandle, {}, &parts)) {
            return false;
        }
        if (setup.mode == "preset" && !reversed) {
            if (rig.slide == preset.slide) {
                rig.derivedHandle = preset.derivedHandle;
            }
            if (rig.bolt == preset.bolt) {
                rig.derivedBolt = preset.derivedBolt;
            }
        }
        const bool boltAction = simulationMechanism() == WeaponMechanism::BoltAction;
        rig.customSetup = true;
        rig.manualCycle = (setup.mode == "manual" || setup.mode == "clip") && boltAction;
        rig.fireFromStroke = setup.fireMode == "stroke";
        rig.presetFire = setup.fireMode == "preset";
        rig.manualEjectSeconds = motion.ejectSeconds;
        rig.motionDuration = motion.duration;
        rig.mechanicalSource = motion.provenance;
        rig.authoring = operatingSetupJson(setup);
        return true;
    }

    // "clip" mode: the bolt and every follower sampled from another native
    // clip, weapon-local, over the recipe's frames. The clip's ejection
    // marker applies unless the setup gives an ejection time.
    bool ReloadMakerController::sampleSetupClip(const OperatingSetup& setup, SimulationParts& parts, ConfiguredMotion& motion, QString& error)
    {
        const auto& recipe = *_reloadRecipe;
        const auto bytes = readAnimationResource(setup.clip, error);
        const auto baseBytes = readAnimationResource(setup.baseline, error);
        const auto clip = HkxReader::readClip(bytes, error);
        const auto base = HkxReader::readClip(baseBytes, error);
        if (!clip.valid() || !base.valid()) {
            return false;
        }
        QVector<int> selected = parts.followers;
        if (!selected.contains(parts.bolt)) {
            selected.append(parts.bolt);
        }
        for (const int track : selected) {
            if (track < 0 || track >= recipe.tracks.size()) {
                error = "A follower is missing from the current reload";
                return false;
            }
        }
        WeaponLocalSampler sampler(_skeleton, _nodeBindings, _reloadScenePlan, _weaponBoneIndex);
        if (!sampler.setBaselineClip(base, _readyLocalPose, error)) {
            return false;
        }
        for (int frame = 0; frame < recipe.frames; ++frame) {
            const float seconds = clip.durationSeconds * frame / (recipe.frames - 1);
            if (!sampler.sample(clip, seconds, error)) {
                return false;
            }
            if (!sampler.weaponFrameValid()) {
                error = "Invalid native movement reference frame";
                return false;
            }
            for (const int track : selected) {
                parts.curves[track].append(sampler.nodePose(_reloadTrackNodes[track]));
            }
        }
        motion.duration = clip.durationSeconds;
        if (motion.ejectSeconds < 0) {
            for (const auto& event : clip.annotations) {
                if (event.text.compare("EjectShellCasing", Qt::CaseInsensitive) == 0) {
                    motion.ejectSeconds = event.timeSeconds;
                    break;
                }
            }
        }
        motion.provenance = {
            { "animation", setup.clip },
            { "baseline", setup.baseline },
            { "duration", motion.duration },
        };
        return true;
    }

    // "manual" mode: each assigned movement drives its part from the rest
    // pose, and its linked followers rigidly with it. Every manual follower
    // needs a movement or a link; no part may have two owners.
    bool ReloadMakerController::composeManualMovements(const OperatingSetup& setup, SimulationParts& parts, ConfiguredMotion& motion, QString& error)
    {
        QJsonArray definitions;
        for (auto it = setup.movements.cbegin(); it != setup.movements.cend(); ++it) {
            if (!composeManualMovement(setup, it.key(), it.value(), parts, definitions, error)) {
                return false;
            }
        }
        for (const int track : parts.followers) {
            if (!parts.curves.contains(track)) {
                error = "Assign a movement to every manual follower, or link it in the operating part's movement";
                return false;
            }
        }
        motion.duration = setup.duration;
        motion.provenance = {
            { "method", "authored-part-local-stages" },
            { "definitions", definitions },
            { "duration", motion.duration },
        };
        return true;
    }

    // One saved movement on `track`, sampled over the recipe's frames. Spring
    // mechanisms run closed to open; a manual bolt runs its whole sequence.
    bool ReloadMakerController::composeManualMovement(const OperatingSetup& setup, int track, const QString& movementId, SimulationParts& parts,
                                                      QJsonArray& definitions, QString& error)
    {
        const auto& recipe = *_reloadRecipe;
        if (track < 0 || track >= recipe.tracks.size()) {
            error = "An authored movement names a missing part";
            return false;
        }
        const auto movement = std::find_if(_mechanisms.cbegin(), _mechanisms.cend(), [&](const auto& m) { return m.id == movementId; });
        if (movement == _mechanisms.cend() || movement->stages.isEmpty() || !compatibleMechanismPart(movement->anchor, recipe.tracks[track].anchor)) {
            error = "Choose a saved movement authored on this exact native part";
            return false;
        }
        if (!validateMechanism(*movement, error)) {
            return false;
        }
        QHash<int, Transformf> followers;
        for (const auto& follower : movement->followers) {
            int found = -1;
            for (int i = 0; i < recipe.tracks.size(); ++i) {
                if (compatibleMechanismPart(follower.anchor, recipe.tracks[i].anchor)) {
                    if (found >= 0) {
                        error = "An authored follower is ambiguous";
                        return false;
                    }
                    found = i;
                }
            }
            if (found < 0) {
                error = "An authored movement follower is not installed";
                return false;
            }
            followers[found] = follower.inPartAtRest;
            if (!parts.followers.contains(found)) {
                parts.followers.append(found);
            }
        }
        if (parts.curves.contains(track)) {
            error = "Two authored movements control the same part";
            return false;
        }
        for (const int follower : followers.keys()) {
            if (parts.curves.contains(follower) || setup.movements.contains(follower)) {
                error = "A rigid follower also has an independent movement; remove the duplicate ownership";
                return false;
            }
        }
        const bool boltAction = simulationMechanism() == WeaponMechanism::BoltAction;
        const auto rest = recipe.tracks[track].poses.back();
        for (int frame = 0; frame < recipe.frames; ++frame) {
            const float u = float(frame) / (recipe.frames - 1);
            const float path = movement->stages.size() * (boltAction ? u : 1 - u);
            const auto pose = composed(rest, mechanismDelta(*movement, path));
            parts.curves[track].append(pose);
            for (auto f = followers.cbegin(); f != followers.cend(); ++f) {
                parts.curves[f.key()].append(composed(pose, f.value()));
            }
        }
        definitions.append(QJsonDocument::fromJson(encodeMechanism(*movement, error)).object());
        return true;
    }

    // Reverses the bolt's and followers' curves and mirrors the window so it
    // runs open to closed.
    bool ReloadMakerController::reverseSetupWindow(SimulationParts& parts, ConfiguredMotion& motion, QString& error)
    {
        const auto& recipe = *_reloadRecipe;
        auto tracks = parts.followers;
        if (!tracks.contains(parts.bolt)) {
            tracks.append(parts.bolt);
        }
        for (const int track : tracks) {
            if (track < 0 || track >= recipe.tracks.size()) {
                error = "A follower is outside this capture";
                return false;
            }
            auto curve = parts.curves.value(track, recipe.tracks[track].poses);
            std::reverse(curve.begin(), curve.end());
            parts.curves[track] = std::move(curve);
        }
        parts.openFrame = recipe.frames - 1 - parts.openFrame;
        parts.closedFrame = recipe.frames - 1 - parts.closedFrame;
        motion.provenance["reversedSourceWindow"] = true;
        return true;
    }

    // The setup dialog and MCP state: the saved setup (or the source defaults),
    // the recipe's tracks, the saved movements and native clips to choose
    // from, and the source rig's suggested window.
    QJsonObject ReloadMakerController::operatingSetupState()
    {
        if (!_reloadRecipe || _loadThread || _reloadCapturing || _reloadCaptureQueued || _rebuildTimer->isActive()) {
            throw std::runtime_error("Capture the current reload before configuring operating parts");
        }
        QJsonArray tracks;
        for (int i = 0; i < _reloadRecipe->tracks.size(); ++i) {
            const auto& track = _reloadRecipe->tracks[i];
            tracks.append(QJsonObject{
                { "index", i },
                { "name", track.anchor.anchorName },
                { "label", track.label },
                { "path", track.anchor.anchorPath },
                { "model", track.anchor.ownerModel },
                { "surface", track.surface },
            });
        }
        QJsonArray movements;
        for (const auto& movement : _mechanisms) {
            movements.append(QJsonObject{
                { "id", movement.id },
                { "name", movement.name },
                { "part", movement.anchor.anchorName },
                { "model", movement.anchor.ownerModel },
            });
        }
        QJsonArray clips;
        for (const auto& action : _animationActions) {
            clips.append(QJsonObject{ { "name", action.label }, { "path", action.resourcePath }, { "baseline", action.baselineResourcePath } });
        }
        const bool saved = authoringPlan().contains("operating");
        QString error;
        OperatingSetup setup;
        setup.recipeId = _reloadRecipe->id;
        setup.baseline = _reloadRecipe->source["baseline"].toString();
        setup.clip = _reloadRecipe->source["animation"].toString();
        // The running rig, when it is this recipe's, supplies the defaults;
        // otherwise the source rig, and the reload itself when there is none.
        SimulationRig source;
        if (_simulationRig && _simulationRig->recipeId == _reloadRecipe->id && _simulationRig->mechanism == simulationMechanism()) {
            source = *_simulationRig;
        } else if (!buildSourceSimulationRig(source, error)) {
            setup.mode = "reload";
        }
        setup.parts.operating = source.slide;
        setup.parts.bolt = source.bolt;
        setup.parts.magazine = source.magazine;
        setup.parts.followers = source.slideTracks;
        if (simulationMechanism() == WeaponMechanism::PistolSlide && setup.parts.bolt < 0) {
            setup.parts.bolt = setup.parts.operating;
        }
        if (saved) {
            OperatingSetup authored;
            if (savedOperatingSetup(authored, error)) {
                setup = authored;
            }
        }
        setup.trackAnchors = recipeTrackAnchors(*_reloadRecipe);
        return {
            { "settings", operatingSetupJson(setup) },
            { "saved", saved },
            { "source", authoringPlanSources()["operating"] },
            { "anchorNote", _operatingFallbackNote },
            { "tracks", tracks },
            { "movements", movements },
            { "clips", clips },
            { "frames", _reloadRecipe->frames },
            { "duration", _reloadRecipe->duration },
            { "suggestedOpenFrame", source.openFrame },
            { "suggestedClosedFrame", source.closedFrame },
            { "error", error },
        };
    }

    // Saves (after building the rig to check it) or resets the operating
    // setup, then restarts the simulation and saves the project.
    QJsonObject ReloadMakerController::configureOperatingSetup(const QJsonObject& request)
    {
        const auto key = authoringRecordKey();
        if (!_reloadRecipe || _loadThread || _reloadCapturing || _reloadCaptureQueued || _rebuildTimer->isActive() || key.isEmpty()) {
            throw std::runtime_error("Wait for the current source capture");
        }
        QJsonValue next;
        if (request["operation"] == "reset") {
            next = QJsonValue(QJsonValue::Undefined);
        } else if (request["operation"] == "save") {
            OperatingSetup setup;
            QString error;
            SimulationRig checked;
            if (!readOperatingSetup(request["settings"].toObject(), setup, error) || !remapOperatingSetup(setup, *_reloadRecipe, error) || !buildConfiguredSimulationRig(setup, checked, error)) {
                throw std::runtime_error(error.toStdString());
            }
            setup.recipeId = _reloadRecipe->id;
            setup.trackAnchors = recipeTrackAnchors(*_reloadRecipe);
            next = operatingSetupJson(setup);
        } else {
            throw std::runtime_error("Choose save or reset for the operating setup");
        }
        stopSimulation();
        writePlanField(QStringLiteral("operating"), next);
        _simulationRig.reset();
        noteDocumentChange(QStringLiteral("operating setup"));
        prepareSimulation();
        return operatingSetupState();
    }
}
