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
        _sourceLoading = loading;
        if (loading) {
            // A failed/cancelled replacement keeps the learned reload.
            // Successful source installation clears it with caches.
            if(_reloadCapturing)invalidateReloadCapture();
            else {
                _reloadPreviewTimer->stop();_reloadPlaying=false;
                _reloadPlay->setText("Play reload");
            }
            if (_mechanismPreview) {stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);}
            stopPlayback(); _rebuildTimer->stop();
        }
        if (_sourceMenu) _sourceMenu->setEnabled(!loading);
        if (_weaponPicker) _weaponPicker->setEnabled(!loading && _weaponPicker->count() > 0);
        if (_pluginFilter) _pluginFilter->setEnabled(!loading);
        if (_pluginSearch) _pluginSearch->setEnabled(!loading && _pluginFilter->count() > 0);
        // Pages stay put while a source loads: the mode switch, the builder
        // checklist and the panels that edit the weapon are disabled.
        _pages->setEnabled(!loading);
        if(_modeSwitch)_modeSwitch->setEnabled(!loading);
        if(_checklistPanel)_checklistPanel->setEnabled(!loading);
        if(_reloadDock)_reloadDock->setEnabled(!loading);
        if(_reloadGraphDock)_reloadGraphDock->setEnabled(!loading);
        if(_simulationDock)_simulationDock->setEnabled(!loading);
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
