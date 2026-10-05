// Capturing collision input from the native scene: the targets offered to
// the collision workshop, the visible geometry under a target, and the
// prepared world model of a part.
#include "ReloadMakerController.h"
#include "CollisionPanel.h"
#include "design/DesignWidgets.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "areas/collisions/CollisionFrames.h"
#include "commands/CommandSupport.h"
#include "gl/glshape.h"
#include "glview.h"
#include "model/nifmodel.h"
#include "physics-interaction/weapon/WeaponEffectGeometryPolicy.h"

#include <QDockWidget>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        // Effect shaders, billboards and effect-named geometry are not solid.
        bool isEffectGeometry(const NifModel& model, const QModelIndex& node)
        {
            const auto geometryName = model.get<QString>(node, "Name").toUtf8();
            bool billboard = false;
            int depthLimit = model.getBlockCount();
            for (auto ancestor = node; ancestor.isValid() && depthLimit-- > 0; ancestor = model.getBlockIndex(model.getParent(ancestor))) {
                if (model.blockInherits(ancestor, "NiBillboardNode")) {
                    billboard = true;
                    break;
                }
            }
            using namespace rock::weapon_effect_geometry_policy;
            const bool effectShader = model.blockInherits(model.getBlockIndex(model.getLink(node, "Shader Property")), "BSEffectShaderProperty");
            return classify({ effectShader, billboard, std::string_view(geometryName.constData(), size_t(geometryName.size())) }) != ExclusionReason::None;
        }

        // Every source merged into one mesh in the target's frame.
        CollisionMesh mergedReference(const CollisionProfile& p, const HandPosePlacement& target, const QMatrix4x4& frame)
        {
            CollisionMesh reference;
            reference.key = p.target;
            reference.name = target.anchorName;
            reference.path = target.anchorPath;
            reference.model = target.ownerModel;
            reference.record = target.ownerRecord;
            reference.frame = frame;
            const auto inverse = reference.frame.inverted();
            for (const auto& source : p.sources) {
                const auto base = quint32(reference.vertices.size());
                for (auto point : source.vertices) {
                    reference.vertices.append(inverse.map(source.frame.map(point)));
                }
                for (auto t : source.triangles) {
                    reference.triangles.append({ t[0] + base, t[1] + base, t[2] + base });
                }
            }
            return reference;
        }
    }

    // Weapon collision lists the whole weapon, its installed parts (the
    // mounted magazine included) and the magazine anchor; nothing consumes
    // them yet. Everywhere else the list is the reload's magazine: the
    // captured loose world object, the only capture a pack accepts.
    void ReloadMakerController::refreshCollisions()
    {
        if (!_collisions) {
            return;
        }
        QStringList names;
        _collisionTargets.clear();
        if (_workspace == WeaponCollisionWorkspace) {
            for (int i = 0; i < _handPoseAnchors.size(); ++i) {
                const auto& description = _handPoseAnchors[i].description;
                if (i == 0 || description.anchorKind == "installed-part" || description.anchorName == "WeaponMagazine") {
                    names << _handPoseAnchors[i].label;
                    _collisionTargets.append(i);
                }
            }
        } else if (_magazineWorldSnapshot) {
            names << (_magazineWorldSnapshot->referencePose["presentation"].toString() + " magazine world object");
            _collisionTargets.append(-1);
        }
        _collisionWorkshop.setTargets(names);
    }

    void ReloadMakerController::showCollisionSubject()
    {
        const bool weapon = _workspace == WeaponCollisionWorkspace;
        _collisionsDock->setWindowTitle(weapon ? QStringLiteral("Weapon collision") : QStringLiteral("Magazines"));
        _collisionsHeader->setSection(weapon ? QStringLiteral("Tools") : QStringLiteral("Reload builder"));
        _collisionsHeader->setTitle(weapon ? QStringLiteral("Weapon collision") : QStringLiteral("Magazines"));
        _magazineSetupPanel->setVisible(!weapon);
        _collisions->setSubject(weapon ? QStringLiteral("Weapon collision") : QStringLiteral("Magazine world models"),
                                weapon ? QStringLiteral("The whole weapon and its installed parts, the mounted magazine included. Nothing uses these yet: "
                                                        "ROCK generates weapon collision in game.")
                                       : QStringLiteral("The full and empty loose magazines a pack ships, in the magazine carrier frame. Capture each "
                                                        "above, then generate its hulls, capture the visible model and cook it."));
    }

    void ReloadMakerController::invalidateCollisions()
    {
        _preparedPart = {};
        _magazineWorldSnapshot.reset();
        _magazineWorldPreview = false;
        if (_collisions) {
            _collisionWorkshop.invalidate();
        }
    }

    // The visible geometry under a workshop target in world space; target 0
    // is the whole weapon and the magazine world object is its own capture.
    // referenceOnly merges it into one mesh in the installed part's frame
    // (DIAMOND's reference input); captureRender keeps each shape's render
    // mesh for the prepared world model.
    CollisionProfile ReloadMakerController::collisionSnapshot(int target, bool referenceOnly, bool captureRender)
    {
        require(collisionWorkspace(_workspace), "Open Reload builder > Magazines or Weapon collision first");
        require(!_loadThread && !_reloadCapturing && !_rebuildTimer->isActive(), "Wait for assembly and reload capture");
        const auto* weapon = currentWeapon();
        auto* model = activeModel();
        auto* view = _window->getGLView();
        require(weapon && model && view->getScene(), "Load a weapon first");
        require(target >= 0 && target < _collisionTargets.size(), "Select a collision target");
        target = _collisionTargets[target];
        if (target < 0) {
            require(_magazineWorldSnapshot.has_value() && !referenceOnly, "Recapture the magazine in Operate");
            return *_magazineWorldSnapshot;
        }
        require(!_playing && !_reloadPlaying, "Pause playback before creating a collision snapshot");
        const auto anchor = _handPoseAnchors[target];
        require(!referenceOnly || (target > 0 && anchor.description.anchorKind == "installed-part"),
                "For DIAMOND input, select the installed magazine model, not an animated bone or whole weapon");
        require(!view->studioCaptureFrame().isNull(), "Wait for the native scene to finish rebuilding before capturing geometry");
        auto p = collisionProfileIdentity(target);
        collectCollisionSources(p, target, captureRender);
        require(!p.sources.isEmpty(), "No visible weapon triangles under this target");
        if (referenceOnly) {
            Transformf world;
            QString error;
            require(sceneNodeWorld(anchor.node, world, error), error);
            p.sources = { mergedReference(p, anchor.description, frameMatrix(world)) };
        }
        return p;
    }

    // Weapon, target and loadout, and the pose the capture was taken in:
    // animation, time, part choices and the target's frame. The whole weapon's
    // shape depends on every part, so its key lists them all, readably; a
    // part target is keyed by that part alone (no configuration hashes,
    // owner 2026-10-05).
    CollisionProfile ReloadMakerController::collisionProfileIdentity(int target) const
    {
        const auto& anchor = _handPoseAnchors[target];
        CollisionProfile p;
        p.weapon = poseRecordKey(currentWeapon()->formId);
        p.target = target ? anchor.description.ownerRecord + "|" + anchor.description.ownerModel + "|" + anchor.description.anchorPath : "whole-weapon";
        QJsonObject choices;
        QStringList parts;
        for (const auto& slot : configuration()->entries()) {
            const auto record = poseRecordKey(configuration()->selected(slot.key));
            choices[QString::number(slot.key)] = record;
            parts.append(record.toLower());
        }
        parts.sort();
        p.configuration = target ? anchor.description.ownerRecord.toLower() : parts.join(';');
        p.referencePose = { { "animation", _activeAnimationPath }, { "seconds", _playbackTime }, { "choices", choices } };
        if (target > 0) {
            Transformf world;
            QString error;
            require(sceneNodeWorld(anchor.node, world, error), error);
            p.referencePose["rootToWorld"] = frameJson(frameMatrix(world));
        }
        return p;
    }

    // Every visible solid shape owned by the weapon or one of its parts, and
    // for a part target only the shapes under it. Each source is named by its
    // owning record, model and node path, which must be unique.
    void ReloadMakerController::collectCollisionSources(CollisionProfile& p, int target, bool captureRender)
    {
        auto* model = activeModel();
        auto* view = _window->getGLView();
        const auto& targetNode = _handPoseAnchors[target].node;
        QSet<QString> keys;
        qsizetype totalTriangles = 0;
        for (auto* shape : view->getScene()->shapes) {
            if (!shape || shape->isHidden()) {
                continue;
            }
            const auto node = model->getBlockIndex(shape->id());
            if (isEffectGeometry(*model, node)) {
                continue;
            }
            QModelIndex owner;
            QStringList path;
            const bool underTarget = findShapeOwner(node, targetNode, owner, path);
            if (!owner.isValid() || (target > 0 && !underTarget)) {
                continue;
            }
            auto source = collisionSource(node, owner, path, p.weapon);
            require(!keys.contains(source.key), "Ambiguous duplicate node path; select a smaller source");
            keys.insert(source.key);
            source.frame = frameMatrix(shape->worldTrans());
            source.material = model->get<QString>(model->getBlockIndex(model->getLink(node, "Shader Property")), "Name");
            if (captureRender) {
                source.render = shape->reloadMakerRigidMeshJson();
                require(!source.render.isEmpty(), "Prepared world capture requires complete rigid geometry with UVs, normals and tangents at its highest detail");
                const auto shader = model->getBlockIndex(model->getLink(node, "Shader Property"));
                source.render["material"] = model->get<QString>(shader, "Name");
                require(!source.render["material"].toString().isEmpty(), "The selected shape needs an explicit source material path");
            }
            for (const auto& v : shape->reloadMakerCollisionVertices()) {
                source.vertices.append({ v[0], v[1], v[2] });
            }
            const auto count = shape->reloadMakerCollisionTriangleCount();
            totalTriangles += count;
            require(totalTriangles <= 1000000, "Snapshot exceeds one million triangles; choose a smaller part");
            for (qsizetype i = 0; i < count; ++i) {
                const auto t = shape->triangles[i];
                source.triangles.append({ quint32(t[0]), quint32(t[1]), quint32(t[2]) });
            }
            if (source.triangles.isEmpty()) {
                continue;
            }
            p.sources.append(std::move(source));
        }
        p.inputTriangles = totalTriangles;
    }

    // Walks up from a shape to the weapon root or the part root that owns it,
    // collecting the node names on the way. True when the walk passed the
    // target node.
    bool ReloadMakerController::findShapeOwner(const QModelIndex& node, const QModelIndex& targetNode, QModelIndex& owner, QStringList& path) const
    {
        auto* model = activeModel();
        bool underTarget = false;
        auto parent = node;
        for (int depth = 0; parent.isValid() && depth < model->getBlockCount(); ++depth) {
            path.prepend(model->get<QString>(parent, "Name"));
            if (parent == targetNode) {
                underTarget = true;
            }
            if (parent == _weaponRoot || _partRoots.values().contains(parent)) {
                owner = parent;
                break;
            }
            parent = model->getBlockIndex(model->getParent(parent));
        }
        return underTarget;
    }

    // A shape's source identity: the weapon's base model, or the selected mod
    // of the slot whose part owns it.
    CollisionMesh ReloadMakerController::collisionSource(const QModelIndex& node, const QModelIndex& owner, const QStringList& path, const QString& weaponRecord) const
    {
        auto* model = activeModel();
        CollisionMesh source;
        source.name = model->get<QString>(node, "Name");
        source.path = path.join('/');
        source.model = currentWeapon()->baseModelPath;
        source.record = weaponRecord;
        for (const auto& slot : configuration()->entries()) {
            if (_partRoots.value(slot.key) != owner) {
                continue;
            }
            if (const auto* mod = configuration()->mod(configuration()->selected(slot.key))) {
                source.model = mod->modelPath;
                source.record = poseRecordKey(mod->formId);
            }
            break;
        }
        source.key = source.record + "|" + source.model + "|" + source.path;
        return source;
    }

    // Remembers whether the full or empty magazine's collision draft has a
    // usable hull, for the checklist's world model item.
    void ReloadMakerController::rememberCollisionDraft()
    {
        if (!_collisions || !_reloadRecipe || !currentWeapon()) {
            return;
        }
        const auto& profile = _collisionWorkshop.profile();
        const auto reference = profile.referencePose;
        const auto presentation = reference["presentation"].toString();
        if (!profile.valid() || reference["space"] != "magazine-carrier" || (presentation != "full" && presentation != "empty")) {
            return;
        }
        const auto key = profile.weapon.toLower() + "/" + profile.configuration;
        if (key != authoringRecordKey()) {
            return;
        }
        const bool usable = std::any_of(profile.hulls.cbegin(), profile.hulls.cend(), [&](const auto& hull) {
            return hull.enabled && std::any_of(profile.sources.cbegin(), profile.sources.cend(), [&](const auto& source) {
                       return source.key == hull.source && source.worldIncluded;
                   });
        });
        auto record = _authoringRecords.value(key).toObject();
        auto drafts = record.value("collisionDrafts").toObject();
        drafts[presentation] = QJsonObject{
            { "usable", usable },
            { "edit", QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-') } };
        record["collisionDrafts"] = drafts;
        _authoringRecords[key] = record;
    }
}
