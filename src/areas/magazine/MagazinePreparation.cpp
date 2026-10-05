// Preparing the magazine interaction for the running simulation: the seated
// connection, the insertion rail built from the magazine and weapon
// geometry, the magazine's visible movement, and which magazine shapes are
// body, cartridge or animation-only spare. Any failing step sets
// _magazineError and leaves _magazineRail empty.
#include "ReloadMakerController.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include "StudioWindow.h"
#include "areas/loadout/SceneTree.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"

#include <QSet>
#include <algorithm>

namespace reloadmaker
{
    void ReloadMakerController::prepareMagazineInteraction()
    {
        _magazineRail.reset();
        _magazineVisuals.clear();
        _magazineError.clear();
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
        const auto guideJson = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineGuide").toObject();
        MagazineGuide guide;
        QVector3D authoredOutward;
        if (!guideJson.isEmpty()) {
            if (!readMagazineGuide(guideJson, guide, _magazineError)) {
                return;
            }
            if (guide.recipeId != _reloadRecipe->id) {
                _magazineError = "Magazine guide source changed. Review or reset its configuration.";
                return;
            }
            const auto outward = guide.outwardInConnection;
            const auto axis = rotated(connection.frame.rotation, { outward.x(), outward.y(), outward.z() });
            authoredOutward = QVector3D(axis.x, axis.y, axis.z).normalized();
        }
        const auto& rig = *_simulationRig;
        // These magazines rock or rotate into the well; the rail measures from
        // the seated end of the reload and may fall back to its final approach.
        const bool finalApproach = isM1AConfiguration() || isBoltActionConfiguration() || isThanatosConfiguration();
        MagazineRail rail;
        if (!buildMagazineRail(*_reloadRecipe, rig.magazine, connection.name, connection.frame, body, frame, rail, _magazineError, finalApproach,
                               _magazineSetup ? _magazineSetup->emptyTrack : -1, guideJson.isEmpty() ? nullptr : &authoredOutward)) {
            return;
        }
        if (!buildMagazineMotion(body, rail)) {
            return;
        }
        bool emptyBody = false;
        bool fullBody = false;
        classifyMagazineShapes(shapes, rail, emptyBody, fullBody);
        if (_magazineSetup && !applyMagazinePieceRoles(rail)) {
            return;
        }
        if ((!emptyBody && !rail.sharedBody()) || !fullBody) {
            _magazineVisuals.clear();
            _magazineError = "Magazine body geometry could not be resolved.";
            return;
        }
        if (!guideJson.isEmpty() && !applyMagazineGuide(guide, rail, _magazineError)) {
            return;
        }
        if (connection.authored) {
            rail.handle = { rail.connection.translation.x, rail.connection.translation.y, rail.connection.translation.z };
        }
        _magazineRail = rail;
    }

    // The saved magazine presentation, when there is one and it still matches
    // this recipe.
    bool ReloadMakerController::loadMagazineSetup()
    {
        const auto setupJson = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineSetup").toObject();
        if (setupJson.isEmpty()) {
            return true;
        }
        MagazineSetup setup;
        if (!readMagazineSetup(setupJson, setup, _magazineError)) {
            return false;
        }
        if (setup.recipeId != _reloadRecipe->id || setup.emptyTrack >= _reloadRecipe->tracks.size() ||
            setup.loadedFrame >= _reloadRecipe->frames || setup.emptyFrame >= _reloadRecipe->frames) {
            _magazineError = "Magazine source changed. Review or reset its presentation.";
            return false;
        }
        _magazineSetup = setup;
        return true;
    }

    // Where the seated magazine connects, in world space: an authored frame in
    // the magazine carrier, the carrier itself for a magazine branch inside a
    // shared receiver (or an embedded SREP magazine), or else the magazine
    // model's connect point on its attachment parent.
    bool ReloadMakerController::resolveMagazineConnection(MagazineConnection& connection)
    {
        const auto& rig = *_simulationRig;
        const auto anchor = simulationMagazineAnchor();
        const auto referenceJson = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineReference").toObject();
        MagazineReference reference;
        if (!referenceJson.isEmpty() &&
            (!readMagazineReference(referenceJson, reference, _magazineError) || reference.recipeId != _reloadRecipe->id || currentMagazineAnchor() < 0)) {
            if (_magazineError.isEmpty()) {
                _magazineError = "Magazine reference changed. Review its current carrier and hand reference.";
            }
            return false;
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
        if (rig.magazineBranchOnly || isSrepEmbeddedMagazine()) {
            connection.name = rig.magazineBranchOnly ? "Magazine carrier" : "P-Mag";
            return sceneNodeWorld(carrierNode, connection.frame, _magazineError);
        }
        auto source = sourceNif(_packageResources, anchor.ownerModel, _magazineError);
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
    // its deepest owning magazine track for classification.
    bool ReloadMakerController::collectMagazineGeometry(QVector<QVector3D>& body, QVector<QVector3D>& frame, QVector<MagazineShape>& shapes)
    {
        auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        const auto& rig = *_simulationRig;
        const auto trackNode = [this](int track) { return _sceneNodes[_reloadTrackNodes[track]]; };
        for (auto* shape : scene->shapes) {
            if (!shape) {
                continue;
            }
            const auto index = model->getBlockIndex(shape->id());
            if (isSrepEmbeddedMagazine() && isSrepSpareMagazineShape(index)) {
                _magazineVisuals.append({ index, model->get<int>(index, "Flags"), false, -1, false, true });
                continue;
            }
            bool magazine = false;
            for (const auto& [root, flags] : _simulationVisibility) {
                if (isWithinBlock(*model, index, root)) {
                    magazine = true;
                    break;
                }
            }
            int owner = -1;
            if (magazine) {
                for (const int track : rig.magazineTracks) {
                    if (isWithinBlock(*model, index, trackNode(track)) && (owner < 0 || isWithinBlock(*model, trackNode(track), trackNode(owner)))) {
                        owner = track;
                    }
                }
            }
            if (magazine && owner < 0 && isMk18Configuration()) {
                for (int track = 0; track < _reloadRecipe->tracks.size(); ++track) {
                    if (_reloadRecipe->tracks[track].anchor.anchorName == "WeaponExtra3" && isWithinBlock(*model, index, trackNode(track))) {
                        owner = track;
                        break;
                    }
                }
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
            if (!weapon || (!magazine && shape->isHidden()) || (magazine && owner != rig.magazine)) {
                continue;
            }
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

    // The SREP's spare magazine sits under MagHelper and only animates.
    bool ReloadMakerController::isSrepSpareMagazineShape(QModelIndex node) const
    {
        auto* model = activeModel();
        for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
            if (model->get<QString>(node, "Name") == "MagHelper") {
                return true;
            }
            node = model->getBlockIndex(model->getParent(node));
        }
        return false;
    }

    // The magazine's visible movement along the rail: the native insertion of
    // the testbeds that use one, or the saved setup's native or manual path.
    // The setup also chooses the loaded and empty payload frames.
    bool ReloadMakerController::buildMagazineMotion(const QVector<QVector3D>& body, MagazineRail& rail)
    {
        const bool presetMotion = !_magazineSetup || _magazineSetup->motion == "preset";
        const bool nativeInsertion = isM1AConfiguration() || isBoltActionConfiguration() || isThanatosConfiguration();
        if (presetMotion && !currentTestbed().isEmpty() && nativeInsertion && !buildTestbedMagazineMotion(body, rail)) {
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

    // The M1A, bolt-action and Thanatos testbeds insert along their native
    // reload, from the clip's magazine-in sound event; their magazine-out
    // event marks the empty payload frame.
    bool ReloadMakerController::buildTestbedMagazineMotion(const QVector<QVector3D>& body, MagazineRail& rail)
    {
        QString magazineIn = "WPNM1AMagIn";
        QString magazineOut = "WPNM1AMagOut";
        if (isL118Configuration()) {
            magazineIn = "L115A3_ReloadMagIn";
            magazineOut = "L115A3_MagOut";
        } else if (isBoltActionConfiguration()) {
            magazineIn = "C14_MagInserting";
            magazineOut = "C14_MagOut";
        } else if (isThanatosConfiguration()) {
            magazineIn = "WPNM107Magin";
            magazineOut = "WPNM107Magout";
        }
        float entry = -1;
        float empty = -1;
        for (const auto& event : _reloadClip.annotations) {
            if (event.text.endsWith(magazineIn, Qt::CaseInsensitive)) {
                entry = event.timeSeconds;
            }
            if (event.text.endsWith(magazineOut, Qt::CaseInsensitive)) {
                empty = event.timeSeconds;
            }
        }
        if (empty < 0) {
            _magazineError = "Native empty-magazine frame is unavailable.";
            return false;
        }
        const auto frameAt = [this](float seconds) {
            return std::clamp(qRound(seconds / _reloadRecipe->duration * (_reloadRecipe->frames - 1)), 0, _reloadRecipe->frames - 1);
        };
        _m1aEmptyMagazineFrame = frameAt(empty);
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
            const bool isEmpty = !rail.sharedBody() && shape.track == rail.emptyTrack;
            if (isEmpty) {
                emptyBody = true;
            }
            if (shape.track == rig.magazine) {
                fullBody = true;
            }
            _magazineVisuals.append({ shape.node, model->get<int>(shape.node, "Flags"), isEmpty, shape.track, isMagazineCartridge(shape, isEmpty),
                                      isMk18Configuration() && !rig.magazineTracks.contains(shape.track) });
        }
    }

    // Cartridge shapes hide when the magazine is empty. Rifles name their
    // rounds; pistols treat every non-body magazine shape as a round. MK18
    // ammo is recognized by material, SREP casings and bullets by name, and
    // bolt-action round stacks by their MagHelperNode ancestor.
    bool ReloadMakerController::isMagazineCartridge(const MagazineShape& shape, bool isEmpty) const
    {
        auto* model = activeModel();
        const auto name = model->get<QString>(shape.node, "Name");
        bool cartridge = false;
        if (isRifleConfiguration()) {
            cartridge = name.startsWith(isM1AConfiguration() ? "Bullet:" : "MagBullet:", Qt::CaseInsensitive);
        } else {
            cartridge = !isEmpty && shape.track != _simulationRig->magazine;
        }
        if (isMk18Configuration()) {
            const auto shader = model->getBlockIndex(model->getLink(shape.node, "Shader Property"));
            auto material = model->get<QString>(shader, "Name");
            material.replace('\\', '/');
            cartridge |= material.contains("/MK18/Ammo/", Qt::CaseInsensitive);
        }
        if (isSrepConfiguration()) {
            cartridge |= name.startsWith("Casing", Qt::CaseInsensitive) || name.startsWith("Bullet", Qt::CaseInsensitive);
        }
        if (isBoltActionConfiguration()) {
            // The source culls this static round stack and Child1's top round
            // on an empty reload. Magazine body/follower helpers are separate.
            auto node = QModelIndex(shape.node);
            for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
                if (model->get<QString>(node, "Name").compare("MagHelperNode", Qt::CaseInsensitive) == 0) {
                    cartridge = true;
                    break;
                }
                node = model->getBlockIndex(model->getParent(node));
            }
        }
        return cartridge;
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
        QSet<QString> seen;
        for (auto& visual : _magazineVisuals) {
            const auto path = magazinePiecePath(visual.node);
            const auto role = _magazineSetup->pieces.value(path);
            if (path.isEmpty() || seen.contains(path) || role.isEmpty()) {
                _magazineError = "Native magazine pieces changed. Review every body, cartridge and spare.";
                return false;
            }
            if (role != "spare" && !inCarrier(visual.node)) {
                _magazineError = "Pieces outside the selected full/empty carriers must be marked as animation-only spares.";
                return false;
            }
            seen.insert(path);
            visual.cartridge = role == "cartridge";
            visual.nativeOnly = role == "spare";
        }
        if (seen.size() != _magazineSetup->pieces.size()) {
            _magazineError = "A saved magazine piece is absent from this source.";
            return false;
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
