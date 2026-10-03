// The part grip form's document state: the saved finger pose and reference
// part a new grip starts from, the grip's name, and the saved grip selected
// in the history list. Their controls only show these fields and report
// user changes. Each setter does what the control's change used to trigger,
// and only when the value changes.
#include "ReloadMakerController.h"

#include <QTimer>
#include <algorithm>

namespace reloadmaker
{
    void ReloadMakerController::setGripPose(const QString& id)
    {
        if (id == _gripPoseId) {
            return;
        }
        _gripPoseId = id;
        showGripPose();
        updatePartGripControls();
        noteDocumentChange("grip pose");
    }

    // Moving an active grip to another part keeps the hand where it is. When
    // the part cannot take the grip, the form returns to the grip's part.
    void ReloadMakerController::setGripAnchor(int anchor)
    {
        if (anchor == _gripAnchor) {
            return;
        }
        _gripAnchor = anchor;
        showGripAnchor();
        if (_partGripDraft && anchor >= 0) {
            QString error;
            if (assignGripPart(anchor, error)) {
                showPartGripValues();
            } else {
                for (int i = 0; i < _handPoseAnchors.size(); ++i) {
                    if (sameGripAnchor(_handPoseAnchors[i].description, _partGripDraft->pose.placement)) {
                        _gripAnchor = i;
                    }
                }
                showGripAnchor();
                setStatus(error, true);
            }
        }
        updatePartGripControls();
        noteDocumentChange("grip reference part");
    }

    // Names longer than kPoseNameLength are cut, as the name field does.
    void ReloadMakerController::setGripDraftName(const QString& name)
    {
        const auto value = name.left(kPoseNameLength);
        if (value == _gripDraftName) {
            return;
        }
        _gripDraftName = value;
        showGripDraftName();
        updatePartGripControls();
        noteDocumentChange("grip name");
    }

    void ReloadMakerController::selectPartGrip(const QString& id)
    {
        if (id == _selectedGripId) {
            return;
        }
        _selectedGripId = id;
        showSelectedPartGrip();
        updatePartGripControls();
        noteDocumentChange("selected part grip");
    }

    const PartGrip* ReloadMakerController::selectedPartGrip() const
    {
        if (_selectedGripId.isEmpty()) {
            return nullptr;
        }
        for (const auto& grip : _partGrips) {
            if (grip.pose.id == _selectedGripId) {
                return &grip;
            }
        }
        return nullptr;
    }

    // The form shows the pose and part of a grip that was loaded or rebuilt;
    // that is not a form edit. A pose missing from the library leaves the
    // form without one.
    void ReloadMakerController::followGripInForm(const QString& poseId, int anchor)
    {
        _gripPoseId = knownHandPoseId(poseId);
        _gripAnchor = anchor;
        showGripPose();
        showGripAnchor();
    }

    // The id when it names a saved hand pose, else empty.
    QString ReloadMakerController::knownHandPoseId(const QString& id) const
    {
        const bool known = std::any_of(_savedHandPoses.cbegin(), _savedHandPoses.cend(), [&](const auto& pose) { return pose.id == id; });
        return known ? id : QString();
    }

    const HandPose* ReloadMakerController::gripPose() const
    {
        for (const auto& pose : _savedHandPoses) {
            if (pose.id == _gripPoseId) {
                return &pose;
            }
        }
        return nullptr;
    }

    // The form's choices after the library or the assembly changed: the
    // same pose and part when they still exist, else the first of each. A
    // refresh is not a user change.
    void ReloadMakerController::refreshPartGripChoices()
    {
        const bool poseListed = std::any_of(_savedHandPoses.cbegin(), _savedHandPoses.cend(), [&](const auto& pose) { return pose.id == _gripPoseId; });
        if (!poseListed) {
            _gripPoseId = _savedHandPoses.isEmpty() ? QString() : _savedHandPoses.first().id;
        }
        // Without a part the form used to report the first one.
        const int anchor = std::max(_gripAnchor, 0);
        if (anchor < _handPoseAnchors.size()) {
            _gripAnchor = anchor;
        } else {
            _gripAnchor = _handPoseAnchors.isEmpty() ? -1 : 0;
        }
        showPartGripChoices();
        updatePartGripControls();
    }

    // A loaded weapon and clip with a valid scene at the shown frame.
    bool ReloadMakerController::partGripFrameReady() const
    {
        return !_loadThread && !_rebuildTimer->isActive() && _clip.valid() && !_geometrySignature.isEmpty() && _renderedWeapon >= 0 &&
               _renderedWeapon == _selectedWeapon;
    }

    bool ReloadMakerController::canStartPartGrip() const
    {
        return partGripFrameReady() && !_gripPoseId.isEmpty() && _gripAnchor >= 0;
    }

    bool ReloadMakerController::canPickGripSurface() const
    {
        return partGripFrameReady() && (_partGripDraft.has_value() || canStartPartGrip());
    }

    // A grip draft outside part simulation.
    bool ReloadMakerController::partGripEditable() const
    {
        return partGripFrameReady() && _partGripDraft.has_value() && !_mechanismPreview;
    }

    bool ReloadMakerController::canSavePartGrip() const
    {
        return partGripEditable() && _gripPreviewValid && !_gripDraftName.trimmed().isEmpty();
    }

    bool ReloadMakerController::canLoadPartGrip() const
    {
        return partGripFrameReady() && selectedPartGrip();
    }
}
