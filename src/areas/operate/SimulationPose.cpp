// Simulation poses: the operating parts, magazine and rounds set over the
// native pose, and which magazine shapes are shown.
//
// Pose targets map a scene node to its pose. Targets are weapon-local unless
// listed as scene targets; the weapon bone's current pose places them.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "design/Theme.h"
#include "glview.h"
#include "gl/glscene.h"
#include "model/nifmodel.h"

#include <QHash>
#include <QSet>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        // NiAVObject flag bit that hides a node.
        constexpr int kHiddenFlag = 1;
        // Authored and native poses closer than this (reloadPoseDistance: game
        // units plus 3 per radian) draw no ghost; the copies would coincide.
        constexpr float kNativeGhostMinimumDistance = 0.05f;

        // Writes a node's flags, and refreshes its drawn node, only when they change.
        void setNodeFlags(NifModel* model, Scene* scene, const QModelIndex& index, int flags)
        {
            if (model->get<int>(index, "Flags") == flags) {
                return;
            }
            model->set<int>(index, "Flags", flags);
            if (auto* node = scene->getNode(model, index)) {
                node->update(model, index);
            }
        }
    }

    // Automatic fire repeats its source clip while the trigger is held.
    float ReloadMakerController::simulationPoseSeconds() const
    {
        if ((isRifleConfiguration() || _simulationAmmo.automatic) && _simulation.action == SimulationAction::Fire && _clip.durationSeconds > 0) {
            return std::fmod(_simulation.seconds, _clip.durationSeconds);
        }
        return _simulation.seconds;
    }

    // Sets the simulated parts over the native pose already in _boundLocalPose.
    bool ReloadMakerController::applySimulationPose()
    {
        _simulationStaticLocalPose.clear();
        _ghostNatives.clear();
        if (!_simulationActive || !_simulationRig || !_reloadRecipe) {
            refreshNativeGhost({});
            return true;
        }
        // A reload started with the action locked open keeps it open until
        // the reload cycles it.
        const bool holdOpen = _simulation.action == SimulationAction::Reload && _simulation.reloadStartedOpen && !_simulation.reloadCycled;
        // Otherwise the native reload clip poses the parts; only an AR-15's
        // bolt is still set.
        if (_simulation.action == SimulationAction::Reload && !holdOpen && !isAr15Configuration()) {
            refreshNativeGhost({});
            return true;
        }
        QVector<Transformf> local;
        QVector<Transformf> world;
        if (!nativeMechanismScene(_reloadScenePlan, _nodeBindings, _boundLocalPose, local, world, _poseError)) {
            return false;
        }
        QHash<int, Transformf> targets;
        addSlideTargets(targets, holdOpen);
        if (isAr15Configuration()) {
            addAr15Targets(targets, holdOpen);
        }
        if (isM1AConfiguration()) {
            addM1ATargets(targets, holdOpen);
        }
        if (isBoltActionConfiguration()) {
            applyBoltActionTargets(targets);
        }
        // A reciprocating bolt fires along its sampled fire cycle, as an AR-15's does.
        if (simulationMechanism() == WeaponMechanism::ReciprocatingBolt && _simulation.action == SimulationAction::Fire) {
            targets.insert(_reloadTrackNodes[_simulationRig->bolt], rifleFireBoltPose());
        }
        if (!addMagazineRoundTargets(targets)) {
            return false;
        }
        QSet<int> sceneTargets;
        addDetachedMagazineTargets(targets, sceneTargets);
        if (!poseSimulationTargets(targets, sceneTargets, local, world)) {
            return false;
        }
        for (int i = 0; i < _nodeBindings.size(); ++i) {
            const int node = _nodeBindings[i].nodeIndex;
            _boundLocalPose[i] = local[node];
            _boundGlobalPose[i] = world[node];
        }
        for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
            if (_assemblyNodes[it.key()].boneIndex < 0) {
                _simulationStaticLocalPose.insert(it.key(), local[it.key()]);
            }
        }
        refreshNativeGhost(targets);
        return true;
    }

    // The simulation pose of an operating track. With the ghost shown, a track
    // whose curve the rig replaces also records its native pose at the same
    // recipe frame.
    Transformf ReloadMakerController::operatingTrackTarget(int track, float progress) const
    {
        const auto& rig = *_simulationRig;
        if (_nativeGhost && simulationTrackAuthored(rig, track)) {
            _ghostNatives.insert(_reloadTrackNodes[track], simulationNativeTrackPose(*_reloadRecipe, rig, track, progress));
        }
        return simulationTrackPose(*_reloadRecipe, rig, track, progress);
    }

    // Each recorded track whose authored pose differs from its native one is
    // drawn again at the native pose: its shapes, up to the next posed node,
    // moved by native x inverse(authored) in world space. Skinned shapes follow
    // their bones and are left out.
    void ReloadMakerController::refreshNativeGhost(const QHash<int, Transformf>& targets)
    {
        QVector<GLView::ReloadMakerHighlight> ghosts;
        auto* model = activeModel();
        if (_nativeGhost && model && !_ghostNatives.isEmpty()) {
            QHash<int, int> posedBlocks; // NIF block of each posed scene node
            for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
                if (it.key() >= 0 && it.key() < _sceneNodes.size() && _sceneNodes[it.key()].isValid()) {
                    posedBlocks.insert(model->getBlockNumber(_sceneNodes[it.key()]), it.key());
                }
            }
            QHash<int, QVector<int>> shapes;
            for (int block = 0; block < model->getBlockCount(); ++block) {
                const auto index = model->getBlockIndex(block);
                if (!model->blockInherits(index, "BSTriShape") && !model->blockInherits(index, "NiGeometry")) {
                    continue;
                }
                auto node = model->getBlockIndex(model->getParent(index));
                for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
                    const auto found = posedBlocks.constFind(model->getBlockNumber(node));
                    if (found != posedBlocks.cend()) {
                        shapes[found.value()].append(block);
                        break;
                    }
                    node = model->getBlockIndex(model->getParent(node));
                }
            }
            const auto weapon = _globalPose[_weaponBoneIndex];
            const QColor color = QColor::fromRgba(design::color::detected);
            for (auto it = _ghostNatives.cbegin(); it != _ghostNatives.cend(); ++it) {
                if (!targets.contains(it.key())) {
                    continue;
                }
                const auto authored = targets.value(it.key());
                if (reloadPoseDistance(authored, it.value()) < kNativeGhostMinimumDistance) {
                    continue;
                }
                Transformf inverse;
                if (!invertUniformTransform(composed(weapon, authored), inverse)) {
                    continue;
                }
                const auto offset = composed(composed(weapon, it.value()), inverse);
                GLView::ReloadMakerHighlight ghost{ shapes.value(it.key()), color, 2.5f, 0.22f, true, {} };
                ghost.worldOffset.rotation.fromQuat(Quat(offset.rotation.w, offset.rotation.x, offset.rotation.y, offset.rotation.z));
                ghost.worldOffset.translation = Vector3(offset.translation.x, offset.translation.y, offset.translation.z);
                ghost.worldOffset.scale = offset.scale.x;
                ghosts.append(std::move(ghost));
            }
        }
        _window->getGLView()->reloadMakerSetGhosts(std::move(ghosts));
    }

    // Slide tracks follow the slide's progress: pistols and reciprocating
    // bolts. A shot that locks the slide back, and a held-open reload, pose
    // them fully open.
    void ReloadMakerController::addSlideTargets(QHash<int, Transformf>& targets, bool holdOpen) const
    {
        const bool slideDriven = !isRifleConfiguration() || simulationMechanism() == WeaponMechanism::ReciprocatingBolt;
        const bool firing = _simulation.action == SimulationAction::Fire && !_simulation.willLockAfterShot();
        if (!slideDriven || firing) {
            return;
        }
        const auto& rig = *_simulationRig;
        const float progress = holdOpen || _simulation.willLockAfterShot() ? 1.f : _simulation.slide;
        for (const int track : rig.slideTracks) {
            // In a held-open reload, magazine tracks follow the reload clip.
            if (holdOpen && rig.magazineTracks.contains(track)) {
                continue;
            }
            targets.insert(_reloadTrackNodes[track], operatingTrackTarget(track, progress));
        }
    }

    // AR-15: the bolt and charging handle move independently. Outside native
    // reloads the handle follows its own input; parts the bolt drives (the
    // rig's other slide tracks) follow the bolt.
    void ReloadMakerController::addAr15Targets(QHash<int, Transformf>& targets, bool holdOpen) const
    {
        const auto& rig = *_simulationRig;
        targets.insert(_reloadTrackNodes[rig.bolt], ar15BoltPose(holdOpen));
        if (_simulation.action == SimulationAction::Reload) {
            return;
        }
        const float handle = _simulation.action == SimulationAction::Fire ? 0.f : _simulation.handle;
        targets.insert(_reloadTrackNodes[rig.slide], operatingTrackTarget(rig.slide, handle));
        const float bolt = _simulation.slideLocked ? 1.f : _simulation.slide;
        for (const int track : rig.slideTracks) {
            if (track != rig.slide) {
                targets.insert(_reloadTrackNodes[track], operatingTrackTarget(track, bolt));
            }
        }
    }

    // A bolt that drives a derived charging handle follows its own reload
    // track, in the clip and when worked by hand; other AR-15 bolts follow
    // the charging handle's travel in the clip and the fire cycle by hand.
    Transformf ReloadMakerController::ar15BoltPose(bool holdOpen) const
    {
        const auto& rig = *_simulationRig;
        const auto& recipe = *_reloadRecipe;
        const bool boltDrivesHandle = !rig.derivedHandle.isEmpty();
        if (_simulation.action == SimulationAction::Fire) {
            return rifleFireBoltPose();
        }
        if (_simulation.action == SimulationAction::Reload && boltDrivesHandle) {
            return holdOpen ? operatingTrackTarget(rig.bolt, 1) : reloadTrackPose(recipe, rig.bolt, _simulation.seconds);
        }
        if (_simulation.action == SimulationAction::Reload) {
            const auto handle = reloadTrackPose(recipe, rig.slide, _simulation.seconds);
            const auto rest = simulationNativeTrackPose(recipe, rig, rig.slide, 0);
            const float travel = reloadPoseDistance(rest, simulationNativeTrackPose(recipe, rig, rig.slide, 1));
            const float progress = holdOpen ? 1.f : std::clamp(reloadPoseDistance(rest, handle) / travel, 0.f, 1.f);
            return rifleBoltPose(progress);
        }
        if (boltDrivesHandle) {
            return operatingTrackTarget(rig.bolt, _simulation.slide);
        }
        return rifleBoltPose(_simulation.slide);
    }

    // M1A: the reciprocating operating handle and the bolt share one progress.
    void ReloadMakerController::addM1ATargets(QHash<int, Transformf>& targets, bool holdOpen) const
    {
        const auto& rig = *_simulationRig;
        float progress = holdOpen ? 1.f : _simulation.slide;
        if (_simulation.action == SimulationAction::Fire) {
            const auto bolt = rifleFireBoltPose();
            targets.insert(_reloadTrackNodes[rig.bolt], bolt);
            const float travel = reloadPoseDistance(_rifleBoltCycle.front(), _rifleBoltCycle[_rifleBoltOpen]);
            progress = std::clamp(reloadPoseDistance(_rifleBoltCycle.front(), bolt) / travel, 0.f, 1.f);
        } else {
            targets.insert(_reloadTrackNodes[rig.bolt], operatingTrackTarget(rig.bolt, progress));
        }
        // The native fire clip omits the operating handle. Its own reload
        // trajectory follows the measured firing bolt stroke, including lock.
        targets.insert(_reloadTrackNodes[rig.slide], operatingTrackTarget(rig.slide, progress));
    }

    // Round shapes inside the magazine, outside native fire and reload clips.
    // Fails with _poseError when the sampled payload frame is not invertible.
    bool ReloadMakerController::addMagazineRoundTargets(QHash<int, Transformf>& targets)
    {
        if (_simulation.action == SimulationAction::Fire || _simulation.action == SimulationAction::Reload) {
            return true;
        }
        const auto& rig = *_simulationRig;
        if (!sampledMagazinePayload()) {
            // A newly seated magazine has a top round, not a magically loaded chamber.
            // The source feed curve takes over only during a completed rack/return cycle.
            if (_simulation.chambered || _simulation.feedCycleActive()) {
                return true;
            }
            for (const auto& visual : _magazineVisuals) {
                if (!visual.empty && visual.track >= 0 && visual.track != rig.magazine) {
                    targets.insert(_reloadTrackNodes[visual.track], _reloadRecipe->tracks[visual.track].poses.front());
                }
            }
            return true;
        }
        const auto& root = _reloadRecipe->tracks[rig.magazine];
        const bool loaded = _simulation.magazineRounds > 0;
        const bool seated = _simulation.magazine.seated();
        // The last reload frame has already fed a round into the chamber.
        // A detached magazine needs its pre-feed round stack at magazine-in.
        int frame = _m1aEmptyMagazineFrame;
        if (loaded) {
            frame = seated ? _reloadRecipe->frames - 1 : _m1aLoadedMagazineFrame;
        }
        const bool feeding = _simulation.feedCycleActive() && seated && loaded;
        const auto parent = feeding ? simulationTrackPose(*_reloadRecipe, rig, rig.magazine, _simulation.slide) : root.poses[frame];
        Transformf inverse;
        if (!invertUniformTransform(parent, inverse)) {
            _poseError = "Invalid magazine payload frame.";
            return false;
        }
        for (const int track : rig.magazineTracks) {
            if (track == rig.magazine) {
                continue;
            }
            const auto child = feeding
                ? simulationTrackPose(*_reloadRecipe, rig, track, _simulation.slide)
                : _reloadRecipe->tracks[track].poses[frame];
            targets.insert(_reloadTrackNodes[track], composed(root.poses.front(), composed(inverse, child)));
        }
        return true;
    }

    // A magazine out of the well, on its insertion line or free in the hand.
    // Its targets are scene poses: the weapon bone does not move it.
    void ReloadMakerController::addDetachedMagazineTargets(QHash<int, Transformf>& targets, QSet<int>& sceneTargets) const
    {
        if (!_magazineRail || _simulation.magazine.seated()) {
            return;
        }
        const auto& rail = *_magazineRail;
        const auto& magazine = _simulation.magazine;
        auto delta = magazineAnimationDelta(rail, magazine.distance);
        if (magazine.phase == MagazinePhase::Free) {
            // A free magazine keeps its rotation at the end of the line, and
            // its handle point moves by the hand offset.
            delta = magazineAnimationDelta(rail, rail.travel);
            const auto turnedHandle = rotated(delta.rotation, { rail.handle.x(), rail.handle.y(), rail.handle.z() });
            delta.translation = {
                rail.handle.x() + magazine.offset.x() - turnedHandle.x,
                rail.handle.y() + magazine.offset.y() - turnedHandle.y,
                rail.handle.z() + magazine.offset.z() - turnedHandle.z,
            };
        }
        const bool sampledPayload = sampledMagazinePayload();
        for (const int track : _simulationRig->magazineTracks) {
            const int node = _reloadTrackNodes[track];
            const auto pose = sampledPayload && targets.contains(node) ? targets.value(node) : _reloadRecipe->tracks[track].poses.front();
            targets.insert(node, composed(delta, pose));
            sceneTargets.insert(node);
        }
    }

    // Applies the targets down the scene plan: a targeted node takes its pose
    // and its local transform is solved against its parent; other nodes keep
    // their local transform under their parent.
    bool ReloadMakerController::poseSimulationTargets(const QHash<int, Transformf>& targets, const QSet<int>& sceneTargets,
                                                      QVector<Transformf>& local, QVector<Transformf>& world)
    {
        const auto weapon = _globalPose[_weaponBoneIndex];
        for (const int node : _reloadScenePlan.order) {
            const int parent = _assemblyNodes[node].parent;
            if (!targets.contains(node)) {
                world[node] = parent >= 0 ? composed(world[parent], local[node]) : local[node];
                continue;
            }
            const auto target = sceneTargets.contains(node) ? targets.value(node) : composed(weapon, targets.value(node));
            Transformf inverse;
            if (parent >= 0 && !invertUniformTransform(world[parent], inverse)) {
                _poseError = "Simulation parent is not invertible.";
                return false;
            }
            local[node] = parent >= 0 ? composed(inverse, target) : target;
            world[node] = target;
        }
        return true;
    }

    // Hides the magazine roots while the magazine is out of view, and each
    // magazine shape the simulation state does not show.
    void ReloadMakerController::applySimulationVisibility()
    {
        auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        if (!model || !scene) {
            return;
        }
        const QSignalBlocker blocker(model);
        const bool magazineHidden = _simulationActive && !_simulation.magazine.visible();
        for (const auto& [index, flags] : _simulationVisibility) {
            if (index.isValid()) {
                setNodeFlags(model, scene, index, magazineHidden ? flags | kHiddenFlag : flags);
            }
        }
        for (const auto& visual : _magazineVisuals) {
            if (visual.node.isValid()) {
                setNodeFlags(model, scene, visual.node, magazineVisualHidden(visual) ? visual.flags | kHiddenFlag : visual.flags);
            }
        }
        _window->getGLView()->update();
    }

    // Outside native reloads the simulation state decides: round shapes need
    // rounds, the empty and full model versions follow the round count, and
    // shapes only the native clip shows stay hidden. During a native reload
    // the clip's cull events decide.
    bool ReloadMakerController::magazineVisualHidden(const MagazineVisual& visual) const
    {
        if (!_simulationActive) {
            return false;
        }
        if (_simulation.action == SimulationAction::Reload) {
            return _reloadRecipe && reloadTrackCulled(visual.track);
        }
        const bool empty = _simulation.magazineRounds == 0;
        bool noRound = empty;
        if (!isRifleConfiguration() && !isBoltActionConfiguration() && _simulation.magazine.seated()) {
            // A pistol's seated magazine keeps its round shape while a round is chambered.
            noRound = empty && !_simulation.chambered;
        }
        const bool separateVersions = !_magazineRail || !_magazineRail->sharedBody();
        const bool wrongVersion = separateVersions && visual.empty != empty;
        return (visual.cartridge ? noRound : wrongVersion) || visual.nativeOnly;
    }

    // Evaluate source visibility afresh at the current time so reverse
    // sampling and action switches never retain an earlier cull event. A
    // track is culled when it or a parent track is.
    bool ReloadMakerController::reloadTrackCulled(int track) const
    {
        for (; track >= 0; track = _reloadRecipe->tracks[track].parent) {
            const auto& name = _reloadRecipe->tracks[track].anchor.anchorName;
            bool culled = false;
            for (const auto& event : _clip.annotations) {
                if (event.timeSeconds <= _simulation.seconds) {
                    if (event.text.compare("cullbone." + name, Qt::CaseInsensitive) == 0) {
                        culled = true;
                    }
                    if (event.text.compare("uncullbone." + name, Qt::CaseInsensitive) == 0) {
                        culled = false;
                    }
                }
            }
            if (culled) {
                return true;
            }
        }
        return false;
    }
}
