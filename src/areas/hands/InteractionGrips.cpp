// The hands used in Operate: which hand reloads (the other holds the firing
// grip), the authored finger pose on the operating part (fingers only; the
// wrist stays live, and an unassigned hand mirrors the other's fingers),
// grabbing the operating part, and the interaction grips exported to packs.
#include "ReloadMakerController.h"
#include "ReloadBundle.h"

#include <QSet>
#include <algorithm>
#include <limits>

namespace reloadmaker
{
    // Assignments are per weapon, operating group and hand; the MK18 and SREP
    // also key them by the operating part's model.
    QString ReloadMakerController::operatingGripKey(PoseHand hand) const
    {
        QString key = poseRecordKey(currentWeapon()->formId) + "|" + operatingGroup();
        const int anchor = operatingAnchor();
        if ((isMk18Configuration() || isSrepConfiguration()) && anchor >= 0) {
            key += "|" + normalizeNifPath(_handPoseAnchors[anchor].description.ownerModel);
        }
        return handAssignmentKey(key, hand);
    }

    // The operating finger pose for each hand: its own assignment, else the
    // other hand's mirrored. The source can be a saved hand pose or a grip.
    void ReloadMakerController::resolveOperatingFingers()
    {
        _operatingFingers = {};
        _operatingFingerError.clear();
        showOperatingFingerChoices();
        if (!currentWeapon() || operatingAnchor() < 0) {
            return;
        }
        for (const auto hand : { PoseHand::Left, PoseHand::Right }) {
            auto id = _savedOperatingGrips.value(operatingGripKey(hand));
            if (id.isEmpty()) {
                id = _savedOperatingGrips.value(operatingGripKey(hand == PoseHand::Left ? PoseHand::Right : PoseHand::Left));
            }
            if (id.isEmpty()) {
                continue;
            }
            const HandPose* source = nullptr;
            for (const auto& grip : _partGrips) {
                if (grip.pose.id == id) {
                    source = &grip.pose;
                }
            }
            for (const auto& pose : _savedHandPoses) {
                if (pose.id == id) {
                    source = &pose;
                }
            }
            if (!source) {
                _operatingFingerError = "Assigned operating finger pose is missing from the library.";
                return;
            }
            HandPose pose;
            if (!handPoseFingersFor(_skeleton, *source, hand, pose, _operatingFingerError)) {
                return;
            }
            pose.source["operatingMirrored"] = source->hand != hand;
            _operatingFingers[int(hand)] = std::move(pose);
        }
    }

    // Assigns a saved pose's fingers to the reload hand on the operating part
    // and saves the project; a failure restores the previous assignment.
    bool ReloadMakerController::useOperatingFingers(const QString& id)
    {
        if (!_simulationActive || operatingAnchor() < 0 || _simulation.action != SimulationAction::Idle || _simulation.magazine.carried()) {
            setStatus("Release the operating part and magazine before selecting a finger pose.", true);
            return false;
        }
        const bool found = std::any_of(_savedHandPoses.cbegin(), _savedHandPoses.cend(), [&](const auto& p) { return p.id == id; }) ||
                           std::any_of(_partGrips.cbegin(), _partGrips.cend(), [&](const auto& g) { return g.pose.id == id; });
        if (!found) {
            setStatus("Select a saved finger pose from the library.", true);
            return false;
        }
        const auto key = operatingGripKey(_interactionHand);
        const auto previous = _savedOperatingGrips.value(key);
        _savedOperatingGrips[key] = id;
        resolveOperatingFingers();
        if (!_operatingFingerError.isEmpty() || !saveSession()) {
            const auto error = _operatingFingerError.isEmpty() ? _lastError : _operatingFingerError;
            if (previous.isEmpty()) {
                _savedOperatingGrips.remove(key);
            } else {
                _savedOperatingGrips[key] = previous;
            }
            resolveOperatingFingers();
            setStatus(error, true);
            return false;
        }
        updateInteractionGripUi();
        ++_revision;
        setStatus("Operating fingers saved. Wrist angle stays live; an unassigned opposite hand uses the mirrored finger shape.");
        return true;
    }

    QString ReloadMakerController::operatingGroup() const
    {
        if (isAr15Configuration()) {
            return "charging_handle";
        }
        if (isBoltActionConfiguration() || isThanatosConfiguration() || isSrepConfiguration()) {
            return "bolt";
        }
        return isM1AConfiguration() ? "operating_handle" : "slide";
    }

    // The hand anchor of the rig's operating part, or -1.
    int ReloadMakerController::operatingAnchor() const
    {
        if (!_simulationRig || !_reloadRecipe) {
            return -1;
        }
        const auto& target = _reloadRecipe->tracks[_simulationRig->slide].anchor;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            if (sameGripAnchor(target, _handPoseAnchors[i].description)) {
                return i;
            }
        }
        return -1;
    }

    // Switches the reload hand when nothing is held. The magazine fit and
    // operating fingers follow; an open magazine fit reopens for the hand.
    bool ReloadMakerController::setInteractionHand(PoseHand hand)
    {
        if (isGlockConfiguration() && hand != PoseHand::Left) {
            setStatus("The Glock uses a right firing hand and left reload hand.", true);
            return false;
        }
        if (_simulation.action != SimulationAction::Idle || _simulation.magazine.carried()) {
            setStatus("Release the part or finish magazine placement before switching hands.", true);
            return false;
        }
        rememberMagazineGrip();
        _interactionHand = hand;
        noteDocumentChange("interaction hand");
        resolveMagazineGrabGrip();
        resolveOperatingFingers();
        if (_workspace == GripsWorkspace && _editingMagazine) {
            _partGripDraft.reset();
            return editMagazineGrip();
        }
        if (_clip.valid()) {
            (void)applyPose(_playbackTime);
        }
        updateInteractionGripUi();
        return true;
    }

    // Creates the opposite hand's magazine fit by mirroring the current one
    // about the magazine and assigns it.
    bool ReloadMakerController::mirrorMagazineGrip()
    {
        const int anchor = currentMagazineAnchor();
        if (anchor < 0 || isGlockConfiguration()) {
            setStatus("Opposite-hand magazines are available on the rifle testbeds.", true);
            return false;
        }
        QString error;
        const auto weapon = poseRecordKey(currentWeapon()->formId);
        const auto source = magazineGripFor(weapon, _handPoseAnchors[anchor].description, _partGrips, _magazineGrabGrips, _magazineGrabDefaults, error, _interactionHand);
        if (!source) {
            setStatus(error.isEmpty() ? "Save a magazine fit for this hand first." : error, true);
            return false;
        }
        Transformf world;
        Transformf inverse;
        if (!sceneNodeWorld(_handPoseAnchors[anchor].node, world, error) || !invertUniformTransform(_globalPose[_weaponBoneIndex], inverse)) {
            setStatus(error, true);
            return false;
        }
        auto fit = *source;
        HandPose mirrored;
        if (!mirrorHandPose(_skeleton, fit.pose, composed(inverse, world), mirrored, error)) {
            setStatus(error, true);
            return false;
        }
        fit.pose = mirrored;
        fit.pose.name = source->pose.name + " · " + (mirrored.hand == PoseHand::Left ? "left" : "right");
        fit.surfaceContact.reset();
        fit.mechanismId.clear();
        _interactionHand = mirrored.hand;
        if (!editMagazineGrip()) {
            return false;
        }
        if (!loadGripRecord(fit)) {
            return false;
        }
        return useMagazineGrabGrip();
    }

    // The operating part's grab point under the cursor. With authored grab
    // locations, the nearest one for the reload hand within its radius;
    // otherwise any point on the operating part.
    std::optional<QVector3D> ReloadMakerController::hoverOperatingPart(QPointF point)
    {
        _operatingHover = false;
        if (!_simulationActive || !_simulationRig || !_reloadRecipe || _simulation.action != SimulationAction::Idle || _simulation.magazine.busy()) {
            return {};
        }
        if (!operatingGrabAvailable()) {
            return {};
        }
        int automatic = -1;
        Vec3f hit;
        QString surface;
        QString error;
        QVector<int> candidates;
        if (!pickWeaponPart(point, automatic, hit, surface, candidates, error)) {
            return {};
        }
        const QVector3D hitPoint(hit.x, hit.y, hit.z);
        const auto locations = currentGrabLocations();
        if (_grabLocations.contains(grabLocationsKey(locations))) {
            const int anchor = operatingAnchor();
            if (anchor < 0 || !candidates.contains(anchor)) {
                return {};
            }
            Transformf world;
            if (!sceneNodeWorld(_handPoseAnchors[anchor].node, world, error)) {
                return {};
            }
            const int handBit = _interactionHand == PoseHand::Left ? 1 : 2;
            std::optional<QVector3D> best;
            float distance = std::numeric_limits<float>::infinity();
            for (const auto& location : locations.points) {
                if (!(location.hands & handBit)) {
                    continue;
                }
                const auto v = composed(world, Transformf{ location.point }).translation;
                const QVector3D position(v.x, v.y, v.z);
                const float d = (position - hitPoint).lengthSquared();
                if (d <= location.radius * location.radius && d < distance) {
                    best = position;
                    distance = d;
                }
            }
            _operatingHover = best.has_value();
            return best;
        }
        const auto& input = _reloadRecipe->tracks[_simulationRig->slide].anchor;
        for (const int i : candidates) {
            if (sameGripAnchor(_handPoseAnchors[i].description, input)) {
                _operatingHover = true;
                return hitPoint;
            }
        }
        return {};
    }

    // Grabbing needs valid operating fingers and, with authored grab
    // locations, at least one for the reload hand.
    bool ReloadMakerController::operatingGrabAvailable() const
    {
        const auto locations = currentGrabLocations();
        const int handBit = _interactionHand == PoseHand::Left ? 1 : 2;
        const bool hasLocation = std::any_of(locations.points.cbegin(), locations.points.cend(), [&](const auto& p) { return p.hands & handBit; });
        return currentWeapon() && _operatingFingerError.isEmpty() && (!_grabLocations.contains(grabLocationsKey(locations)) || hasLocation);
    }

    QJsonObject ReloadMakerController::interactionGripState() const
    {
        const auto& fingers = _operatingFingers[int(_interactionHand)];
        return {
            { "hand", _interactionHand == PoseHand::Left ? "left" : "right" },
            { "support", _interactionHand == PoseHand::Left ? "right-firing-grip" : "left-support-grip" },
            { "group", operatingGroup() },
            { "canGrab", operatingGrabAvailable() },
            { "posePolicy", "authored-fingers-live-wrist" },
            { "authored", bool(fingers) },
            { "gripId", fingers ? fingers->id : QString() },
            { "applied", bool(fingers) && _simulation.action == SimulationAction::Held },
            { "error", _operatingFingerError },
            { "markerVisible", _operatingHover },
        };
    }

    // Version 3: operating finger locals per hand, the right firing hand, and
    // the authored grab locations when there are any.
    QJsonObject ReloadMakerController::exportInteractionGrips() const
    {
        if (!_reloadRecipe || !currentWeapon() || !_operatingFingerError.isEmpty()) {
            return {};
        }
        QJsonArray hands;
        for (const auto& pose : _operatingFingers) {
            if (!pose) {
                continue;
            }
            QString error;
            QJsonArray joints;
            for (const auto& finger : pose->fingers) {
                QJsonObject joint;
                if (!encodePoseTransform(finger, joint, error)) {
                    return {};
                }
                joints.append(joint);
            }
            hands.append(QJsonObject{
                { "hand", pose->hand == PoseHand::Left ? "left" : "right" },
                { "sourcePoseId", pose->id },
                { "name", pose->name },
                { "fingerLocalTransformMask", 0x7fff },
                { "fingerLocalTransforms", joints },
            });
        }
        QJsonObject result{
            { "version", 3 },
            { "firingHand", "right" },
            { "operatingPosePolicy", "authored-fingers-live-wrist" },
            { "otherHandPolicy", "mirror-fingers" },
            { "operatingFingers", hands },
        };
        const auto locations = exportGrabLocations();
        if (!locations.isEmpty()) {
            result["grabLocations"] = locations;
        }
        return result;
    }
}
