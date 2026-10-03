// The mechanism workshop's document state: the selected movement step, the
// hand pose choice for the part, the saved definition selected in the
// definition list, and whether the chosen hand is shown while grabbing.
// The part definition itself is _mechanismDraft; its form reports user
// edits through applyMechanismForm.
#include "ReloadMakerController.h"
#include "GripViewport.h"

#include <QTimer>
#include <algorithm>

namespace reloadmaker
{
    // Selecting a step while editing movement shows the part at that step's
    // end with its tool.
    void ReloadMakerController::selectMechanismStage(int stage)
    {
        if (stage == _mechanismStage) {
            return;
        }
        _mechanismStage = stage;
        showMechanismStage();
        displayMechanism();
        if (_mechanismTeaching) {
            const int i = _mechanismStage;
            if (i >= 0) {
                _teachingRotation = _mechanismDraft->stages[i].rotation;
            }
            _mechanismState.path = float(i + 1);
            (void)applyPose(_playbackTime);
            updateMechanismControls();
            updateGripViewport();
        }
        noteDocumentChange("mechanism step");
    }

    // The selected step stays within the draft's steps; the first step when
    // none was selected, none without steps.
    void ReloadMakerController::normalizeMechanismStage()
    {
        if (!_mechanismDraft || _mechanismDraft->stages.isEmpty()) {
            _mechanismStage = -1;
            return;
        }
        _mechanismStage = std::clamp(_mechanismStage, 0, int(_mechanismDraft->stages.size()) - 1);
    }

    void ReloadMakerController::setWorkshopPoseChoice(const QString& choice)
    {
        if (choice == _workshopPoseChoice) {
            return;
        }
        _workshopPoseChoice = choice;
        showWorkshopPoseChoice();
        noteDocumentChange("workshop hand pose");
    }

    // Choosing a hand pose applies it even when it is already chosen.
    void ReloadMakerController::chooseWorkshopPose(const QString& choice)
    {
        setWorkshopPoseChoice(choice);
        applyWorkshopPose();
    }

    // "No hand", then this part's saved grips, then every finger pose. Keys
    // are "grip:<id>" and "pose:<id>"; no hand is the empty key.
    QVector<ReloadMakerController::WorkshopPoseChoice> ReloadMakerController::workshopPoseChoices() const
    {
        QVector<WorkshopPoseChoice> choices{ { QStringLiteral("No hand — edit the part only"), QString() } };
        if (!_mechanismDraft) {
            return choices;
        }
        for (const auto& grip : _partGrips) {
            if (grip.weaponRecord.compare(_mechanismDraft->weaponRecord, Qt::CaseInsensitive) == 0 &&
                sameMechanismPart(grip.pose.placement, _mechanismDraft->anchor)) {
                const auto side = grip.pose.hand == PoseHand::Left ? "Left" : "Right";
                choices.append({ QStringLiteral("Saved grip · %1 · %2").arg(side, grip.pose.name), "grip:" + grip.pose.id });
            }
        }
        for (const auto& pose : _savedHandPoses) {
            const auto side = pose.hand == PoseHand::Left ? "Left" : "Right";
            choices.append({ QStringLiteral("Finger pose · %1 · %2").arg(side, pose.name), "pose:" + pose.id });
        }
        return choices;
    }

    bool ReloadMakerController::workshopPoseListed(const QString& choice) const
    {
        const auto choices = workshopPoseChoices();
        return std::any_of(choices.cbegin(), choices.cend(), [&](const auto& c) { return c.key == choice; });
    }

    void ReloadMakerController::selectMechanism(const QString& id)
    {
        if (id == _selectedMechanismId) {
            return;
        }
        _selectedMechanismId = id;
        showSelectedMechanism();
        noteDocumentChange("selected mechanism");
    }

    void ReloadMakerController::setMechanismHandShownOnGrab(bool shown)
    {
        if (shown == _mechanismHandShownOnGrab) {
            return;
        }
        _mechanismHandShownOnGrab = shown;
        showMechanismHandShownOnGrab();
        if (_mechanismPreview) {
            (void)applyPose(_playbackTime);
            updateMechanismControls();
        }
        noteDocumentChange("mechanism hand shown on grab");
    }

    // The definition form's values: name, the selected step's fields and the
    // checked follower parts. Followers keep their captured rest offsets;
    // new ones are captured relative to the part at the current frame.
    void ReloadMakerController::applyMechanismForm(const MechanismForm& form)
    {
        if (_mechanismUpdating || !_mechanismDraft || (_mechanismPreview && !_mechanismTeaching)) {
            return;
        }
        auto& p = *_mechanismDraft;
        p.name = form.name;
        const int i = _mechanismStage;
        if (i >= 0 && i < p.stages.size()) {
            auto& s = p.stages[i];
            s.forwardName = form.forwardName;
            s.reverseName = form.reverseName;
            s.axis = form.motion % 3;
            s.rotation = form.motion >= 3;
            s.from = form.from;
            s.to = form.to;
            s.returnRate = form.returnRate;
            s.release = ReleaseRule(form.release);
            s.pivot = form.pivot;
        }
        noteDocumentChange("mechanism definition");
        QString error;
        const int master = resolveMechanismAnchor(p.anchor, error);
        Transformf world;
        Transformf inverse;
        if (master < 0 || !sceneNodeWorld(_handPoseAnchors[master].node, world, error) || !invertUniformTransform(world, inverse)) {
            setStatus(error, true);
            return;
        }
        QVector<MechanismFollower> followers;
        for (const int index : form.followers) {
            const auto& anchor = _handPoseAnchors[index];
            const auto old = std::find_if(p.followers.cbegin(), p.followers.cend(), [&](const auto& f) { return sameGripAnchor(f.anchor, anchor.description); });
            if (old != p.followers.cend()) {
                followers.append(*old);
                continue;
            }
            Transformf follower;
            if (!sceneNodeWorld(anchor.node, follower, error)) {
                setStatus(error, true);
                return;
            }
            followers.append({ anchor.description, composed(inverse, follower) });
        }
        p.followers = followers;
        if (i >= 0 && i < p.stages.size()) {
            showMechanismStageSummary(i);
        }
        updateMechanismControls();
        if (_mechanismTeaching) {
            if (i >= 0) {
                _teachingRotation = p.stages[i].rotation;
            }
            (void)applyPose(_playbackTime);
            updateGripViewport();
        }
    }

    // The workshop has a loaded clip and a stable assembly.
    bool ReloadMakerController::workshopFrameReady() const
    {
        return _clip.valid() && !_loadThread && !_rebuildTimer->isActive();
    }

    // A valid definition outside a grab test.
    bool ReloadMakerController::canSaveMechanism() const
    {
        const bool testing = _mechanismPreview && !_mechanismTeaching;
        QString error;
        const bool valid = _mechanismDraft && (testing || validateMechanism(*_mechanismDraft, error));
        return workshopFrameReady() && _mechanismDraft.has_value() && valid && !testing;
    }
}
