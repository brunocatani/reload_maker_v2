// Saved hand poses: the reference parts a hand can be captured against,
// capturing the current hand into the library, previewing a saved pose's
// fingers, editing finger joints, and restoring the native animation.
#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "model/nifmodel.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        constexpr int kPoseNotesLength = 4096;
    }

    void ReloadMakerController::refreshHandPoseLibrary()
    {
        QStringList diagnostics;
        _savedHandPoses = _handPoseLibrary.load(diagnostics);
        refreshHandPoseList();
        refreshPartGripLibrary();
        refreshMechanisms();
        for (const auto& message : diagnostics) {
            appendDiagnostic(QStringLiteral("POSE FILE %1").arg(message));
        }
        if (!diagnostics.isEmpty()) {
            setStatus(QStringLiteral("Loaded %1 poses; %2 invalid pose files were left untouched. See Diagnostics.")
                          .arg(_savedHandPoses.size())
                          .arg(diagnostics.size()),
                      true);
        }
    }

    // A form's plugin-qualified record key (Plugin.esp:00abcd), from its
    // origin in the load order or else its master index.
    QString ReloadMakerController::poseRecordKey(std::uint32_t formId) const
    {
        const auto* plugin = currentPlugin();
        if (!plugin || !formId) {
            return {};
        }
        const auto found = plugin->origins.constFind(formId);
        if (found != plugin->origins.cend()) {
            return QStringLiteral("%1:%2").arg(found->plugin).arg(found->objectId, 6, 16, QLatin1Char('0'));
        }
        const auto index = formId >> 24;
        const QString origin = index < static_cast<std::uint32_t>(plugin->masters.size()) ? plugin->masters[index] : plugin->fileName;
        return QStringLiteral("%1:%2").arg(origin).arg(formId & 0xffffffU, 6, 16, QLatin1Char('0'));
    }

    // Rebuilds the reference parts after the assembly changed. The capture
    // form keeps the part with the same label when it still exists. Grip,
    // scope and mechanism choices follow, and the reload is learned again
    // for the rebuilt assembly.
    void ReloadMakerController::refreshHandPoseAnchors()
    {
        const bool hadAnchor = _handCaptureAnchor >= 0 && _handCaptureAnchor < _handPoseAnchors.size();
        const QString previous = hadAnchor ? _handPoseAnchors[_handCaptureAnchor].label : QString();
        const bool complete = collectHandPoseAnchors();
        // The rebuild itself is not a user change: the form simply shows the
        // first part, or the previous one when the assembly is complete.
        _handCaptureAnchor = _handPoseAnchors.isEmpty() ? -1 : 0;
        if (complete) {
            for (int i = 0; i < _handPoseAnchors.size(); ++i) {
                if (_handPoseAnchors[i].label == previous) {
                    _handCaptureAnchor = i;
                    break;
                }
            }
        }
        showHandPoseAnchors();
        if (!complete) {
            updateHandPoseControls();
            return;
        }
        refreshPartGripChoices();
        refreshScopeTargets();
        refreshPartGripList();
        refreshMechanismChoices();
        // A rebuilt assembly is re-learned wherever the reload is consumed
        // (Reload phases or Operate), so part swaps take effect in place.
        // Queued, not immediate: the rebuild must finish first. The queued
        // capture counts as busy so MCP clients do not race its revision bump.
        if (reloadCaptureAllowed()) {
            _reloadCaptureQueued = true;
            QTimer::singleShot(0, this, &ReloadMakerController::startReloadCapture);
        }
        updateHandPoseControls();
    }

    // The whole weapon, then each installed part (base model and every slot's
    // modification), then every animated node of the assembly with its
    // owning part. Returns false when the assembly is not ready; the list
    // then holds at most the whole weapon.
    bool ReloadMakerController::collectHandPoseAnchors()
    {
        _handPoseAnchors.clear();
        if (!_skeleton.valid() || _renderedWeapon < 0) {
            return false;
        }
        HandPoseAnchor weapon;
        weapon.description = { "weapon", "Weapon", "Weapon", {}, {}, {}, {} };
        weapon.label = QStringLiteral("Weapon · whole weapon");
        _handPoseAnchors.append(weapon);
        const auto* model = activeModel();
        const auto* config = configuration();
        if (!model || !config) {
            return false;
        }
        if (_weaponRoot.isValid() && currentWeapon()) {
            addHandPoseAnchor(_weaponRoot, "installed-part", QStringLiteral("Base weapon model"), currentWeapon()->baseModelPath,
                              poseRecordKey(currentWeapon()->formId), _weaponRoot);
        }
        for (const auto& slot : config->entries()) {
            const auto root = _partRoots.value(slot.key);
            const auto* mod = config->mod(config->selected(slot.key));
            if (root.isValid() && mod) {
                addHandPoseAnchor(root, "installed-part", QStringLiteral("%1 · %2").arg(slot.label, mod->name), mod->modelPath, poseRecordKey(mod->formId), root);
            }
        }
        for (const auto& binding : _nodeBindings) {
            if (binding.weaponPart) {
                addAnimatedPartAnchor(binding);
            }
        }
        return true;
    }

    // An animated node belongs to the nearest installed part above it, else
    // to the base weapon, else it is labelled as part of the native rig.
    void ReloadMakerController::addAnimatedPartAnchor(const ScenePoseBinding& binding)
    {
        const auto* model = activeModel();
        const auto* config = configuration();
        const auto node = _sceneNodes[binding.nodeIndex];
        const auto& bone = _skeleton.bones[binding.boneIndex].name;
        auto ancestor = QModelIndex(node);
        for (int depth = 0; ancestor.isValid() && depth < model->getBlockCount(); ++depth) {
            for (const auto& slot : config->entries()) {
                const auto root = _partRoots.value(slot.key);
                if (!root.isValid() || root != ancestor) {
                    continue;
                }
                const auto* mod = config->mod(config->selected(slot.key));
                if (mod) {
                    addHandPoseAnchor(node, "animated-part", QStringLiteral("%1 · %2").arg(bone, slot.label), mod->modelPath, poseRecordKey(mod->formId), root);
                    return;
                }
                break;
            }
            if (ancestor == _weaponRoot && currentWeapon()) {
                addHandPoseAnchor(node, "animated-part", QStringLiteral("%1 · base weapon").arg(bone), currentWeapon()->baseModelPath,
                                  poseRecordKey(currentWeapon()->formId), _weaponRoot);
                return;
            }
            ancestor = model->getBlockIndex(model->getParent(ancestor));
        }
        if (currentWeapon()) {
            addHandPoseAnchor(node, "animated-part", QStringLiteral("%1 · native rig").arg(bone), currentWeapon()->baseModelPath,
                              poseRecordKey(currentWeapon()->formId), node);
        }
    }

    // The anchor's path runs from its owning part's root down to the node.
    void ReloadMakerController::addHandPoseAnchor(const QModelIndex& node, const QString& kind, const QString& label, const QString& modelPath,
                                                  const QString& record, const QModelIndex& owner)
    {
        if (!node.isValid()) {
            return;
        }
        const auto* model = activeModel();
        QStringList path;
        auto ancestor = node;
        for (int depth = 0; ancestor.isValid() && depth < model->getBlockCount(); ++depth) {
            path.prepend(model->get<QString>(ancestor, "Name"));
            if (ancestor == owner) {
                break;
            }
            ancestor = model->getBlockIndex(model->getParent(ancestor));
        }
        HandPoseAnchor anchor;
        anchor.description = { kind, model->get<QString>(node, "Name"), path.join('/'), modelPath, record, {}, {} };
        anchor.node = node;
        anchor.label = label;
        anchor.tooltip = QStringLiteral("%1\n%2\n%3").arg(record, modelPath, path.join('/'));
        _handPoseAnchors.append(anchor);
    }

    // The node's world transform from its parent chain in the model.
    bool ReloadMakerController::sceneNodeWorld(const QModelIndex& node, Transformf& world, QString& error) const
    {
        const auto* model = activeModel();
        if (!model || !node.isValid()) {
            error = QStringLiteral("The placement part is no longer available");
            return false;
        }
        auto ancestor = node;
        world = {};
        for (int depth = 0; ancestor.isValid() && depth < model->getBlockCount(); ++depth) {
            Transformf local;
            if (!readNodeTransform(ancestor, local)) {
                error = QStringLiteral("The selected part has an invalid or zero-scaled frame");
                return false;
            }
            world = composed(local, world);
            const int parent = model->getParent(ancestor);
            if (parent < 0) {
                return true;
            }
            ancestor = model->getBlockIndex(parent);
        }
        error = QStringLiteral("Cannot resolve the selected part's complete parent chain");
        return false;
    }

    // Saves the shown hand as a new library pose relative to the capture
    // form's reference part. Playback pauses at the current frame.
    bool ReloadMakerController::saveCurrentHandPose()
    {
        updateHandPoseControls();
        if (!canSaveHandPose()) {
            return false;
        }
        if (_handCaptureNotes.size() > kPoseNotesLength) {
            setStatus(QStringLiteral("Keep pose notes below 4097 characters."), true);
            return false;
        }
        stopPlayback();
        if (!applyPose(_playbackTime)) {
            updateHandPoseControls();
            return false;
        }
        const int selection = _handCaptureAnchor;
        if (selection < 0 || selection >= _handPoseAnchors.size()) {
            return false;
        }
        const auto& anchor = _handPoseAnchors[selection];
        Transformf frame = _globalPose[_weaponBoneIndex];
        QString error;
        if (anchor.description.anchorKind != "weapon" && !sceneNodeWorld(anchor.node, frame, error)) {
            setStatus(error, true);
            return false;
        }
        HandPose pose;
        pose.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pose.name = _handCaptureName.trimmed();
        pose.notes = _handCaptureNotes;
        pose.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        pose.hand = _editedHand;
        pose.placement = anchor.description;
        pose.source = currentPoseSource();
        if (!captureHandPose(_skeleton, _localPose, _globalPose, frame, pose, error) || !_handPoseLibrary.save(pose, error)) {
            setStatus(QStringLiteral("Hand pose was not saved: %1").arg(error), true);
            return false;
        }
        clearHandPoseSearch();
        refreshHandPoseLibrary();
        if (handPoseListed(pose.id)) {
            selectHandPose(pose.id);
        }
        appendDiagnostic(QStringLiteral("SAVED HAND POSE %1 · %2 · %3").arg(pose.name, pose.id, _handPoseLibrary.directory()));
        setHandCaptureName(QString());
        setStatus(QStringLiteral("Saved %1 hand pose “%2”. It will remain available after restart.").arg(pose.hand == PoseHand::Left ? "left" : "right", pose.name));
        return true;
    }

    // Where a captured pose came from: weapon, modifications, clip, time,
    // body, motion mode and resource provider.
    QJsonObject ReloadMakerController::currentPoseSource() const
    {
        QJsonArray mods;
        for (const auto* mod : configuredObjectMods()) {
            mods.append(QJsonObject{ { "record", poseRecordKey(mod->formId) }, { "editorId", mod->editorId }, { "name", mod->name }, { "model", mod->modelPath } });
        }
        const auto* weapon = currentWeapon();
        if (!weapon) {
            return {};
        }
        return {
            { "weaponName", weapon->name },
            { "weaponEditorId", weapon->editorId },
            { "weaponRecord", poseRecordKey(weapon->formId) },
            { "modifications", mods },
            { "action", _activeAction },
            { "animation", _activeAnimationPath },
            { "baselineAnimation", _stateBaselineAnimationPath },
            { "timeSeconds", _playbackTime },
            { "clipTimeSeconds", std::min(_playbackTime, _clip.durationSeconds) },
            { "body", _armsBody },
            { "viewMode", motionModeName() },
            { "approximateTransitions", _approximateTransitions },
            { "resourceProvider", _libraryMode ? _packageResources.provider(_activeAnimationPath) : _package.sourcePath },
        };
    }

    // Shows the selected pose's fingers on its own hand at the current
    // wrist; its saved placement is not applied.
    void ReloadMakerController::previewSavedHandPose()
    {
        updateHandPoseControls();
        const auto* selected = selectedHandPose();
        if (!canPreviewHandPose() || !selected) {
            return;
        }
        restoreHandPosePreview();
        auto check = _localPose;
        QString error;
        if (!applyHandPoseFingers(_skeleton, *selected, check, error)) {
            setStatus(error, true);
            return;
        }
        stopPlayback();
        _handPosePreview = *selected;
        setArmsShown(true);
        if (!applyPose(_playbackTime)) {
            _handPosePreview.reset();
            updateHandPoseControls();
            return;
        }
        updateHandPoseControls();
        setStatus(QStringLiteral("Previewing “%1”: saved fingers, current wrist. Placement metadata has not been applied.").arg(selected->name));
    }

    // Ends every hand preview (saved pose, part grip, mechanism) and shows
    // the native animation again. An unsaved magazine fit is remembered.
    void ReloadMakerController::restoreHandPosePreview()
    {
        rememberMagazineGrip();
        _workshopHandEditing = false;
        _workshopPicking = false;
        if (_gripViewport) {
            _gripViewport->setPicking(false);
        }
        if (!_handPosePreview && !_partGripDraft && !_mechanismPreview) {
            return;
        }
        stopMechanismPreview();
        _handPosePreview.reset();
        _partGripDraft.reset();
        _gripPreviewValid = false;
        _gripError.clear();
        if (_clip.valid() && !_loadThread) {
            (void)applyPose(_playbackTime);
        }
        updateHandPoseControls();
    }

    // Sets one finger joint of the edited hand. The first edit captures the
    // hand's current fingers as that hand's draft; a preview of the same hand
    // ends. Only on the finger poses page, paused, outside part simulation.
    void ReloadMakerController::editFinger(int joint, const Quaternionf& rotation)
    {
        const bool editable = _clip.valid() && !_loadThread && !_reloadCapturing && !_mechanismPreview && _workspace == GripsWorkspace &&
                              _handEditorPage == HandEditorPage::FingerPoses;
        if (joint < 0 || joint >= 15 || !editable) {
            throw std::runtime_error("Pause an animation and leave part simulation before editing fingers.");
        }
        const float norm = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;
        if (!std::isfinite(norm) || std::abs(norm - 1) > .0005f) {
            throw std::runtime_error("Expected a finite unit quaternion.");
        }
        const auto hand = _editedHand;
        auto candidate = _fingerDrafts[int(hand)];
        QString error;
        if (!candidate) {
            candidate = captureFingerDraft(hand);
        }
        candidate->fingers[joint].rotation = rotation;
        auto check = _localPose;
        if (!applyHandPoseFingers(_skeleton, *candidate, check, error)) {
            throw std::runtime_error(error.toStdString());
        }
        stopPlayback();
        if (_handPosePreview && _handPosePreview->hand == hand) {
            _handPosePreview.reset();
        }
        _fingerDrafts[int(hand)] = candidate;
        if (!applyPose(_playbackTime)) {
            throw std::runtime_error(_poseError.toStdString());
        }
        ++_revision;
        refreshFingerEditor();
    }

    // The hand as currently shown, relative to the whole weapon.
    HandPose ReloadMakerController::captureFingerDraft(PoseHand hand) const
    {
        HandPose pose;
        pose.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pose.name = "Finger edit";
        pose.hand = hand;
        pose.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        pose.placement.anchorKind = "weapon";
        pose.placement.anchorName = "Weapon";
        pose.source = currentPoseSource();
        QString error;
        if (_weaponBoneIndex < 0 || _weaponBoneIndex >= _globalPose.size() ||
            !captureHandPose(_skeleton, _localPose, _globalPose, _globalPose[_weaponBoneIndex], pose, error)) {
            throw std::runtime_error(error.toStdString());
        }
        return pose;
    }

    // Prepares capturing reload hand poses on the configured magazine: the
    // reload clip with the weapon fixed, at the simulation's reload time,
    // left hand, magazine frame as the reference part.
    bool ReloadMakerController::authorReloadGrips()
    {
        if (_loadThread || _reloadCapturing || !_simulationRig || !_reloadRecipe) {
            setStatus("Wait for the configured reload to finish preparing.", true);
            return false;
        }
        const auto magazine = _reloadRecipe->tracks[_simulationRig->magazine].anchor;
        const float seconds = _simulationActive && _simulation.action == SimulationAction::Reload ? _simulation.seconds : 0.f;
        int anchor = -1;
        for (int i = 0; i < _handPoseAnchors.size(); ++i) {
            if (!sameMechanismPart(_handPoseAnchors[i].description, magazine)) {
                continue;
            }
            if (anchor >= 0) {
                setStatus("The magazine frame is ambiguous in this assembly.", true);
                return false;
            }
            anchor = i;
        }
        if (anchor < 0) {
            setStatus("The configured magazine has no capture frame.", true);
            return false;
        }
        setWorkspace(GripsWorkspace);
        restoreHandPosePreview();
        setHandEditorPage(HandEditorPage::FingerPoses);
        setWeaponFixedMotion(true);
        if (!selectSimulationClip(false, true)) {
            setStatus(_simulationError, true);
            return false;
        }
        _playbackTime = std::min(seconds, playbackDuration());
        setHandCaptureAnchor(anchor);
        setEditedHand(PoseHand::Left);
        setArmsShown(true);
        if (!applyPose(_playbackTime)) {
            return false;
        }
        updateTimelineText();
        refreshFingerEditor();
        updateHandPoseControls();
        setStatus("Weapon fixed. Scrub to the hand pose you want and save it. Select a saved pose to place it on a weapon part.");
        return true;
    }
}
