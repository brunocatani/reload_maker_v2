#include "ReloadMakerController.h"
#include "commands/CommandSupport.h"

#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>

namespace reloadmaker
{
    using commands::require;

    namespace
    {
        // Finger joint indices 0–14: five fingers with three joints each.
        constexpr int kFingerJoints = 15;

        PoseHand handParameter(const QJsonObject& params, const QString& error)
        {
            const QString side = params["hand"].toString();
            require(side == "left" || side == "right", error);
            return side == "left" ? PoseHand::Left : PoseHand::Right;
        }
    }

    void ReloadMakerController::registerHandCommands()
    {
        _commands.add({ "interaction_hand", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto hand = handParameter(params, "Specify left or right");
                            require(setInteractionHand(hand), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "use_operating_fingers", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            require(useOperatingFingers(params["id"].toString()), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "restore_hand", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto hand = handParameter(params, "Choose left or right");
                            _fingerDrafts[int(hand)].reset();
                            if (_handPosePreview && _handPosePreview->hand == hand) {
                                _handPosePreview.reset();
                            }
                            if (_clip.valid()) {
                                require(applyPose(_playbackTime), _poseError);
                            }
                            refreshFingerEditor();
                            return std::nullopt;
                        } });

        _commands.add({ "edit_fingers", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            editFingerJoints(params);
                            return std::nullopt;
                        } });

        _commands.add({ "save_hand_pose", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            saveHandPoseFromCommand(params);
                            return std::nullopt;
                        } });

        _commands.add({ "start_grip", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const auto pose = knownHandPoseId(params["poseId"].toString());
                            const int anchor = params["anchor"].toInt(-1);
                            require(!pose.isEmpty() && anchor >= 0 && anchor < _handPoseAnchors.size(), "Unknown saved pose or reference part");
                            setWorkspace(2);
                            setHandEditorPage(HandEditorPage::PartGrip);
                            setGripPose(pose);
                            setGripAnchor(anchor);
                            startPartGrip(params["captured"].toBool(true));
                            return std::nullopt;
                        } });

        _commands.add({ "set_grip", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            setGripPlacement(params);
                            return std::nullopt;
                        } });

        _commands.add({ "save_grip", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject&) -> std::optional<QJsonObject> {
                            require(_partGripDraft.has_value(), "No grip draft");
                            require(savePartGrip(), _lastError);
                            return std::nullopt;
                        } });

        _commands.add({ "select_part", CommandAccess::Edit, CommandGate::WhenIdle,
                        [this](const QJsonObject& params) -> std::optional<QJsonObject> {
                            const int anchor = params["anchor"].toInt(-1);
                            require(anchor > 0 && anchor < _handPoseAnchors.size(), "Choose a concrete part");
                            setWorkspace(GripsWorkspace);
                            setHandEditorPage(HandEditorPage::Mechanism);
                            selectWorkshopPart(anchor);
                            return std::nullopt;
                        } });
    }

    // All joint rotations are validated before the first one is applied.
    void ReloadMakerController::editFingerJoints(const QJsonObject& params)
    {
        const auto hand = handParameter(params, "Choose left or right");
        const auto edits = params["joints"].toArray();
        require(!edits.isEmpty() && edits.size() <= kFingerJoints, "Expected 1–15 joint edits");
        QVector<std::pair<int, Quaternionf>> valid;
        for (const auto value : edits) {
            const auto edit = value.toObject();
            const int joint = edit["index"].toInt(-1);
            require(joint >= 0 && joint < kFingerJoints, "Invalid finger joint");
            valid.append({ joint, commands::unitQuaternion(edit["rotation"]) });
        }
        setWorkspace(2);
        setHandEditorPage(HandEditorPage::FingerPoses);
        setEditedHand(hand);
        for (const auto& [joint, rotation] : valid) {
            editFinger(joint, rotation);
        }
    }

    void ReloadMakerController::saveHandPoseFromCommand(const QJsonObject& params)
    {
        const auto hand = handParameter(params, "Choose left or right");
        require(!params["name"].toString().trimmed().isEmpty(), "Name the pose");
        const int anchor = params.contains("anchor") ? params["anchor"].toInt(-1) : _handCaptureAnchor;
        require(anchor >= 0 && anchor < _handPoseAnchors.size(), "Choose a current reference part");
        setWorkspace(2);
        setHandEditorPage(HandEditorPage::FingerPoses);
        setHandCaptureAnchor(anchor);
        setEditedHand(hand);
        setHandCaptureName(params["name"].toString());
        setHandCaptureNotes(params["notes"].toString());
        updateHandPoseControls();
        require(canSaveHandPose(), "The current native hand cannot be captured");
        require(saveCurrentHandPose(), _lastError);
    }

    // A placement that cannot be posed restores the previous draft and preview
    // before reporting the failure.
    void ReloadMakerController::setGripPlacement(const QJsonObject& params)
    {
        require(_partGripDraft.has_value(), "Start a part grip first");
        auto candidate = *_partGripDraft;
        QString error;
        require(decodePoseTransform(params["transform"], candidate.pose.placement.handInAnchor, error), error);
        const auto previous = _partGripDraft;
        const auto previousPreview = _handPosePreview;
        _partGripDraft = candidate;
        setGripDraftName(params["name"].toString(candidate.pose.name));
        _partGripDraft->pose.name = _gripDraftName;
        if (!applyPose(_playbackTime)) {
            const QString failure = _poseError;
            _partGripDraft = previous;
            _handPosePreview = previousPreview;
            (void)applyPose(_playbackTime);
            showPartGripValues();
            require(false, failure);
        }
        showPartGripValues();
    }
}
