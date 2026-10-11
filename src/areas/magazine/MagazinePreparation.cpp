// Preparing the magazine interaction for the running simulation: the seated
// connection, the insertion rail built from the magazine and weapon
// geometry, the magazine's visible movement, and which magazine shapes are
// body, cartridge or animation-only spare. Any failing step sets
// _magazineError and leaves _magazineRail empty.
#include "ReloadMakerController.h"
#include "areas/properties/PropertyInputs.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include "StudioWindow.h"
#include "areas/loadout/SceneTree.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace reloadmaker
{
    void ReloadMakerController::prepareMagazineInteraction()
    {
        _magazineRail.reset();
        _magazineVisuals.clear();
        _magazineError.clear();
        _magazineNote.clear();
        _magazineSetup.reset();
        _m1aLoadedMagazineFrame = 0;
        _m1aEmptyMagazineFrame = 0;
        if (!_simulationRig || !_reloadRecipe) {
            return;
        }
        MagazineConnection connection;
        if (!loadMagazineSetup() || !resolveMagazineConnection(connection)) {
            return;
        }
        QVector<QVector3D> body;
        QVector<QVector3D> frame;
        QVector<MagazineShape> shapes;
        if (!collectMagazineGeometry(body, frame, shapes)) {
            return;
        }
        const auto prepareContact=[&](MagazineRail& rail){
            QVector3D low=body.front(),high=low;
            for(const auto& p:body)for(int i=0;i<3;++i){low[i]=std::min(low[i],p[i]);high[i]=std::max(high[i],p[i]);}
            const auto middle=(low+high)*.5f;
            const auto center=composed(rail.sourceToSeat,Transformf{{middle.x(),middle.y(),middle.z()}}).translation;
            rail.contactBodyCenter={center.x,center.y,center.z};rail.contactBodyRadius=(high-low).length()*.5f;
            rail.contactAssistRadius=std::max(2.5f,rail.captureRadius*3.f);
            // Prepare a bounded surface proxy once; no mesh traversal while dragging.
            const int stride=std::max(1,int((frame.size()+511)/512));
            for(int i=0;i<frame.size();i+=stride)rail.contactPoints.append(frame[i]);
        };
        if(unitLoadingConfiguration()) {
            const auto& recipe=*_reloadRecipe;const int track=_simulationRig->magazine;
            MagazineRail rail;rail.fullTrack=rail.emptyTrack=track;
            bool empty=false,full=false;classifyMagazineShapes(shapes,rail,empty,full);
            for(auto& visual:_magazineVisuals)if(!visual.nativeOnly){visual.cartridge=false;visual.empty=false;}
            if(!_magazineSetup||_magazineSetup->seatSeconds<=_magazineSetup->entrySeconds||_magazineSetup->seatSeconds>recipe.duration){
                _magazineError="Choose the bottom-loading entry and seated source times for the shell.";return;
            }
            const float start=_magazineSetup->entrySeconds,end=_magazineSetup->seatSeconds;
            const auto seat=reloadTrackPose(recipe,track,end),entry=reloadTrackPose(recipe,track,start);
            Transformf inverseSeat,inverseRest;
            if(!invertUniformTransform(seat,inverseSeat)||!invertUniformTransform(recipe.tracks[track].poses.front(),inverseRest)){
                _magazineError="Select a visible shell at the tube opening.";return;
            }
            rail.sourceToSeat=composed(seat,inverseRest);rail.connection=seat;rail.connectionName="Tube loading port";
            rail.handle={seat.translation.x,seat.translation.y,seat.translation.z};
            const QVector3D delta(entry.translation.x-seat.translation.x,entry.translation.y-seat.translation.y,entry.translation.z-seat.translation.z);
            rail.travel=delta.length();
            if(rail.travel<.1f){_magazineError="The selected shell-loading stroke has no measurable travel.";return;}
            rail.outward=delta.normalized();
            QVector3D low=body.front(),high=low;
            for(const auto& p:body)for(int axis=0;axis<3;++axis){low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);}
            rail.captureRadius=std::max(.35f,(high-low).length()*.18f);rail.releaseMargin=rail.captureRadius*.6f;
            rail.motion={Transformf{}};rail.motionDistances={0};rail.motionSeconds={end};float distance=0;
            for(int sample=1;sample<=64;++sample){
                const auto pose=composed(reloadTrackPose(recipe,track,end-(end-start)*sample/64.f),inverseSeat);
                distance+=reloadPoseDistance(rail.motion.back(),pose);rail.motion.append(pose);rail.motionDistances.append(distance);rail.motionSeconds.append(end-(end-start)*sample/64.f);
            }
            if(!applyMagazinePieceRoles(rail))return;
            const auto guideJson=authoringPlan().value("magazineGuide").toObject();
            if(!guideJson.isEmpty()){MagazineGuide guide;if(!readMagazineGuide(guideJson,guide,_magazineError)||!applyMagazineGuide(guide,rail,_magazineError,false))return;}
            prepareContact(rail);_magazineRail=rail;return;
        }
        const auto guideJson = authoringPlan().value("magazineGuide").toObject();
        MagazineGuide guide;
        QVector3D authoredOutward;
        if (!guideJson.isEmpty()) {
            if (!readMagazineGuide(guideJson, guide, _magazineError)) {
                return;
            }
            const auto outward = guide.outwardInConnection;
            const auto axis = rotated(connection.frame.rotation, { outward.x(), outward.y(), outward.z() });
            authoredOutward = QVector3D(axis.x, axis.y, axis.z).normalized();
        }
        const auto& rig = *_simulationRig;
        // A magazine that follows its clip's curve may rock or turn into the
        // well; the rail measures from the seated end of the reload and may
        // fall back to its final approach.
        const bool finalApproach = magazineInsertionPath() == InsertionPath::NativeCurve;
        MagazineRail rail;
        if (!buildMagazineRail(*_reloadRecipe, rig.magazine, connection.name, connection.frame, body, frame, rail, _magazineError, finalApproach,
                               _magazineSetup ? _magazineSetup->emptyTrack : -1, guideJson.isEmpty() ? nullptr : &authoredOutward)) {
            return;
        }
        if (connection.authored) {
            rail.connection = connection.frame;
            rail.handle = { connection.frame.translation.x, connection.frame.translation.y, connection.frame.translation.z };
        }
        bool emptyBody = false;
        bool fullBody = false;
        classifyMagazineShapes(shapes, rail, emptyBody, fullBody);
        applyMagazineRounds(rail);
        if (_magazineSetup && !applyMagazinePieceRoles(rail)) {
            return;
        }
        if ((!emptyBody && !rail.sharedBody()) || !fullBody) {
            _magazineVisuals.clear();
            _magazineError = "Magazine body geometry could not be resolved.";
            return;
        }
        // Keep the discovered bodies editable even when automatic motion
        // detection needs an author-selected source interval.
        if (!buildMagazineMotion(body, rail)) return;
        if (!guideJson.isEmpty() && !applyMagazineGuide(guide, rail, _magazineError, _magazineSetup && _magazineSetup->motion == "linear")) {
            return;
        }
        prepareContact(rail);_magazineRail = rail;
    }

    // The saved magazine presentation, when there is one and it still matches
    // this recipe.
    bool ReloadMakerController::loadMagazineSetup()
    {
        const auto setupJson = authoringPlan().value("magazineSetup").toObject();
        if (setupJson.isEmpty()) {
            return true;
        }
        MagazineSetup setup;
        if (!readMagazineSetup(setupJson, setup, _magazineError)) {
            return false;
        }
        // The empty body's track maps onto this configuration's recipe by node.
        setup.emptyTrack = mapRecipeTrack(setup.trackAnchors, *_reloadRecipe, setup.emptyTrack);
        if (setup.emptyTrack < 0 || setup.loadedFrame >= _reloadRecipe->frames || setup.emptyFrame >= _reloadRecipe->frames) {
            // Authored for another reload of this magazine: not a gate
            // (owner, 2026-10-08); the native presentation is used.
            _magazineNote = "The saved magazine presentation does not fit this reload; the native presentation is used.";
            return true;
        }
        QVector<int> followers;for(int track:setup.loadingFollowers){const int mapped=mapRecipeTrack(setup.trackAnchors,*_reloadRecipe,track);if(mapped>=0)followers.append(mapped);}setup.loadingFollowers=followers;
        setup.trackAnchors = recipeTrackAnchors(*_reloadRecipe);
        _magazineSetup = setup;
        return true;
    }

    // Where the seated magazine connects, in world space: an authored frame in
    // the magazine carrier, the carrier itself for a magazine branch inside a
    // shared receiver or a magazine built into the receiver, or else the
    // magazine model's connect point on its attachment parent.
    bool ReloadMakerController::resolveMagazineConnection(MagazineConnection& connection)
    {
        const auto& rig = *_simulationRig;
        const auto anchor = simulationMagazineAnchor();
        const auto referenceJson = authoringPlan().value("magazineReference").toObject();
        MagazineReference reference;
        if (!referenceJson.isEmpty() &&
            (!readMagazineReference(referenceJson, reference, _magazineError) || currentMagazineAnchor() < 0)) {
            // A reference whose node this source no longer has is ignored
            // (owner, 2026-10-08: no review gates); the source connection is used.
            _magazineNote = "The saved magazine reference names a part this source does not have; the source connection is used.";
            _magazineError.clear();
            reference = {};
        }
        const auto carrierNode = _sceneNodes[_reloadTrackNodes[rig.magazine]];
        if (!referenceJson.isEmpty() && reference.connectionMode != "source") {
            Transformf carrier;
            if (!sceneNodeWorld(carrierNode, carrier, _magazineError)) {
                return false;
            }
            connection.name = "Authored magazine connection";
            connection.frame = composed(carrier, reference.connectionMode == "custom" ? reference.connectionInCarrier : Transformf{});
            connection.authored = true;
            return true;
        }
        if (rig.magazineBranchOnly || magazineBuiltIn()) {
            connection.name = rig.magazineBranchOnly ? "Magazine carrier" : "P-Mag";
            return sceneNodeWorld(carrierNode, connection.frame, _magazineError);
        }
        auto source = sourceNif(_packageResources, anchor.ownerModel, _magazineError, true);
        if (!source) {
            return false;
        }
        ConnectTarget target;
        for (const auto& name : childConnectPoints(*source)) {
            target = findConnectTarget(name, nullptr, _simulationMagazineSlot);
            if (target.valid()) {
                connection.name = "P-" + name.mid(2);
                break;
            }
        }
        Transformf parent;
        if (!target.valid() || !sceneNodeWorld(target.parent, parent, _magazineError)) {
            _magazineError = "Could not resolve the magazine's seated connection.";
            return false;
        }
        const Vec3f scale{ target.scale, target.scale, target.scale };
        connection.frame = composed(parent, Transformf{ target.translation, target.rotation, scale });
        return true;
    }

    // World-space vertices of the magazine body (shapes under the magazine
    // carrier track) and of the surrounding weapon frame, for the rail's
    // clearance. Every shape under a hidden magazine root is recorded with
    // its deepest owning magazine track for classification; one on another
    // track is the magazine model's parked spare. Outside the magazine, a
    // copy of a body shape (same material and vertex count) on a track the
    // reload moves is a parked spare too (rework 14, item 6).
    bool ReloadMakerController::collectMagazineGeometry(QVector<QVector3D>& body, QVector<QVector3D>& frame, QVector<MagazineShape>& shapes)
    {
        auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        const auto& rig = *_simulationRig;
        const auto trackNode = [this](int track) { return _sceneNodes[_reloadTrackNodes[track]]; };
        const auto deepestTrack = [&](const QModelIndex& index, const QVector<int>& among) {
            int owner = -1;
            for (const int track : among) {
                if (isWithinBlock(*model, index, trackNode(track)) && (owner < 0 || isWithinBlock(*model, trackNode(track), trackNode(owner)))) {
                    owner = track;
                }
            }
            return owner;
        };
        QVector<int> allTracks(_reloadRecipe->tracks.size());
        std::iota(allTracks.begin(), allTracks.end(), 0);
        const auto moves = [&](int track) {
            if (track < 0) {
                return false;
            }
            const auto& poses = _reloadRecipe->tracks[track].poses;
            return std::any_of(poses.cbegin(), poses.cend(), [&](const Transformf& pose) {
                return reloadPoseDistance(pose, poses.front()) > .01f || std::abs(pose.scale.x - poses.front().scale.x) > 1e-4f;
            });
        };
        const auto inMagazine = [&](const QModelIndex& index) {
            return std::any_of(_simulationVisibility.cbegin(), _simulationVisibility.cend(),
                               [&](const auto& entry) { return isWithinBlock(*model, index, entry.first); });
        };
        const auto signature = [&](const Shape* shape, const QModelIndex& index) {
            const auto shader = model->getBlockIndex(model->getLink(index, "Shader Property"));
            return qMakePair(model->get<QString>(shader, "Name").toLower(), int(shape->verts.size()));
        };
        QSet<QPair<QString, int>> bodySignatures;
        for (auto* shape : scene->shapes) {
            if (shape) {
                const auto index = model->getBlockIndex(shape->id());
                if (inMagazine(index) && isWithinBlock(*model, index, trackNode(rig.magazine))) {
                    bodySignatures.insert(signature(shape, index));
                }
            }
        }
        for (auto* shape : scene->shapes) {
            if (!shape) {
                continue;
            }
            const auto index = model->getBlockIndex(shape->id());
            const bool magazine = inMagazine(index);
            int owner = -1;
            if (magazine) {
                owner = deepestTrack(index, rig.magazineTracks);
                if (owner < 0) {
                    owner = deepestTrack(index, allTracks);
                }
            } else if (bodySignatures.contains(signature(shape, index)) && moves(deepestTrack(index, allTracks))) {
                _magazineVisuals.append({ index, model->get<int>(index, "Flags"), false, -1, false, true, true });
                continue;
            }
            if (magazine && owner >= 0) {
                shapes.append({ index, owner });
            }
            bool weapon = isWithinBlock(*model, index, _weaponRoot);
            if (!weapon) {
                for (const auto& root : _partRoots) {
                    if (isWithinBlock(*model, index, root)) {
                        weapon = true;
                        break;
                    }
                }
            }
            // Static mesh tracks and payload bones can sit below the carrier.
            // Their deepest track is for animation, not assembly membership.
            const bool inFullCarrier = magazine && isWithinBlock(*model, index, trackNode(rig.magazine));
            if (!weapon || (!magazine && shape->isHidden()) || (magazine && !inFullCarrier)) {
                continue;
            }
            // Zero scale is also used to hide optional receiver/sight pieces.
            // A collapsed accessory has no clearance surface and must not
            // invalidate the physical magazine's insertion guide.
            if(!magazine&&std::isfinite(shape->worldTrans().scale)&&std::abs(shape->worldTrans().scale)<1e-6f)continue;
            if (!shape->reloadMakerSkinBones().isEmpty()) {
                _magazineError = "Magazine rail geometry needs a rigid source surface.";
                return false;
            }
            Transformf world;
            if (!sceneNodeWorld(index, world, _magazineError)) {
                return false;
            }
            auto& output = magazine ? body : frame;
            for (const auto& vertex : shape->verts) {
                const auto point = composed(world, Transformf{ { vertex[0], vertex[1], vertex[2] } }).translation;
                output.append({ point.x, point.y, point.z });
            }
        }
        return true;
    }

    // The magazine's visible movement along the rail: the clip's insertion
    // curve when the weapon follows it, or the saved setup's native or manual
    // path. The setup also chooses the loaded and empty payload frames.
    bool ReloadMakerController::buildMagazineMotion(const QVector<QVector3D>& body, MagazineRail& rail)
    {
        const bool presetMotion = !_magazineSetup || _magazineSetup->motion == "preset";
        if (presetMotion && magazineInsertionPath() == InsertionPath::NativeCurve && !buildNativeMagazineMotion(body, rail)) {
            return false;
        }
        if (!_magazineSetup) {
            return true;
        }
        _m1aLoadedMagazineFrame = _magazineSetup->loadedFrame;
        _m1aEmptyMagazineFrame = _magazineSetup->emptyFrame;
        if (_magazineSetup->motion == "native" && !buildMagazineAnimation(*_reloadRecipe, _magazineSetup->entrySeconds, body, rail, _magazineError)) {
            return false;
        }
        if (_magazineSetup->motion != "manual") {
            return true;
        }
        const auto movement = std::find_if(_mechanisms.cbegin(), _mechanisms.cend(), [&](const auto& m) { return m.id == _magazineSetup->movementId; });
        if (movement == _mechanisms.cend()) {
            _magazineError = "The saved magazine movement is missing";
            return false;
        }
        auto* model = activeModel();
        const auto carrier = _sceneNodes[_reloadTrackNodes[_simulationRig->magazine]];
        for (const auto& follower : movement->followers) {
            const auto found = std::find_if(_handPoseAnchors.cbegin(), _handPoseAnchors.cend(), [&](const auto& a) {
                return sameMechanismPart(a.description, follower.anchor);
            });
            if (found == _handPoseAnchors.cend() || !isWithinBlock(*model, found->node, carrier)) {
                _magazineError = "Magazine movement followers must belong to its physical carrier";
                return false;
            }
        }
        return buildManualMagazineAnimation(*_reloadRecipe, *movement, body, rail, _magazineError);
    }

    // Insertion along the native reload from the clip's magazine entry event;
    // the removal event marks the empty payload frame.
    bool ReloadMakerController::buildNativeMagazineMotion(const QVector<QVector3D>& body, MagazineRail& rail)
    {
        const auto events = detectMagazineEvents(clipEvents(_reloadClip));
        const float entry = events.entrySeconds;
        const float empty = events.removalSeconds;
        if (empty < 0 && sampledMagazinePayload()) {
            _magazineError = "Native empty-magazine frame is unavailable.";
            return false;
        }
        const auto frameAt = [this](float seconds) {
            return std::clamp(qRound(seconds / _reloadRecipe->duration * (_reloadRecipe->frames - 1)), 0, _reloadRecipe->frames - 1);
        };
        // A shared cell body has no visible cartridge payload and needs no
        // separate empty animation event. Sampled cartridge feeds still do.
        _m1aEmptyMagazineFrame = empty < 0 ? 0 : frameAt(empty);
        _m1aLoadedMagazineFrame = frameAt(entry);
        return buildMagazineAnimation(*_reloadRecipe, entry, body, rail, _magazineError);
    }

    // Turns the recorded magazine shapes into visuals: whether each belongs
    // to the empty body and whether it is a cartridge. Reports whether an
    // empty body and the full carrier's body were found.
    void ReloadMakerController::classifyMagazineShapes(const QVector<MagazineShape>& shapes, const MagazineRail& rail, bool& emptyBody, bool& fullBody)
    {
        auto* model = activeModel();
        const auto& rig = *_simulationRig;
        for (const auto& shape : shapes) {
            const bool isEmpty = !rail.sharedBody() && isWithinBlock(*model, shape.node, _sceneNodes[_reloadTrackNodes[rail.emptyTrack]]);
            if (isEmpty) {
                emptyBody = true;
            }
            const bool inFull = isWithinBlock(*model, shape.node, _sceneNodes[_reloadTrackNodes[rig.magazine]]);
            if (inFull) {
                fullBody = true;
            }
            const bool parked = !inFull && !isEmpty;
            _magazineVisuals.append({ shape.node, model->get<int>(shape.node, "Flags"), isEmpty, shape.track, magazinePieceProposal(shape.node).cartridge,
                                      parked, parked });
        }
    }

    // The saved setup's role for every magazine piece (body, cartridge or
    // spare). Every piece needs exactly one saved role; pieces outside the
    // full and empty carriers must be spares; both presentations need a body.
    bool ReloadMakerController::applyMagazinePieceRoles(const MagazineRail& rail)
    {
        auto* model = activeModel();
        const auto inCarrier = [&](const QModelIndex& node) {
            return isWithinBlock(*model, node, _sceneNodes[_reloadTrackNodes[rail.fullTrack]]) ||
                   isWithinBlock(*model, node, _sceneNodes[_reloadTrackNodes[rail.emptyTrack]]);
        };
        // Saved roles apply to the pieces that still exist; a new piece keeps
        // its native classification and a missing one is ignored (owner,
        // 2026-10-08: no review gates). Pieces outside the carriers are spares.
        QSet<QString> seen;
        for (auto& visual : _magazineVisuals) {
            const auto path = magazinePiecePath(visual.node);
            const auto role = path.isEmpty() || seen.contains(path) ? QString() : _magazineSetup->pieces.value(path);
            seen.insert(path);
            if (role.isEmpty()) {
                if (!inCarrier(visual.node)) {
                    visual.nativeOnly = true;
                }
                continue;
            }
            visual.cartridge = role == "cartridge";
            visual.nativeOnly = role == "spare" || !inCarrier(visual.node);
        }
        for (const bool emptyState : { false, true }) {
            const bool hasBody = std::any_of(_magazineVisuals.begin(), _magazineVisuals.end(), [&](const auto& v) {
                return !v.cartridge && !v.nativeOnly && (rail.sharedBody() || v.empty == emptyState);
            });
            if (!hasBody) {
                _magazineError = "Full and empty presentations each need a body.";
                return false;
            }
        }
        return true;
    }
}
