// Which magazine the hands work with, and moving it in the simulation: free
// in the hand, along the insertion rail, or cancelled back to its start.
#include "ReloadMakerController.h"
#include "MagazineReference.h"
#include "areas/loadout/SceneTree.h"
#include "model/nifmodel.h"

#include <QTimer>
#include <algorithm>


namespace reloadmaker
{
    // The magazine's hand anchor: the configured or installed magazine anchor
    // when that anchor decides the magazine, otherwise the rig's magazine
    // track.
    HandPosePlacement ReloadMakerController::simulationMagazineAnchor() const
    {
        if (magazineAnchorDecides()) {
            const int index = currentMagazineAnchor();
            return index >= 0 ? _handPoseAnchors[index].description : HandPosePlacement{};
        }
        if (_simulationRig && _reloadRecipe) {
            return _reloadRecipe->tracks[_simulationRig->magazine].anchor;
        }
        return {};
    }

    // Whether the hand anchor decides the magazine rather than the rig's
    // magazine track: rifles, a saved magazine reference, and a magazine
    // mounted into the receiver's bone. The last is the usual pistol build
    // (vanilla's included); the track there belongs to the receiver, whose
    // record cannot name the magazine.
    bool ReloadMakerController::magazineAnchorDecides() const
    {
        if (isRifleConfiguration() || _authoringRecords.value(authoringRecordKey()).toObject().contains("magazineReference")) {
            return true;
        }
        const int index = currentMagazineAnchor();
        return index >= 0 && _handPoseAnchors[index].description.anchorKind == QLatin1String("installed-part");
    }

    // Index of the magazine's hand anchor, or -1 when it is missing or
    // ambiguous. A saved magazine reference names it. Otherwise, when the
    // magazine carrier belongs to the frame (the receiver the export names,
    // or the weapon's own model), the magazine is the part mounted into the
    // carrier: the outermost installed part inside it. A magazine model that
    // brings its own carrier, or one built into the receiver with nothing
    // mounted, is grabbed by that carrier. Parts nested in a magazine (the
    // Timberwolf's rounds model) are never the magazine.
    int ReloadMakerController::currentMagazineAnchor() const
    {
        const auto saved = _authoringRecords.value(authoringRecordKey()).toObject().value("magazineReference").toObject();
        if (!saved.isEmpty()) {
            MagazineReference reference;
            QString error;
            if (!_reloadRecipe || !readMagazineReference(saved, reference, error)) {
                return -1;
            }
            int found = -1;
            for (int i = 1; i < _handPoseAnchors.size(); ++i) {
                if (magazineReferenceKey(_handPoseAnchors[i].description) != reference.anchorKey) {
                    continue;
                }
                if (found >= 0 || !magazineReferenceCandidate(i)) {
                    return -1;
                }
                found = i;
            }
            return found;
        }
        auto* model = activeModel();
        if (!model) {
            return -1;
        }
        // The carrier is the rig's magazine track, so the hand anchor and the
        // exported identity are the same node even when the operating setup
        // picks a helper pivot (the USP's WeaponMagazineChild2). Before the
        // reload is prepared, the WeaponMagazine bone.
        QVector<int> carriers;
        const bool rigged = _simulationRig && _reloadRecipe && _simulationRig->magazine >= 0 && _simulationRig->magazine < _reloadRecipe->tracks.size();
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            const auto& anchor = _handPoseAnchors[i].description;
            if (rigged ? sameMechanismPart(anchor, _reloadRecipe->tracks[_simulationRig->magazine].anchor)
                       : anchor.anchorName.compare("WeaponMagazine", Qt::CaseInsensitive) == 0) {
                carriers.append(i);
            }
        }
        const auto* weapon = currentWeapon();
        const auto* receiver = resolvedReceiverObjectMod();
        const QString weaponKey = weapon ? poseRecordKey(weapon->formId) : QString();
        const QString receiverKey = receiver ? poseRecordKey(receiver->formId) : QString();
        const auto framed = [&](int carrier) {
            const auto& owner = _handPoseAnchors[carrier].description.ownerRecord;
            return !owner.isEmpty() && (owner.compare(weaponKey, Qt::CaseInsensitive) == 0 || owner.compare(receiverKey, Qt::CaseInsensitive) == 0);
        };
        const auto inside = [&](int part, int container) {
            return _handPoseAnchors[part].node != _handPoseAnchors[container].node &&
                   isWithinBlock(*model, _handPoseAnchors[part].node, _handPoseAnchors[container].node);
        };
        const auto installedPart = [&](int i) { return _handPoseAnchors[i].description.anchorKind == QLatin1String("installed-part"); };
        int installed = -1;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            if (!installedPart(i)) {
                continue;
            }
            const auto carrier = std::find_if(carriers.cbegin(), carriers.cend(), [&](int c) { return framed(c) && inside(i, c); });
            if (carrier == carriers.cend()) {
                continue;
            }
            bool nested = false;
            for (int j = 0; j < _handPoseAnchors.size() && !nested; ++j) {
                nested = j != i && installedPart(j) && inside(j, *carrier) && inside(i, j);
            }
            if (nested) {
                continue;
            }
            if (installed >= 0) {
                return -1;
            }
            installed = i;
        }
        if (installed >= 0) {
            return installed;
        }
        return carriers.size() == 1 ? carriers.front() : -1;
    }

    // Block names from the root down to `leaf`, joined by '/'. An unnamed
    // block appears as unnamed:<block number>.
    QString ReloadMakerController::magazinePiecePath(const QModelIndex& leaf) const
    {
        auto* model = activeModel();
        if (!model) {
            return {};
        }
        QStringList path;
        auto node = leaf;
        for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
            auto name = model->get<QString>(node, "Name");
            if (name.isEmpty()) {
                name = QString("unnamed:%1").arg(model->getBlockNumber(node));
            }
            path.prepend(name);
            node = model->getBlockIndex(model->getParent(node));
        }
        return path.join('/');
    }

    // Whether the rounds inside the magazine are posed from sampled payload
    // frames relative to its carrier (the saved setup decides; the M1A
    // testbed does without one).
    bool ReloadMakerController::sampledMagazinePayload() const
    {
        return _magazineSetup ? _magazineSetup->sampledPayload : isM1AConfiguration();
    }

    // Shows the magazine in Operate outside the weapon, full or empty, so its
    // presentation can be checked. Needs the magazine rail.
    void ReloadMakerController::previewMagazineOutside(bool empty)
    {
        if (!_magazineRail) {
            return;
        }
        setWorkspace(OperateWorkspace);
        _simulationTimer->stop();
        _simulation.reset();
        _simulation.magazine.phase = MagazinePhase::OutsideEmpty;
        _simulation.magazine.distance = _magazineRail->travel;
        _simulation.magazine.offset = magazineInputOffset(*_magazineRail, _magazineRail->travel);
        _simulation.magazineRounds = empty ? 0 : _simulationAmmo.capacity;
        updateMagazinePose();
    }

    // Re-poses the simulation after the magazine moved.
    void ReloadMakerController::updateMagazinePose()
    {
        if (!_simulationActive) {
            return;
        }
        if (!applyPose(0)) {
            _simulationError = _lastError;
            stopSimulation();
        }
        updateSimulationSounds();
        applySimulationVisibility();
        updateSimulationControls();
        ++_revision;
    }

    void ReloadMakerController::moveFreeMagazine(QVector3D offset)
    {
        if (!_simulationActive || !_magazineRail) {
            return;
        }
        _simulation.magazine.moveFree(*_magazineRail, offset);
        updateMagazinePose();
    }

    void ReloadMakerController::moveMagazineRail(float distance)
    {
        if (!_simulationActive || !_magazineRail) {
            return;
        }
        const bool wasSeated = _simulation.magazine.seated();
        _simulation.magazine.moveRail(*_magazineRail, distance);
        updateMagazinePose();
        if (!wasSeated && _simulation.magazine.seated()) {
            setStatus("Full magazine seated.");
        }
    }

    void ReloadMakerController::cancelMagazine()
    {
        _simulation.cancelMagazine();
        resetSoundTracking();
        updateMagazinePose();
        setStatus("Magazine placement cancelled.");
    }
}
