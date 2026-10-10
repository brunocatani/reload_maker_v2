// Weapon assembly: builds the first-person scene for the selected weapon
// and loadout. Skeleton and arms first, then the base model, the receiver
// and every attachment at its connect point, then the node bindings that
// animation drives.
#include "ReloadMakerController.h"
#include "PoseEvaluator.h"
#include "ReloadTemplates.h"
#include "SlotOverlay.h"
#include "StudioWindow.h"
#include "areas/loadout/LoadoutNames.h"
#include "areas/loadout/SceneTree.h"
#include "data/niftypes.h"
#include "gl/glscene.h"
#include "glview.h"
#include "model/nifmodel.h"
#include "spells/blocks.h"
#include "viewport/PartPicker.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QSlider>
#include <QTimer>

#include <array>
#include <vector>

namespace reloadmaker
{
    namespace
    {
        constexpr std::array<const char*, 3> kSkinHelperBoneNames{
            "Chest_Rear_Skin",
            "LArm_ShoulderFat_skin",
            "RArm_ShoulderFat_skin",
        };

        // Shows the wait cursor while a configuration assembles; every exit
        // restores it.
        class WaitCursor
        {
        public:
            WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
            ~WaitCursor() { QApplication::restoreOverrideCursor(); }
            WaitCursor(const WaitCursor&) = delete;
            WaitCursor& operator=(const WaitCursor&) = delete;
        };
    }

    void ReloadMakerController::recordPartSurfaces(SlotKey slot, int firstBlock, int lastBlock)
    {
        auto* model = activeModel();
        QVector<QPersistentModelIndex> surfaces;
        for (int block = firstBlock; model && block < lastBlock; ++block) {
            const auto index = model->getBlockIndex(block);
            if (model->blockInherits(index, "BSTriShape") || model->blockInherits(index, "NiGeometry")) {
                surfaces.append(index);
            }
        }
        _partSurfaces.insert(slot, std::move(surfaces));
    }

    // An OMOD keyword can move a named branch of the assembly to another
    // connect point (for example a handguard moving a sight). With
    // `completeAssembly` every OMOD is checked once more after all parts are
    // in place, so remaps into parts attached later also resolve.
    bool ReloadMakerController::remapAttachmentNodes(const ObjectModRecord& mod, int firstBlock, int lastBlock, QString& error,
                                                     bool completeAssembly)
    {
        auto* model = activeModel();
        const auto* plugin = currentPlugin();
        if (!model || !plugin) {
            error = "No assembly for attachment remapping";
            return false;
        }
        QSet<int> moved;
        for (const auto keyword : mod.addedKeywords) {
            const auto remap = plugin->nodeRemaps.constFind(keyword);
            if (remap == plugin->nodeRemaps.cend()) {
                continue;
            }
            QPersistentModelIndex node;
            for (int block = firstBlock; block < lastBlock; ++block) {
                const auto candidate = model->getBlockIndex(block);
                const bool named = model->blockInherits(candidate, "NiNode") &&
                                   model->get<QString>(candidate, "Name").compare(remap->node, Qt::CaseInsensitive) == 0;
                if (!named) {
                    continue;
                }
                if (node.isValid()) {
                    error = "Ambiguous remapped branch: " + remap->node;
                    return false;
                }
                node = candidate;
            }
            // Handguards may supply a remap for a separately installed sight,
            // handstop or bipod. Resolve those after the entire assembly exists.
            // An unequipped optional branch has no transform to remap.
            if (!node.isValid()) {
                if (completeAssembly) {
                    appendDiagnostic("NODE REMAP inactive (branch not equipped): " + remap->node);
                }
                continue;
            }
            const auto applied = std::find_if(_remappedAttachments.cbegin(), _remappedAttachments.cend(),
                                              [&](const auto& remapped) { return remapped.node == node; });
            if (applied != _remappedAttachments.cend()) {
                if (applied->point != remap->point) {
                    error = "Conflicting remaps for branch: " + remap->node;
                    return false;
                }
                continue;
            }
            const auto target = findConnectTarget("C-" + remap->point.mid(2), nullptr, configurationSlotKey(mod));
            if (!target.valid()) {
                error = target.error;
                return false;
            }
            const int block = model->getBlockNumber(node);
            if (moved.contains(block)) {
                error = "Conflicting remaps for branch: " + remap->node;
                return false;
            }
            auto ancestor = QModelIndex(target.parent);
            for (int depth = 0; ancestor.isValid() && depth < model->getBlockCount(); ++depth) {
                if (ancestor == node) {
                    error = "Cyclic attachment remap: " + remap->node;
                    return false;
                }
                ancestor = model->getBlockIndex(model->getParent(ancestor));
            }
            if (ancestor.isValid()) {
                error = "Cyclic remap destination ancestry";
                return false;
            }
            const QPersistentModelIndex oldParent = model->getBlockIndex(model->getParent(node));
            if (!oldParent.isValid()) {
                error = "Invalid remapped branch parent or transform";
                return false;
            }
            const Transformf connection{ target.translation, target.rotation, { target.scale, target.scale, target.scale } };
            // Move only the explicitly named branch. A rail-mounted optic can
            // share this attachment with iron sights but stay on the receiver.
            delLink(model, oldParent, "Children", block);
            // The remap selects a new attachment frame. The branch's old
            // placement is not an additional offset from that frame (the
            // Handmade front sight otherwise adds its old 27.6-unit position).
            if (!addLink(model, target.parent, "Children", block) || !setNodeTransform(node, connection)) {
                error = "Cannot attach remapped branch: " + remap->node;
                return false;
            }
            moved.insert(block);
            const auto name = plugin->keywordNames.value(keyword);
            _remappedAttachments.append({ node, target.parent, name, remap->point, configurationSlotKey(mod) });
            appendDiagnostic(QString("NODE REMAP %1 · node=%2 parent=%3 via %4")
                                 .arg(name)
                                 .arg(block)
                                 .arg(model->getBlockNumber(target.parent))
                                 .arg(remap->point));
        }
        return true;
    }

    bool ReloadMakerController::recordPartAlignment(int firstBlock, int lastBlock, int anchorBone, QString& error)
    {
        const auto* model = activeModel();
        if (!model || anchorBone < 0 || anchorBone >= _skeleton.bones.size()) {
            error = QStringLiteral("Invalid attachment alignment source");
            return false;
        }
        int anchor = -1;
        QVector<int> siblings;
        for (int block = firstBlock; block < lastBlock; ++block) {
            const auto node = model->getBlockIndex(block);
            if (!model->blockInherits(node, "NiNode")) { continue; }
            const int bone = _skeleton.boneIndex(model->get<QString>(node, "Name"));
            if (bone == anchorBone) {
                if (anchor >= 0) {
                    error = QStringLiteral("Attachment supplies multiple alignment anchors: %1").arg(_skeleton.bones.at(bone).name);
                    return false;
                }
                anchor = block;
            }
            if (bone >= 0 && _skeleton.bones.at(bone).parentIndex == _skeleton.bones.at(anchorBone).parentIndex) {
                siblings.append(block);
            }
        }
        if (anchor < 0) {
            error = QStringLiteral("Attachment's declared alignment bone is missing");
            return false;
        }
        // Align the model once using its connection, not each sibling's idle
        // placement. HK433 parks its auxiliary magazine off-weapon in idle;
        // cancelling that branch's own idle puts a second magazine in the gun.
        // Descendant cartridge tracks and other imported models stay independent.
        for (int block : siblings) { _partAlignmentAnchors.insert(block, anchor); }
        return true;
    }

    QHash<QString, QPersistentModelIndex> ReloadMakerController::collectCanonicalNodes(
        const int firstBlock, int lastBlock) const
    {
        QHash<QString, QPersistentModelIndex> result;
        NifModel* model = activeModel();
        if (!model) {
            return result;
        }
        if (lastBlock < 0 || lastBlock > model->getBlockCount()) {
            lastBlock = model->getBlockCount();
        }
        for (int block = (std::max)(0, firstBlock); block < lastBlock; ++block) {
            const QModelIndex index = model->getBlockIndex(block);
            if (!index.isValid() || !model->blockInherits(index, "NiNode")) {
                continue;
            }
            const QString name = model->get<QString>(index, "Name");
            if (!name.isEmpty() && !result.contains(normalizedKey(name))) {
                result.insert(normalizedKey(name), QPersistentModelIndex(index));
            }
        }
        return result;
    }

    int ReloadMakerController::mergeWeaponModelNodes(
        const QHash<QString, QPersistentModelIndex>& nodes)
    {
        int replaced = 0;
        for (auto iterator = nodes.cbegin(); iterator != nodes.cend(); ++iterator) {
            const bool visibleWeaponPart =
                iterator.key() != QStringLiteral("weapon") &&
                iterator.key().startsWith(QStringLiteral("weapon"));
            if (visibleWeaponPart) {
                if (_canonicalNodes.contains(iterator.key()) &&
                    _canonicalNodes.value(iterator.key()) != iterator.value()) {
                    ++replaced;
                }
                _canonicalNodes.insert(iterator.key(), iterator.value());
            } else if (!_canonicalNodes.contains(iterator.key())) {
                _canonicalNodes.insert(iterator.key(), iterator.value());
            }
        }
        return replaced;
    }

    QPersistentModelIndex ReloadMakerController::createBoneNode(const int boneIndex)
    {
        NifModel* model = activeModel();
        if (!model || boneIndex < 0 || boneIndex >= _skeleton.bones.size()) {
            return {};
        }
        const HkxBone& bone = _skeleton.bones.at(boneIndex);
        if (bone.parentIndex < 0 || bone.parentIndex >= _skeleton.bones.size()) {
            return {};
        }
        const QPersistentModelIndex parent = _canonicalNodes.value(
            normalizedKey(_skeleton.bones.at(bone.parentIndex).name));
        if (!parent.isValid()) {
            return {};
        }
        const QModelIndex node = model->insertNiBlock(QStringLiteral("NiNode"), -1);
        if (!node.isValid()) {
            return {};
        }
        model->set<QString>(node, "Name", bone.name);
        (void)setNodeTransform(node, bone.referenceLocal);
        blockLink(model, parent, node);
        const QPersistentModelIndex persistent(node);
        _canonicalNodes.insert(normalizedKey(bone.name), persistent);
        _synthesizedBones.insert(persistent);
        return persistent;
    }

    bool ReloadMakerController::addSkinHelperBones(QString& error)
    {
        error.clear();
        const QString sourcePath = QStringLiteral(
            "meshes/actors/character/characterassets/skeleton.nif");
        const auto sourceModel = sourceNif(referenceMeshes(), sourcePath, error);
        if (!sourceModel) { return false; }
        NifModel& source = *sourceModel;

        for (const char* helperName : kSkinHelperBoneNames) {
            QModelIndex sourceNode;
            for (int block = 0; block < source.getBlockCount(); ++block) {
                const QModelIndex candidate = source.getBlockIndex(block);
                if (candidate.isValid() && source.blockInherits(candidate, "NiNode") &&
                    source.get<QString>(candidate, "Name").compare(
                        QLatin1String(helperName), Qt::CaseInsensitive) == 0) {
                    sourceNode = candidate;
                    break;
                }
            }
            if (!sourceNode.isValid()) {
                error = QStringLiteral("Full skeleton is missing helper bone '%1'.")
                            .arg(QLatin1String(helperName));
                return false;
            }

            const QModelIndex sourceParent = source.getBlockIndex(source.getParent(sourceNode));
            if (!sourceParent.isValid()) {
                error = QStringLiteral("Helper bone '%1' has no unique parent.")
                            .arg(QLatin1String(helperName));
                return false;
            }
            const QString parentName = source.get<QString>(sourceParent, "Name");
            const QPersistentModelIndex destinationParent =
                _canonicalNodes.value(normalizedKey(parentName));
            if (!destinationParent.isValid()) {
                error = QStringLiteral("First-person skeleton is missing helper parent '%1'.")
                            .arg(parentName);
                return false;
            }

            const QPersistentModelIndex pasted = pasteBranch(
                source, sourceNode, destinationParent);
            if (!pasted.isValid()) {
                error = QStringLiteral("Cannot attach helper bone '%1'.")
                            .arg(QLatin1String(helperName));
                return false;
            }
            _canonicalNodes.insert(normalizedKey(QLatin1String(helperName)), pasted);
        }
        return true;
    }

    ReloadMakerController::SkinRetargetResult ReloadMakerController::retargetSkin(
        const int firstBlock, const int lastBlock, const QModelIndex& skeletonRoot)
    {
        SkinRetargetResult result;
        NifModel* model = activeModel();
        if (!model || !skeletonRoot.isValid()) {
            return result;
        }

        const int skeletonRootBlock = model->getBlockNumber(skeletonRoot);
        const QSignalBlocker signalBlocker(model);
        const bool updatesWereHeld = model->holdUpdates(true);
        for (int block = (std::max)(0, firstBlock);
             block < (std::min)(lastBlock, model->getBlockCount()); ++block) {
            const QModelIndex skin = model->getBlockIndex(block);
            if (!skin.isValid() ||
                model->itemName(skin) != QStringLiteral("BSSkin::Instance")) {
                continue;
            }
            (void)model->setLink(skin, "Skeleton Root", skeletonRootBlock);
            const QModelIndex bones = model->getIndex(skin, "Bones");
            for (int row = 0; row < model->rowCount(bones); ++row) {
                const QModelIndex boneLink = model->getIndex(bones, row);
                const QModelIndex sourceBone = model->getBlockIndex(model->getLink(boneLink));
                if (!sourceBone.isValid()) {
                    continue;
                }
                ++result.totalBones;
                const QString name = model->get<QString>(sourceBone, "Name");
                const QPersistentModelIndex canonical = _canonicalNodes.value(normalizedKey(name));
                if (!canonical.isValid()) {
                    continue;
                }
                if (model->setLink(boneLink, model->getBlockNumber(canonical))) {
                    ++result.retargetedBones;
                }
            }
        }
        (void)model->holdUpdates(updatesWereHeld);
        return result;
    }

    bool ReloadMakerController::buildNodeBindings(QString& error)
    {
        _holdingMirrorPlaneReady=false;
        _holdingMirrorPlaneX=0;
        _nodeBindings.clear();
        _sceneNodes.clear();
        _auxiliaryReloadNodes.clear();
        NifModel* model = activeModel();
        if (!model || !_skeleton.valid()) {
            error = QStringLiteral("No scene or skeleton for binding");
            return false;
        }
        QVector<ScenePoseNode> nodes;
        QHash<int, int> blockToNode;
        QVector<int> canonical(_skeleton.bones.size(), -1);
        // A mounted secondary weapon can bring another complete magazine rig.
        // Keep its geometry, but it must not acquire the host rifle's HKX bones.
        QHash<int,SlotKey> partOwners;
        for(auto it=_partRoots.cbegin();it!=_partRoots.cend();++it)partOwners.insert(model->getBlockNumber(it.value()),it.key());
        const auto ownerOf=[&](QModelIndex node) {
            for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth) {
                const int block=model->getBlockNumber(node);
                if(partOwners.contains(block))return block;
                node=model->getBlockIndex(model->getParent(node));
            }
            return -1;
        };
        QSet<SlotKey> primarySlots;
        if(const auto* receiver=resolvedReceiverObjectMod())primarySlots.insert(configurationSlotKey(*receiver));
        if(const auto* config=configuration())for(const auto& slot:config->entries())
            if(poseRecordKey(slot.attachPoint).compare("Fallout4.esm:05d4d7",Qt::CaseInsensitive)==0)primarySlots.insert(slot.key);
        for(const auto& axis:reloadAxes())primarySlots.insert(axis.second);
        const auto primary=[&](int owner){return owner<0||primarySlots.contains(partOwners.value(owner));};
        QVector<int> magazineOwners;
        bool primaryMagazine=false;
        for(int block=0;block<model->getBlockCount();++block) {
            const auto node=model->getBlockIndex(block);
            if(!model->blockInherits(node,"NiNode")||model->get<QString>(node,"Name").compare("WeaponMagazine",Qt::CaseInsensitive)!=0)continue;
            bool surface=false;
            for(int shape=0;shape<model->getBlockCount()&&!surface;++shape) {
                const auto geometry=model->getBlockIndex(shape);
                surface=(model->blockInherits(geometry,"BSTriShape")||model->blockInherits(geometry,"NiGeometry"))&&isWithinBlock(*model,geometry,node);
            }
            if(!surface)continue;
            const int owner=ownerOf(node);magazineOwners.append(owner);primaryMagazine|=primary(owner);
        }
        QSet<int> auxiliaryOwners;
        if(primaryMagazine)for(const int owner:magazineOwners)if(!primary(owner))auxiliaryOwners.insert(owner);
        for (int block = 0; block < model->getBlockCount(); ++block) {
            const QModelIndex index = model->getBlockIndex(block);
            if (!model->blockInherits(index, "NiNode")) { continue; }
            Transformf rest;
            if (!readNodeTransform(index, rest)) {
                error = QStringLiteral("Invalid assembly node transform at block %1").arg(block);
                return false;
            }
            const QString name = model->get<QString>(index, "Name");
            const int bone = _attachmentBoneAliases.contains(block)||auxiliaryOwners.contains(ownerOf(index)) ? -1 : _skeleton.boneIndex(name);
            const bool isCanonical = bone >= 0 &&
                _canonicalNodes.value(normalizedKey(name)) == index;
            const bool part = bone >= 0 && bone != _weaponBoneIndex &&
                isDescendantOf(bone, _weaponBoneIndex);
            const int node = nodes.size();
            if(auxiliaryOwners.contains(ownerOf(index)))_auxiliaryReloadNodes.insert(node);
            nodes.append(ScenePoseNode{ -1, isCanonical || part ? bone : -1, rest });
            _sceneNodes.append(QPersistentModelIndex(index));
            blockToNode.insert(block, node);
            if (isCanonical) { canonical[bone] = node; }
        }
        for (int node = 0; node < nodes.size(); ++node) {
            const int block = model->getBlockNumber(_sceneNodes.at(node));
            const auto alignment = _partAlignmentAnchors.constFind(block);
            if (alignment != _partAlignmentAnchors.cend()) {
                nodes[node].alignmentNode = blockToNode.value(alignment.value(), -1);
                if (nodes.at(node).alignmentNode < 0) {
                    error = QStringLiteral("Attachment alignment anchor disappeared before binding");
                    return false;
                }
            }
            const int parentBlock = model->getParent(_sceneNodes.at(node));
            if (parentBlock >= 0 && !blockToNode.contains(parentBlock)) {
                error = QStringLiteral("Unsupported non-node parent for assembly block %1")
                            .arg(model->getBlockNumber(_sceneNodes.at(node)));
                return false;
            }
            nodes[node].parent = blockToNode.value(parentBlock, -1);
        }
        if (!compileSceneBindings(_skeleton, _weaponBoneIndex, nodes, canonical, _nodeBindings, error)) {
            return false;
        }
        _assemblyNodes=nodes;
        int parts = 0;
        for (const auto& binding : _nodeBindings) {
            if (binding.weaponPart) {
                ++parts;
                appendDiagnostic(QStringLiteral("PART BIND %1 block=%2 parent=%3 rest=(%4,%5,%6)")
                    .arg(_skeleton.bones.at(binding.boneIndex).name)
                    .arg(model->getBlockNumber(_sceneNodes.at(binding.nodeIndex)))
                    .arg(model->getParent(_sceneNodes.at(binding.nodeIndex)))
                    .arg(binding.restInBoneParent.translation.x, 0, 'f', 4)
                    .arg(binding.restInBoneParent.translation.y, 0, 'f', 4)
                    .arg(binding.restInBoneParent.translation.z, 0, 'f', 4));
                if (binding.alignmentBoneIndex != binding.boneIndex) {
                    appendDiagnostic(QStringLiteral("PART ALIGN %1 block=%2 via attachment anchor %3; authored branch placement retained")
                        .arg(_skeleton.bones.at(binding.boneIndex).name)
                        .arg(model->getBlockNumber(_sceneNodes.at(binding.nodeIndex)))
                        .arg(_skeleton.bones.at(binding.alignmentBoneIndex).name));
                }
            }
        }
        appendDiagnostic(QStringLiteral("BIND %1 total nodes; %2 actual part bindings").arg(_nodeBindings.size()).arg(parts));
        return true;
    }

    // Assembles the selected weapon and loadout, then shows it in its Ready
    // pose. When only non-geometry state changed, the assembled scene is kept
    // and only the animation side is rebuilt.
    void ReloadMakerController::rebuildWeapon()
    {
        if (_loadThread) {
            return;
        }
        const bool resumeMagazineFit = _magazineFitPreview && _workspace == GripsWorkspace;
        invalidateCollisions();
        invalidateReloadCapture();
        _rebuildTimer->stop();
        _assignedMechanism = templateMechanism(assignedTemplate());
        restoreHandPosePreview();
        const auto poseControls = qScopeGuard([this]() { updateHandPoseControls(); });
        QElapsedTimer rebuildClock;
        rebuildClock.start();
        _sourceMisses = 0;
        _copyMilliseconds = _pasteMilliseconds = _decodeMilliseconds = 0;
        const QString previousAction = _activeAction;
        const bool sameWeapon = _renderedWeapon == _selectedWeapon;
        const PluginContent* plugin = currentPlugin();
        const WeaponRecord* weapon = currentWeapon();
        if (!plugin || !weapon || !_packageResources.isOpen()) {
            return;
        }
        if (_gameData.isEmpty() || (!_libraryMode && (!_gameMeshes.isOpen() || !_gameAnimations.isOpen()))) {
            setStatus(QStringLiteral("Select the Fallout 4 VR Data folder before building the weapon."), true);
            return;
        }
        const QString geometrySignature = assemblyGeometrySignature(*plugin, *weapon);
        const bool reuseGeometry = geometrySignature == _geometrySignature && !_nodeBindings.isEmpty();
        const WaitCursor busy;
        resetAssemblyState(reuseGeometry);

        QString error;
        bool animations = false;
        {
            StageClock clock(_loadTimings, "rebuild.animations");
            animations = rebuildAnimationActions(error);
        }
        if (!animations) {
            appendDiagnostic(QStringLiteral("ANIMATION unavailable: %1 — configuration preview remains available").arg(error));
        }
        bool skeleton = false;
        {
            StageClock clock(_loadTimings, "rebuild.skeleton");
            skeleton = loadFirstPersonSkeleton(error);
        }
        if (!skeleton) {
            setStatus(error, true);
            return;
        }
        NifModel* destination = activeModel();
        if (!destination) {
            setStatus(QStringLiteral("NifSkope render model is unavailable."), true);
            return;
        }
        bool assembled = true;
        if (!reuseGeometry) {
            StageClock clock(_loadTimings, "rebuild.assemble");
            assembled = assembleScene(*destination, *weapon, geometrySignature, error);
        }
        if (!assembled) {
            setStatus(error, true);
            return;
        }
        bool ready = false;
        {
            StageClock clock(_loadTimings, "rebuild.readyPose");
            ready = showReadyPose(sameWeapon, previousAction, error);
        }
        if (!ready) {
            setStatus(error, true);
            return;
        }
        if (resumeMagazineFit) {
            showMagazineStep(MagazineStep::HandFit);
        }
        _loadTimings.add(reuseGeometry ? QStringLiteral("rebuild.total scene-reused") : QStringLiteral("rebuild.total"), rebuildClock.elapsed());
        _loadTimings.add(QStringLiteral("rebuild.decode"), _decodeMilliseconds);
        _loadTimings.add(QStringLiteral("rebuild.copy"), _copyMilliseconds);
        _loadTimings.add(QStringLiteral("rebuild.paste"), _pasteMilliseconds);
        _loadTimings.add(QStringLiteral("rebuild.sourceDecodes"), _sourceMisses);
        appendDiagnostic(QStringLiteral("CONFIGURATION %1 ms · %2 · %3 source decodes (warm data retained)")
            .arg(rebuildClock.elapsed()).arg(reuseGeometry ? QStringLiteral("scene reused") : QStringLiteral("assembled from source cache"))
            .arg(_sourceMisses));
        appendDiagnostic(QStringLiteral("  COST model decode=%1 ms branch copy=%2 ms branch paste=%3 ms")
            .arg(_decodeMilliseconds).arg(_copyMilliseconds).arg(_pasteMilliseconds));
        if (!_clip.valid()) {
            setStatus(QStringLiteral("Built %1 with %2 animated node bindings.")
                          .arg(weapon->name)
                          .arg(_nodeBindings.size()));
        }
    }

    // Everything that decides the assembled geometry. An unchanged signature
    // lets a rebuild keep the scene.
    QString ReloadMakerController::assemblyGeometrySignature(const PluginContent& plugin, const WeaponRecord& weapon) const
    {
        QStringList parts{ QString::number(_selectedWeapon), _armsBody, normalizeNifPath(weapon.baseModelPath) };
        for (const auto* mod : configuredObjectMods()) {
            if (mod->modelPath.isEmpty()) {
                continue;
            }
            parts.append(QString::number(configurationSlotKey(*mod)) + QLatin1Char(':') + normalizeNifPath(mod->modelPath));
            for (const auto keyword : mod->addedKeywords) {
                if (plugin.nodeRemaps.contains(keyword)) {
                    parts.append("remap:" + recordIdentity(plugin, keyword));
                }
            }
        }
        return parts.join(QLatin1Char('|'));
    }

    void ReloadMakerController::resetAssemblyState(bool reuseGeometry)
    {
        stopPlayback();
        _clip = {};
        _readyClip = {};
        _handPosePreview.reset();
        _fingerDrafts = {};
        clearHandPoseAnchors();
        _readyLocalPose.clear();
        _readyGlobalPose.clear();
        _activeAction.clear();
        _activeAnimationPath.clear();
        _readyAnimationPath.clear();
        _stateBaselineAnimationPath.clear();
        _animationActions.clear();
        _animationSearchPaths.clear();
        _sourceLocalPose.clear();
        _sourceGlobalPose.clear();
        _stateBaselineLocalPose.clear();
        if (!reuseGeometry) {
            _sceneNodes.clear();
            _boundLocalPose.clear();
            _boundGlobalPose.clear();
        }
        _playbackTime = 0.0f;
        _timeline->setEnabled(false);
        refreshAnimations();
        if (!reuseGeometry) {
            _canonicalNodes.clear();
            _synthesizedBones.clear();
            _partRoots.clear();
            _partSurfaces.clear();
            _partAlignmentAnchors.clear();
            _attachmentRuntimeNames.clear();
            _attachmentBoneAliases.clear();
            _attachmentConnections.clear();
            _remappedAttachments.clear();
            _slotOverlay->setCallouts({});
            _partPicker->setParts({});
            _nodeBindings.clear();
            _weaponRoot = QPersistentModelIndex{};
            _bodyRoot = QPersistentModelIndex{};
            _handsRoot = QPersistentModelIndex{};
            _bodyRootFlags = 0;
            _handsRootFlags = 0;
            _weaponBoneIndex = -1;
        }
        _clipBlendInSeconds = 0.0f;
        _clipBlendOutSeconds = 0.0f;
    }

    // The HKX skeleton is read once and kept; it must have both arms.
    bool ReloadMakerController::loadFirstPersonSkeleton(QString& error)
    {
        const QString skeletonHkxPath = QStringLiteral("meshes/actors/character/_1stperson/characterassets/skeleton.hkx");
        QString detail;
        if (!_skeleton.valid()) {
            _skeleton = HkxReader::readSkeleton(readAnimationResource(skeletonHkxPath, detail), detail);
        }
        if (!_skeleton.valid()) {
            error = QStringLiteral("Cannot load first-person HKX skeleton: %1").arg(detail);
            return false;
        }
        for (const char* name : { "LArm_Collarbone", "RArm_Collarbone", "LArm_Hand", "RArm_Hand" }) {
            if (_skeleton.boneIndex(QLatin1String(name)) < 0) {
                error = QStringLiteral("First-person skeleton is missing %1").arg(QLatin1String(name));
                return false;
            }
        }
        return true;
    }

    bool ReloadMakerController::assembleScene(NifModel& destination, const WeaponRecord& weapon,
                                              const QString& geometrySignature, QString& error)
    {
        _geometrySignature.clear();
        {
            StageClock clock(_loadTimings, "rebuild.arms");
            if (!attachFirstPersonArms(destination, error)) {
                return false;
            }
        }
        {
            StageClock clock(_loadTimings, "rebuild.weapon");
            if (!attachWeaponModels(destination, weapon, error)) {
                return false;
            }
        }
        // An incomplete loadout still shows, but is reassembled next time.
        bool complete = true;
        {
            StageClock clock(_loadTimings, "rebuild.mods");
            if (!attachObjectMods(destination, complete, error)) {
                return false;
            }
        }
        QString detail;
        StageClock bindClock(_loadTimings, "rebuild.bind");
        if (!buildNodeBindings(detail)) {
            error = QStringLiteral("Cannot bind assembled parts: %1").arg(detail);
            return false;
        }
        if (complete) {
            _geometrySignature = geometrySignature;
        }
        _renderedWeapon = _selectedWeapon;
        return true;
    }

    bool ReloadMakerController::attachFirstPersonArms(NifModel& destination, QString& error)
    {
        QString detail;
        const QString skeletonNifPath = QStringLiteral("meshes/actors/character/_1stperson/characterassets/skeleton.nif");
        if (!loadNif(referenceMeshes(), skeletonNifPath, destination, detail)) {
            error = QStringLiteral("Cannot load first-person NIF skeleton: %1").arg(detail);
            return false;
        }
        // Model reset already schedules compilation. setNif also resets the camera.
        _window->getGLView()->update();
        const int baseSkeletonBlockCount = destination.getBlockCount();
        _canonicalNodes = collectCanonicalNodes(0, baseSkeletonBlockCount);
        _synthesizedBones.clear();
        if (!addSkinHelperBones(detail)) {
            error = QStringLiteral("Cannot complete the first-person skin skeleton: %1").arg(detail);
            return false;
        }
        const bool female = _armsBody == QStringLiteral("female");
        const QString bodyPath = female ? QStringLiteral("meshes/actors/character/characterassets/1stpersonfemalebody.nif")
                                        : QStringLiteral("meshes/actors/character/characterassets/1stpersonmalebody.nif");
        const QString handsPath = female ? QStringLiteral("meshes/actors/character/characterassets/1stpersonfemalehands.nif")
                                         : QStringLiteral("meshes/actors/character/characterassets/1stpersonmalehands.nif");
        return attachArmsModel(destination, bodyPath, QStringLiteral("body"), _bodyRoot, _bodyRootFlags, error) &&
               attachArmsModel(destination, handsPath, QStringLiteral("hands"), _handsRoot, _handsRootFlags, error);
    }

    // Pastes the first-person body or hands under the skeleton root and
    // retargets every skin bone to the canonical skeleton. `name` is "body"
    // or "hands" and appears in diagnostics and errors.
    bool ReloadMakerController::attachArmsModel(NifModel& destination, const QString& path, const QString& name,
                                                QPersistentModelIndex& root, int& flags, QString& error)
    {
        QString detail;
        const auto model = sourceNif(referenceMeshes(), path, detail);
        if (!model) {
            error = QStringLiteral("Cannot load first-person %1: %2").arg(name, detail);
            return false;
        }
        const int firstBlock = destination.getBlockCount();
        root = pasteRoot(*model, destination.getBlockIndex(0));
        if (!root.isValid()) {
            error = QStringLiteral("Cannot attach the first-person %1 to the skeleton.").arg(name);
            return false;
        }
        flags = destination.get<int>(root, "Flags");
        const SkinRetargetResult skin = retargetSkin(firstBlock, destination.getBlockCount(), destination.getBlockIndex(0));
        appendDiagnostic(QStringLiteral("SKIN %1 canonical-bones=%2/%3").arg(name).arg(skin.retargetedBones).arg(skin.totalBones));
        if (skin.totalBones == 0 || skin.retargetedBones != skin.totalBones) {
            const QString bone = name == QLatin1String("hands") ? QStringLiteral("hand") : name;
            error = QStringLiteral("Cannot retarget every first-person %1 skin bone.").arg(bone);
            return false;
        }
        return true;
    }

    // The base model goes on the skeleton's Weapon bone; every modelled part,
    // the receiver included, then connects through attachObjectMods.
    bool ReloadMakerController::attachWeaponModels(NifModel& destination, const WeaponRecord& weapon, QString& error)
    {
        const int weaponBone = _skeleton.boneIndex(QStringLiteral("Weapon"));
        if (weaponBone < 0 || _skeleton.bones.at(weaponBone).parentIndex < 0) {
            error = QStringLiteral("First-person HKX skeleton has no Weapon bone.");
            return false;
        }
        _weaponBoneIndex = weaponBone;
        QPersistentModelIndex weaponMount = _canonicalNodes.value(normalizedKey(_skeleton.bones.at(weaponBone).name));
        if (!weaponMount.isValid()) {
            weaponMount = createBoneNode(weaponBone);
        }
        if (!weaponMount.isValid()) {
            error = QStringLiteral("The NIF skeleton has no usable Weapon mount.");
            return false;
        }

        const QString basePath = normalizeNifPath(weapon.baseModelPath);
        QString detail;
        const auto baseModel = sourceNif(_packageResources.contains(basePath) ? _packageResources : referenceMeshes(),
                                         basePath, detail, true);
        if (weapon.baseModelPath.isEmpty() || !baseModel) {
            error = QStringLiteral("Cannot load weapon base model '%1': %2").arg(weapon.baseModelPath, detail);
            return false;
        }
        const int baseFirstBlock = destination.getBlockCount();
        _weaponRoot = pasteRoot(*baseModel, weaponMount);
        if (!_weaponRoot.isValid()) {
            error = QStringLiteral("Cannot attach the weapon base model to the Weapon bone.");
            return false;
        }
        _baseNodesReplaced = mergeWeaponModelNodes(collectCanonicalNodes(baseFirstBlock));
        return true;
    }

    // The receiver's visible weapon nodes become the canonical animated ones.
    bool ReloadMakerController::placeReceiver(NifModel& destination, const ObjectModRecord& receiver, NifModel& receiverModel,
                                              const ConnectTarget& target, const QString& point, QString& error)
    {
        const int firstBlock = destination.getBlockCount();
        const QPersistentModelIndex root = pasteRoot(receiverModel, target.parent);
        const Transformf placement{ target.translation, target.rotation, Vec3f{ target.scale, target.scale, target.scale } };
        if (!root.isValid() || !setNodeTransform(root, placement)) {
            error = QStringLiteral("Cannot attach the receiver to the weapon base model.");
            return false;
        }
        const auto receiverNodes = collectCanonicalNodes(firstBlock);
        _partRoots.insert(configurationSlotKey(receiver), root);
        recordPartSurfaces(configurationSlotKey(receiver), firstBlock, destination.getBlockCount());
        const int receiverNodesReplaced = mergeWeaponModelNodes(receiverNodes);
        appendDiagnostic(QStringLiteral("BIND visible weapon nodes base-replaced=%1 receiver-replaced=%2")
                             .arg(_baseNodesReplaced)
                             .arg(receiverNodesReplaced));
        appendDiagnostic(QStringLiteral("ATTACH %1 via %2").arg(receiver.editorId, point));
        appendDiagnostic(QStringLiteral("RECEIVER  authority=slot configuration  model=%1").arg(receiver.modelPath));
        return true;
    }

    // Every modelled OMOD connects to a point the scene already has. An
    // attachment can connect to a point another attachment brings (a receiver
    // can hang on a stock), so the loop places what connects and retries the
    // rest, the receiver first. Parts wait for their declared owner slots'
    // models. When a pass places nothing, missing skeleton bones are created;
    // when none are, the owner wait is released for one pass (the Remington
    // 870's receiver shape hangs on the stock model whose slot the receiver
    // owns, so each waited for the other). `complete`
    // turns false when an attachment cannot be loaded or connected; a receiver
    // that cannot is an error.
    bool ReloadMakerController::attachObjectMods(NifModel& destination, bool& complete, QString& error)
    {
        struct PendingAttachment
        {
            const ObjectModRecord* record;
            QSharedPointer<NifModel> model;
            QString connectionError;
            bool receiver{ false };
        };
        const QVector<const ObjectModRecord*> selected = configuredObjectMods();
        std::vector<PendingAttachment> pending;
        QSet<QString> providedBones;
        for (const ObjectModRecord* objectMod : selected) {
            if (!objectMod || objectMod->modelPath.isEmpty()) {
                continue;
            }
            const bool receiver = isReceiverObjectMod(*objectMod);
            QString detail;
            auto attachment = sourceNif(_packageResources, objectMod->modelPath, detail, true);
            if (!attachment) {
                if (receiver) {
                    error = QStringLiteral("Cannot load receiver '%1': %2").arg(objectMod->modelPath, detail);
                    return false;
                }
                complete = false;
                appendDiagnostic(QStringLiteral("SKIP %1: %2").arg(objectMod->editorId, detail));
                continue;
            }
            for (int block = 0; block < attachment->getBlockCount(); ++block) {
                const QModelIndex index = attachment->getBlockIndex(block);
                if (attachment->blockInherits(index, "NiNode")) {
                    providedBones.insert(normalizedKey(attachment->get<QString>(index, "Name")));
                }
            }
            pending.push_back({ objectMod, std::move(attachment), {}, receiver });
        }
        std::stable_partition(pending.begin(), pending.end(), [](const PendingAttachment& entry) { return entry.receiver; });
        // Never manufacture a reference-pose substitute for a bone owned by an
        // incoming model. Preserve missing-bone support for passive attachments.
        const auto createMissingBones = [this, &providedBones]() {
            int created = 0;
            for (int bone = 0; bone < _skeleton.bones.size(); ++bone) {
                const QString key = normalizedKey(_skeleton.bones.at(bone).name);
                if (bone != _weaponBoneIndex && isDescendantOf(bone, _weaponBoneIndex) &&
                    !_canonicalNodes.contains(key) && !providedBones.contains(key) &&
                    createBoneNode(bone).isValid()) {
                    ++created;
                }
            }
            return created;
        };
        bool waitForOwners = true;
        while (!pending.empty()) {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                NifModel& attachment = *it->model;
                ConnectTarget target;
                QString point;
                for (const QString& candidate : childConnectPoints(attachment)) {
                    target = findConnectTarget(candidate, it->receiver ? nullptr : &attachment, configurationSlotKey(*it->record), waitForOwners);
                    if (target.valid()) {
                        point = candidate;
                        break;
                    }
                }
                if (!target.valid()) {
                    it->connectionError = target.error;
                    ++it;
                    continue;
                }
                const bool placed = it->receiver ? placeReceiver(destination, *it->record, attachment, target, point, error)
                                                 : placeAttachment(destination, *it->record, attachment, target, point, error);
                if (!placed) {
                    return false;
                }
                it = pending.erase(it);
                progress = true;
            }
            if (progress) {
                waitForOwners = true;
                continue;
            }
            if (createMissingBones() > 0) {
                continue;
            }
            if (waitForOwners) {
                QStringList waiting;
                for (const auto& entry : pending) {
                    waiting.append(entry.record->editorId);
                }
                appendDiagnostic(QStringLiteral("ORDER  declared owners pending for %1; connecting to any installed point").arg(waiting.join(QStringLiteral(", "))));
                waitForOwners = false;
                continue;
            }
            {
                for (const auto& entry : pending) {
                    if (entry.receiver) {
                        error = QStringLiteral("Receiver '%1': %2").arg(entry.record->modelPath, entry.connectionError);
                        return false;
                    }
                }
                complete = false;
                for (const auto& entry : pending) {
                    appendDiagnostic(QStringLiteral("SKIP %1: %2 (%3)")
                        .arg(entry.record->editorId, entry.connectionError.isEmpty() ? QStringLiteral("No child connect points") : entry.connectionError,
                             childConnectPoints(*entry.model).join(QStringLiteral(", "))));
                }
                break;
            }
        }
        (void)createMissingBones();
        for (const auto* mod : selected) {
            QString detail;
            if (mod && !remapAttachmentNodes(*mod, 0, destination.getBlockCount(), detail, true)) {
                error = "Cannot remap assembled attachment " + mod->editorId + ": " + detail;
                return false;
            }
        }
        return true;
    }

    // Pastes one attachment at its connect target. An attachment that brings
    // its own animated connection bone is placed so that bone lands on the
    // skeleton's bone of the same name.
    bool ReloadMakerController::placeAttachment(NifModel& destination, const ObjectModRecord& record, NifModel& attachment,
                                                const ConnectTarget& target, const QString& point, QString& error)
    {
        const int firstBlock = destination.getBlockCount();
        const QPersistentModelIndex root = pasteRoot(attachment, target.parent);
        if (!root.isValid()) {
            error = QStringLiteral("Cannot paste attachment %1").arg(record.editorId);
            return false;
        }
        _attachmentRuntimeNames.insert(destination.getBlockNumber(root), "P-" + point.mid(2));
        Transformf placement{ target.translation, target.rotation, { target.scale, target.scale, target.scale } };
        if (target.suppliedBone >= 0) {
            Transformf authoredRoot;
            if (!readNodeTransform(root, authoredRoot)) {
                error = QStringLiteral("Cannot read authored attachment root");
                return false;
            }
            QModelIndex anchor = findNode(_skeleton.bones.at(target.suppliedBone).name, firstBlock);
            Transformf anchorInMount;
            bool reachedRoot = false;
            for (int depth = 0; depth < destination.getBlockCount() && anchor.isValid(); ++depth) {
                Transformf local;
                if (!readNodeTransform(anchor, local)) {
                    break;
                }
                anchorInMount = composed(local, anchorInMount);
                if (anchor == root) {
                    reachedRoot = true;
                    break;
                }
                anchor = destination.getBlockIndex(destination.getParent(anchor));
            }
            auto ownerInMount = anchorInMount;
            if (target.connectionParent.isValid()) {
                Transformf ownerWorld, mountWorld, inverseMount;
                if (!sceneNodeWorld(target.connectionParent, ownerWorld, error) ||
                    !sceneNodeWorld(target.parent, mountWorld, error) || !invertUniformTransform(mountWorld, inverseMount)) {
                    error = QStringLiteral("Cannot resolve attachment connection parent %1").arg(record.editorId);
                    return false;
                }
                ownerInMount = composed(inverseMount, ownerWorld);
            }
            if (!reachedRoot || !suppliedAnchorPlacement(authoredRoot, anchorInMount, ownerInMount, placement, placement)) {
                error = QStringLiteral("Cannot resolve supplied attachment anchor %1").arg(record.editorId);
                return false;
            }
        }
        if (!setNodeTransform(root, placement)) {
            error = QStringLiteral("Cannot place attachment %1").arg(record.editorId);
            return false;
        }
        if (target.mountedBone >= 0) {
            // The actual parent already animates this attachment. Its own
            // same-named node is an authored static bridge, not a second
            // animation driver; payload descendants keep their own bindings.
            for (int block = firstBlock; block < destination.getBlockCount(); ++block) {
                const auto node = destination.getBlockIndex(block);
                if (destination.blockInherits(node, "NiNode") &&
                    _skeleton.boneIndex(destination.get<QString>(node, "Name")) == target.mountedBone) {
                    _attachmentBoneAliases.insert(block);
                }
            }
        }
        QString detail;
        if (target.suppliedBone >= 0 && !recordPartAlignment(firstBlock, destination.getBlockCount(), target.suppliedBone, detail)) {
            error = QStringLiteral("Cannot align attachment %1: %2").arg(record.editorId, detail);
            return false;
        }
        if (!remapAttachmentNodes(record, firstBlock, destination.getBlockCount(), detail)) {
            error = "Cannot remap attachment " + record.editorId + ": " + detail;
            return false;
        }
        (void)mergeWeaponModelNodes(collectCanonicalNodes(firstBlock));
        _partRoots.insert(configurationSlotKey(record), root);
        recordPartSurfaces(configurationSlotKey(record), firstBlock, destination.getBlockCount());
        appendDiagnostic(QStringLiteral("ATTACH %1 via %2 · owner=%3 parent=%4 root=%5 t=(%6,%7,%8)%9")
            .arg(record.editorId, point).arg(target.ownerRoot)
            .arg(destination.getBlockNumber(target.parent)).arg(destination.getBlockNumber(root))
            .arg(placement.translation.x, 0, 'f', 3).arg(placement.translation.y, 0, 'f', 3)
            .arg(placement.translation.z, 0, 'f', 3)
            .arg(target.suppliedBone >= 0 ? QStringLiteral(" — owns animated connection bone") : QString{}));
        return true;
    }

    // Shows the assembled weapon: arms visibility, part callouts and picking,
    // then the graph's first action as the Ready pose that fitting and every
    // baseline use. A rebuild of the same weapon returns to its previous action.
    bool ReloadMakerController::showReadyPose(bool sameWeapon, const QString& previousAction, QString& error)
    {
        setArmsVisible(_armsShown);
        refreshSlotCallouts();
        refreshPartPicking();
        _window->getGLView()->updateScene();
        refreshAnimations();
        if (_animationActions.isEmpty()) {
            error = QStringLiteral("The configured graph exposes no playable animation actions.");
            return false;
        }

        _readyAnimationPath = _animationActions.first().resourcePath;
        QString detail;
        _readyClip = cachedClip(_readyAnimationPath, detail);
        if (!_readyClip.valid() ||
            !samplePose(_skeleton, _readyClip, 0.0f, nullptr, _sampledTracks, _readyLocalPose, _readyGlobalPose, detail)) {
            error = QStringLiteral("Cannot establish graph-selected Ready authority: %1")
                        .arg(detail.isEmpty() ? QStringLiteral("pose sampling failed") : detail);
            return false;
        }
        _stateBaselineLocalPose = _readyLocalPose;
        _stateBaselineAnimationPath = _readyAnimationPath;
        // Cache the assembled weapon in Ready before any firing/reload pose.
        // Fit never includes arms, detached magazines or parked source copies.
        selectAction(0);
        cacheWeaponFitBounds();
        appendDiagnostic(QStringLiteral("BASE  Ready pose=%1 sample=0.000s").arg(_readyAnimationPath));
        int initialAction = 0;
        if (sameWeapon && !previousAction.isEmpty()) {
            for (int i = 0; i < _animationActions.size(); ++i) {
                if (_animationActions[i].label == previousAction) {
                    initialAction = i;
                    break;
                }
            }
        }
        selectAction(initialAction);
        refreshHandPoseAnchors();
        if (!sameWeapon) {
            (void)fitWeapon();
        }
        _window->getGLView()->update();
        return true;
    }
}
