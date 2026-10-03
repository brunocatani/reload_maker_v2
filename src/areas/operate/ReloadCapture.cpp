// Learning the weapon's base reload: the recipe Operate and the reload page
// work from. The clip is sampled at 120 Hz; every animated weapon part with
// a hand anchor becomes a track of weapon-local poses. Sampling runs in
// batches on a zero-interval timer so the window stays responsive.
#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include "ReloadGraph.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QTabWidget>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        constexpr float kCaptureSamplesPerSecond = 120.0f;
        // Frames sampled per timer tick.
        constexpr int kCaptureBatchFrames = 16;

        QString sha256Hex(const QByteArray& data)
        {
            return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        }
    }

    // Drops the recipe and any capture in progress, and the simulation built on it.
    void ReloadMakerController::invalidateReloadCapture()
    {
        stopSimulation();
        _simulationRig.reset();
        _simulationAmmo = {};
        _simulation.configureAmmunition(0, false);
        if (_reloadCaptureTimer) {
            _reloadCaptureTimer->stop();
        }
        if (_reloadPreviewTimer) {
            _reloadPreviewTimer->stop();
        }
        // The graph keeps a pointer to the recipe; detach it before the recipe goes.
        if (_reloadGraph) {
            _reloadGraph->setRecipe(nullptr);
        }
        _reloadRecipe.reset();
        _reloadTrackNodes.clear();
        _reloadClip = {};
        _reloadBase.clear();
        _reloadExpectedPath.clear();
        _reloadCapturing = false;
        _reloadPlaying = false;
        _reloadReturning = false;
        _reloadHeld = false;
        _reloadWindow = -1;
        _reloadTrack = -1;
        if (_reloadDrive) {
            _reloadDrive->clearCurve();
            _reloadDrive->setActive(_reloadWorkspaceActive, false);
        }
        resetReloadPage();
    }

    // Selects the base reload on the timeline and starts learning it. A
    // recipe already learned from the same clip is kept.
    void ReloadMakerController::startReloadCapture()
    {
        _reloadCaptureQueued = false;
        if (!reloadCaptureAllowed() || _loadThread || _rebuildTimer->isActive() || _reloadCapturing) {
            return;
        }
        const auto action = std::find_if(_animationActions.cbegin(), _animationActions.cend(), [](const auto& a) { return a.baseReload; });
        if (action == _animationActions.cend() || _nodeBindings.isEmpty() || _renderedWeapon != _selectedWeapon) {
            _reloadSummary->setText("This configuration has no resolved base WPNReload. No fire, charge, unload or alternative clip will be substituted.");
            return;
        }
        const auto expected = action->resourcePath;
        // Phase selection drives the reload page's grip preview; Operate only
        // consumes the recipe and must not inherit a phase member's grip or
        // have its active clip replaced when the recipe is already current.
        if (!_reloadWorkspaceActive && _reloadRecipe && _reloadExpectedPath == expected) {
            return;
        }
        const int actionRow = actionIndex(expected);
        if (actionRow < 0) {
            return;
        }
        if (_workspace == GripsWorkspace && _handEditorPage == HandEditorPage::GrabLocations) {
            _grabLocationCapturePreview = std::pair{ _activeAnimationPath, _playbackTime };
        }
        selectAction(actionRow);
        if (!_clip.valid() || _activeAnimationPath != expected) {
            return;
        }
        if (_reloadRecipe && _reloadExpectedPath == expected) {
            refreshReloadWindows();
            selectReloadWindow(std::max(0, _reloadWindow));
            return;
        }
        invalidateReloadCapture();
        _reloadExpectedPath = expected;
        _reloadClip = _clip;
        _reloadBase = _stateBaselineLocalPose;
        ReloadRecipe recipe;
        QString error;
        if (!readReloadSource(expected, recipe, error) || !addReloadTracks(recipe, error)) {
            setStatus(error, true);
            return;
        }
        _reloadRecipe = std::move(recipe);
        _reloadCaptureFrame = 0;
        _reloadCapturing = true;
        showReloadCaptureStarted(expected);
        _reloadCaptureTimer->start();
    }

    // Reads the reload clip and its baseline, samples the baseline's first
    // frame as the reload base, and fills the recipe's timing and source
    // identity. On failure `error` holds the user-facing message.
    bool ReloadMakerController::readReloadSource(const QString& expected, ReloadRecipe& recipe, QString& error)
    {
        const auto clipBytes = readAnimationResource(expected, error);
        if (clipBytes.isEmpty()) {
            return false;
        }
        const auto baseBytes = readAnimationResource(_stateBaselineAnimationPath, error);
        if (baseBytes.isEmpty()) {
            return false;
        }
        // Analyze and display the same bytes whose identity we persist, even if
        // the user replaced the loose clip since the normal preview cached it.
        const auto sourceClip = HkxReader::readClip(clipBytes, error);
        const auto sourceBase = HkxReader::readClip(baseBytes, error);
        QVector<Transformf> sourceTracks;
        QVector<Transformf> baseGlobal;
        if (!sourceClip.valid() || !sourceBase.valid() ||
            !samplePose(_skeleton, sourceBase, 0, sourceBase.additive ? &_readyLocalPose : nullptr, sourceTracks, _reloadBase, baseGlobal, error)) {
            error = "Cannot sample the resolved reload source: " + error;
            return false;
        }
        _reloadClip = sourceClip;
        _clip = sourceClip;
        _stateBaselineLocalPose = _reloadBase;
        recipe.duration = sourceClip.durationSeconds;
        if (!std::isfinite(recipe.duration) || recipe.duration <= 0 || recipe.duration * kCaptureSamplesPerSecond > maximumReloadFrames - 1) {
            error = "Reload exceeds the supported 120 Hz analysis duration; no frames were silently dropped.";
            return false;
        }
        recipe.frames = int(std::ceil(recipe.duration * kCaptureSamplesPerSecond)) + 1;
        QJsonArray mods;
        for (const auto* mod : configuredObjectMods()) {
            mods.append(QJsonObject{ { "record", poseRecordKey(mod->formId) }, { "model", mod->modelPath } });
        }
        recipe.source = {
            { "action", "WPNReload" },
            { "weaponRecord", poseRecordKey(currentWeapon()->formId) },
            { "baseModel", currentWeapon()->baseModelPath },
            { "mods", mods },
            { "animation", expected },
            { "baseline", _stateBaselineAnimationPath },
            { "animationSha256", sha256Hex(clipBytes) },
            { "baselineSha256", sha256Hex(baseBytes) },
        };
        return true;
    }

    // One track per bound weapon part that has an animated-part hand anchor.
    // A track is a grab surface when the part draws geometry; its parent is
    // the track of its semantic parent binding.
    bool ReloadMakerController::addReloadTracks(ReloadRecipe& recipe, QString& error)
    {
        if (!compileMechanismScene(_assemblyNodes, _nodeBindings, {}, _reloadScenePlan, error)) {
            return false;
        }
        const QSet<int> geometry = weaponGeometryBlocks();
        auto* model = activeModel();
        QVector<int> bindingIndices;
        for (int i = 0; i < _nodeBindings.size(); ++i) {
            const auto& binding = _nodeBindings[i];
            if (!binding.weaponPart) {
                continue;
            }
            int anchor = -1;
            for (int a = 1; a < _handPoseAnchors.size(); ++a) {
                if (_handPoseAnchors[a].node == _sceneNodes[binding.nodeIndex] && _handPoseAnchors[a].description.anchorKind == "animated-part") {
                    anchor = a;
                    break;
                }
            }
            if (anchor < 0) {
                continue;
            }
            ReloadTrack track;
            track.anchor = _handPoseAnchors[anchor].description;
            track.label = _handPoseAnchors[anchor].label;
            track.surface = geometry.contains(model->getBlockNumber(_sceneNodes[binding.nodeIndex]));
            track.poses.reserve(recipe.frames);
            recipe.tracks.append(std::move(track));
            _reloadTrackNodes.append(binding.nodeIndex);
            bindingIndices.append(i);
        }
        if (recipe.tracks.isEmpty() || recipe.tracks.size() > maximumReloadTracks) {
            error = "No supported concrete animated-part tracks, or too many tracks for this capture.";
            return false;
        }
        for (int i = 0; i < bindingIndices.size(); ++i) {
            recipe.tracks[i].parent = bindingIndices.indexOf(_nodeBindings[bindingIndices[i]].semanticParentBinding);
        }
        return true;
    }

    // Blocks that draw the weapon's geometry: for each shape under the weapon
    // or an attached part, its skin bones, or its node ancestry when unskinned.
    QSet<int> ReloadMakerController::weaponGeometryBlocks() const
    {
        QSet<int> geometry;
        auto* model = activeModel();
        auto* scene = _window->getGLView()->getScene();
        for (auto* shape : scene->shapes) {
            if (!shape) {
                continue;
            }
            auto node = model->getBlockIndex(shape->id());
            bool weapon = false;
            QVector<int> ancestors;
            for (int depth = 0; node.isValid() && depth < model->getBlockCount(); ++depth) {
                ancestors.append(model->getBlockNumber(node));
                if (node == _weaponRoot) {
                    weapon = true;
                }
                for (const auto& root : _partRoots) {
                    if (root == node) {
                        weapon = true;
                    }
                }
                node = model->getBlockIndex(model->getParent(node));
            }
            if (!weapon) {
                continue;
            }
            const auto skin = shape->reloadMakerSkinBones();
            if (skin.isEmpty()) {
                for (const int block : ancestors) {
                    geometry.insert(block);
                }
            } else {
                for (const int bone : skin) {
                    geometry.insert(bone);
                }
            }
        }
        return geometry;
    }

    // Capture timer tick: samples the next batch of frames, then finishes.
    void ReloadMakerController::advanceReloadCapture()
    {
        if (!_reloadCapturing || !_reloadRecipe || !reloadCaptureAllowed() || _loadThread || _rebuildTimer->isActive()) {
            invalidateReloadCapture();
            return;
        }
        QString error;
        QVector<Transformf> tracks, local, global, boundLocal, boundGlobal, nativeLocal, world;
        auto& recipe = *_reloadRecipe;
        for (int batch = 0; batch < kCaptureBatchFrames && _reloadCaptureFrame < recipe.frames; ++batch, ++_reloadCaptureFrame) {
            const float seconds = recipe.duration * _reloadCaptureFrame / (recipe.frames - 1);
            if (!samplePose(_skeleton, _reloadClip, seconds, &_reloadBase, tracks, local, global, error) ||
                !evaluateSceneBindings(_skeleton, _nodeBindings, local, _reloadBase, global, boundLocal, boundGlobal, error) ||
                !nativeMechanismScene(_reloadScenePlan, _nodeBindings, boundLocal, nativeLocal, world, error)) {
                invalidateReloadCapture();
                setStatus("Reload capture failed: " + error, true);
                return;
            }
            Transformf inverse;
            if (!invertUniformTransform(global[_weaponBoneIndex], inverse)) {
                invalidateReloadCapture();
                setStatus("Reload Weapon frame cannot be inverted", true);
                return;
            }
            for (int i = 0; i < recipe.tracks.size(); ++i) {
                recipe.tracks[i].poses.append(composed(inverse, world[_reloadTrackNodes[i]]));
            }
        }
        _reloadProgress->setValue(_reloadCaptureFrame);
        if (_reloadCaptureFrame < recipe.frames) {
            return;
        }
        finishReloadCapture(error);
    }

    // Splits the learned motion into reload windows and takes the saved setup
    // for the same recipe (or its revision only, after LEARN RELOAD AGAIN), or
    // the project's unsaved draft. Then the waiting workspace continues.
    void ReloadMakerController::finishReloadCapture(QString& error)
    {
        auto& recipe = *_reloadRecipe;
        _reloadCaptureTimer->stop();
        _reloadCapturing = false;
        _reloadProgress->hide();
        _reloadAnalyze->setEnabled(true);
        if (!analyzeReloadWindows(recipe, error)) {
            invalidateReloadCapture();
            setStatus("Reload window analysis failed: " + error, true);
            return;
        }
        ReloadRecipe saved;
        if (_reloadRecipeLibrary.load(recipe.id, saved, error)) {
            if (_reloadIgnoreSavedOnce) {
                recipe.revision = saved.revision;
            } else {
                recipe = std::move(saved);
            }
        } else if (!error.isEmpty()) {
            appendDiagnostic("RELOAD SETUP: " + error);
        }
        _reloadIgnoreSavedOnce = false;
        if (!_pendingReloadDraft.isEmpty()) {
            ReloadRecipe restored;
            if (decodeReloadRecipe(QJsonDocument(_pendingReloadDraft).toJson(), restored, error) && restored.id == recipe.id) {
                recipe = std::move(restored);
            } else {
                appendDiagnostic("PROJECT: saved reload draft does not match the recaptured native source; review required.");
            }
            _pendingReloadDraft = {};
        }
        refreshReloadWindows();
        if (!recipe.windows.isEmpty()) {
            if (_reloadWorkspaceActive) {
                selectReloadWindow(0);
            }
        } else {
            _reloadSummary->setText("This base reload contains no measurable weapon-part movement. No substitute animation was used.");
        }
        if (simulationWorkspace(_workspace)) {
            prepareSimulation();
        } else if (_workspace == GripsWorkspace && _handEditorPage == HandEditorPage::GrabLocations) {
            // Learning establishes live part bindings without replacing the
            // frame the user chose for surface picking.
            if (_grabLocationCapturePreview) {
                const auto [path, seconds] = *_grabLocationCapturePreview;
                _grabLocationCapturePreview.reset();
                if (const int i = actionIndex(path); i >= 0) {
                    selectAction(i);
                    _playbackTime = std::clamp(seconds, 0.f, playbackDuration());
                    (void)applyPose(_playbackTime);
                    updateTimelineText();
                }
            }
            prepareGrabLocationFrame();
        }
    }

    void ReloadMakerController::saveReloadRecipe()
    {
        if (!_reloadRecipe || _reloadCapturing) {
            return;
        }
        QString error;
        if (!_reloadRecipeLibrary.save(*_reloadRecipe, error)) {
            setStatus(error, true);
            return;
        }
        setStatus(QStringLiteral("Saved reload setup revision %1: source curves, windows, grabbable parts and grip links.").arg(_reloadRecipe->revision));
    }
}
