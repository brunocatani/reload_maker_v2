// Saved magazine settings for the open weapon: the straight insertion guide,
// the hand reference and connection, and the full / empty presentation.
// Each has a state for its dialog and MCP, and a save / reset that restarts
// the simulation and saves the project. A setting belongs to the reload
// recipe it was saved against; a changed recipe makes it stale.
#include "ReloadMakerController.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include "areas/loadout/SceneTree.h"
#include "model/nifmodel.h"

#include <QJsonArray>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        [[noreturn]] void fail(const QString& message)
        {
            throw std::runtime_error(message.toStdString());
        }
    }

    bool ReloadMakerController::magazineSettingsBusy() const
    {
        return _loadThread || _reloadCapturing || _reloadCaptureQueued || _rebuildTimer->isActive();
    }

    // The saved guide, else the guide measured from the running rail, else
    // manual starting values.
    QJsonObject ReloadMakerController::magazineGuideState() const
    {
        if (!_reloadRecipe || magazineSettingsBusy()) {
            fail("Wait for the magazine's current source capture");
        }
        const auto saved = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineGuide").toObject();
        if (!_simulationRig && saved.isEmpty()) {
            fail("Choose the operating and magazine roles first");
        }
        QJsonObject settings = saved;
        if (!settings.isEmpty()) {
            MagazineGuide guide;QString error;
            if (!readMagazineGuide(settings, guide, error)) fail(error);
            settings = magazineGuideJson(guide);
        }
        if (settings.isEmpty() && _magazineRail) {
            const auto rail = magazineRailJson(*_magazineRail);
            settings = {
                { "version", 2 },
                { "recipeId", _reloadRecipe->id },
                { "outwardInConnection", rail["outwardInConnection"] },
                { "travel", rail["travel"] },
                { "captureRadius", rail["captureRadius"] },
                { "releaseMargin", rail["releaseMargin"] },
            };
        }
        if (settings.isEmpty()) {
            settings = magazineGuideJson(MagazineGuide{ _reloadRecipe->id, QVector3D(0, 0, -1), 10, 1, .6f });
        }
        return {
            { "settings", settings },
            { "saved", !saved.isEmpty() },
            { "ready", _magazineRail.has_value() },
            { "canConfigure", _simulationRig.has_value() },
            { "rail", _magazineRail ? magazineRailJson(*_magazineRail) : QJsonObject{} },
            { "error", _magazineError },
        };
    }

    QJsonObject ReloadMakerController::configureMagazineGuide(const QJsonObject& request)
    {
        if (!_reloadRecipe || magazineSettingsBusy()) {
            fail("Wait for the magazine's current source capture");
        }
        const auto key = authoringRecordKey();
        auto plan = _authoringRecords.value(key).toObject();
        if (request["operation"] == "reset") {
            plan.remove("magazineGuide");
        } else if (request["operation"] == "save") {
            MagazineGuide guide;
            QString error;
            if (!readMagazineGuide(request["settings"].toObject(), guide, error)) {
                fail(error);
            }
            if (!_simulationRig) {
                fail("Open the magazine in Operate before saving its guide");
            }
            guide.recipeId = _reloadRecipe->id;
            if (_magazineRail) {
                auto checked = *_magazineRail;
                if (!applyMagazineGuide(guide, checked, error)) {
                    fail(error);
                }
            }
            plan["magazineGuide"] = magazineGuideJson(guide);
        } else {
            fail("Choose save or reset for the magazine guide");
        }
        stopSimulation();
        _authoringRecords[key] = plan;
        noteDocumentChange(QStringLiteral("magazine guide"));
        prepareSimulation();
        return magazineGuideState();
    }

    // A hand anchor can be the magazine reference when it is in the magazine
    // carrier's branch (above or below the carrier) and not part of the
    // operating part or bolt.
    bool ReloadMakerController::magazineReferenceCandidate(int index) const
    {
        if (!_simulationRig || !_reloadRecipe || index <= 0 || index >= _handPoseAnchors.size()) {
            return false;
        }
        auto* model = activeModel();
        if (!model) {
            return false;
        }
        const auto trackNode = [this](int track) -> QModelIndex {
            if (track < 0 || track >= _reloadTrackNodes.size()) {
                return {};
            }
            return QModelIndex(_sceneNodes.value(_reloadTrackNodes[track]));
        };
        const auto node = _handPoseAnchors[index].node;
        const auto carrier = trackNode(_simulationRig->magazine);
        if (!node.isValid() || !carrier.isValid()) {
            return false;
        }
        const bool inCarrierBranch = isWithinBlock(*model, node, carrier) || isWithinBlock(*model, carrier, node);
        return inCarrierBranch && !isWithinBlock(*model, trackNode(_simulationRig->slide), node) &&
               !isWithinBlock(*model, trackNode(_simulationRig->bolt), node);
    }

    // The candidate anchors and the saved reference, or one built from the
    // current anchor and the running rail's connection.
    QJsonObject ReloadMakerController::magazineReferenceState() const
    {
        if (!_reloadRecipe || !_simulationRig || magazineSettingsBusy()) {
            fail("Prepare the operating roles before choosing a magazine reference");
        }
        QJsonArray anchors;
        for (int i = 1; i < _handPoseAnchors.size(); ++i) {
            if (!magazineReferenceCandidate(i)) {
                continue;
            }
            const auto& placement = _handPoseAnchors[i].description;
            anchors.append(QJsonObject{
                { "index", i },
                { "key", magazineReferenceKey(placement) },
                { "name", placement.anchorName },
                { "kind", placement.anchorKind },
                { "path", placement.anchorPath },
                { "model", placement.ownerModel },
                { "record", placement.ownerRecord },
            });
        }
        const auto saved = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineReference").toObject();
        MagazineReference initial;
        initial.recipeId = _reloadRecipe->id;
        const int selected = currentMagazineAnchor();
        if (selected >= 0) {
            initial.anchorKey = magazineReferenceKey(_handPoseAnchors[selected].description);
        }
        Transformf carrier;
        Transformf inverse;
        QString error;
        if (_magazineRail && sceneNodeWorld(_sceneNodes[_reloadTrackNodes[_simulationRig->magazine]], carrier, error) &&
            invertUniformTransform(carrier, inverse)) {
            initial.connectionInCarrier = composed(inverse, _magazineRail->connection);
        }
        return {
            { "settings", saved.isEmpty() ? magazineReferenceJson(initial) : saved },
            { "anchors", anchors },
            { "selectedAnchor", selected },
            { "saved", !saved.isEmpty() },
            { "error", _magazineError },
        };
    }

    QJsonObject ReloadMakerController::configureMagazineReference(const QJsonObject& request)
    {
        (void)magazineReferenceState();
        const auto key = authoringRecordKey();
        auto plan = _authoringRecords.value(key).toObject();
        if (request["operation"] == "reset") {
            plan.remove("magazineReference");
        } else if (request["operation"] == "save") {
            MagazineReference reference;
            QString error;
            if (!readMagazineReference(request["settings"].toObject(), reference, error)) {
                fail(error);
            }
            reference.recipeId = _reloadRecipe->id;
            int found = -1;
            for (int i = 1; i < _handPoseAnchors.size(); ++i) {
                if (magazineReferenceKey(_handPoseAnchors[i].description) != reference.anchorKey) {
                    continue;
                }
                if (found >= 0 || !magazineReferenceCandidate(i)) {
                    fail("Choose a unique reference within the magazine assembly, excluding operating parts");
                }
                found = i;
            }
            if (found < 0) {
                fail("The selected magazine reference is missing");
            }
            plan["magazineReference"] = magazineReferenceJson(reference);
        } else {
            fail("Choose save or reset for magazine references");
        }
        stopSimulation();
        _authoringRecords[key] = plan;
        prepareSimulation();
        noteDocumentChange(QStringLiteral("magazine reference"));
        return magazineReferenceState();
    }

    // The magazine pieces with their current roles, the tracks that can be
    // the empty body, and the saved movements authored on the full carrier.
    QJsonObject ReloadMakerController::magazineSetupState() const
    {
        if (!_reloadRecipe || !_simulationRig || magazineSettingsBusy()) {
            fail("Prepare the operating roles and native magazine first");
        }
        const auto saved = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineSetup").toObject();
        if (!_magazineRail && saved.isEmpty()) {
            fail(_magazineError.isEmpty() ? QString("The magazine source is not ready") : _magazineError);
        }
        MagazineSetup value;
        value.recipeId = _reloadRecipe->id;
        value.emptyTrack = _magazineRail ? _magazineRail->emptyTrack : _simulationRig->magazine;
        value.sampledPayload = isM1AConfiguration();
        value.loadedFrame = value.sampledPayload ? _m1aLoadedMagazineFrame : 0;
        value.emptyFrame = value.sampledPayload ? _m1aEmptyMagazineFrame : 0;
        value.entrySeconds = _reloadRecipe->duration * value.loadedFrame / (_reloadRecipe->frames - 1);
        QJsonArray pieces;
        QSet<QString> seen;
        for (const auto& visual : _magazineVisuals) {
            const auto path = magazinePiecePath(visual.node);
            if (seen.contains(path)) {
                fail("Magazine pieces have ambiguous native paths");
            }
            seen.insert(path);
            const auto role = visual.nativeOnly ? "spare" : visual.cartridge ? "cartridge" : "body";
            value.pieces[path] = role;
            pieces.append(QJsonObject{
                { "path", path },
                { "name", activeModel()->get<QString>(visual.node, "Name") },
                { "role", role },
                { "empty", visual.empty },
                { "track", visual.track },
            });
        }
        QJsonArray tracks;
        for (int i = 0; i < _reloadRecipe->tracks.size(); ++i) {
            const auto& track = _reloadRecipe->tracks[i];
            const bool hasBody = std::any_of(_magazineVisuals.cbegin(), _magazineVisuals.cend(), [&](const auto& v) { return v.track == i && !v.cartridge; });
            if (track.surface && hasBody) {
                tracks.append(QJsonObject{ { "index", i }, { "name", track.anchor.anchorName }, { "model", track.anchor.ownerModel } });
            }
        }
        // A saved presentation is shown on this configuration's reload: its
        // track index maps by node, and the node list is recorded again on save.
        QJsonObject settings = magazineSetupJson(value);
        if (!saved.isEmpty()) {
            MagazineSetup current;
            QString error;
            settings = saved;
            if (readMagazineSetup(saved, current, error)) {
                current.emptyTrack = mapRecipeTrack(current.trackAnchors, *_reloadRecipe, current.emptyTrack);
                current.recipeId = _reloadRecipe->id;
                current.trackAnchors = {};
                settings = magazineSetupJson(current);
            }
        }
        const auto& carrier = _reloadRecipe->tracks[_simulationRig->magazine].anchor;
        QJsonArray movements;
        for (const auto& movement : _mechanisms) {
            if (compatibleMechanismPart(movement.anchor, carrier)) {
                movements.append(QJsonObject{ { "id", movement.id }, { "name", movement.name } });
            }
        }
        return {
            { "movements", movements },
            { "settings", settings },
            { "saved", !saved.isEmpty() },
            { "fullTrack", _simulationRig->magazine },
            { "fullName", carrier.anchorName },
            { "tracks", tracks },
            { "pieces", pieces },
            { "frames", _reloadRecipe->frames },
            { "duration", _reloadRecipe->duration },
            { "error", _magazineError },
        };
    }

    // Saves or resets the presentation. A saved presentation that the
    // simulation then rejects is rolled back to the previous plan.
    QJsonObject ReloadMakerController::configureMagazineSetup(const QJsonObject& request)
    {
        if (!_reloadRecipe || !_simulationRig || magazineSettingsBusy()) {
            fail("Wait for the native magazine source");
        }
        const auto key = authoringRecordKey();
        const auto previous = _authoringRecords.value(key);
        auto plan = previous.toObject();
        if (request["operation"] == "reset") {
            plan.remove("magazineSetup");
        } else if (request["operation"] == "save") {
            MagazineSetup value;
            QString error;
            if (!readMagazineSetup(request["settings"].toObject(), value, error)) {
                fail(error);
            }
            value.emptyTrack = mapRecipeTrack(value.trackAnchors, *_reloadRecipe, value.emptyTrack);
            checkMagazineSetup(value);
            value.recipeId = _reloadRecipe->id;
            value.trackAnchors = recipeTrackAnchors(*_reloadRecipe);
            plan["magazineSetup"] = magazineSetupJson(value);
        } else {
            fail("Choose save or reset for magazine presentation");
        }
        stopSimulation();
        _authoringRecords[key] = plan;
        prepareSimulation();
        if (!_magazineError.isEmpty() || !_simulationError.isEmpty()) {
            const auto failure = _magazineError.isEmpty() ? _simulationError : _magazineError;
            stopSimulation();
            if (previous.isUndefined()) {
                _authoringRecords.remove(key);
            } else {
                _authoringRecords[key] = previous;
            }
            prepareSimulation();
            fail(failure);
        }
        noteDocumentChange(QStringLiteral("magazine presentation"));
        return magazineSetupState();
    }

    // Throws unless the presentation fits this recipe and magazine: an empty
    // body from this assembly, separate full and empty branches, a native
    // insertion inside the clip, and a role for exactly the current pieces.
    void ReloadMakerController::checkMagazineSetup(const MagazineSetup& value) const
    {
        const auto& recipe = *_reloadRecipe;
        if (value.emptyTrack < 0 || value.emptyTrack >= recipe.tracks.size() || value.loadedFrame >= recipe.frames || value.emptyFrame >= recipe.frames) {
            fail("Choose an empty body and samples of this configuration's reload");
        }
        const auto state = magazineSetupState();
        bool body = false;
        for (const auto& row : state["tracks"].toArray()) {
            body |= row.toObject()["index"].toInt() == value.emptyTrack;
        }
        if (!body) {
            fail("Choose an empty body from this magazine assembly");
        }
        const auto descendant = [&](int child, int ancestor) {
            for (int depth = 0; child >= 0 && depth < recipe.tracks.size(); child = recipe.tracks[child].parent, ++depth) {
                if (child == ancestor) {
                    return true;
                }
            }
            return false;
        };
        const int full = _simulationRig->magazine;
        if (value.emptyTrack != full && (descendant(value.emptyTrack, full) || descendant(full, value.emptyTrack))) {
            fail("Separate full and empty carriers must be independent branches; choose the full carrier for a shared body");
        }
        if (value.motion == "native" && (value.entrySeconds <= 0 || value.entrySeconds >= recipe.duration)) {
            fail("Native insertion must begin inside the reload clip");
        }
        const auto pieces = state["pieces"].toArray();
        if (pieces.size() != value.pieces.size()) {
            fail("Review all native magazine pieces");
        }
        for (const auto& row : pieces) {
            if (!value.pieces.contains(row.toObject()["path"].toString())) {
                fail("A saved piece is not in this source");
            }
        }
    }
}
