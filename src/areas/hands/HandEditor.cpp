// The hands editor's document state: the open page, the hand and finger
// joint being edited, the capture form (reference part, name and notes) and
// the selected saved pose and the search over saved poses. Their controls
// only show these fields and report
// user changes. Each setter does what the control's change used to trigger,
// and only when the value changes.
#include "ReloadMakerController.h"
#include "GripViewport.h"

#include <QTimer>
#include <algorithm>

namespace reloadmaker
{
    void ReloadMakerController::setHandEditorPage(HandEditorPage page)
    {
        if (page == _handEditorPage) {
            return;
        }
        _handEditorPage = page;
        showHandEditorPage();
        handEditorPageChanged();
        noteDocumentChange("hand editor page");
    }

    // Leaving a page ends its picking and previews; finger, grip and
    // mechanism previews depend on the open page, so the pose is reapplied.
    // The editor reacts once the grip viewport exists.
    void ReloadMakerController::handEditorPageChanged()
    {
        if (!_gripViewport) {
            return;
        }
        if (_handEditorPage != HandEditorPage::GrabLocations) {
            _pickingGrabLocation = false;
            _gripViewport->setPicking(false);
        } else {
            prepareGrabLocationFrame();
        }
        if (_handEditorPage != HandEditorPage::Mechanism) {
            _workshopPicking = false;
            _workshopHandEditing = false;
            _gripViewport->setPicking(false);
            if (_mechanismPreview) {
                stopMechanismPreview();
                if (_clip.valid()) {
                    (void)applyPose(_playbackTime);
                }
            }
        }
        updatePartGripControls();
        if (_clip.valid() && !_reloadCapturing) {
            (void)applyPose(_playbackTime);
        }
        refreshFingerEditor();
        updateGripViewport();
    }

    void ReloadMakerController::setEditedHand(PoseHand hand)
    {
        if (hand == _editedHand) {
            return;
        }
        _editedHand = hand;
        showEditedHand();
        refreshFingerEditor();
        noteDocumentChange("edited hand");
    }

    void ReloadMakerController::setFingerJoint(int joint)
    {
        if (joint == _fingerJointIndex) {
            return;
        }
        _fingerJointIndex = joint;
        showFingerJoint();
        refreshFingerEditor();
        noteDocumentChange("finger joint");
    }

    // -1 when the assembly has no reference parts.
    void ReloadMakerController::setHandCaptureAnchor(int anchor)
    {
        if (anchor == _handCaptureAnchor) {
            return;
        }
        _handCaptureAnchor = anchor;
        showHandCaptureAnchor();
        updateHandPoseControls();
        noteDocumentChange("hand capture reference");
    }

    // Names longer than kPoseNameLength are cut, as the name field does.
    void ReloadMakerController::setHandCaptureName(const QString& name)
    {
        const auto value = name.left(kPoseNameLength);
        if (value == _handCaptureName) {
            return;
        }
        _handCaptureName = value;
        showHandCaptureName();
        updateHandPoseControls();
        noteDocumentChange("hand pose name");
    }

    void ReloadMakerController::setHandCaptureNotes(const QString& notes)
    {
        if (notes == _handCaptureNotes) {
            return;
        }
        _handCaptureNotes = notes;
        showHandCaptureNotes();
        noteDocumentChange("hand pose notes");
    }

    void ReloadMakerController::selectHandPose(const QString& id)
    {
        if (id == _selectedHandPoseId) {
            return;
        }
        _selectedHandPoseId = id;
        showSelectedHandPose();
        updateHandPoseControls();
        noteDocumentChange("selected hand pose");
    }

    // The list follows the search, and the selection follows the list.
    void ReloadMakerController::setHandPoseFilter(const QString& text)
    {
        if (text == _handPoseFilter) {
            return;
        }
        _handPoseFilter = text;
        showHandPoseFilter();
        refreshHandPoseList();
        noteDocumentChange("hand pose search");
    }

    bool ReloadMakerController::handPoseMatchesFilter(const HandPose& pose) const
    {
        const QString side = pose.hand == PoseHand::Left ? QStringLiteral("Left") : QStringLiteral("Right");
        const QString weapon = pose.source.value("weaponName").toString();
        return (pose.name + side + weapon + pose.notes).contains(_handPoseFilter.trimmed(), Qt::CaseInsensitive);
    }

    bool ReloadMakerController::handPoseListed(const QString& id) const
    {
        return std::any_of(_savedHandPoses.cbegin(), _savedHandPoses.cend(),
                           [&](const HandPose& pose) { return pose.id == id && handPoseMatchesFilter(pose); });
    }

    const HandPose* ReloadMakerController::selectedHandPose() const
    {
        if (_selectedHandPoseId.isEmpty()) {
            return nullptr;
        }
        for (const auto& pose : _savedHandPoses) {
            if (pose.id == _selectedHandPoseId) {
                return &pose;
            }
        }
        return nullptr;
    }

    // The anchor list is gone with the assembly it described.
    void ReloadMakerController::clearHandPoseAnchors()
    {
        _handPoseAnchors.clear();
        showHandPoseAnchors();
        setHandCaptureAnchor(-1);
    }

    // A loaded weapon and clip with a posed skeleton at the shown frame.
    bool ReloadMakerController::handPoseFrameReady() const
    {
        return !_loadThread && !_rebuildTimer->isActive() && _clip.valid() && _renderedWeapon == _selectedWeapon && _renderedWeapon >= 0 &&
               !_geometrySignature.isEmpty() && _globalPose.size() == _skeleton.bones.size();
    }

    // A part grip previewed outside the finger poses page.
    bool ReloadMakerController::partGripPreviewActive() const
    {
        return _partGripDraft && _handEditorPage != HandEditorPage::FingerPoses;
    }

    bool ReloadMakerController::canSaveHandPose() const
    {
        return handPoseFrameReady() && !_handPosePreview && !partGripPreviewActive() && !_handCaptureName.trimmed().isEmpty() &&
               !_handPoseAnchors.isEmpty();
    }

    bool ReloadMakerController::canPreviewHandPose() const
    {
        return handPoseFrameReady() && selectedHandPose();
    }
}
