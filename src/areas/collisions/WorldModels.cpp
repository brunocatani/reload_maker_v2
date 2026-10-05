// World models: the magazine's full and empty world objects captured in
// their carrier frame, the prepared world model of a part, and the cook job
// that adds the collision to a prepared model.
#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "areas/collisions/CollisionFrames.h"
#include "commands/CommandSupport.h"
#include "gl/glshape.h"
#include "glview.h"
#include "model/nifmodel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker
{
    using commands::require;

    // The magazine as Operate presents it on the rail at full travel, full or
    // empty, captured in its carrier track's frame. The simulation is
    // restored afterwards; the capture becomes the collision workshop's
    // magazine world object, generated with ROCK's defaults.
    QJsonObject ReloadMakerController::captureMagazineWorld(bool empty)
    {
        require(_workspace == OperateWorkspace && _simulationActive && _simulationRig && _reloadRecipe && _magazineRail && currentWeapon(),
                "Prepare the selected weapon in Operate first");
        require(_simulation.action == SimulationAction::Idle && !_packProcess, "Finish the current interaction or asset job first");
        require(_simulationError.isEmpty() && _magazineError.isEmpty(), "Resolve the current magazine simulation error first");
        const auto anchor = simulationMagazineAnchor();
        const auto original = _simulation;
        CollisionProfile captured;
        auto presentation = original;
        {
            // Use the very same payload state as Operate, including M14's pre-feed
            // follower positions and separately authored empty magazine body.
            const auto restore = qScopeGuard([&] {
                _simulation = original;
                (void)applyPose(0);
                applySimulationVisibility();
            });
            _simulation.magazine.phase = MagazinePhase::Rail;
            _simulation.magazine.distance = _magazineRail->travel;
            _simulation.magazine.offset = magazineInputOffset(*_magazineRail, _magazineRail->travel);
            _simulation.magazineRounds = empty ? 0 : _simulationAmmo.capacity;
            _simulation.chambered = false;
            presentation = _simulation;
            require(applyPose(0), _lastError);
            applySimulationVisibility();
            require(!_window->getGLView()->studioCaptureFrame().isNull(), "Wait for the native scene before capturing the magazine");
            const QPersistentModelIndex carrierNode = _sceneNodes[_reloadTrackNodes[empty ? _magazineRail->emptyTrack : _magazineRail->fullTrack]];
            Transformf carrier;
            QString error;
            require(sceneNodeWorld(carrierNode, carrier, error), error);
            captured = magazineWorldIdentity(empty, anchor, frameMatrix(carrier));
            collectMagazineWorldSources(captured, empty, carrierNode, anchor);
            require(!captured.sources.isEmpty(), "No visible magazine body found for this presentation");
        }
        _magazineWorldSnapshot = std::move(captured);
        _simulation = presentation;
        _magazineWorldPreview = true;
        setWorkspace(CollisionsWorkspace);
        refreshCollisions();
        const auto result = _collisionWorkshop.command({ { "operation", "defaults" }, { "target", _collisionTargets.size() - 1 } });
        setStatus(QString("%1 magazine captured in its carrier frame. Review world pieces and collision, then capture and cook the model.").arg(empty ? "Empty" : "Full"));
        return result;
    }

    // Weapon, magazine presentation and reload parts, and the evidence the
    // capture was made from: carrier frame, source recipe and the magazine
    // setup.
    CollisionProfile ReloadMakerController::magazineWorldIdentity(bool empty, const HandPosePlacement& anchor, const QMatrix4x4& root) const
    {
        CollisionProfile captured;
        captured.weapon = poseRecordKey(currentWeapon()->formId);
        captured.target = anchor.ownerRecord + "|magazine-carrier|" + (empty ? "empty" : "full");
        captured.configuration = authoringRecordKey().section('/', 1);
        captured.referencePose = { { "space", "magazine-carrier" },          { "magazineRecord", anchor.ownerRecord }, { "model", anchor.ownerModel },
                                   { "presentation", empty ? "empty" : "full" }, { "sourceRecipeId", _reloadRecipe->id },   { "rootToWorld", frameJson(root) } };
        if (const auto presentation = magazinePresentationJson(_magazineSetup ? magazineSetupJson(*_magazineSetup) : QJsonObject{}); !presentation.isEmpty()) {
            captured.referencePose["presentationSetup"] = presentation;
        }
        return captured;
    }

    // The visible magazine pieces of the presentation: no native-only pieces,
    // no cartridges when empty, and only the presentation's own body unless
    // both share one. Paths run from the carrier node and must be unique.
    void ReloadMakerController::collectMagazineWorldSources(CollisionProfile& captured, bool empty, const QModelIndex& carrierNode, const HandPosePlacement& anchor)
    {
        auto* view = _window->getGLView();
        auto* model = activeModel();
        QSet<int> included;
        qsizetype total = 0;
        for (auto* shape : view->getScene()->shapes) {
            if (!shape || shape->isHidden()) {
                continue;
            }
            const auto node = model->getBlockIndex(shape->id());
            const auto visual = std::find_if(_magazineVisuals.cbegin(), _magazineVisuals.cend(), [&](const auto& v) { return v.node == node; });
            if (visual == _magazineVisuals.cend() || visual->nativeOnly || (empty && visual->cartridge)) {
                continue;
            }
            if (!_magazineRail->sharedBody() && visual->empty != empty) {
                continue;
            }
            require(!included.contains(shape->id()), "Duplicate magazine surface");
            included.insert(shape->id());
            CollisionMesh source;
            source.name = model->get<QString>(node, "Name");
            source.model = anchor.ownerModel;
            source.record = anchor.ownerRecord;
            QStringList path;
            auto parent = node;
            for (int depth = 0; parent.isValid() && depth < model->getBlockCount(); ++depth) {
                path.prepend(model->get<QString>(parent, "Name"));
                if (parent == carrierNode) {
                    break;
                }
                parent = model->getBlockIndex(model->getParent(parent));
            }
            source.path = path.join('/');
            source.key = source.record + "|" + source.model + "|" + source.path;
            require(std::none_of(captured.sources.cbegin(), captured.sources.cend(), [&](const auto& s) { return s.key == source.key; }),
                    "Magazine pieces need unique source paths");
            Transformf world;
            QString error;
            require(sceneNodeWorld(node, world, error), error);
            source.frame = frameMatrix(world);
            source.render = shape->reloadMakerRigidMeshJson();
            require(!source.render.isEmpty(), "Magazine export needs complete rigid source geometry");
            source.material = model->get<QString>(model->getBlockIndex(model->getLink(node, "Shader Property")), "Name");
            require(!source.material.isEmpty(), "A magazine piece is missing its material path");
            source.render["material"] = source.material;
            for (const auto& v : shape->verts) {
                source.vertices.append({ v[0], v[1], v[2] });
            }
            for (const auto& t : shape->triangles) {
                source.triangles.append({ quint32(t[0]), quint32(t[1]), quint32(t[2]) });
            }
            total += source.triangles.size();
            require(total <= 1000000, "Magazine exceeds one million triangles");
            captured.sources.append(std::move(source));
        }
        captured.inputTriangles = total;
    }

    // Freezes the selected part's included world pieces in its frame and
    // starts the native job that writes the prepared world model.
    QJsonObject ReloadMakerController::capturePreparedPart(const QString& output)
    {
        require(!_packProcess, "Wait for the active asset job");
        const auto profile = collisionSnapshot(_collisionWorkshop.targetIndex(), false, true);
        const auto rootArray = profile.referencePose["rootToWorld"].toArray();
        require(rootArray.size() == 16, "Select one magazine/part as the reference, not the whole weapon");
        QMatrix4x4 root;
        for (int i = 0; i < 16; ++i) {
            root(i / 4, i % 4) = float(rootArray[i].toDouble());
        }
        bool invertible = false;
        const auto inverse = root.inverted(&invertible);
        require(invertible, "Invalid prepared-model root");
        const auto meshes = preparedPartMeshes(profile, inverse);
        require(!meshes.isEmpty(), "No selected world geometry remains");
        const QJsonObject snapshot{ { "format", "reload-maker-visible-part" }, { "version", 1 },      { "units", "game-units" },
                                    { "weapon", profile.weapon },            { "target", profile.target }, { "configuration", profile.configuration },
                                    { "referencePose", profile.referencePose }, { "meshes", meshes } };
        const auto path = studioDataPath("local/asset-jobs/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "/visible.json");
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        require(bytes.size() <= 64 * 1024 * 1024, "Choose a smaller part; snapshot exceeds 64 MiB");
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot freeze visible part geometry");
        return packCommand({ { "operation", "prepare" }, { "job", path }, { "output", output } });
    }

    // The render meshes in the part's frame. When the workshop shows the same
    // capture, its excluded world pieces stay out.
    QJsonArray ReloadMakerController::preparedPartMeshes(const CollisionProfile& profile, const QMatrix4x4& inverse) const
    {
        const auto& selected = _collisionWorkshop.profile();
        const bool same = selected.valid() && selected.weapon == profile.weapon && selected.configuration == profile.configuration && selected.target == profile.target;
        if (same) {
            require(std::any_of(selected.sources.begin(), selected.sources.end(), [](const auto& source) { return source.worldIncluded; }), "Include at least one world-model piece");
        }
        QJsonArray meshes;
        for (const auto& source : profile.sources) {
            if (same) {
                const auto old = std::find_if(selected.sources.begin(), selected.sources.end(), [&](const auto& s) { return s.key == source.key; });
                if (old != selected.sources.end() && !old->worldIncluded) {
                    continue;
                }
            }
            auto mesh = source.render;
            mesh["name"] = source.name;
            mesh["source"] = source.key;
            mesh["localToRoot"] = frameJson(inverse * source.frame);
            meshes.append(mesh);
        }
        return meshes;
    }

    // The prepared world model made from the workshop's current capture with
    // the same included pieces; empty when there is none.
    QString ReloadMakerController::preparedPartForCollision() const
    {
        const auto& profile = _collisionWorkshop.profile();
        QJsonArray pieces;
        for (const auto& piece : profile.sources) {
            if (piece.worldIncluded) {
                pieces.append(piece.key);
            }
        }
        const bool same = _preparedPart["target"].toString() == profile.target && _preparedPart["configuration"].toString() == profile.configuration
                          && _preparedPart["pieces"].toArray() == pieces && _preparedPart["referencePose"].toObject() == profile.referencePose;
        return same ? _preparedPart["output"].toString() : QString();
    }

    // Freezes the enabled hulls and the prepared model by content hash in a
    // job folder, then starts the native cook of the finished model.
    void ReloadMakerController::startCollisionCook(const QString& geometry, const QString& output)
    {
        const auto folder = studioDataPath("local/asset-jobs/" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QDir().mkpath(folder);
        const auto hulls = folder + "/hulls.json";
        _collisionWorkshop.command({ { "operation", "export-hulls" }, { "path", hulls } });
        const auto reference = [](const QString& path) {
            if (!QFileInfo(path).isFile() || QFileInfo(path).size() > 64 * 1024 * 1024) {
                throw std::runtime_error("Cannot read prepared asset input (maximum 64 MiB)");
            }
            return QJsonObject{ { "path", path } };
        };
        const QJsonObject job{ { "format", "reload-maker-world-object-job" }, { "version", 1 }, { "geometry", reference(geometry) }, { "collision", reference(hulls) } };
        const auto path = folder + "/job.json";
        const auto bytes = QJsonDocument(job).toJson();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            throw std::runtime_error("Could not freeze the asset cook job");
        }
        packCommand({ { "operation", "cook" }, { "job", path }, { "output", output } });
    }
}
