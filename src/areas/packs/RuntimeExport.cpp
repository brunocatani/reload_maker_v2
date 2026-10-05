// The runtime configuration DIAMOND's pack tools consume, for the
// configuration prepared in Operate: the operating motion, the magazine
// object with its hand fits and presentation, the grab points, sounds, and
// the source evidence the pack is checked against.
#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include "ReloadTemplates.h"
#include "areas/loadout/SceneTree.h"
#include "commands/CommandSupport.h"
#include "model/nifmodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        // Translation, rotation (x, y, z, w) and scale.
        QJsonArray runtimePose(const Transformf& p)
        {
            return { p.translation.x, p.translation.y, p.translation.z, p.rotation.x, p.rotation.y,
                     p.rotation.z,    p.rotation.w,    p.scale.x,       p.scale.y,    p.scale.z };
        }

        // The poses of frames first..last, or last..first when reversed.
        QJsonArray runtimeCurve(const QVector<Transformf>& poses, int first, int last, bool reverse = false)
        {
            require(first >= 0 && last >= first && last < poses.size(), "Invalid captured motion window");
            QJsonArray result;
            if (reverse) {
                for (int i = last; i >= first; --i) {
                    result.append(runtimePose(poses[i]));
                }
            } else {
                for (int i = first; i <= last; ++i) {
                    result.append(runtimePose(poses[i]));
                }
            }
            return result;
        }
    }

    // A track's bone path from the native Weapon bone, as the game names it.
    QString ReloadMakerController::runtimeTrackPath(int track) const
    {
        require(track >= 0 && track < _reloadTrackNodes.size() && _weaponBoneIndex >= 0, "Missing native part binding");
        const auto binding = std::find_if(_nodeBindings.cbegin(), _nodeBindings.cend(), [&](const auto& b) { return b.nodeIndex == _reloadTrackNodes[track]; });
        require(binding != _nodeBindings.cend(), "Part has no native animation binding");
        QStringList path;
        int bone = binding->boneIndex;
        for (int depth = 0; bone >= 0 && bone < _skeleton.bones.size() && depth < 64; ++depth) {
            path.prepend(_skeleton.bones[bone].name);
            if (bone == _weaponBoneIndex) {
                break;
            }
            bone = _skeleton.bones[bone].parentIndex;
        }
        require(bone == _weaponBoneIndex && path.size() > 1, "Part is outside the native Weapon hierarchy");
        return path.join('/');
    }

    // Requires a seated, idle Operate session with spring return (manual
    // bolts excepted), resolved errors, a concrete receiver and a magazine
    // record, the operating fingers of both hands and the operating grab
    // points (pistols excepted), and unchanged source resources.
    QJsonObject ReloadMakerController::exportRuntimeConfiguration() const
    {
        require(_simulationActive && _simulationRig && _reloadRecipe && _magazineRail && currentWeapon() && configuration(), "Prepare this configuration in Operate first");
        require(_simulationError.isEmpty() && _magazineError.isEmpty() && _magazineGrabError.isEmpty(), "Resolve the current simulation/magazine errors before capture");
        require(_simulation.action == SimulationAction::Idle && _simulation.magazine.seated(), "Reset Operate to a seated magazine before capturing runtime data");
        const auto& recipe = *_reloadRecipe;
        const auto& rig = *_simulationRig;
        require(rig.mechanism == WeaponMechanism::BoltAction || _slideSpringReturn, "This template requires spring return; enable it before export");
        const auto anchor = simulationMagazineAnchor();
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto* receiver = resolvedReceiverObjectMod();
        require(receiver || recipe.tracks[rig.slide].anchor.ownerRecord.compare(weapon, Qt::CaseInsensitive) == 0, "This template needs a concrete receiver selection");
        const auto receiverKey = receiver ? poseRecordKey(receiver->formId) : weapon;
        const auto magazineKey = anchor.ownerRecord;
        require(!magazineKey.isEmpty(), "The selected magazine has no source record");
        QJsonObject selection{ { "receiver", receiverKey }, { "magazine", magazineKey } };
        const auto type = rig.mechanism;
        auto profile = runtimeOperatingProfile();
        profile["receiver"] = receiverKey;
        profile["magazine"] = magazineKey;
        if (rig.customSetup) {
            profile["operatingSetup"] = rig.authoring;
            profile["motionSource"] = rig.mechanicalSource;
        }
        const auto& operating = recipe.tracks[rig.slide].anchor;
        if (operating.ownerRecord != receiverKey && operating.ownerRecord != magazineKey) {
            selection["operatingPart"] = operating.ownerRecord;
            profile["operatingPart"] = operating.ownerRecord;
        }
        const auto interactions = exportInteractionGrips();
        require(!interactions.isEmpty(), "Resolve the operating finger assignments before capture");
        const auto fingers = interactions["operatingFingers"].toArray();
        require(type == WeaponMechanism::PistolSlide || fingers.size() == 2, "Configure operating fingers for both hands before capture");
        if (!fingers.isEmpty()) {
            profile["operatingFingers"] = fingers;
        }
        const auto magazine = runtimeMagazine(anchor, weapon);
        const auto grabs = exportGrabLocations();
        require(type == WeaponMechanism::PistolSlide || !grabs.isEmpty(), "Configure the operating surface grab points before capture");
        const auto sources = sourceEvidence();
        require(auditSources(sources)["compatible"].toBool(), "Source resources changed; reload and review the configuration before export");
        const auto runtimeSources = runtimeSourceResources(sources, anchor);
        auto header = runtimeProfileHeader(weapon, receiver != nullptr, receiverKey, magazineKey, selection);
        const auto compatibility = sourcePolicyState();
        const auto policy = compatibility["policy"].toObject();
        for (auto it = policy.begin(); it != policy.end(); ++it) {
            header[it.key()] = it.value();
        }
        return { { "format", "reload-maker-runtime-configuration" },
                 { "version", 1 },
                 { "weapon", weapon },
                 { "template", templateIdentity(type) },
                 { "sourcePolicy", policy },
                 { "sourcePolicyEdited", compatibility["saved"] },
                 { "name", currentWeapon()->name },
                 { "profileHeader", header },
                 { "selection", selection },
                 { "profile", profile },
                 { "magazine", magazine },
                 { "grabs", grabs },
                 { "sounds", exportSounds() },
                 { "sources", sources },
                 { "runtimeSources", runtimeSources },
                 { "worldModelSpace", "magazine-carrier" } };
    }

    // The operating motion between the open and closed frames: the pistol's
    // native slide binding, or every operating track with the firing bolt
    // cycle, or a manual bolt's opening and closing strokes.
    QJsonObject ReloadMakerController::runtimeOperatingProfile() const
    {
        const auto& recipe = *_reloadRecipe;
        const auto& rig = *_simulationRig;
        const auto type = rig.mechanism;
        if (type == WeaponMechanism::PistolSlide) {
            auto profile = exportSlideProfile();
            require(!profile.isEmpty(), "The pistol has no complete native slide binding");
            return profile;
        }
        const auto indices = runtimeOperatingTracks();
        QJsonObject profile{ { "recipeId", recipe.id },
                             { "animation", recipe.source["animation"] },
                             { "animationSha256", recipe.source["animationSha256"] },
                             { "duration", (rig.motionDuration > 0 ? rig.motionDuration : recipe.duration) * (rig.closedFrame - rig.openFrame) / (recipe.frames - 1) },
                             { "slideTrack", indices.indexOf(rig.slide) },
                             { "boltTrack", indices.indexOf(rig.bolt) } };
        QJsonArray tracks;
        for (const int index : indices) {
            tracks.append(runtimeOperatingTrack(index));
        }
        profile["tracks"] = tracks;
        if (type == WeaponMechanism::BoltAction) {
            QJsonArray opening;
            QJsonArray closing;
            for (float value : _boltCycle.opening) {
                opening.append(value);
            }
            for (auto it = _boltCycle.closing.crbegin(); it != _boltCycle.closing.crend(); ++it) {
                closing.append(1 - *it);
            }
            profile["openingProgress"] = opening;
            profile["closingProgress"] = closing;
            profile["inputAxis"] = QJsonArray{ _boltCycle.inputAxis.x, _boltCycle.inputAxis.y, _boltCycle.inputAxis.z };
            profile["inputTravel"] = _boltCycle.inputTravel;
            profile["ejectProgress"] = _boltCycle.ejectProgress;
        } else {
            require(!_rifleBoltCycle.isEmpty(), "Missing native firing bolt motion");
            profile["fireCycle"] = runtimeCurve(_rifleBoltCycle, 0, _rifleBoltCycle.size() - 1);
            profile["fireOpen"] = _rifleBoltOpen;
            profile["fireDuration"] = _rifleBoltDuration;
        }
        return profile;
    }

    // The slide's followers, then the bolt, the M1A's magazine children and,
    // with a magazine setup, the sampled payload tracks.
    QVector<int> ReloadMakerController::runtimeOperatingTracks() const
    {
        const auto& recipe = *_reloadRecipe;
        const auto& rig = *_simulationRig;
        QVector<int> indices = rig.slideTracks;
        if (!indices.contains(rig.bolt)) {
            indices.append(rig.bolt);
        }
        if (rig.mechanism == WeaponMechanism::M1A) {
            for (int i = 0; i < recipe.tracks.size(); ++i) {
                const auto& name = recipe.tracks[i].anchor.anchorName;
                if ((name == "WeaponMagazineChild1" || name == "WeaponMagazineChild2" || name == "WeaponMagazineChild3") && !indices.contains(i)) {
                    indices.append(i);
                }
            }
        }
        if (_magazineSetup) {
            for (int i = 0; i < recipe.tracks.size(); ++i) {
                if (runtimePayloadTrack(i) && !indices.contains(i)) {
                    indices.append(i);
                }
            }
        }
        return indices;
    }

    // One operating track's captured poses. Payload pieces are relative to
    // the full magazine's seat, with their loaded and emptied rests; manual
    // bolt followers add their opening and closing strokes.
    QJsonObject ReloadMakerController::runtimeOperatingTrack(int index) const
    {
        const auto& recipe = *_reloadRecipe;
        const auto& rig = *_simulationRig;
        const auto& rail = *_magazineRail;
        const auto type = rig.mechanism;
        require(index >= 0 && index < recipe.tracks.size(), "An operating follower has no captured source track");
        const auto& source = recipe.tracks[index];
        const auto& a = source.anchor;
        const auto& poses = simulationTrackCurve(recipe, rig, index);
        QJsonObject track{ { "name", a.anchorName },
                           { "model", a.ownerModel },
                           { "record", a.ownerRecord },
                           { "sourcePath", a.anchorPath },
                           { "runtimePath", runtimeTrackPath(index) },
                           { "poses", runtimeCurve(poses, rig.openFrame, rig.closedFrame) } };
        if (type == WeaponMechanism::Ar15 && isSrepConfiguration() && a.anchorName == "WeaponExtra2") {
            track["boltFollower"] = true;
        }
        if (_magazineSetup ? runtimePayloadTrack(index) : type == WeaponMechanism::M1A && a.anchorName.startsWith("WeaponMagazineChild")) {
            const auto normalized = [&](int frame) {
                Transformf inverse;
                require(invertUniformTransform(recipe.tracks[rail.fullTrack].poses[frame], inverse), "Invalid magazine payload frame");
                return composed(composed(recipe.tracks[rail.fullTrack].poses[0], inverse), source.poses[frame]);
            };
            QJsonArray payload;
            for (int frame = rig.openFrame; frame <= rig.closedFrame; ++frame) {
                payload.append(runtimePose(normalized(frame)));
            }
            track["payload"] = true;
            track["poses"] = payload;
            track["loadedRest"] = runtimePose(normalized(recipe.frames - 1));
            track["emptyRest"] = runtimePose(normalized(_m1aEmptyMagazineFrame));
        }
        if (type == WeaponMechanism::BoltAction) {
            const int slot = _boltCycle.tracks.indexOf(index);
            require(slot >= 0 && slot < _boltCycle.poses.size() && slot < _boltFiredPose.size(), "Incomplete manual bolt follower");
            track["opening"] = runtimeCurve(_boltCycle.poses[slot], _boltCycle.first, _boltCycle.rear);
            track["closing"] = runtimeCurve(_boltCycle.poses[slot], _boltCycle.rear, _boltCycle.last, true);
            track["firedRest"] = runtimePose(_boltFiredPose[slot]);
        }
        return track;
    }

    // Whether a recipe track hangs below the full magazine's carrier track.
    bool ReloadMakerController::runtimeFullTrack(int index) const
    {
        const auto& recipe = *_reloadRecipe;
        for (int parent = index, depth = 0; parent >= 0 && depth < recipe.tracks.size(); parent = recipe.tracks[parent].parent, ++depth) {
            if (parent == _magazineRail->fullTrack) {
                return true;
            }
        }
        return false;
    }

    // A sampled payload surface moving inside the full magazine.
    bool ReloadMakerController::runtimePayloadTrack(int index) const
    {
        return sampledMagazinePayload() && index != _magazineRail->fullTrack && _reloadRecipe->tracks[index].surface && runtimeFullTrack(index);
    }

    // The magazine object: record, model, capacity, rail and seat, its full
    // and empty carrier paths, an authored movement, both hands' fits, the
    // model's frame in the carrier, its parts and rail motion, the pieces
    // hidden or excluded per presentation, and where a parked magazine goes.
    QJsonObject ReloadMakerController::runtimeMagazine(const HandPosePlacement& anchor, const QString& weapon) const
    {
        const auto& recipe = *_reloadRecipe;
        const auto& rail = *_magazineRail;
        const auto& full = recipe.tracks[rail.fullTrack];
        QJsonObject magazine{ { "record", anchor.ownerRecord },
                              { "model", anchor.ownerModel },
                              { "capacity", _simulationAmmo.capacity },
                              { "rail", magazineRailJson(rail) },
                              { "seat", runtimePose(full.poses[0]) },
                              { "fullName", full.anchor.anchorName },
                              { "emptyName", recipe.tracks[rail.emptyTrack].anchor.anchorName },
                              { "sourceRecipeId", recipe.id },
                              { "emptyHidePrefix", "" } };
        magazine["fullPath"] = runtimeTrackPath(rail.fullTrack);
        magazine["emptyPath"] = runtimeTrackPath(rail.emptyTrack);
        if (_magazineSetup) {
            magazine["presentationSetup"] = magazineSetupJson(*_magazineSetup);
        }
        if (_magazineSetup && _magazineSetup->motion == "manual") {
            const auto movement = std::find_if(_mechanisms.cbegin(), _mechanisms.cend(), [&](const auto& m) { return m.id == _magazineSetup->movementId; });
            require(movement != _mechanisms.cend(), "The authored magazine movement is missing");
            QString sourceError;
            const auto bytes = encodeMechanism(*movement, sourceError);
            require(!bytes.isEmpty(), sourceError);
            magazine["motionSource"] = QJsonObject{ { "method", "authored-part-local-stages" }, { "definition", QJsonDocument::fromJson(bytes).object() } };
        }
        addRuntimeMagazineGrips(magazine, anchor, weapon);
        addRuntimeMagazineReference(magazine, anchor);
        addRuntimeMagazineParts(magazine);
        addRuntimeMagazineHiding(magazine);
        if (isSrepEmbeddedMagazine()) {
            magazine["parkedMagazinePath"] = "Weapon/WeaponOptics2/MagHelperRotate/MagHelper";
        } else if (isMk18Configuration()) {
            magazine["parkedMagazinePath"] = "Weapon/WeaponExtra3";
        }
        return magazine;
    }

    // Both hands' magazine fits as saved hand poses; a pistol has the left.
    void ReloadMakerController::addRuntimeMagazineGrips(QJsonObject& magazine, const HandPosePlacement& anchor, const QString& weapon) const
    {
        QString error;
        for (const auto hand : { PoseHand::Left, PoseHand::Right }) {
            if (hand == PoseHand::Right && _simulationRig->mechanism == WeaponMechanism::PistolSlide) {
                continue;
            }
            const auto grip = magazineGripFor(weapon, anchor, _partGrips, _magazineGrabGrips, _magazineGrabDefaults, error, hand);
            require(grip.has_value(), hand == PoseHand::Left ? "Configure the left magazine hand before capture" : "Configure the right magazine hand before capture");
            const auto bytes = encodeHandPose(grip->pose, error);
            require(!bytes.isEmpty(), error);
            magazine[hand == PoseHand::Left ? "grip" : "rightGrip"] = QJsonDocument::fromJson(bytes).object();
        }
    }

    // The magazine model's frame in its carrier, and the authored capture
    // point when the reference is not the source's own connection. This is
    // the native carrier/reference bridge, not the skeleton-binding bridge
    // used by the offline arm solver. Both roots are read in one frame.
    void ReloadMakerController::addRuntimeMagazineReference(QJsonObject& magazine, const HandPosePlacement& anchor) const
    {
        const auto& rail = *_magazineRail;
        int reference = -1;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            if (sameGripAnchor(_handPoseAnchors[i].description, anchor)) {
                reference = i;
                break;
            }
        }
        require(reference >= 0 && rail.fullTrack < _reloadTrackNodes.size(), "Missing native magazine reference");
        Transformf carrier;
        Transformf referenceWorld;
        Transformf inverse;
        QString error;
        require(sceneNodeWorld(_sceneNodes[_reloadTrackNodes[rail.fullTrack]], carrier, error) && sceneNodeWorld(_handPoseAnchors[reference].node, referenceWorld, error)
                    && invertUniformTransform(carrier, inverse),
                "Cannot establish the magazine carrier frame");
        magazine["modelInMagazine"] = runtimePose(composed(inverse, referenceWorld));
        const auto referenceSetup = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineReference").toObject();
        if (!referenceSetup.isEmpty() && referenceSetup.value("connectionMode") != "source") {
            const auto point = composed(inverse, rail.connection).translation;
            magazine["capturePointInMagazine"] = QJsonArray{ point.x, point.y, point.z };
        }
    }

    // The magazine's visible parts at rest in the carrier frame (a sampled
    // payload at its loaded frame), the emptied payload's parts, and the rail
    // motion by progress.
    void ReloadMakerController::addRuntimeMagazineParts(QJsonObject& magazine) const
    {
        const auto& recipe = *_reloadRecipe;
        const auto& rail = *_magazineRail;
        const auto& full = recipe.tracks[rail.fullTrack];
        const auto type = _simulationRig->mechanism;
        QJsonArray parts;
        QJsonArray emptyParts;
        const auto normalizedPart = [&](const ReloadTrack& track, int frame) {
            Transformf inv;
            require(invertUniformTransform(full.poses[frame], inv), "Invalid payload carrier");
            return composed(composed(full.poses[0], inv), track.poses[frame]);
        };
        // These assemblies synthesize timing-only magazine bones absent from their
        // native meshes. Other established templates retain their complete chain.
        const bool nativeSurfacesOnly = isMk18Configuration() || isSrepConfiguration();
        for (int index = 0; index < recipe.tracks.size(); ++index) {
            const auto& track = recipe.tracks[index];
            const bool magazinePart = _magazineSetup ? runtimeFullTrack(index) : track.anchor.anchorName.startsWith("WeaponMagazine");
            if (!magazinePart || (nativeSurfacesOnly && !track.surface)) {
                continue;
            }
            const int frame = sampledMagazinePayload() ? _m1aLoadedMagazineFrame : 0;
            const auto pose = sampledMagazinePayload() ? normalizedPart(track, frame) : track.poses[0];
            if (pose.scale.x > 1e-5f) {
                parts.append(QJsonObject{ { "name", track.anchor.anchorName }, { "pose", runtimePose(pose) } });
            }
            const bool emptied = _magazineSetup ? sampledMagazinePayload() && index != rail.fullTrack && runtimeFullTrack(index)
                                                : type == WeaponMechanism::M1A && track.anchor.anchorName.startsWith("WeaponMagazineChild");
            if (emptied) {
                const auto empty = normalizedPart(track, _m1aEmptyMagazineFrame);
                if (empty.scale.x > 1e-5f) {
                    emptyParts.append(QJsonObject{ { "name", track.anchor.anchorName }, { "pose", runtimePose(empty) } });
                }
            }
        }
        magazine["parts"] = parts;
        if (!emptyParts.isEmpty()) {
            magazine["emptyParts"] = emptyParts;
        }
        QJsonArray motion;
        for (int i = 0; i < rail.motion.size(); ++i) {
            motion.append(QJsonObject{ { "progress", rail.motionDistances[i] / rail.motionDistances.back() }, { "pose", runtimePose(rail.motion[i]) } });
        }
        magazine["motion"] = motion;
    }

    // The payload groups hidden on an empty magazine (at most 16, each the
    // highest node holding no body piece), and with a magazine setup the
    // native-only pieces excluded from each body, by node path.
    void ReloadMakerController::addRuntimeMagazineHiding(QJsonObject& magazine) const
    {
        const auto& rail = *_magazineRail;
        auto* model = activeModel();
        const auto root = _sceneNodes[_reloadTrackNodes[rail.fullTrack]];
        QJsonArray payload;
        for (const auto& visual : _magazineVisuals) {
            if (!visual.cartridge || visual.nativeOnly || visual.empty || !visual.node.isValid() || !isWithinBlock(*model, visual.node, root)) {
                continue;
            }
            QModelIndex candidate = visual.node;
            for (int depth = 0; depth < model->getBlockCount(); ++depth) {
                const auto parent = model->getBlockIndex(model->getParent(candidate));
                if (!parent.isValid() || parent == root) {
                    break;
                }
                const bool body = std::any_of(_magazineVisuals.begin(), _magazineVisuals.end(), [&](const auto& other) {
                    return !other.nativeOnly && !other.empty && !other.cartridge && isWithinBlock(*model, other.node, parent);
                });
                if (body) {
                    break;
                }
                candidate = parent;
            }
            const auto name = model->get<QString>(candidate, "Name");
            require(!name.isEmpty() && name != "WeaponMagazine", "Payload needs an explicit non-body selector");
            if (!payload.contains(name)) {
                payload.append(name);
            }
        }
        require(payload.size() <= 16, "Payload needs fewer independently named groups for this template");
        if (!payload.isEmpty()) {
            magazine["emptyHideNodes"] = payload;
        }
        if (!_magazineSetup) {
            return;
        }
        for (bool fullBody : { true, false }) {
            const auto carrier = _sceneNodes[_reloadTrackNodes[fullBody ? rail.fullTrack : rail.emptyTrack]];
            QJsonArray excluded;
            for (const auto& visual : _magazineVisuals) {
                if (!visual.nativeOnly || !visual.node.isValid() || !isWithinBlock(*model, visual.node, carrier)) {
                    continue;
                }
                QStringList names;
                auto node = QModelIndex(visual.node);
                for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
                    const auto name = model->get<QString>(node, "Name");
                    require(!name.isEmpty(), "Excluded magazine pieces need named native ancestry");
                    names.prepend(name);
                    if (node == carrier) {
                        break;
                    }
                    node = model->getBlockIndex(model->getParent(node));
                }
                excluded.append(names.join('/'));
            }
            if (!excluded.isEmpty()) {
                magazine[fullBody ? "excludedFullPieces" : "excludedEmptyPieces"] = excluded;
            }
        }
    }

    // The animations, and the models the runtime motion depends on: the
    // weapon, the magazine and the owner of every operating and magazine track.
    QJsonArray ReloadMakerController::runtimeSourceResources(const QJsonObject& sources, const HandPosePlacement& anchor) const
    {
        const auto& recipe = *_reloadRecipe;
        const auto& rig = *_simulationRig;
        QSet<QString> criticalModels{ normalizeNifPath(currentWeapon()->baseModelPath), normalizeNifPath(anchor.ownerModel) };
        auto criticalTracks = rig.slideTracks;
        for (int track : rig.magazineTracks) {
            if (!criticalTracks.contains(track)) {
                criticalTracks.append(track);
            }
        }
        if (rig.bolt >= 0 && !criticalTracks.contains(rig.bolt)) {
            criticalTracks.append(rig.bolt);
        }
        for (int track : criticalTracks) {
            criticalModels.insert(normalizeNifPath(recipe.tracks[track].anchor.ownerModel));
        }
        QJsonArray runtimeSources;
        for (const auto& value : sources["resources"].toArray()) {
            const auto source = value.toObject();
            if (source["kind"] == "animation" || criticalModels.contains(source["path"].toString())) {
                runtimeSources.append(source);
            }
        }
        return runtimeSources;
    }

    // The pack header: format and mechanism, the standard receiver, and the
    // catalog slots with their records and attach points.
    QJsonObject ReloadMakerController::runtimeProfileHeader(const QString& weapon, bool concreteReceiver, const QString& receiverKey, const QString& magazineKey,
                                                            const QJsonObject& selection) const
    {
        const auto type = _simulationRig->mechanism;
        QJsonObject header{ { "format", type == WeaponMechanism::PistolSlide ? "diamond-pistol-pack" : "diamond-weapon-pack" }, { "version", 1 }, { "weapon", weapon } };
        QJsonArray catalogSlots;
        if (type == WeaponMechanism::PistolSlide) {
            header["standardReceiver"] = receiverKey;
            header["strictReceiver"] = true;
            for (const auto* mod : configuredObjectMods()) {
                const auto key = poseRecordKey(mod->formId);
                const auto* slot = configuration()->slot(configurationSlotKey(*mod));
                catalogSlots.append(QJsonObject{ { "name", key == receiverKey ? "Receiver" : key == magazineKey ? "Magazine" : slot ? slot->label : key }, { "records", QJsonArray{ key } } });
                if (key == magazineKey && mod->attachPoint) {
                    header["magazineAttachPoint"] = poseRecordKey(mod->attachPoint);
                }
            }
        } else {
            header["mechanism"] = type == WeaponMechanism::Ar15 ? "ar15" : type == WeaponMechanism::M1A ? "m1a" : type == WeaponMechanism::BoltAction ? "manual-bolt-action" : "reciprocating-bolt";
            header["standardReceiver"] = receiverKey;
            header["strictReceiver"] = concreteReceiver;
            for (const auto& pair : { qMakePair(QString("receiver"), QString("Receiver")), qMakePair(QString("magazine"), QString("Magazine")),
                                      qMakePair(QString("operatingPart"), QString("OperatingPart")) }) {
                if (!selection.contains(pair.first)) {
                    continue;
                }
                const auto record = selection[pair.first].toString();
                catalogSlots.append(QJsonObject{ { "name", pair.second }, { "records", QJsonArray{ record } } });
                if (pair.first == "receiver") {
                    continue;
                }
                for (const auto* mod : configuredObjectMods()) {
                    if (poseRecordKey(mod->formId) == record && mod->attachPoint) {
                        header[pair.first == "magazine" ? "magazineAttachPoint" : "operatingAttachPoint"] = poseRecordKey(mod->attachPoint);
                    }
                }
            }
            if (type == WeaponMechanism::Ar15 && isSrepConfiguration()) {
                header["manualBoltFromReload"] = true;
            }
            if (isSrepEmbeddedMagazine()) {
                header["embeddedMagazineReceivers"] = QJsonArray{ receiverKey };
            }
        }
        header["slots"] = catalogSlots;
        return header;
    }
}
