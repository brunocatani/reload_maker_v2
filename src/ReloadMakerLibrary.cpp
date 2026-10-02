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

    void ReloadMakerController::startLibraryLoad()
    {
        if (_loadThread) { return; }
        if (!insideMo2VirtualFilesystem()) {
            const auto message = QStringLiteral("Launch ReloadMakerV2.exe through MO2 to browse its active profile. You can open a mod folder or ZIP directly in this window.");
            setStatus(message, true); return;
        }
        const auto instance = _mo2Instance;
        const auto data = _gameDataText;
        const auto launch = _launchSnapshot;
        const auto requested = _requestedMo2Profile;
        auto result = std::make_shared<LoadOrderLibrary>();
        setLoading(true, QStringLiteral("Reading the active MO2 profile…"));
        const auto progress = [this](const QString& message, int value, int total) {
            QMetaObject::invokeMethod(this, [this, message, value, total]() {
                _loadingLabel->setText(message);
                _loadingProgress->setRange(0, total);
                _loadingProgress->setValue(value);
            }, Qt::QueuedConnection);
        };
        _loadThread = QThread::create([result, instance, data, launch, progress, requested]() {
            try {
                const auto snapshot = readLoadOrderSnapshot(instance, data, requested);
                if (!snapshot.valid()) { result->error = snapshot.error; return; }
                if (launch.valid() && (launch.instancePath != snapshot.instancePath || launch.fingerprint != snapshot.fingerprint)) {
                    result->error = QStringLiteral("MO2 profile/load order changed since this viewer attached. Relaunch through MO2 so virtual files and plugin order match.");
                    return;
                }
                *result = loadOrderLibrary(snapshot, progress, []() { return QThread::currentThread()->isInterruptionRequested(); });
            } catch (const std::exception& exception) { result->error = QString::fromUtf8(exception.what()); }
        });
        _loadThread->setParent(this);
        connect(_loadThread, &QThread::finished, this, [this, result]() {
            result->cancelled |= _loadThread->isInterruptionRequested();
            _loadThread->deleteLater(); _loadThread = nullptr;
            if (_closing) { return; }
            setLoading(false);
            // Cancellation or a bad asset must not allow a later retry to pair
            // a newly selected profile with this process's earlier VFS launch.
            if (!_launchSnapshot.valid() && result->snapshot.valid()) { _launchSnapshot = result->snapshot; }
            if (result->cancelled) { setStatus(QStringLiteral("Library loading cancelled; previous source retained.")); return; }
            if (!result->error.isEmpty()) {
                for (const auto& plugin : result->plugins) {
                    if (!plugin.error.isEmpty()) { appendDiagnostic(QStringLiteral("PLUGIN ERROR %1: %2").arg(plugin.name, plugin.error)); }
                }
                setStatus(result->error, true); return;
            }
            setLibraryResources(nullptr);
            if(_pendingSession.isEmpty())_sessionPath.clear();
            clearAssetCaches();
            _libraryMode = true;
            _launchSnapshot = result->snapshot;
            _gameData = result->snapshot.dataPath;
            _gameDataText = _gameData;
            _package = {};_package.rootPath=_gameData;
            _packageResources = std::move(result->resources);
            _gameMeshes.close(); _gameAnimations.close();_gameSounds.close();
            _plugins.clear(); _plugins.append(std::move(result->catalog));
            _libraryPlugins = result->plugins;
            _details->clear();
            configureResourcePaths();
            refreshWeapons(); refreshAnimations();
            appendDiagnostic(QStringLiteral("LIBRARY %1 profile=%2 · loose VFS wins; archive priority follows enabled plugin load order")
                .arg(_launchSnapshot.instancePath, _launchSnapshot.profileName));
            for (const auto& diagnostic : _plugins.first().diagnostics) { appendDiagnostic(diagnostic); }
            refreshSourceBar();
            QSettings settings;
            settings.setValue("paths/mo2Instance", _launchSnapshot.instancePath);
            settings.setValue("paths/gameData", _gameData);
            restoreSessionSelection();
            setStatus(QStringLiteral("MO2 profile indexed. Choose a plugin, then its weapon, in the source bar."));
            if (_weaponCombo->currentIndex() < 0 && _pluginFilter->count() > 1) _pluginFilter->showPopup();
        });
        _loadThread->start();
    }

    const ResourceArchive& ReloadMakerController::referenceMeshes() const
    {
        return _libraryMode||!_package.resourceData.isEmpty() ? _packageResources : _gameMeshes;
    }
}
