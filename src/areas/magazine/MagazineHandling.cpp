// Which magazine the hands work with, and moving it in the simulation: free
// in the hand, along the insertion rail, or cancelled back to its start.
#include "ReloadMakerController.h"
#include "MagazineReference.h"
#include "ReloadBundle.h"
#include "model/nifmodel.h"


namespace reloadmaker
{
    // The magazine's hand anchor: the configured or installed magazine anchor
    // for rifles and for any weapon with a saved magazine reference, otherwise
    // the rig's magazine track.
    HandPosePlacement ReloadMakerController::simulationMagazineAnchor() const
    {
        if (isRifleConfiguration() || _authoringRoadmaps.value(roadmapKey()).toObject().contains("magazineReference")) {
            const int index = currentMagazineAnchor();
            return index >= 0 ? _handPoseAnchors[index].description : HandPosePlacement{};
        }
        if (_simulationRig && _reloadRecipe) {
            return _reloadRecipe->tracks[_simulationRig->magazine].anchor;
        }
        return {};
    }

    // Index of the magazine's hand anchor, or -1 when it is missing or
    // ambiguous. A saved magazine reference names it. Otherwise it is the one
    // grab-capable anchor that is an installed part (rifles other than the
    // MK18 and embedded SREP magazines) or is named WeaponMagazine.
    int ReloadMakerController::currentMagazineAnchor() const
    {
        const auto saved = _authoringRoadmaps.value(roadmapKey()).toObject().value("magazineReference").toObject();
        if (!saved.isEmpty()) {
            MagazineReference reference;
            QString error;
            if (!_reloadRecipe || !readMagazineReference(saved, reference, error) || reference.recipeId != _reloadRecipe->id) {
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
        const bool installedPart = isRifleConfiguration() && !isMk18Configuration() && !isSrepEmbeddedMagazine();
        int result = -1;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            const auto& anchor = _handPoseAnchors[i].description;
            if (!magazineGrabSupported(anchor)) {
                continue;
            }
            const bool candidate = installedPart ? anchor.anchorKind == "installed-part"
                                                 : anchor.anchorName.compare("WeaponMagazine", Qt::CaseInsensitive) == 0;
            if (!candidate) {
                continue;
            }
            if (result >= 0) {
                return -1;
            }
            result = i;
        }
        return result;
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
