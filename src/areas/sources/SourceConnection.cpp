// Installation setup, the home route choice and the three ways a source is
// connected: the MO2 profile the editor runs in, a hand-off that relaunches
// the editor inside another MO2 profile, and the base game outside MO2.
#include "ReloadMakerController.h"
#include "areas/sources/PackageLoad.h"
#include "InstallationSetup.h"
#include "LibraryResources.h"
#include "StudioPaths.h"
#include "StudioWindow.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QSaveFile>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <memory>

#include <stdexcept>

namespace reloadmaker
{
    namespace
    {
        constexpr int kHandoffTimeoutMs = 20000;

        bool samePath(const QString& left, const QString& right)
        {
            return QDir::cleanPath(left).compare(QDir::cleanPath(right), Qt::CaseInsensitive) == 0;
        }

        QString quoted(QString value)
        {
            value.replace('"', QStringLiteral("\\\""));
            return QLatin1Char('"') + value + QLatin1Char('"');
        }

        // True when this editor already runs inside the configured profile's
        // virtual file system with an unchanged load order.
        bool attachedToProfile(const LoadOrderSnapshot& launch, const InstallationSetup& setup, const InstallationCheck& check)
        {
            return insideMo2VirtualFilesystem() && launch.valid() && launch.profileName == setup.profile &&
                   samePath(launch.instancePath, setup.mo2Instance) && samePath(launch.dataPath, check.dataPath) &&
                   launch.fingerprint == profileFingerprint(setup.mo2Instance, check.profilePath);
        }
    }

    QJsonObject ReloadMakerController::installationState() const
    {
        QString error;
        const InstallationSetup setup = loadInstallationSetup(&error);
        QJsonObject result = checkInstallation(setup).json();
        result["settingsError"] = error;
        result["settings"] = installationJson(setup);
        result["workspace"] = studioRoot();
        result["virtualFilesystem"] = insideMo2VirtualFilesystem();
        return result;
    }

    bool ReloadMakerController::configureInstallation(const QJsonObject& json)
    {
        InstallationSetup setup;
        QString error;
        if (!readInstallation(json, setup, error) || !saveInstallationSetup(setup, error)) {
            setStatus(error, true);
            return false;
        }
        // Setup changes future loads, never the active source under an open project.
        const InstallationCheck check = checkInstallation(setup);
        if (check.collisionReady) {
            qputenv("ELRIC_EXE", check.elric.toUtf8());
        } else {
            qunsetenv("ELRIC_EXE");
        }
        ++_revision;
        setStatus(check.gameReady ? "Setup saved. Connect or open a source when ready."
                                  : "Setup draft saved. Select a valid game folder to load sources.");
        return true;
    }

    QJsonObject ReloadMakerController::homeState() const
    {
        QSettings settings;
        const QString recent = settings.value("projects/lastProject").toString();
        const auto* dialog = _window->findChild<QDialog*>("MakerHome");
        return {
            { "role", settings.value("home/role").toString() },
            { "workspace", studioRoot() },
            { "recentProject", recent },
            { "canResume", !recent.isEmpty() && QFileInfo(recent).isFile() },
            { "visible", dialog && dialog->isVisible() },
            { "installation", installationState() },
            { "playerNeedsCreationKit", false },
            { "playerNeedsSourceScene", false },
            { "creatorNeedsKitForNewCollision", true },
        };
    }

    QJsonObject ReloadMakerController::chooseHomeRole(const QString& role)
    {
        if (role != "creator" && role != "player") {
            throw std::runtime_error("Choose creator or player");
        }
        QSettings settings;
        settings.setValue("home/role", role);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            throw std::runtime_error("Could not save the workspace's home preference");
        }
        return homeState();
    }

    bool ReloadMakerController::connectInstalledProfile()
    {
        const InstallationSetup setup = loadInstallationSetup();
        const InstallationCheck check = checkInstallation(setup);
        if (!check.gameReady || !check.profileReady) {
            setStatus(check.errors.join('\n'), true);
            return false;
        }
        if (!attachedToProfile(_launchSnapshot, setup, check)) {
            return handOffToMo2(setup, check);
        }
        _mo2Instance = setup.mo2Instance;
        _requestedMo2Profile = setup.profile;
        _gameDataText = check.dataPath;
        startLibraryLoad();
        return true;
    }

    bool ReloadMakerController::handOffToMo2(const InstallationSetup& setup, const InstallationCheck& check)
    {
        // MO2 starts a second editor inside the profile's virtual file system.
        // That editor verifies the hand-off token, then this one closes; on any
        // failure this editor and its project stay open.
        if (!_profileHandoff.isEmpty()) {
            setStatus("An MO2 connection is already starting.", true);
            return false;
        }
        if (_loadThread || _reloadCapturing || !saveSession()) {
            setStatus("Finish the current load/capture and save the project before connecting.", true);
            return false;
        }
        const QString executable = QDir(setup.mo2Instance).filePath("ModOrganizer.exe");
        if (!QFileInfo::exists(executable)) {
            setStatus("ModOrganizer.exe is missing from the selected instance.", true);
            return false;
        }
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString arguments = "--project " + quoted(studioRoot()) + " --mo2-instance " + quoted(setup.mo2Instance) +
                                  " --mo2-profile " + quoted(setup.profile) + " --game-data " + quoted(check.dataPath) +
                                  " --handoff " + token + " --library";
        auto* process = new QProcess(this);
        process->setProgram(executable);
        process->setArguments({ "--profile", setup.profile, "run", "--arguments", arguments, QCoreApplication::applicationFilePath() });
        process->setWorkingDirectory(setup.mo2Instance);
        _profileHandoff = token;
        connect(process, &QProcess::errorOccurred, this, [this, process, token] {
            if (_profileHandoff == token) {
                _profileHandoff.clear();
                setStatus("MO2 connection failed: " + process->errorString() + ". Your project is still open.", true);
            }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, process, token](int code, QProcess::ExitStatus status) {
                    if (_profileHandoff == token && (code != 0 || status != QProcess::NormalExit)) {
                        _profileHandoff.clear();
                        setStatus("MO2 rejected the connection. Your project is still open.", true);
                    }
                    process->deleteLater();
                });
        process->start();
        QTimer::singleShot(kHandoffTimeoutMs, this, [this, token] {
            if (_profileHandoff == token) {
                _profileHandoff.clear();
                setStatus("MO2 did not connect a verified editor session. Your current project was retained.", true);
            }
        });
        setStatus("Opening the selected MO2 profile. This project will close only after the new source session verifies its virtual files.");
        return true;
    }

    bool ReloadMakerController::openBaseGame()
    {
        if (_loadThread || _reloadCapturing || _packProcess) {
            setStatus("Finish the current source or asset job first.", true);
            return false;
        }
        if (insideMo2VirtualFilesystem()) {
            setStatus("Use the connected MO2 library and choose Fallout4.esm to author base weapons in that profile's effective file view.", true);
            return false;
        }
        const InstallationCheck check = checkInstallation(loadInstallationSetup());
        if (!check.gameReady) {
            setStatus(check.errors.join('\n'), true);
            return false;
        }
        if (currentWeapon() && !saveSession()) {
            setStatus("Save the current project before opening base game sources.", true);
            return false;
        }

        const QDir data(check.dataPath);
        const QDir game(check.gameRoot);
        QStringList plugins{ "Fallout4.esm" };
        if (QFileInfo::exists(data.filePath("Fallout4_VR.esm"))) {
            plugins.append("Fallout4_VR.esm");
        }
        const QString ini = game.filePath(QFileInfo::exists(game.filePath("Fallout4.ini")) ? "Fallout4.ini" : "Fallout4_Default.ini");
        QString error;
        const QStringList archives = resolveGameArchives(check.dataPath, plugins, { ini }, error);
        if (!error.isEmpty()) {
            setStatus(error, true);
            return false;
        }

        // The base game loads as a source pack written to a fresh session folder.
        QJsonArray inputs;
        for (const QString& plugin : plugins) {
            inputs.append(data.filePath(plugin));
        }
        const QString directory = studioDataPath("local/source-sessions/" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!QDir().mkpath(directory)) {
            setStatus("Cannot create the base source snapshot.", true);
            return false;
        }
        const QString path = directory + "/base-game.reloadsources.json";
        const QJsonObject definition{
            { "format", "reload-maker-source-pack" },
            { "version", 1 },
            { "name", "Base game and loose Data" },
            { "plugins", inputs },
            { "resourceData", check.dataPath },
            { "archives", QJsonArray::fromStringList(archives) },
        };
        const QByteArray bytes = QJsonDocument(definition).toJson();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            setStatus("Cannot save the base source snapshot.", true);
            return false;
        }
        _packageText = path;
        _gameDataText = check.dataPath;
        startPackageLoad(path, check.dataPath, true);
        return true;
    }

    void ReloadMakerController::startLibraryLoad(bool reuse)
    {
        if (_loadThread) {
            return;
        }
        if (!insideMo2VirtualFilesystem()) {
            setStatus("Launch ReloadMakerV2.exe through MO2 to browse its active profile. You can open a mod folder or ZIP directly in this window.", true);
            return;
        }
        const QString instance = _mo2Instance;
        const QString data = _gameDataText;
        const LoadOrderSnapshot launch = _launchSnapshot;
        const QString requested = _requestedMo2Profile;
        auto result = std::make_shared<LoadOrderLibrary>();
        setLoading(true, "Reading the active MO2 profile…");
        const auto progress = [this](const QString& message, int value, int total) {
            QMetaObject::invokeMethod(this, [this, message, value, total]() {
                _loadingLabel->setText(message);
                _loadingProgress->setRange(0, total);
                _loadingProgress->setValue(value);
            }, Qt::QueuedConnection);
        };
        // The worker reads only the values copied above and fills `result`.
        auto timings = std::make_shared<LoadTimings>();
        // Opening a project from the installed profile installs it again when
        // its profile files and enabled plugins and archives are unchanged.
        struct Reuse
        {
            bool installed{ false };
            QByteArray stamp;
        };
        auto reuseState = std::make_shared<Reuse>();
        const bool installed = reuse && _libraryMode && _packageResources.isOpen() && !_libraryStamp.isEmpty() && _launchSnapshot.valid() &&
                               (requested.isEmpty() || requested == _launchSnapshot.profileName);
        const QByteArray installedStamp = _libraryStamp;
        const QByteArray installedFingerprint = _launchSnapshot.fingerprint;
        const QString installedInstance = _launchSnapshot.instancePath;
        _loadThread = QThread::create([result, timings, reuseState, installed, installedStamp, installedFingerprint, installedInstance, instance, data,
                                       launch, progress, requested]() {
            StageClock clock(*timings, "library.load");
            try {
                const LoadOrderSnapshot snapshot = readLoadOrderSnapshot(instance, data, requested);
                if (!snapshot.valid()) {
                    result->error = snapshot.error;
                    return;
                }
                if (launch.valid() && (launch.instancePath != snapshot.instancePath || launch.fingerprint != snapshot.fingerprint)) {
                    result->error = "MO2 profile/load order changed since this viewer attached. Relaunch through MO2 so virtual files and plugin order match.";
                    return;
                }
                reuseState->stamp = sourceStamp(libraryStampFiles(snapshot));
                if (installed && snapshot.instancePath == installedInstance && snapshot.fingerprint == installedFingerprint &&
                    reuseState->stamp == installedStamp) {
                    reuseState->installed = true;
                    timings->add(QStringLiteral("library.reused installed"), 0);
                    return;
                }
                *result = loadOrderLibrary(snapshot, progress, []() { return QThread::currentThread()->isInterruptionRequested(); });
            } catch (const std::exception& exception) {
                result->error = QString::fromUtf8(exception.what());
            }
        });
        _loadThread->setParent(this);
        connect(_loadThread, &QThread::finished, this, [this, result, timings, reuseState]() {
            _loadTimings.append(*timings);
            result->cancelled |= _loadThread->isInterruptionRequested();
            _loadThread->deleteLater();
            _loadThread = nullptr;
            if (_closing) {
                return;
            }
            setLoading(false);
            // Cancellation or a bad asset must not allow a later retry to pair
            // a newly selected profile with this process's earlier VFS launch.
            if (!_launchSnapshot.valid() && result->snapshot.valid()) {
                _launchSnapshot = result->snapshot;
            }
            if (result->cancelled) {
                setStatus("Library loading cancelled; previous source retained.");
                return;
            }
            if (!result->error.isEmpty()) {
                for (const auto& plugin : result->plugins) {
                    if (!plugin.error.isEmpty()) {
                        appendDiagnostic(QStringLiteral("PLUGIN ERROR %1: %2").arg(plugin.name, plugin.error));
                    }
                }
                setStatus(result->error, true);
                return;
            }
            StageClock install(_loadTimings, "library.install");
            if (reuseState->installed) {
                setLibraryResources(nullptr);
                LoadOrderLibrary current;
                current.snapshot = _launchSnapshot;
                current.catalog = std::move(_plugins.first());
                current.plugins = _libraryPlugins;
                current.resources = std::move(_packageResources);
                installLibrary(current, true);
                return;
            }
            installLibrary(*result);
            _libraryStamp = reuseState->stamp;
        });
        _loadThread->start();
    }

    void ReloadMakerController::installLibrary(LoadOrderLibrary& library, bool keepTextures)
    {
        setLibraryResources(nullptr);
        parkCurrentPackage();
        if (_pendingSession.isEmpty()) {
            _sessionPath.clear();
        }
        clearAssetCaches(keepTextures);
        _libraryMode = true;
        _launchSnapshot = library.snapshot;
        _gameData = library.snapshot.dataPath;
        _gameDataText = _gameData;
        _package = {};
        _package.rootPath = _gameData;
        _packageResources = std::move(library.resources);
        _gameMeshes.close();
        _gameAnimations.close();
        _gameSounds.close();
        _plugins.clear();
        _plugins.append(std::move(library.catalog));
        _libraryPlugins = library.plugins;
        clearDiagnostics();
        configureResourcePaths();
        refreshWeapons();
        refreshAnimations();
        appendDiagnostic(QStringLiteral("LIBRARY %1 profile=%2 · loose VFS wins; archive priority follows enabled plugin load order")
                             .arg(_launchSnapshot.instancePath, _launchSnapshot.profileName));
        for (const auto& diagnostic : _plugins.first().diagnostics) {
            appendDiagnostic(diagnostic);
        }
        refreshSourceBar();
        QSettings settings;
        settings.setValue("paths/mo2Instance", _launchSnapshot.instancePath);
        settings.setValue("paths/gameData", _gameData);
        restoreSessionSelection();
        setStatus("MO2 profile indexed. Choose a plugin, then its weapon, with the weapon button.");
        if (_selectedWeapon < 0) {
            offerPluginChoice();
        }
    }

    const ResourceArchive& ReloadMakerController::referenceMeshes() const
    {
        return _libraryMode || !_package.resourceData.isEmpty() ? _packageResources : _gameMeshes;
    }
}
