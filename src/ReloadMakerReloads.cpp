#include "ReloadMakerController.h"
#include "ReloadGraph.h"
#include "StudioWindow.h"
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace reloadmaker {
    // The reload page: the learned base reload, played or scrubbed on its
    // graph of moving parts, and the native clip list.
    void ReloadMakerController::createReloadUi()
    {
        auto* page = new QWidget(_pages);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(8, 12, 8, 8);
        auto* title = new QLabel("RELOAD / LEARN", page);
        title->setStyleSheet("color:#e6bd7d;font-size:19px;font-weight:700;");
        layout->addWidget(title);
        auto* intro = new QLabel("Maker learns the weapon's base reload from its source. Play it, or click the graph to see any moment.", page);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        _reloadSourceLabel = new QLabel("Load a weapon to resolve its base reload.", page);
        _reloadSourceLabel->setWordWrap(true);
        _reloadSourceLabel->setTextFormat(Qt::PlainText);
        layout->addWidget(_reloadSourceLabel);
        _reloadAnalyze = new QPushButton("LEARN RELOAD AGAIN", page);
        layout->addWidget(_reloadAnalyze);
        _reloadProgress = new QProgressBar(page);
        layout->addWidget(_reloadProgress);
        _reloadProgress->hide();
        _reloadSummary = new QLabel(page);
        _reloadSummary->setWordWrap(true);
        layout->addWidget(_reloadSummary);
        _reloadPlay = new QPushButton("PLAY RELOAD", page);
        layout->addWidget(_reloadPlay);
        auto* clips = new QLabel("NATIVE CLIPS", page);
        clips->setObjectName("Section");
        layout->addWidget(clips);
        auto* clipsHint = new QLabel("Inspect other native clips on the timeline. PLAY RELOAD or a click on the graph returns to the reload.", page);
        clipsHint->setWordWrap(true);
        clipsHint->setObjectName("Hint");
        layout->addWidget(clipsHint);
        _animationList->setParent(page);
        _animationList->setMinimumHeight(140);
        _animationList->setMaximumHeight(220);
        _animationList->show();
        layout->addWidget(_animationList);
        layout->addStretch(1);
        _pages->addTab(page, "04  RELOAD");

        _reloadGraphDock = new QDockWidget("RELOAD PARTS · when each part moves · click to see that moment", _window);
        _reloadGraphDock->setObjectName("ReloadMakerReloadGraphDock");
        _reloadGraphDock->setAllowedAreas(Qt::BottomDockWidgetArea);
        auto* graphScroll = new QScrollArea(_reloadGraphDock);
        graphScroll->setWidgetResizable(true);
        _reloadGraph = new ReloadGraph(graphScroll);
        graphScroll->setWidget(_reloadGraph);
        _reloadGraphDock->setWidget(graphScroll);
        _window->addDockWidget(Qt::BottomDockWidgetArea, _reloadGraphDock);
        _reloadGraphDock->hide();

        _reloadCaptureTimer = new QTimer(this);
        _reloadCaptureTimer->setInterval(0);
        _reloadPreviewTimer = new QTimer(this);
        _reloadPreviewTimer->setInterval(16);
        _reloadPreviewTimer->setTimerType(Qt::PreciseTimer);
        connect(_reloadCaptureTimer, &QTimer::timeout, this, &ReloadMakerController::advanceReloadCapture);
        connect(_reloadPreviewTimer, &QTimer::timeout, this, &ReloadMakerController::tickReloadPreview);
        connect(_pages, &QTabWidget::currentChanged, this, [this](int i) { setReloadWorkspace(i == kReloadPage); });
        connect(_reloadAnalyze, &QPushButton::clicked, this, [this] {
            invalidateReloadCapture();
            startReloadCapture();
        });
        connect(_reloadPlay, &QPushButton::clicked, this, [this] { setReloadPlaying(!_reloadPlaying); });
        _reloadGraph->sought = [this](float seconds) {
            restoreReloadClip();
            _reloadPlaying = false;
            _reloadPreviewTimer->stop();
            _reloadPlay->setText("PLAY RELOAD");
            seekReload(seconds);
        };
        invalidateReloadCapture();
    }

    void ReloadMakerController::setReloadWorkspace(bool active)
    {
        if (_reloadWorkspaceActive == active) {
            return;
        }
        _reloadWorkspaceActive = active;
        if (active) {
            restoreHandPosePreview();
            stopPlayback();
            // Dock visibility is owned by setWorkspace; the reload page only
            // forces the weapon-fixed frame and restores the user's choice after.
            _reloadOldWeaponFixed = _weaponFixedMotion;
            _weaponFixedMotion = true;
            showWeaponFixedMotion();
            _window->resizeDocks({ _reloadGraphDock }, { 230 }, Qt::Vertical);
            startReloadCapture();
        } else {
            if (_reloadCapturing) {
                invalidateReloadCapture();
            }
            _reloadPreviewTimer->stop();
            _reloadPlaying = false;
            _weaponFixedMotion = _reloadOldWeaponFixed;
            showWeaponFixedMotion();
            if (_clip.valid()) {
                (void)applyPose(_playbackTime);
            }
        }
        updateGripViewport();
    }

    // Shows the learned reload: the graph of its moving parts and the summary.
    void ReloadMakerController::showReloadRecipe()
    {
        if (!_reloadRecipe || _reloadCapturing) {
            return;
        }
        const auto& r = *_reloadRecipe;
        _reloadGraph->setRecipe(&r);
        _reloadGraph->setTime(_playbackTime);
        _reloadPlay->setEnabled(true);
        if (_reloadGraph->movingParts() == 0) {
            _reloadSummary->setText("This base reload contains no measurable weapon-part movement. No substitute animation was used.");
        } else {
            _reloadSummary->setText(QStringLiteral("%1 moving parts · %2 native part tracks").arg(_reloadGraph->movingParts()).arg(r.tracks.size()));
        }
    }

    // Plays or pauses the learned reload; playing from its end starts over.
    void ReloadMakerController::setReloadPlaying(bool playing)
    {
        if (!_reloadRecipe || _reloadCapturing || playing == _reloadPlaying) {
            return;
        }
        restoreReloadClip();
        _reloadPlaying = playing;
        if (playing) {
            if (_playbackTime >= _reloadRecipe->duration) {
                seekReload(0);
            }
            _reloadClock.restart();
            _reloadPreviewTimer->start();
        } else {
            _reloadPreviewTimer->stop();
        }
        _reloadPlay->setText(playing ? "PAUSE RELOAD" : "PLAY RELOAD");
    }

    void ReloadMakerController::seekReload(float seconds)
    {
        if (!_reloadWorkspaceActive || !_reloadRecipe || _reloadCapturing || !_clip.valid() || _activeAnimationPath != _reloadExpectedPath) {
            return;
        }
        _playbackTime = std::clamp(seconds, 0.f, _reloadRecipe->duration);
        if (!applyPose(_playbackTime)) {
            invalidateReloadCapture();
            return;
        }
        _reloadGraph->setTime(_playbackTime);
    }

    // Playback clock tick. It only refreshes the view, so the revision stays.
    void ReloadMakerController::tickReloadPreview()
    {
        const QScopedValueRollback<bool> viewOnly(_viewRefresh, true);
        if (!_reloadRecipe || !_reloadWorkspaceActive || !_reloadPlaying) {
            _reloadPreviewTimer->stop();
            return;
        }
        const float delta = float(_reloadClock.nsecsElapsed()) / 1e9f;
        _reloadClock.restart();
        const auto previous = _playbackTime;
        seekReload(_playbackTime + delta);
        playSourceSounds(_reloadClip, previous, _playbackTime);
        if (_playbackTime >= _reloadRecipe->duration) {
            _reloadPlaying = false;
            _reloadPlay->setText("PLAY RELOAD");
            _reloadPreviewTimer->stop();
        }
    }

    // Empties the reload page after the recipe is dropped.
    void ReloadMakerController::resetReloadPage()
    {
        if (!_reloadPlay) {
            return;
        }
        _reloadAnalyze->setEnabled(true);
        _reloadPlay->setEnabled(false);
        _reloadPlay->setText("PLAY RELOAD");
        _reloadProgress->hide();
        _reloadSummary->setText("Select a configured weapon. Its base Reload is the only motion source for this page.");
    }

    void ReloadMakerController::showReloadCaptureStarted(const QString& expected)
    {
        _reloadSourceLabel->setText(QStringLiteral("Native reload · %1").arg(QFileInfo(QFileInfo(expected).path()).fileName()));
        _reloadSourceLabel->setToolTip(expected + "\n" + (_libraryMode ? _packageResources.provider(expected) : _package.sourcePath));
        _reloadAnalyze->setEnabled(false);
        _reloadProgress->setRange(0, _reloadRecipe->frames);
        _reloadProgress->setValue(0);
        _reloadProgress->show();
        _reloadSummary->setText("Sampling native weapon-part poses…");
    }
}
