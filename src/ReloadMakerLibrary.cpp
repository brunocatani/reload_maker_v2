#include "ReloadMakerController.h"
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

namespace reloadmaker {
    void ReloadMakerController::createLibraryPage()
    {
        auto* page = new QWidget(_pages);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 10, 0, 0);
        auto* help = new QLabel(QStringLiteral(
            "ACTIVE LOAD ORDER\nLaunch this executable through MO2. Browse weapons by plugin; parts and animations use the whole profile's winning files."), page);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* instanceRow = new QHBoxLayout();
        _mo2Path = new QLineEdit(page);
        _mo2Path->setPlaceholderText(QStringLiteral("MO2 instance folder"));
        _mo2Path->setText(QSettings().value("paths/mo2Instance", "D:/FO4").toString());
        auto* browse = new QPushButton(QStringLiteral("MO2…"), page);
        instanceRow->addWidget(_mo2Path, 1); instanceRow->addWidget(browse);
        layout->addLayout(instanceRow);
        _scanLibrary = new QPushButton(QStringLiteral("LOAD ACTIVE PROFILE"), page);
        layout->addWidget(_scanLibrary);
        _librarySummary = new QLabel(QStringLiteral("No library loaded. Folder / ZIP mode remains available."), page);
        _librarySummary->setWordWrap(true);
        layout->addWidget(_librarySummary);
        _pluginFilter = new QComboBox(page);
        _pluginFilter->setMinimumContentsLength(16);
        _pluginFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        _pluginFilter->addItem(QStringLiteral("All enabled plugins"), QString());
        layout->addWidget(_pluginFilter);
        _weaponSearch = new QLineEdit(page);
        _weaponSearch->setPlaceholderText(QStringLiteral("Search weapon name, editor ID or ESP…"));
        _weaponSearch->setClearButtonEnabled(true);
        layout->addWidget(_weaponSearch);
        _libraryWeapons = new QListWidget(page);
        _libraryWeapons->setAlternatingRowColors(true);
        layout->addWidget(_libraryWeapons, 1);
        auto* hint = new QLabel(QStringLiteral("Select a weapon, then open Modding or Animations. Indexing does not preload every mesh and texture into graphics memory."), page);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        _pages->addTab(page, QStringLiteral("03  LIBRARY"));
        connect(browse, &QPushButton::clicked, this, [this]() {
            const auto folder = QFileDialog::getExistingDirectory(_window, QStringLiteral("Select MO2 instance"), _mo2Path->text());
            if (!folder.isEmpty()) { _mo2Path->setText(folder); }
        });
        connect(_scanLibrary, &QPushButton::clicked, this, &ReloadMakerController::startLibraryLoad);
        connect(_weaponSearch, &QLineEdit::textChanged, this, &ReloadMakerController::refreshLibraryWeapons);
        connect(_pluginFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, &ReloadMakerController::refreshLibraryWeapons);
        const auto select = [this](QListWidgetItem* item) {
            if (item && _libraryMode && !_loadThread) {
                _weaponCombo->setCurrentIndex(item->data(Qt::UserRole).toInt());
            }
        };
        connect(_libraryWeapons, &QListWidget::itemClicked, this, select);
        connect(_libraryWeapons, &QListWidget::itemActivated, this, select);
    }

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
        _packageGroup->setEnabled(!loading);
        _referenceGroup->setEnabled(!loading);
        _weaponCombo->setEnabled(!loading);
        _pages->setTabEnabled(0, !loading); _pages->setTabEnabled(1, !loading);
        if(_pages->count()>3)_pages->setTabEnabled(3,!loading);
        if(_reloadInspectorDock)_reloadInspectorDock->setEnabled(!loading);
        if(_reloadGraphDock)_reloadGraphDock->setEnabled(!loading);
        if (!loading) { _pages->setCurrentIndex(_pageBeforeLoad); }
        _scanLibrary->setEnabled(!loading); _mo2Path->setEnabled(!loading);
        _libraryWeapons->setEnabled(!loading);
        _reloadSource->setEnabled(!loading);
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
            _librarySummary->setText(message); setStatus(message, true); return;
        }
        const auto instance = _mo2Path->text();
        const auto data = _gameDataPath->text();
        const auto launch = _launchSnapshot;
        auto result = std::make_shared<LoadOrderLibrary>();
        setLoading(true, QStringLiteral("Reading the active MO2 profile…"));
        const auto progress = [this](const QString& message, int value, int total) {
            QMetaObject::invokeMethod(this, [this, message, value, total]() {
                _loadingLabel->setText(message);
                _loadingProgress->setRange(0, total);
                _loadingProgress->setValue(value);
            }, Qt::QueuedConnection);
        };
        _loadThread = QThread::create([result, instance, data, launch, progress]() {
            try {
                const auto snapshot = readLoadOrderSnapshot(instance, data);
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
                _librarySummary->setText(result->error);
                for (const auto& plugin : result->plugins) {
                    if (!plugin.error.isEmpty()) { appendDiagnostic(QStringLiteral("PLUGIN ERROR %1: %2").arg(plugin.name, plugin.error)); }
                }
                setStatus(result->error, true); return;
            }
            setLibraryResources(nullptr);
            clearAssetCaches();
            _libraryMode = true;
            _launchSnapshot = result->snapshot;
            _gameData = result->snapshot.dataPath;
            _gameDataPath->setText(_gameData);
            _package = { _gameData, {}, {}, false };
            _packageResources = std::move(result->resources);
            _gameMeshes.close(); _gameAnimations.close();
            _plugins.clear(); _plugins.append(std::move(result->catalog));
            _libraryPlugins = result->plugins;
            configureResourcePaths();
            refreshWeapons(); refreshAnimations();
            _details->clear();
            {
                const QSignalBlocker blocker(_pluginFilter);
                _pluginFilter->clear();
                _pluginFilter->addItem(QStringLiteral("All enabled plugins"), QString());
                for (const auto& plugin : _libraryPlugins) {
                    _pluginFilter->addItem(QStringLiteral("%1  ·  %2 WEAP / %3 OMOD")
                        .arg(plugin.name).arg(plugin.weapons).arg(plugin.objectMods), plugin.name);
                }
            }
            _librarySummary->setText(QStringLiteral("%1  /  %2\n%3 weapons · %4 plugins · %5 winning resources")
                .arg(QFileInfo(_launchSnapshot.instancePath).fileName(), _launchSnapshot.profileName)
                .arg(_plugins.first().weapons.size()).arg(_libraryPlugins.size()).arg(_packageResources.files().size()));
            _packageGroup->setTitle(QStringLiteral("STANDALONE FALLBACK — FOLDER / ZIP"));
            appendDiagnostic(QStringLiteral("LIBRARY %1 profile=%2 · loose VFS wins; archive priority follows enabled plugin load order")
                .arg(_launchSnapshot.instancePath, _launchSnapshot.profileName));
            for (const auto& diagnostic : _plugins.first().diagnostics) { appendDiagnostic(diagnostic); }
            refreshLibraryWeapons();
            QSettings settings;
            settings.setValue("paths/mo2Instance", _launchSnapshot.instancePath);
            settings.setValue("paths/gameData", _gameData);
            restoreSessionSelection();
            setStatus(QStringLiteral("Library indexed. Select a plugin and weapon; Modding and Animations share the same winning resource view."));
        });
        _loadThread->start();
    }

    void ReloadMakerController::refreshLibraryWeapons()
    {
        _libraryWeapons->clear();
        if (!_libraryMode || _plugins.isEmpty()) { return; }
        const auto& plugin = _plugins.first();
        const QString filter = _pluginFilter->currentData().toString();
        const QString query = _weaponSearch->text().trimmed();
        for (int index = 0; index < _weaponSelections.size(); ++index) {
            const auto& weapon = plugin.weapons[_weaponSelections[index].weaponIndex];
            const auto origin = plugin.origins.value(weapon.formId);
            if (!filter.isEmpty() && origin.plugin.compare(filter, Qt::CaseInsensitive) != 0 && origin.winner.compare(filter, Qt::CaseInsensitive) != 0) { continue; }
            const auto text = QStringLiteral("%1\n%2  ·  %3").arg(weapon.name, weapon.editorId, origin.plugin);
            if (!query.isEmpty() && !(text + origin.winner).contains(query, Qt::CaseInsensitive)) { continue; }
            auto* item = new QListWidgetItem(text, _libraryWeapons);
            item->setData(Qt::UserRole, index);
            item->setToolTip(QStringLiteral("Origin: %1:%2\nWinning record: %3\nAssets and compatible modifications resolve across the entire profile.")
                .arg(origin.plugin).arg(origin.objectId, 6, 16, QLatin1Char('0')).arg(origin.winner));
            if (index == _weaponCombo->currentIndex()) { _libraryWeapons->setCurrentItem(item); }
        }
    }

    const ResourceArchive& ReloadMakerController::referenceMeshes() const
    {
        return _libraryMode ? _packageResources : _gameMeshes;
    }
}
