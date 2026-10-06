// Development sources: a mod folder, ZIP, plugin or source pack opened
// directly, outside an MO2 profile. Indexing runs on a worker that owns its
// own result; installing the result, and every NifModel or GL call, stays on
// the GUI thread. A failed or cancelled load never replaces the last usable
// source.
#include "ReloadMakerController.h"
#include "LibraryResources.h"
#include "areas/sources/PackageLoad.h"
#include "StudioPaths.h"
#include "StudioWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSet>
#include <QSettings>
#include <QThread>

#include <memory>

namespace reloadmaker
{
    namespace
    {
        using Progress = std::function<void(const QString&)>;

        // Base-game meshes and animations are required; sounds are optional.
        bool openReferenceArchives(PackageLoad& load, const QString& data, const Progress& progress,
                                   const LibraryCancelled& cancelled)
        {
            progress("Indexing reference meshes…");
            if (!load.meshes.open(QDir(data).filePath("Fallout4 - Meshes.ba2"), load.error)) {
                return false;
            }
            if (cancelled()) {
                load.cancelled = true;
                return false;
            }
            progress("Indexing reference animations…");
            if (!load.animations.open(QDir(data).filePath("Fallout4 - Animations.ba2"), load.error)) {
                return false;
            }
            QString audioError;
            (void)load.sounds.open(QDir(data).filePath("Fallout4 - Sounds.ba2"), audioError);
            return true;
        }

        // A source pack lists its plugins; a folder contributes every plugin inside it.
        QVector<PluginContent> readPackagePlugins(const PreparedPackage& package, const Progress& progress,
                                                  const LibraryCancelled& cancelled)
        {
            QVector<PluginContent> catalogs;
            if (!package.plugins.isEmpty()) {
                for (const QString& file : package.plugins) {
                    catalogs.append(EspReader::read(file, cancelled));
                }
                return catalogs;
            }
            QDirIterator iterator(package.rootPath, { "*.esp", "*.esm", "*.esl" }, QDir::Files, QDirIterator::Subdirectories);
            while (iterator.hasNext() && !cancelled()) {
                const QString pluginPath = iterator.next();
                progress(QStringLiteral("Reading %1").arg(QFileInfo(pluginPath).fileName()));
                catalogs.append(EspReader::read(pluginPath, cancelled));
            }
            return catalogs;
        }

        bool openPackageResources(PackageLoad& load, const QVector<PluginContent>& catalogs, const LibraryCancelled& cancelled)
        {
            const PreparedPackage& package = load.package;
            if (!package.resourceData.isEmpty()) {
                return load.resources.openLoadOrder(package.resourceData, package.archives, load.error, {}, cancelled);
            }
            const QStringList roots = package.roots.isEmpty() ? QStringList{ package.rootPath } : package.roots;
            return load.resources.openPackages(packageResourceLayers(catalogs, roots), load.error);
        }

        // Masters in the game's Data folder name the ammunition, keywords and
        // sounds the source refers to without overriding them.
        void readReferenceCatalogs(PackageLoad& load, const QString& data, const Progress& progress,
                                   const LibraryCancelled& cancelled)
        {
            QSet<QString> read;
            for (const PluginContent& plugin : load.plugins) {
                for (const QString& master : plugin.masters) {
                    if (read.contains(master.toLower()) || cancelled()) {
                        continue;
                    }
                    read.insert(master.toLower());
                    const QString file = QDir(data).filePath(master);
                    if (!QFileInfo::exists(file)) {
                        continue;
                    }
                    progress("Reading ammunition and keyword catalog: " + master);
                    PluginContent catalog;
                    {
                        StageClock clock(load.timings, "source.referenceCatalog " + master);
                        catalog = EspReader::read(file, cancelled, true);
                    }
                    for (const auto& sound : catalog.sounds) {
                        const auto descriptor = soundDescriptor(catalog, sound);
                        load.referenceSounds.insert(descriptor.record.toLower(), descriptor);
                    }
                    for (const auto& mapping : catalog.soundMappings) {
                        // Masters rank below the package, in the order first met.
                        SoundMapping value{ recordIdentity(catalog, mapping.formId).toLower(), mapping.editorId,
                                            recordIdentity(catalog, mapping.descriptor).toLower(), {}, int(read.size()) };
                        for (const auto keyword : mapping.keywords) {
                            value.keywords.insert(recordIdentity(catalog, keyword).toLower());
                        }
                        load.referenceSoundMappings.append(std::move(value));
                    }
                    for (const auto keyword : catalog.soundKeywords) {
                        load.referenceSoundKeywords.insert(recordIdentity(catalog, keyword).toLower());
                    }
                    for (auto it = catalog.ammoNames.cbegin(); it != catalog.ammoNames.cend(); ++it) {
                        load.referenceNames.insert(recordIdentity(catalog, it.key()).toLower(), it.value());
                    }
                    for (auto it = catalog.keywordNames.cbegin(); it != catalog.keywordNames.cend(); ++it) {
                        load.referenceNames.insert(recordIdentity(catalog, it.key()).toLower(), it.value());
                    }
                }
            }
        }

        // Worker body. Returns early with `load.error` or `load.cancelled` set.
        void loadPackage(PackageLoad& load, const QString& path, const QString& dataPath, const QString& dependencies,
                         const Progress& progress)
        {
            const LibraryCancelled cancelled = [] { return QThread::currentThread()->isInterruptionRequested(); };
            // Without a game folder `data` still resolves against the working
            // directory; masters and the INI language are looked up there.
            const QString data = referenceDataFolder(dataPath);
            load.data = dataPath.isEmpty() ? QString() : data;
            {
                StageClock clock(load.timings, "source.referenceArchives");
                if (!dataPath.isEmpty() && !openReferenceArchives(load, data, progress, cancelled)) {
                    return;
                }
            }
            if (!path.isEmpty() && !cancelled()) {
                progress("Preparing package…");
                {
                    StageClock clock(load.timings, "source.prepare");
                    load.package = PackageSource::prepare(path);
                }
                if (!load.package.ok()) {
                    load.error = load.package.error;
                    return;
                }
                progress("Indexing package resources…");
                QVector<PluginContent> own;
                {
                    StageClock clock(load.timings, "source.plugins");
                    own = readPackagePlugins(load.package, progress, cancelled);
                }
                QVector<PluginContent> catalogs;
                {
                    StageClock clock(load.timings, "source.masters");
                    catalogs = includePackageMasters(own, data, dependencies, cancelled, load.error);
                }
                {
                    StageClock clock(load.timings, "source.resources");
                    if (!load.error.isEmpty() || !openPackageResources(load, catalogs, cancelled)) {
                        return;
                    }
                }
                const QDir game(QFileInfo(data).absolutePath());
                const QString language = sourceLanguage({ game.filePath("Fallout4_Default.ini"), game.filePath("Fallout4.ini") });
                {
                    StageClock clock(load.timings, "source.names");
                    for (PluginContent& plugin : catalogs) {
                        EspReader::resolveNames(plugin, language, [&load](const QString& resource, QString& error) {
                            return load.resources.read(resource, error);
                        });
                    }
                }
                if (load.package.plugins.isEmpty()) {
                    load.plugins = std::move(catalogs);
                } else {
                    PluginContent merged = mergeLoadOrder(catalogs, load.error);
                    if (!load.error.isEmpty()) {
                        return;
                    }
                    merged.fileName = "Local source pack";
                    load.plugins.append(std::move(merged));
                }
                readReferenceCatalogs(load, data, progress, cancelled);
            }
            load.stamp = sourceStamp(packageStampFiles(load.package, load.plugins, load.data));
            load.cancelled = cancelled();
        }

        // An OMOD need not override the master KYWD that names its slot.
        void nameAttachPointsFromMasters(QVector<PluginContent>& plugins, const QHash<QString, QString>& referenceNames)
        {
            for (PluginContent& plugin : plugins) {
                for (const auto& mod : plugin.objectMods) {
                    if (plugin.keywordNames.contains(mod.attachPoint)) {
                        continue;
                    }
                    const QString name = referenceNames.value(recordIdentity(plugin, mod.attachPoint).toLower());
                    if (!name.isEmpty()) {
                        plugin.keywordNames.insert(mod.attachPoint, name);
                    }
                }
            }
        }
    }

    void ReloadMakerController::openInitialPaths(const QString& packagePath, const QString& gameDataPath,
                                                 const QString& mo2Instance, bool library)
    {
        QSettings settings;
        const InstallationSetup installation = loadInstallationSetup();
        QString game = gameDataPath;
        if (game.isEmpty()) {
            const InstallationCheck checked = checkInstallation(installation);
            game = checked.gameReady ? checked.dataPath : settings.value("paths/gameData").toString();
        }
        if (game.isEmpty() || !QFileInfo(game).isDir()) {
            const InstallationCheck checked = checkInstallation(installation);
            game = checked.gameReady ? checked.dataPath : QString();
        }
        _gameDataText = game;
        if (mo2Instance.isEmpty() && !installation.mo2Instance.isEmpty()) {
            _mo2Instance = installation.mo2Instance;
        }
        _requestedMo2Profile = QCoreApplication::instance()->property("requestedMo2Profile").toString();
        if (_requestedMo2Profile.isEmpty()) {
            _requestedMo2Profile = installation.profile;
        }
        if (!mo2Instance.isEmpty()) {
            _mo2Instance = mo2Instance;
        }
        if (library || (insideMo2VirtualFilesystem() && packagePath.isEmpty())) {
            startLibraryLoad();
            return;
        }

        QString package = packagePath;
        if (package.isEmpty()) {
            package = settings.value("paths/lastPackage").toString();
        }
        if (!package.isEmpty() && QFileInfo(package).exists()) {
            _packageText = package;
            startPackageLoad(package, game);
        } else if (!game.isEmpty()) {
            startPackageLoad({}, game);
        } else {
            showHome();
        }
    }

    void ReloadMakerController::browsePackage()
    {
        const QString folder = QFileDialog::getExistingDirectory(_window, "Open mod folder", _packageText,
                                                                 QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!folder.isEmpty()) {
            _packageText = folder;
            openPackage(folder);
        }
    }

    void ReloadMakerController::setGameData(const QString& path)
    {
        if (_loadThread) {
            return;
        }
        if (_libraryMode) {
            _gameDataText = path;
            startLibraryLoad();
            return;
        }
        startPackageLoad(_package.sourcePath, path);
    }

    void ReloadMakerController::openPackage(const QString& path)
    {
        startPackageLoad(path, _gameDataText, true);
    }

    // With `reuse` (opening a project), the installed or a resident copy of
    // the same source is installed again when none of its files changed;
    // otherwise, and for every explicit open, the source is read.
    void ReloadMakerController::startPackageLoad(const QString& path, const QString& dataPath, bool newDocument, bool reuse)
    {
        if (_loadThread) {
            return;
        }
        if (_pendingSession.isEmpty() && !leaveDocument(QStringLiteral("opening another source"))) {
            return;
        }
        auto result = std::make_shared<PackageLoad>();
        const QString key = packageSourceKey(path, dataPath);
        std::shared_ptr<PackageLoad> resident = takeResidentPackage(key);
        if (!reuse) {
            resident.reset();
        }
        const bool current = reuse && !_libraryMode && _packageResources.isOpen() && !_packageStamp.isEmpty() &&
                             packageSourceKey(_package.sourcePath, _gameData) == key;
        const QStringList currentFiles = current ? packageStampFiles(_package, _plugins, _gameData) : QStringList{};
        const QByteArray currentStamp = _packageStamp;
        setLoading(true, "Opening source and reference assets…");
        const Progress progress = [this](const QString& message) {
            QMetaObject::invokeMethod(this, [this, message]() { _loadingLabel->setText(message); }, Qt::QueuedConnection);
        };
        // Masters the owner keeps out of Data (they live in MO2 mods), copied into
        // workspace/dependencies with their archives. Test workers mirror it.
        const QString dependencies = studioDataPath(QStringLiteral("workspace/dependencies"));
        _loadThread = QThread::create([result, resident, current, currentFiles, currentStamp, path, dataPath, dependencies, progress]() {
            try {
                StageClock clock(result->timings, "source.load");
                if (current && sourceStamp(currentFiles) == currentStamp) {
                    result->reuseCurrent = true;
                    result->timings.add(QStringLiteral("source.reused installed"), 0);
                    return;
                }
                if (resident && sourceStamp(packageStampFiles(resident->package, resident->plugins, resident->data)) == resident->stamp) {
                    LoadTimings timings = std::move(result->timings);
                    *result = std::move(*resident);
                    result->timings = std::move(timings);
                    result->timings.add(QStringLiteral("source.reused resident"), 0);
                    return;
                }
                loadPackage(*result, path, dataPath, dependencies, progress);
            } catch (const std::exception& exception) {
                result->error = QString::fromUtf8(exception.what());
            }
        });
        _loadThread->setParent(this);
        connect(_loadThread, &QThread::finished, this, [this, result, newDocument]() {
            result->cancelled |= _loadThread->isInterruptionRequested();
            _loadThread->deleteLater();
            _loadThread = nullptr;
            if (_closing) {
                return;
            }
            setLoading(false);
            if (result->cancelled) {
                _pendingSession = {};
                _pendingSessionPath.clear();
                _gameDataText = _gameData;
                setStatus("Loading cancelled; previous source retained.");
                return;
            }
            if (!result->error.isEmpty()) {
                _pendingSession = {};
                _pendingSessionPath.clear();
                _gameDataText = _gameData;
                setStatus(result->error, true);
                return;
            }

            _loadTimings.append(result->timings);
            StageClock install(_loadTimings, "source.install");
            if (newDocument) {
                _sessionPath.clear();
            }
            setLibraryResources(nullptr);
            if (result->reuseCurrent) {
                takeCurrentPackage(*result);
            } else if (_libraryMode || packageSourceKey(_package.sourcePath, _gameData) != packageSourceKey(result->package.sourcePath, result->data)) {
                // A changed source read again replaces its old copy instead of parking it.
                parkCurrentPackage();
            }
            clearAssetCaches(result->reuseCurrent);
            _libraryMode = false;
            _package = result->package;
            _packageResources = std::move(result->resources);
            _gameMeshes = std::move(result->meshes);
            _gameAnimations = std::move(result->animations);
            _gameSounds = std::move(result->sounds);
            _gameData = result->data;
            _plugins = std::move(result->plugins);
            _referenceNames = std::move(result->referenceNames);
            _referenceSounds = std::move(result->referenceSounds);
            _referenceSoundMappings = std::move(result->referenceSoundMappings);
            _referenceSoundKeywords = std::move(result->referenceSoundKeywords);
            _packageStamp = std::move(result->stamp);
            nameAttachPointsFromMasters(_plugins, _referenceNames);
            _packageText = _package.sourcePath;
            _gameDataText = _gameData;
            QSettings settings;
            settings.setValue("paths/gameData", _gameData);
            settings.setValue("paths/lastPackage", _package.sourcePath);

            clearDiagnostics();
            for (const PluginContent& plugin : _plugins) {
                appendDiagnostic(QStringLiteral("PLUGIN %1 weapons=%2 omods=%3")
                                     .arg(plugin.fileName)
                                     .arg(plugin.weapons.size())
                                     .arg(plugin.objectMods.size()));
                for (const auto& diagnostic : plugin.diagnostics) {
                    appendDiagnostic(diagnostic);
                }
            }
            configureResourcePaths();
            refreshWeapons();
            refreshAnimations();
            refreshSourceBar();
            setStatus("Source indexed. Select a weapon configuration and action.");
            restoreSessionSelection();
        });
        _loadThread->start();
    }
}
