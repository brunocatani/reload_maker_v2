// Scene plumbing for assembly: decoded source NIF and clip caches, copying
// NIF branches into the render model, finding nodes and connect points, and
// reading and writing node transforms.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "areas/loadout/LoadoutNames.h"
#include "data/niftypes.h"
#include "gamemanager.h"
#include "gl/glscene.h"
#include "glview.h"
#include "model/nifmodel.h"
#include "spells/blocks.h"
#include "SlotOverlay.h"
#include "viewport/PartPicker.h"

#include <QBuffer>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMimeData>
#include <QSlider>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace reloadmaker
{
    NifModel* ReloadMakerController::activeModel() const
    {
        if (!_window || !_window->getGLView() || !_window->getGLView()->getScene()) {
            return nullptr;
        }
        return const_cast<NifModel*>(_window->getGLView()->getScene()->nifModel);
    }

    QString ReloadMakerController::normalizeNifPath(const QString& path) const
    {
        QString result = ResourceArchive::normalizeResourcePath(path);
        const qsizetype meshes = result.indexOf(QStringLiteral("meshes/"), 0,
                                                Qt::CaseInsensitive);
        if (meshes > 0) {
            result.remove(0, meshes);
        } else if (meshes < 0) {
            result.prepend(QStringLiteral("meshes/"));
        }
        if (!result.endsWith(QStringLiteral(".nif"), Qt::CaseInsensitive)) {
            result.append(QStringLiteral(".nif"));
        }
        return result;
    }

    bool ReloadMakerController::loadNif(const ResourceArchive& archive,
                                        const QString& resourcePath,
                                        NifModel& destination, QString& error) const
    {
        return loadNifBytes(nifBytes(archive, resourcePath, error), destination, error);
    }

    // Drops everything derived from the current source: decoded caches, the
    // assembled scene and the per-weapon authoring state built on it.
    void ReloadMakerController::clearAssetCaches(bool keepTextures)
    {
        _scopeProfiles.clear();
        _grabLocations.clear();
        _pickingGrabLocation = false;
        _grabLocationCapturePreview.reset();
        _audio.clear();
        _soundTracking = false;
        _soundCatalog.clear();
        _referenceSounds.clear();
        _referenceSoundMappings.clear();
        _referenceSoundKeywords.clear();
        _soundDescriptors.clear();
        _fireSound = {};
        _fireSoundMapping.clear();
        _fireSoundError.clear();
        _fireSoundContext.clear();
        _soundProfile = {};
        _savedSounds.clear();
        _soundsDirty = false;
        invalidateReloadCapture();
        _referenceNames.clear();
        _weaponFitCorners.clear();
        _handPosePreview.reset();
        _partGripDraft.reset();
        _gripPreviewValid = false;
        _gripError.clear();
        _fingerDrafts = {};
        _operateSuspendedGrip.reset();
        clearHandPoseAnchors();
        _playing = false;
        _playbackTimer->stop();
        _rebuildTimer->stop();
        _clip = {};
        _readyClip = {};
        _activeAction.clear();
        _activeAnimationPath.clear();
        _animationActions.clear();
        _animationSearchPaths.clear();
        _nodeBindings.clear();
        _sceneNodes.clear();
        _assemblyNodes.clear();
        stopMechanismPreview();
        _mechanismDraft.reset();
        _mechanismState = {};
        displayMechanism();
        _timeline->setEnabled(false);
        updateTimelineText();
        refreshFingerEditor();
        _nifCache.clear();
        _nifBytesCache.clear();
        _clipCache.clear();
        _assignedMechanism.reset();
        _graphCache.clear();
        _skeleton = {};
        _geometrySignature.clear();
        _renderedWeapon = -1;
        _partRoots.clear();
        _partSurfaces.clear();
        _partAlignmentAnchors.clear();
        _attachmentConnections.clear();
        _remappedAttachments.clear();
        if (_slotOverlay) { _slotOverlay->setCallouts({}); }
        if (_partPicker) { _partPicker->setParts({}); }
        if (auto* model = activeModel()) { model->clear(); }
        if (!keepTextures) {
            _window->getGLView()->flush();
        }
        refreshPartGripChoices();
        refreshPartGripList();
        updateHandPoseControls();
        updateSimulationControls();
    }

    QByteArray ReloadMakerController::nifBytes(const ResourceArchive& archive,
                                               const QString& resourcePath, QString& error) const
    {
        const QString path = normalizeNifPath(resourcePath);
        const QString key = (&archive == &_gameMeshes ? QStringLiteral("game:") : QStringLiteral("mod:")) + path;
        error.clear();
        if (const auto* bytes = _nifBytesCache.object(key)) { return *bytes; }
        const QByteArray bytes = archive.read(path, error);
        if (!bytes.isEmpty()) {
            _nifBytesCache.insert(key, new QByteArray(bytes), std::max(1, int((bytes.size() + 1023) / 1024)));
        }
        return bytes;
    }

    QSharedPointer<NifModel> ReloadMakerController::sourceNif(const ResourceArchive& archive,
                                                              const QString& resourcePath, QString& error)
    {
        const QString path = normalizeNifPath(resourcePath);
        const QString key = (&archive == &_gameMeshes ? QStringLiteral("game:") : QStringLiteral("mod:")) + path;
        error.clear();
        if (const auto* model = _nifCache.object(key)) { return *model; }
        QElapsedTimer decodeClock;
        decodeClock.start();
        auto model = QSharedPointer<NifModel>::create();
        if (!loadNif(archive, path, *model, error)) { return {}; }
        _decodeMilliseconds += decodeClock.elapsed();
        ++_sourceMisses;
        _nifCache.insert(key, new QSharedPointer<NifModel>(model));
        return model;
    }

    HkxClip ReloadMakerController::cachedClip(const QString& resourcePath, QString& error)
    {
        const QString path = ResourceArchive::normalizeResourcePath(resourcePath);
        error.clear();
        if (const auto* clip = _clipCache.object(path)) { return *clip; }
        const QByteArray bytes = readAnimationResource(path, error);
        if (bytes.isEmpty()) { return {}; }
        HkxClip clip = HkxReader::readClip(bytes, error);
        if (clip.valid()) {
            ++_sourceMisses;
            _clipCache.insert(path, new HkxClip(clip));
        }
        return clip;
    }

    bool ReloadMakerController::loadNifBytes(const QByteArray& bytes, NifModel& destination,
                                             QString& error) const
    {
        if (bytes.isEmpty()) {
            if (error.isEmpty()) {
                error = QStringLiteral("NIF resource is empty");
            }
            return false;
        }
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly) || !destination.load(buffer)) {
            error = QStringLiteral("NifSkope could not parse the NIF resource");
            return false;
        }
        return true;
    }

    QPersistentModelIndex ReloadMakerController::pasteRoot(NifModel& source,
                                                           const QModelIndex& parent,
                                                           const QString& rootName)
    {
        const QList<int> roots = source.getRootLinks();
        if (roots.isEmpty()) {
            return {};
        }
        const QModelIndex sourceRoot = source.getBlockIndex(roots.first());
        if (!sourceRoot.isValid()) {
            return {};
        }
        // The interactive clipboard paste rebuilds every block under the live
        // model's observers. Whole single-root assets can instead be decoded in
        // isolation and transferred by NifSkope with its native link/string map.
        // Partial/multi-root imports retain branch selection semantics.
        NifModel* destination = activeModel();
        if (!destination || !parent.isValid() || source.getVersionNumber() != destination->getVersionNumber()) { return {}; }
        const int firstBlock = destination->getBlockCount();
        if (roots.size() != 1) {
            const auto root = pasteBranch(source, sourceRoot, parent, rootName);
            if (!root.isValid() || destination->getBlockNumber(root) < firstBlock) { return {}; }
            registerAttachmentModel(root, firstBlock, destination->getBlockCount());
            return root;
        }
        QElapsedTimer transferClock;
        transferClock.start();
        QByteArray bytes;
        QBuffer output(&bytes);
        if (!output.open(QIODevice::WriteOnly) || !source.save(output)) { return {}; }
        NifModel transfer;
        QString error;
        if (!loadNifBytes(bytes, transfer, error)) { return {}; }
        _copyMilliseconds += transferClock.restart();
        QPersistentModelIndex root;
        {
            // Native block transfer updates many string/link fields. Defer its
            // repeated header/link scans until the complete asset is attached;
            // structural model notifications remain enabled for Qt observers.
            const bool held = destination->holdUpdates(true);
            destination->setState(BaseModel::Processing);
            const auto restore = qScopeGuard([destination, held]() {
                destination->restoreState();
                (void)destination->holdUpdates(held);
            });
            const auto blockMap = transfer.moveAllNiBlocks(destination);
            if (!blockMap.contains(roots.first())) { return {}; }
            root = destination->getBlockIndex(blockMap.value(roots.first()));
            if (!rootName.isEmpty()) { destination->set<QString>(root, "Name", rootName); }
            blockLink(destination, parent, root);
        }
        _pasteMilliseconds += transferClock.elapsed();
        registerAttachmentModel(root, firstBlock, destination->getBlockCount());
        return root;
    }

    QPersistentModelIndex ReloadMakerController::pasteBranch(NifModel& source,
                                                             const QModelIndex& sourceRoot,
                                                             const QModelIndex& parent,
                                                             const QString& rootName)
    {
        NifModel* destination = activeModel();
        if (!sourceRoot.isValid() || !parent.isValid() || !destination || source.getVersionNumber()!=destination->getVersionNumber()) {
            return {};
        }
        const QString originalName = source.get<QString>(sourceRoot, "Name");
        if (!rootName.isEmpty()) { source.set<QString>(sourceRoot, "Name", rootName); }
        spCopyBranch copy;
        QMimeData payload;
        QElapsedTimer transferClock;
        transferClock.start();
        copy.copyData(&source, sourceRoot, payload);
        _copyMilliseconds += transferClock.restart();
        if (!rootName.isEmpty()) { source.set<QString>(sourceRoot, "Name", originalName); }
        if(payload.formats().isEmpty())return {};
        spPasteBranch paste;
        const QPersistentModelIndex result(paste.pasteData(destination, parent, payload));
        _pasteMilliseconds += transferClock.elapsed();
        return result;
    }

    QPersistentModelIndex ReloadMakerController::findNode(const QString& name,
                                                          const int firstBlock,
                                                          int lastBlock) const
    {
        NifModel* model = activeModel();
        if (!model) {
            return {};
        }
        if (lastBlock < 0 || lastBlock > model->getBlockCount()) {
            lastBlock = model->getBlockCount();
        }
        for (int block = (std::max)(0, firstBlock); block < lastBlock; ++block) {
            const QModelIndex index = model->getBlockIndex(block);
            if (index.isValid() && model->blockInherits(index, "NiNode") &&
                model->get<QString>(index, "Name").compare(name, Qt::CaseInsensitive) == 0) {
                return QPersistentModelIndex(index);
            }
        }
        return {};
    }

    QStringList ReloadMakerController::childConnectPoints(const NifModel& source) const
    {
        QStringList result;
        for (int block = 0; block < source.getBlockCount(); ++block) {
            const QModelIndex index = source.getBlockIndex(block);
            if (!index.isValid() || source.itemName(index) != QStringLiteral("BSConnectPoint::Children")) {
                continue;
            }
            const QModelIndex points = source.getIndex(index, "Point Name");
            for (int row = 0; row < source.rowCount(points); ++row) {
                const QString point = source.get<QString>(source.getIndex(points, row));
                if (!point.isEmpty()) {
                    result.append(point);
                }
            }
        }
        return result;
    }

    void ReloadMakerController::registerAttachmentModel(const QModelIndex& root, int firstBlock, int lastBlock)
    {
        const auto* model = activeModel();
        if (!model || !root.isValid()) { return; }
        AttachmentModelScope scope;
        scope.root = model->getBlockNumber(root);
        for (int block = firstBlock; block < lastBlock; ++block) {
            const auto index = model->getBlockIndex(block);
            if (model->blockInherits(index, "NiNode")) {
                scope.nodes.append({block, model->get<QString>(index, "Name")});
            }
            if (model->itemName(index) != QStringLiteral("BSConnectPoint::Parents")) { continue; }
            const auto points = model->getIndex(index, "Connect Points");
            for (int row = 0; row < model->rowCount(points); ++row) {
                const auto point = model->getIndex(points, row);
                const auto t = model->get<Vector3>(point, "Translation");
                const auto q = model->get<Quat>(point, "Rotation");
                const float scale = model->get<float>(point, "Scale");
                scope.points.append({model->get<QString>(point, "Name"), model->get<QString>(point, "Parent"),
                    {{t[0], t[1], t[2]}, {q[1], q[2], q[3], q[0]}, {scale, scale, scale}}});
            }
        }
        // A CPA frame may name a rigid shape instead of a node (the SREP Honey
        // Badger's "Upper"). Shapes cannot own pasted branches, so carry the
        // exact shape-local transform into the connection and attach beneath
        // the shape's real parent node. A node of the same name still wins.
        for (auto& point : scope.points) {
            const bool namesNode = std::any_of(scope.nodes.cbegin(), scope.nodes.cend(), [&](const auto& node) {
                return node.name.compare(point.parentName, Qt::CaseInsensitive) == 0;
            });
            if (point.parentName.isEmpty() || namesNode) {
                continue;
            }
            QModelIndex shape;
            int matches = 0;
            for (int block = firstBlock; block < lastBlock; ++block) {
                const auto index = model->getBlockIndex(block);
                if (model->blockInherits(index, "BSTriShape") &&
                    model->get<QString>(index, "Name").compare(point.parentName, Qt::CaseInsensitive) == 0) {
                    shape = index;
                    ++matches;
                }
            }
            if (matches != 1) {
                continue;
            }
            const auto parent = model->getBlockIndex(model->getParent(shape));
            Transformf local;
            if (parent.isValid() && model->blockInherits(parent, "NiNode") && readNodeTransform(shape, local)) {
                point.local = composed(local, point.local);
                point.parentName = model->get<QString>(parent, "Name");
            }
        }
        if (!scope.points.isEmpty()) { _attachmentConnections.add(std::move(scope)); }
    }

    // Resolves a child connect point to an installed parent point, preferring
    // the declared owner slots' models. While `waitForOwner`, a declared owner
    // that is selected but not yet placed defers the part instead.
    ReloadMakerController::ConnectTarget ReloadMakerController::findConnectTarget(
        const QString& childPoint, const NifModel* incoming, SlotKey slot, bool waitForOwner) const
    {
        ConnectTarget result;
        const auto* model = activeModel();
        if (!model || !childPoint.startsWith(QStringLiteral("C-"), Qt::CaseInsensitive)) {
            result.error = QStringLiteral("Invalid child connection %1").arg(childPoint);
            return result;
        }
        QVector<int> preferredRoots;
        if (const auto* config = configuration()) {
            const auto available = config->availableSlots();
            SlotKey owner = available.value(slot);
            for (int depth = 0; owner && depth < config->entries().size(); ++depth) {
                const auto partRoot = _partRoots.value(owner);
                if (partRoot.isValid()) {
                    preferredRoots.append(model->getBlockNumber(partRoot));
                } else if (const auto* part = config->mod(config->selected(owner)); waitForOwner && part && !part->modelPath.isEmpty()) {
                    // Do not steal another part's identically named connection
                    // while the declared geometric owner is still pending.
                    result.error = QStringLiteral("Waiting for installed owner %1").arg(part->editorId);
                    return result;
                }
                owner = available.value(owner);
            }
        }
        if (_weaponRoot.isValid()) { preferredRoots.append(model->getBlockNumber(_weaponRoot)); }
        const auto match = _attachmentConnections.find(QStringLiteral("P-") + childPoint.mid(2), preferredRoots, result.error);
        const auto* point = _attachmentConnections.point(match);
        if (!point) { return result; }
        result.ownerRoot = _attachmentConnections.ownerRoot(match);
        const QString& parentName = point->parentName;
        const int bone = _skeleton.boneIndex(parentName);
        bool suppliesParent = false;
        if (incoming && bone >= 0 && bone != _weaponBoneIndex && isDescendantOf(bone, _weaponBoneIndex)) {
            for (int block = 0; block < incoming->getBlockCount(); ++block) {
                const auto node = incoming->getBlockIndex(block);
                if (incoming->blockInherits(node, "NiNode") &&
                    incoming->get<QString>(node, "Name").compare(parentName, Qt::CaseInsensitive) == 0) {
                    suppliesParent = true;
                    break;
                }
            }
        }
        if (suppliesParent) {
            // Preserve incoming magazine/cartridge ownership: attach in the
            // semantic parent frame, not under a second copy of its own bone.
            const int parentBone = _skeleton.bones.at(bone).parentIndex;
            if (parentBone >= 0) {
                result.parent = _canonicalNodes.value(normalizedKey(_skeleton.bones.at(parentBone).name));
            }
            result.suppliedBone = bone;
        } else {
            // A parent naming neither a node of the owner model nor a rig bone
            // is a label without an emitted node: a donor model's root name
            // (SREP "HandmadeARReceiver", "AR10 Handguard") or a connection
            // frame label (MK18 Empire "WorkshopConnectPoints"). Its offset is
            // owner-local. A node of that name in the owner model still wins.
            auto canonical = bone >= 0               ? _canonicalNodes.value(normalizedKey(parentName))
                             : !parentName.isEmpty() ? QPersistentModelIndex(model->getBlockIndex(result.ownerRoot))
                                                     : QPersistentModelIndex{};
            const int anchor = _attachmentConnections.anchor(match,
                canonical.isValid() ? model->getBlockNumber(canonical) : -1, result.error);
            if (anchor >= 0) { result.parent = model->getBlockIndex(anchor); }
        }
        if (!result.parent.isValid()) {
            if (result.error.isEmpty()) { result.error = QStringLiteral("Unavailable rig parent for %1").arg(childPoint); }
            return result;
        }
        result.translation = point->local.translation;
        result.rotation = point->local.rotation;
        result.scale = point->local.scale.x;
        return result;
    }

    bool ReloadMakerController::setNodeTransform(const QModelIndex& node,
                                                 const Transformf& transform)
    {
        NifModel* model = activeModel();
        if (!model || !node.isValid()) {
            return false;
        }
        Matrix rotation;
        rotation.fromQuat(Quat(transform.rotation.w, transform.rotation.x,
                               transform.rotation.y, transform.rotation.z));
        const float scale = (transform.scale.x + transform.scale.y + transform.scale.z) / 3.0f;
        return model->set<Vector3>(node, "Translation",
                                   Vector3(transform.translation.x, transform.translation.y,
                                           transform.translation.z)) &&
               model->set<Matrix>(node, "Rotation", rotation) &&
               model->set<float>(node, "Scale", std::isfinite(scale) ? scale : 1.0f);
    }

    bool ReloadMakerController::readNodeTransform(
        const QModelIndex& node, Transformf& transform) const
    {
        NifModel* model = activeModel();
        if (!model || !node.isValid()) {
            return false;
        }
        const Vector3 translation = model->get<Vector3>(node, "Translation");
        const Matrix rotation = model->get<Matrix>(node, "Rotation");
        const Quat quaternion = rotation.toQuat();
        const float scale = model->get<float>(node, "Scale");
        if (!std::isfinite(translation[0]) ||
            !std::isfinite(translation[1]) ||
            !std::isfinite(translation[2]) ||
            !std::isfinite(quaternion[0]) ||
            !std::isfinite(quaternion[1]) ||
            !std::isfinite(quaternion[2]) ||
            !std::isfinite(quaternion[3]) ||
            !std::isfinite(scale) || std::abs(scale) <= 1.0e-6f) {
            return false;
        }
        transform = Transformf{
            Vec3f{ translation[0], translation[1], translation[2] },
            normalized(Quaternionf{
                quaternion[1], quaternion[2], quaternion[3], quaternion[0]
            }),
            Vec3f{ scale, scale, scale },
        };
        return true;
    }

    // Whether `ancestorIndex` is `boneIndex` or one of its skeleton parents.
    bool ReloadMakerController::isDescendantOf(const int boneIndex, const int ancestorIndex) const
    {
        const int bones = int(_skeleton.bones.size());
        if (boneIndex < 0 || boneIndex >= bones || ancestorIndex < 0 || ancestorIndex >= bones) {
            return false;
        }
        int current = boneIndex;
        for (int depth = 0; depth < bones && current >= 0; ++depth) {
            if (current == ancestorIndex) {
                return true;
            }
            current = _skeleton.bones.at(current).parentIndex;
        }
        return false;
    }
}
