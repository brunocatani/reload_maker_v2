// The runtime configuration DIAMOND's pack tools consume, for the
// configuration prepared in Operate: the operating motion, the magazine
// object with its hand fits and presentation, the grab points, sounds, and
// the source evidence the pack is checked against.
#include "ReloadMakerController.h"
#include "domain/presets/WeaponPresets.h"
#include "ReloadBundle.h"
#include "RuntimeSourcePath.h"
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
        if (binding == _nodeBindings.cend()) {
            require(_reloadRecipe && track < _reloadRecipe->tracks.size(), "Static part has no captured source anchor");
            const auto& anchor = _reloadRecipe->tracks[track].anchor;
            auto path = anchor.anchorPath.split('/');
            require(path.size() > 1 && path.back() == anchor.anchorName && !anchor.ownerModel.isEmpty(),
                    "Static part has no unambiguous model-relative path");
            // The source path retains the exact model root for native binding.
            // The runtime's logical path starts at Weapon, just as bound tracks
            // do; source-only children need no invented animation skeleton bone.
            path.front() = _skeleton.bones[_weaponBoneIndex].name;
            return path.join('/');
        }
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
        return runtimeSourcePath(path.join('/'),_reloadRecipe->tracks[track].anchor.anchorPath);
    }

    // Spare branches need not share a parent (for example a retained magazine
    // and a separate feed-round animation). Never hide their weapon ancestor.
    QStringList ReloadMakerController::parkedMagazinePaths() const
    {
        auto* model=activeModel();QStringList paths;
        for(const auto& visual:_magazineVisuals) {
            if(!visual.parked||!visual.node.isValid())continue;
            bool found=false;
            for(auto node=model->getBlockIndex(model->getParent(visual.node));node.isValid()&&!found;node=model->getBlockIndex(model->getParent(node))) {
                for(int track=0;track<_reloadTrackNodes.size();++track) {
                    if(QModelIndex(_sceneNodes[_reloadTrackNodes[track]])!=node)continue;
                    const auto path=runtimeTrackPath(track);
                    if(!paths.contains(path))paths.append(path);
                    found=true;break;
                }
            }
            require(found,"An animation-only magazine piece has no bound source branch");
        }
        QStringList roots;
        for(const auto& path:paths) {
            const bool covered=std::any_of(paths.cbegin(),paths.cend(),[&](const auto& other){return other!=path&&path.startsWith(other+'/');});
            if(!covered)roots.append(path);
        }
        return roots;
    }

    // Requires a seated, idle Operate session with spring return (manual
    // bolts excepted), resolved errors, a concrete receiver and a magazine
    // record, the operating fingers of both hands and the operating grab
    // points (pistols excepted), and unchanged source resources.
    QJsonObject ReloadMakerController::exportRuntimeConfiguration() const
    {
        require(_simulationActive && _simulationRig && _reloadRecipe && _magazineRail && currentWeapon() && configuration(), "Prepare this configuration in Operate first");
        require(_simulationError.isEmpty() && _magazineError.isEmpty() && _magazineGrabError.isEmpty(), "Resolve the current simulation/magazine errors before capture");
        require(_simulation.action == SimulationAction::Idle && (_simulation.magazine.seated()||(_simulation.unitLoading&&!_simulation.magazine.carried())), "Reset Operate to a seated magazine before capturing runtime data");
        const auto& recipe = *_reloadRecipe;
        const auto& rig = *_simulationRig;
        const auto anchor = simulationMagazineAnchor();
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto* receiver = reloadReceiverObjectMod();
        require(receiver || recipe.tracks[rig.slide].anchor.ownerRecord.compare(weapon, Qt::CaseInsensitive) == 0, "This template needs a concrete receiver selection");
        const auto receiverKey = receiver ? poseRecordKey(receiver->formId) : weapon;
        const auto magazineKey = anchor.ownerRecord;
        require(!magazineKey.isEmpty(), "The selected magazine has no source record");
        QJsonObject selection{ { "receiver", receiverKey }, { "magazine", magazineKey } };
        const auto type = rig.mechanism;
        auto profile = runtimeOperatingProfile();
        profile["receiver"] = receiverKey;
        profile["openBolt"] = rig.openBolt;
        profile["slapClose"] = rig.slapClose;
        profile["lastShotHoldOpen"] = rig.lastShotHoldOpen;
        if(const auto* preset=checklistPreset())profile["magazineRelease"]=magazineReleaseName(preset->model.feed.magazineRelease);
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
        // Fingers and grab points are optional refinements (owner,
        // 2026-10-08): without authored fingers the game keeps ROCK's own hand
        // pose, and without grab points the whole operating part is grabbable.
        const auto fingers = interactions["operatingFingers"].toArray();
        if (!fingers.isEmpty()) {
            profile["operatingFingers"] = fingers;
        }
        auto magazine = runtimeMagazine(anchor, weapon);
        magazine["ejectionSpeed"] = rig.magazineEjectionSpeed;
        const auto grabs = exportGrabLocations();
        auto header = runtimeProfileHeader(weapon, receiverKey, magazineKey, selection);
        header["hkSlap"]=rig.hkSlap;
        header["lastShotHoldOpen"]=rig.lastShotHoldOpen;
        if(const auto* preset=checklistPreset())header["magazineRelease"]=magazineReleaseName(preset->model.feed.magazineRelease);
        header["singleHandle"]=rig.singleHandle;
        const auto compatibility = sourcePolicyState();
        const auto policy = compatibility["policy"].toObject();
        for (auto it = policy.begin(); it != policy.end(); ++it) {
            header[it.key()] = it.value();
        }
        return { { "format", "reload-maker-runtime-configuration" },
                 { "version", 1 },
                 { "weapon", weapon },
                 { "template", assignedTemplate() },
                 { "sourcePolicy", policy },
                 { "sourcePolicyEdited", compatibility["saved"] },
                 { "name", currentWeapon()->name },
                 { "profileHeader", header },
                 { "selection", selection },
                 { "profile", profile },
                 { "magazine", magazine },
                 { "grabs", grabs },
                 { "sounds", exportSounds() },
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
                             { "duration", simulationStrokeDuration(recipe, rig) },
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
        } else if (type != WeaponMechanism::CellLever) {
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
        if(rig.grabCurves.contains(index)) {
            QJsonArray poses;for(const auto& pose:rig.grabCurves[index])poses.append(runtimePose(pose));
            track["grabPoses"]=poses;
        }
        if (type == WeaponMechanism::Ar15 && index != rig.slide && rig.slideTracks.contains(index) && !rig.authoring["grabDeployIndependent"].toBool()) {
            track["boltFollower"] = true; // a part the bolt drives
        }
        if (_magazineSetup ? runtimePayloadTrack(index) : type == WeaponMechanism::M1A && a.anchorName.startsWith("WeaponMagazineChild")) {
            const auto normalized = [&](float frame) {
                frame = std::clamp(frame, 0.f, float(recipe.frames - 1));
                const int first = int(frame), last = std::min(first + 1, recipe.frames - 1);
                const float fraction = frame - first;
                const auto& carrier = recipe.tracks[rail.fullTrack].poses;
                const auto carrierPose = fraction == 0 ? carrier[first] : interpolatedTransform(carrier[first], carrier[last], fraction);
                const auto partPose = fraction == 0 ? source.poses[first] : interpolatedTransform(source.poses[first], source.poses[last], fraction);
                Transformf inverse;
                require(invertUniformTransform(carrierPose, inverse), "Invalid magazine payload frame");
                return composed(composed(carrier[0], inverse), partPose);
            };
            QJsonArray payload;
            for (int frame = rig.openFrame; frame <= rig.closedFrame; ++frame) {
                const float nativeFrame = simulationNativeFrame(recipe, rig, float(frame));
                payload.append(runtimePose(normalized(nativeFrame)));
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
                              { "capacity", _simulation.unitLoading?_simulation.capacity:_simulationAmmo.capacity },
                              { "rail", magazineRailJson(rail) },
                              { "seat", runtimePose(composed(rail.sourceToSeat,full.poses[0])) },
                              { "fullName", full.anchor.anchorName },
                              { "emptyName", recipe.tracks[rail.emptyTrack].anchor.anchorName },
                              { "sourceRecipeId", recipe.id },
                              { "emptyHidePrefix", "" } };
        magazine["unitLoading"]=unitLoadingConfiguration();
        magazine["fullPath"] = runtimeTrackPath(rail.fullTrack);
        magazine["emptyPath"] = runtimeTrackPath(rail.emptyTrack);
        if (const auto presentation = magazinePresentationJson(_magazineSetup ? magazineSetupJson(*_magazineSetup) : QJsonObject{}); !presentation.isEmpty()) {
            magazine["presentationSetup"] = presentation;
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
        if (const auto parked = parkedMagazinePaths(); !parked.isEmpty()) {
            if(parked.size()==1)magazine["parkedMagazinePath"]=parked.front();
            else magazine["parkedMagazinePaths"] = QJsonArray::fromStringList(parked);
        }
        return magazine;
    }

    // Both hands' magazine fits as saved hand poses; a pistol has the left.
    void ReloadMakerController::addRuntimeMagazineGrips(QJsonObject& magazine, const HandPosePlacement& anchor, const QString& weapon) const
    {
        QString error;
        for (const auto hand : { PoseHand::Left, PoseHand::Right }) {
            if (!magazineHandAllowed(hand)) {
                continue;
            }
            const auto grip = magazineGripFor(weapon, anchor, _partGrips, _magazineGrabGrips, _magazineGrabDefaults, error, hand, _siblingMagazineFits);
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
        const auto referenceSetup = authoringPlan().value("magazineReference").toObject();
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
        const auto partPath = [&](int index) {
            auto* model = activeModel();
            const auto carrier = _sceneNodes[_reloadTrackNodes[rail.fullTrack]];
            auto node = _sceneNodes[_reloadTrackNodes[index]];
            QStringList path;
            for (int depth = 0; node.isValid() && depth < 64; ++depth) {
                path.prepend(node != carrier && _attachmentRuntimeNames.contains(model->getBlockNumber(node))
                    ? _attachmentRuntimeNames.value(model->getBlockNumber(node)) : model->get<QString>(node, "Name"));
                if (node == carrier) {
                    // The game retains the receiver's named parent and inserts
                    // the incoming model below a connect-point wrapper. Maker
                    // separates its animated copy to avoid double animation.
                    const int block = model->getBlockNumber(carrier);
                    const int carrierNode = _reloadTrackNodes[rail.fullTrack];
                    if (_partAlignmentAnchors.contains(block) &&
                        std::any_of(_nodeBindings.cbegin(), _nodeBindings.cend(), [&](const auto& b) {
                            return b.nodeIndex != carrierNode && b.boneIndex == _assemblyNodes[carrierNode].boneIndex;
                        })) {
                        auto mount = carrier;
                        QString connection;
                        for (int depth = 0; mount.isValid() && depth < 64; ++depth) {
                            connection = _attachmentRuntimeNames.value(model->getBlockNumber(mount));
                            if (!connection.isEmpty()) break;
                            mount = model->getBlockIndex(model->getParent(mount));
                        }
                        require(!connection.isEmpty(), "Missing native magazine attachment name");
                        path.prepend(connection);
                        path.prepend(model->get<QString>(carrier, "Name"));
                    }
                    return path.join('/');
                }
                node = model->getBlockIndex(model->getParent(node));
            }
            throw std::runtime_error("Magazine part is outside the physical carrier");
        };
        // Every magazine bone the game has is posed, empty or not: in game
        // another attachment may hang below a bone that is empty here. A bone
        // Maker synthesized from the skeleton (for a passive attachment) does
        // not exist in the game; with no geometry below it, it is left out.
        for (int index = 0; index < recipe.tracks.size(); ++index) {
            const auto& track = recipe.tracks[index];
            // Membership follows the actual carrier, including receivers
            // whose magazine rides WeaponExtraN. Names alone also include
            // unrelated animation-only magazine/payload branches.
            const bool magazinePart = runtimeFullTrack(index);
            const bool synthesized = index < _reloadTrackNodes.size() && _reloadTrackNodes[index] >= 0 &&
                                     _reloadTrackNodes[index] < _sceneNodes.size() && _synthesizedBones.contains(_sceneNodes[_reloadTrackNodes[index]]);
            if (!magazinePart || (synthesized && !track.surface)) {
                continue;
            }
            // An installed model can repeat the receiver's empty timing bones.
            // The runtime selector is a bone name; include the physical owner,
            // not an additional empty copy with a different mount transform.
            if (!track.surface && std::any_of(recipe.tracks.cbegin(), recipe.tracks.cend(), [&](const auto& other) {
                    return &other != &track && other.anchor.anchorName == track.anchor.anchorName;
                })) {
                continue;
            }
            const int frame = sampledMagazinePayload() ? _m1aLoadedMagazineFrame : 0;
            const auto pose = composed(rail.sourceToSeat,sampledMagazinePayload() ? normalizedPart(track, frame) : track.poses[0]);
            if (pose.scale.x > 1e-5f) {
                parts.append(QJsonObject{ { "name", track.anchor.anchorName }, { "pose", runtimePose(pose) }, { "path", partPath(index) } });
            }
            const bool emptied = _magazineSetup ? sampledMagazinePayload() && index != rail.fullTrack && runtimeFullTrack(index)
                                                : type == WeaponMechanism::M1A && track.anchor.anchorName.startsWith("WeaponMagazineChild");
            if (emptied) {
                const auto empty = normalizedPart(track, _m1aEmptyMagazineFrame);
                if (empty.scale.x > 1e-5f) {
                    emptyParts.append(QJsonObject{ { "name", track.anchor.anchorName }, { "pose", runtimePose(empty) }, { "path", partPath(index) } });
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
        if(unitLoadingConfiguration()&&_magazineSetup){
            QJsonArray followers;
            for(int track:_magazineSetup->loadingFollowers){
                QJsonArray keys;
                for(int i=0;i<rail.motionDistances.size();++i){
                    const float seconds=rail.motionSeconds[i];
                    keys.append(QJsonObject{{"progress",rail.motionDistances[i]/rail.motionDistances.back()},{"pose",runtimePose(reloadTrackPose(recipe,track,seconds))}});
                }
                followers.append(QJsonObject{{"path",runtimeTrackPath(track)},{"motion",keys}});
            }
            magazine["loadingParts"]=followers;
        }
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
                // Nested attachment aliases may repeat the carrier's name.
                // Hiding that name would also hide the outer magazine body;
                // keep the payload's narrower child selector instead.
                if (model->get<QString>(parent, "Name") == model->get<QString>(root, "Name")) {
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

    // The pack header: format and mechanism, the standard receiver, and the
    // catalog slots with their records and attach points.
    QJsonObject ReloadMakerController::runtimeProfileHeader(const QString& weapon, const QString& receiverKey, const QString& magazineKey,
                                                            const QJsonObject& selection) const
    {
        const auto type = _simulationRig->mechanism;
        QJsonObject header{ { "format", type == WeaponMechanism::PistolSlide ? "diamond-pistol-pack" : "diamond-weapon-pack" }, { "version", 1 }, { "weapon", weapon } };
        QJsonArray catalogSlots;
        if (type == WeaponMechanism::PistolSlide) {
            header["standardReceiver"] = receiverKey;
            for (const auto* mod : configuredObjectMods()) {
                const auto key = poseRecordKey(mod->formId);
                const auto* slot = configuration()->slot(configurationSlotKey(*mod));
                catalogSlots.append(QJsonObject{ { "name", key == receiverKey ? "Receiver" : key == magazineKey ? "Magazine" : slot ? slot->label : key }, { "records", QJsonArray{ key } } });
                if (key == magazineKey && mod->attachPoint) {
                    header["magazineAttachPoint"] = poseRecordKey(mod->attachPoint);
                }
            }
        } else {
            header["mechanism"] = type == WeaponMechanism::CellLever ? "cell-lever" : type == WeaponMechanism::Ar15 ? "ar15" : type == WeaponMechanism::M1A ? "m1a" : type == WeaponMechanism::BoltAction ? "manual-bolt-action" : "reciprocating-bolt";
            header["standardReceiver"] = receiverKey;
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
            if (manualBoltFromReload()) {
                header["manualBoltFromReload"] = true;
            }
            if (magazineBuiltIn()) {
                header["embeddedMagazineReceivers"] = QJsonArray{ receiverKey };
            }
        }
        header["slots"] = catalogSlots;
        return header;
    }
}
