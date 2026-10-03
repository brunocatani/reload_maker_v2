// The clip on the timeline: which catalog action is selected, playing and
// scrubbing it, and sampling its pose with the approximate entry and exit
// blends. _selectedAction is the document field; the clip list and the
// transport's clip picker only show it.
#include "ReloadMakerController.h"
#include "PoseEvaluator.h"
#include "areas/operate/PlaybackTimeline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace reloadmaker
{
    namespace
    {
        // Graph blend times that are not finite and positive count as none.
        float blendSeconds(float value)
        {
            return std::isfinite(value) && value > 0.0f ? value : 0.0f;
        }

        // Smoothstep easing of a 0..1 blend progress.
        float smoothed(float progress)
        {
            return progress * progress * (3.0f - 2.0f * progress);
        }
    }

    int ReloadMakerController::actionIndex(const QString& resourcePath) const
    {
        for (int i = 0; i < _animationActions.size(); ++i) {
            if (_animationActions[i].resourcePath == resourcePath) {
                return i;
            }
        }
        return -1;
    }

    // Loads catalog action `index` onto the timeline at its start. The clip is
    // sampled over its state's baseline pose; when the clip or baseline cannot
    // be evaluated no clip stays loaded.
    void ReloadMakerController::selectAction(int index)
    {
        if (_loadThread || index < 0 || index >= _animationActions.size() || !_skeleton.valid()) {
            return;
        }
        invalidateCollisions();
        if (index != _selectedAction) {
            _selectedAction = index;
            noteDocumentChange("clip selection");
        }
        showSelectedAction();
        const auto poseControls = qScopeGuard([this]() { updateHandPoseControls(); });
        restoreHandPosePreview();
        // A failed request must never leave the previous clip playing under the new label.
        stopPlayback();
        _clip = {};
        _activeAction.clear();
        _timeline->setEnabled(false);
        updateTimelineText();
        refreshFingerEditor();
        const AnimationAction action = _animationActions[index];
        const QString& path = action.resourcePath;
        const QString& baselinePath = action.baselineResourcePath;
        if (_libraryMode) {
            appendDiagnostic(QStringLiteral("WINNING ACTION %1\n  %2\nWINNING STATE %3\n  %4")
                                 .arg(path, _packageResources.provider(path), baselinePath, _packageResources.provider(baselinePath)));
        }
        QString error;
        const auto loadClip = [this, &error](const QString& resource) {
            if (resource.compare(_readyAnimationPath, Qt::CaseInsensitive) == 0) {
                return _readyClip;
            }
            return cachedClip(resource, error);
        };
        HkxClip clip = loadClip(path);
        if (!clip.valid()) {
            setStatus(QStringLiteral("Cannot load animation '%1': %2").arg(path, error), true);
            return;
        }
        HkxClip baselineClip = baselinePath == path ? clip : loadClip(baselinePath);
        QVector<Transformf> baselineLocal;
        QVector<Transformf> baselineGlobal;
        QVector<Transformf> firstLocal;
        QVector<Transformf> firstGlobal;
        if (!baselineClip.valid() ||
            !samplePose(_skeleton, baselineClip, 0.0f, baselineClip.additive ? &_readyLocalPose : nullptr, _sampledTracks,
                        baselineLocal, baselineGlobal, error) ||
            !samplePose(_skeleton, clip, 0.0f, &baselineLocal, _sampledTracks, firstLocal, firstGlobal, error)) {
            setStatus(QStringLiteral("Cannot evaluate '%1' on state '%2': %3").arg(path, baselinePath, error), true);
            return;
        }
        _stateBaselineLocalPose = std::move(baselineLocal);
        _stateBaselineAnimationPath = baselinePath;
        _clip = std::move(clip);
        if (!_simulationActive) {
            _audio.stop();
        }
        cacheSourceSounds(_clip);
        _activeAction = action.label;
        _activeAnimationPath = path;
        _clipBlendInSeconds = blendSeconds(action.blendInSeconds);
        _clipBlendOutSeconds = blendSeconds(action.blendOutSeconds);
        _playbackTime = 0.0f;
        if (!applyPose(0.0f)) {
            return;
        }
        _timeline->setEnabled(true);
        updateTimelineText();
        refreshFingerEditor();
        appendDiagnostic(QStringLiteral("\nACTION  %1  graph clip=%2\n  SOURCE %3\n  BASE %4")
                             .arg(_activeAction, action.graphNode, path, baselinePath));
        showClipDetails();
        setStatus(QStringLiteral("Loaded %1: %2 tracks, %3 annotations, %4 s; offline clip preview")
                      .arg(_activeAction)
                      .arg(_clip.transformTrackCount)
                      .arg(_clip.annotations.size())
                      .arg(playbackDuration(), 0, 'f', 3));
    }

    void ReloadMakerController::showSelectedAction()
    {
        {
            const QSignalBlocker block(_animationList);
            _animationList->setCurrentRow(_selectedAction);
        }
        if (_clipCombo) {
            const QSignalBlocker block(_clipCombo);
            _clipCombo->setCurrentIndex(_selectedAction);
        }
    }

    void ReloadMakerController::togglePlayback()
    {
        restoreHandPosePreview();
        if (!_clip.valid()) {
            return;
        }
        _playing = !_playing;
        if (_playing) {
            if (_playbackTime >= playbackDuration()) {
                _playbackTime = 0.0f;
            }
            _playbackClock.restart();
            _playbackTimer->start();
        } else {
            _playbackTimer->stop();
        }
        showPlaybackState();
    }

    void ReloadMakerController::stopPlayback()
    {
        _playing = false;
        _playbackTimer->stop();
        showPlaybackState();
    }

    void ReloadMakerController::showPlaybackState()
    {
        _playButton->setText(_playing ? QStringLiteral("PAUSE") : QStringLiteral("PLAY"));
    }

    void ReloadMakerController::replay()
    {
        restoreHandPosePreview();
        if (!_clip.valid()) {
            return;
        }
        _playbackTime = 0.0f;
        if (!applyPose(_playbackTime)) {
            return;
        }
        updateTimelineText();
        refreshFingerEditor();
        if (_playing) {
            _playbackClock.restart();
        }
    }

    // Playback timer tick. It only refreshes the view, so the revision stays.
    // Source sounds between the previous and the new time play, across the
    // loop point when the clip wraps.
    void ReloadMakerController::advancePlayback()
    {
        const QScopedValueRollback<bool> viewOnly(_viewRefresh, true);
        if (!_playing || !_clip.valid()) {
            return;
        }
        const qint64 elapsedNanoseconds = _playbackClock.nsecsElapsed();
        _playbackClock.restart();
        const float elapsedSeconds = static_cast<float>(elapsedNanoseconds) / 1'000'000'000.0f;
        const float soundFrom = _playbackTime;
        _playbackTime += elapsedSeconds * _playbackRate;
        const float duration = playbackDuration();
        if (_playbackTime >= duration) {
            if (_loopPlayback) {
                _playbackTime = std::fmod(_playbackTime, duration);
            } else {
                _playbackTime = duration;
                stopPlayback();
            }
        }
        if (!applyPose(_playbackTime)) {
            return;
        }
        if (_playbackTime >= soundFrom) {
            playSourceSounds(_clip, soundFrom, _playbackTime);
        } else {
            playSourceSounds(_clip, soundFrom, duration);
            playSourceSounds(_clip, -.0001f, _playbackTime);
        }
        updateTimelineText();
        refreshFingerEditor();
    }

    // Moves the timeline to slider position `value` (0..kTimelineMaximum).
    void ReloadMakerController::scrubTimeline(const int value)
    {
        invalidateCollisions();
        restoreHandPosePreview();
        if (!_clip.valid()) {
            return;
        }
        _playbackTime = playbackDuration() * static_cast<float>(value) / static_cast<float>(kTimelineMaximum);
        if (!applyPose(_playbackTime)) {
            return;
        }
        updateTimelineText();
        refreshFingerEditor();
        if (_playing) {
            _playbackClock.restart();
        }
    }

    // Playback speed multiplier; anything not positive plays at normal speed.
    void ReloadMakerController::setPlaybackRate(float rate)
    {
        _playbackRate = rate > 0.0f ? rate : 1.0f;
    }

    void ReloadMakerController::setLoopPlayback(bool loop)
    {
        if (loop == _loopPlayback) {
            return;
        }
        _loopPlayback = loop;
        if (_loopCheck) {
            const QSignalBlocker block(_loopCheck);
            _loopCheck->setChecked(loop);
        }
        noteDocumentChange("loop playback");
    }

    // Clip length plus the approximate exit blend when transitions are shown.
    float ReloadMakerController::playbackDuration() const noexcept
    {
        if (!_clip.valid()) {
            return 0.0f;
        }
        return _clip.durationSeconds + (_approximateTransitions ? _clipBlendOutSeconds : 0.0f);
    }

    // Samples the clip over its state baseline at `timeSeconds`, clamped to
    // the clip. Learned reloads and the interactive simulation use exact
    // source poses; only free clip preview applies the approximate entry and
    // exit blend.
    bool ReloadMakerController::sampleActionPose(const float timeSeconds, QVector<Transformf>& localPose, QVector<Transformf>& globalPose)
    {
        if (!_clip.valid()) {
            return false;
        }
        const float clipTime = std::clamp(timeSeconds, 0.0f, _clip.durationSeconds);
        if (!samplePose(_skeleton, _clip, clipTime, &_stateBaselineLocalPose, _sampledTracks, localPose, globalPose, _poseError)) {
            return false;
        }
        if (_stateBaselineLocalPose.size() != localPose.size()) {
            return false;
        }
        const float actionWeight = (_reloadWorkspaceActive || _simulationActive) ? 1.0f : actionBlendWeight(timeSeconds);
        if (actionWeight < 1.0f) {
            for (qsizetype bone = 0; bone < localPose.size(); ++bone) {
                localPose[bone] = interpolatedTransform(_stateBaselineLocalPose.at(bone), localPose.at(bone), actionWeight);
            }
        }
        return actionWeight >= 1.0f || buildPoseGlobals(_skeleton, localPose, globalPose, _poseError);
    }

    // Weight of the clip over its state baseline: eased in over the entry
    // blend, eased out over the exit blend after the clip ends.
    float ReloadMakerController::actionBlendWeight(const float timeSeconds) const noexcept
    {
        if (!_approximateTransitions) {
            return 1.0f;
        }
        if (_clipBlendInSeconds > 0.0f && timeSeconds < _clipBlendInSeconds) {
            return smoothed(std::clamp(timeSeconds / _clipBlendInSeconds, 0.0f, 1.0f));
        }
        if (_clipBlendOutSeconds > 0.0f && timeSeconds > _clip.durationSeconds) {
            const float progress = std::clamp((timeSeconds - _clip.durationSeconds) / _clipBlendOutSeconds, 0.0f, 1.0f);
            return 1.0f - smoothed(progress);
        }
        return 1.0f;
    }
}
