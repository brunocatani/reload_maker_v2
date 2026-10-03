#include "ReloadMakerController.h"
#include <QMenu>
#include "LibraryResources.h"
#include "StudioWindow.h"
#include "model/nifmodel.h"

#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QToolButton>

namespace reloadmaker {
    void ReloadMakerController::setLoading(bool loading, const QString& message)
    {
        if (loading) {
            // A failed/cancelled replacement must retain completed authored
            // windows. Successful source installation clears them with caches.
            if(_reloadCapturing)invalidateReloadCapture();
            else {
                _reloadPreviewTimer->stop();_reloadPlaying=false;_reloadReturning=false;_reloadHeld=false;
                _reloadPlay->setText("PLAY RELOAD");
                if(_reloadDrive)_reloadDrive->setActive(false,false);
            }
            if (_mechanismPreview) {stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);}
            _pageBeforeLoad = _pages->currentIndex();
            _playing = false; _playbackTimer->stop(); _rebuildTimer->stop();
            _playButton->setText(QStringLiteral("PLAY"));
        }
        if (_sourceMenu) _sourceMenu->setEnabled(!loading);
        if (_weaponPicker) _weaponPicker->setEnabled(!loading && _weaponPicker->count() > 0);
        if (_pluginFilter) _pluginFilter->setEnabled(!loading);
        if (_pluginSearch) _pluginSearch->setEnabled(!loading && _pluginFilter->count() > 0);
        _pages->setTabEnabled(kLoadoutPage, !loading);
        if(_pages->count()>kReloadPage)_pages->setTabEnabled(kReloadPage,!loading);
        if(_reloadInspectorDock)_reloadInspectorDock->setEnabled(!loading);
        if(_reloadGraphDock)_reloadGraphDock->setEnabled(!loading);
        if(_simulationDock)_simulationDock->setEnabled(!loading);
        if (!loading) { _pages->setCurrentIndex(_pageBeforeLoad); }
        _playButton->setEnabled(!loading);
        _timelineDock->setEnabled(!loading);
        _handPoseDock->widget()->setEnabled(!loading);
        updateGripViewport();
        if (!loading) { updateHandPoseControls(); }
        _loadingLabel->setText(message);
        _loadingLabel->setVisible(loading);
        _loadingProgress->setRange(0, 0);
        _loadingProgress->setVisible(loading);
        _cancelLoading->setEnabled(loading);
        _cancelLoading->setVisible(loading);
        if (loading) { setStatus(message); }
    }

}
