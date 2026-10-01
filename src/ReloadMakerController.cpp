#include "ReloadMakerController.h"
#include "PoseEvaluator.h"
#include "SlotOverlay.h"
#include "LibraryResources.h"
#include "LoadoutView.h"

#include "data/niftypes.h"
#include "gamemanager.h"
#include "gl/glscene.h"
#include "glview.h"
#include "model/nifmodel.h"
#include "StudioWindow.h"
#include "spells/blocks.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QScopeGuard>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QTabWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QThread>
#include <QProgressBar>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace reloadmaker
{
    namespace
    {
        constexpr int kTimelineMaximum = 100'000;
        constexpr int kAnimationNodeRole = Qt::UserRole + 2;
        constexpr int kAnimationBaselineRole = Qt::UserRole + 4;
        constexpr int kActionBlendInRole = Qt::UserRole + 6;
        constexpr int kActionBlendOutRole = Qt::UserRole + 7;
        constexpr int kPlaybackIntervalMilliseconds = 16;
        constexpr std::array<const char*, 3> kSkinHelperBoneNames{
            "Chest_Rear_Skin",
            "LArm_ShoulderFat_skin",
            "RArm_ShoulderFat_skin",
        };

        [[nodiscard]] QString formIdText(const std::uint32_t value)
        {
            return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
        }

        [[nodiscard]] QString displayName(const ObjectModRecord& record)
        {
            const QString primary = record.name.isEmpty() ? record.editorId : record.name;
            return QStringLiteral("%1  ·  %2").arg(primary, record.editorId);
        }

        [[nodiscard]] bool isReceiverObjectMod(const ObjectModRecord& record)
        {
            return !record.modelPath.isEmpty() &&
                   (record.editorId.contains(
                        QStringLiteral("receiver"), Qt::CaseInsensitive) ||
                    record.editorId.contains(
                        QStringLiteral("reciever"), Qt::CaseInsensitive) ||
                    record.name.contains(
                        QStringLiteral("receiver"), Qt::CaseInsensitive));
        }

        [[nodiscard]] QString significantAnimationRoot(
            const AnimationSubgraphEntry& entry)
        {
            for (QString path : entry.animationSearchPaths) {
                path.replace(QLatin1Char('\\'), QLatin1Char('/'));
                const QString lower = path.toLower();
                if (lower.endsWith(QStringLiteral("/paired")) ||
                    lower.endsWith(QStringLiteral("/common")) ||
                    lower.endsWith(QStringLiteral("/gammagun"))) {
                    continue;
                }
                const qsizetype animations = lower.indexOf(
                    QStringLiteral("/animations/"));
                if (animations >= 0) {
                    path = path.mid(animations + 12);
                }
                path.replace(QLatin1Char('/'), QStringLiteral(" › "));
                return path;
            }
            return QStringLiteral("Unnamed animation search stack");
        }

        [[nodiscard]] bool subgraphCompatibleWithWeapon(
            const AnimationSubgraphEntry& entry,
            const PluginContent& plugin,
            const WeaponRecord& weapon)
        {
            for (const std::uint32_t required : entry.requiredKeywords) {
                const bool identifiesAWeapon = std::any_of(
                    plugin.weapons.cbegin(), plugin.weapons.cend(),
                    [required](const WeaponRecord& candidate) {
                        return candidate.keywords.contains(required);
                    });
                if (identifiesAWeapon && !weapon.keywords.contains(required)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] QString normalizedKey(const QString& value)
        {
            return value.trimmed().toLower();
        }

        [[nodiscard]] QString defaultGameDataPath()
        {
            const QString known = QStringLiteral(
                "D:/SteamLibrary/steamapps/common/Fallout 4 VR/Data");
            if (QFileInfo(known).isDir()) {
                return QFileInfo(known).canonicalFilePath();
            }
            return {};
        }
    }

    ReloadMakerController::ReloadMakerController(StudioWindow* window)
        : QObject(window), _window(window), _playbackTimer(new QTimer(this))
    {
        _rebuildTimer = new QTimer(this);
        _rebuildTimer->setSingleShot(true);
        _rebuildTimer->setInterval(60);
        connect(_rebuildTimer, &QTimer::timeout, this, &ReloadMakerController::rebuildWeapon);
        createUi();
        _slotOverlay = new SlotOverlay(_window->getGLView(), this);
        _slotOverlay->selected = [this](SlotKey slot) {
            _focusedSlot = slot;
            _partSearch->clear();
            refreshAttachments();
            scrollToFocusedSlot();
        };
        createGripViewport();
        createReloadViewport();
        createSimulationViewport();
        applyStyle();
        setWorkspace(OperateWorkspace);
        startCommandServer();
        _window->prepareClose = [this] { return saveSession(); };
        _playbackTimer->setTimerType(Qt::PreciseTimer);
        _playbackTimer->setInterval(kPlaybackIntervalMilliseconds);
        connect(_playbackTimer, &QTimer::timeout, this, &ReloadMakerController::advancePlayback);
        setStatus(QStringLiteral("Select a mod folder or ZIP file."));
    }

    ReloadMakerController::~ReloadMakerController()
    {
        if(_simulationTimer)_simulationTimer->stop();
        if(_reloadCaptureTimer)_reloadCaptureTimer->stop();
        if(_reloadPreviewTimer)_reloadPreviewTimer->stop();
        _closing = true;
        if (_loadThread) {
            _loadThread->requestInterruption();
            // All worker inputs are value-owned. Join before any controller UI
            // or callback target is destroyed; no detached work survives close.
            _loadThread->wait();
        }
        setLibraryResources(nullptr);
    }

    void ReloadMakerController::openInitialPaths(const QString& packagePath,
        const QString& gameDataPath, const QString& mo2Instance, bool library)
    {
        QSettings settings;
        QString game = gameDataPath;
        if (game.isEmpty()) {
            game = settings.value(QStringLiteral("paths/gameData")).toString();
        }
        if (game.isEmpty() || !QFileInfo(game).isDir()) {
            game = defaultGameDataPath();
        }
        _gameDataText = game;
        if (!mo2Instance.isEmpty()) { _mo2Instance = mo2Instance; }
        if (library || (insideMo2VirtualFilesystem() && packagePath.isEmpty())) {
            startLibraryLoad();
            return;
        }

        QString package = packagePath;
        if (package.isEmpty()) {
            package = settings.value(QStringLiteral("paths/lastPackage")).toString();
        }
        if (!package.isEmpty() && QFileInfo(package).exists()) {
            _packageText = package;
            startPackageLoad(package, game);
        } else if (!game.isEmpty()) {
            startPackageLoad({}, game);
        }
    }

    void ReloadMakerController::browsePackage()
    {
        const QString folder = QFileDialog::getExistingDirectory(
            _window, QStringLiteral("Open mod folder"), _packageText,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!folder.isEmpty()) {
            _packageText = folder;
            openPackage(folder);
        }
    }

    void ReloadMakerController::browseGameData()
    {
        const QString folder = QFileDialog::getExistingDirectory(
            _window, QStringLiteral("Select Fallout 4 VR Data folder"),
            _gameDataText, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!folder.isEmpty()) {
            _gameDataText = folder;
            setGameData(folder);
        }
    }

    void ReloadMakerController::setGameData(const QString& path)
    {
        if (_loadThread) { return; }
        if (_libraryMode) {
            _gameDataText = path;
            startLibraryLoad();
            return;
        }
        startPackageLoad(_package.sourcePath, path);
    }

    void ReloadMakerController::openPackage(const QString& path)
    {
        startPackageLoad(path, _gameDataText);
    }

    /* Source indexing is worker-owned; installation and every NifModel/GL call
       remain on the GUI thread. A failed or cancelled source never replaces the
       last usable catalog. */
    void ReloadMakerController::startPackageLoad(const QString& path, const QString& dataPath)
    {
        if (_loadThread) { return; }
        if (_pendingSession.isEmpty() && !saveSession()) { return; }
        struct Result {
            PreparedPackage package;
            ResourceArchive resources, meshes, animations, sounds;
            QVector<PluginContent> plugins;
            QHash<QString,QString> referenceNames;
            QHash<QString,SoundCue> referenceSounds;
            QString data, error;
            bool cancelled{false};
        };
        auto result = std::make_shared<Result>();
        setLoading(true, QStringLiteral("Opening source and reference assets…"));
        auto progress = [this](const QString& message) {
            QMetaObject::invokeMethod(this, [this, message]() { _loadingLabel->setText(message); }, Qt::QueuedConnection);
        };
        _loadThread = QThread::create([result, path, dataPath, progress]() {
            try {
                const auto cancelled = []() { return QThread::currentThread()->isInterruptionRequested(); };
                QString data = QDir(dataPath).absolutePath();
                if (QFileInfo(QDir(data).filePath("Data")).isDir()) { data = QDir(data).filePath("Data"); }
                result->data = data;
                if (!dataPath.isEmpty()) {
                    progress(QStringLiteral("Indexing reference meshes…"));
                    if (!result->meshes.open(QDir(data).filePath("Fallout4 - Meshes.ba2"), result->error)) { return; }
                    if (cancelled()) { result->cancelled = true; return; }
                    progress(QStringLiteral("Indexing reference animations…"));
                    if (!result->animations.open(QDir(data).filePath("Fallout4 - Animations.ba2"), result->error)) { return; }
                    QString audioError;(void)result->sounds.open(QDir(data).filePath("Fallout4 - Sounds.ba2"),audioError);
                } else { result->data.clear(); }
                if (!path.isEmpty() && !cancelled()) {
                    progress(QStringLiteral("Preparing package…"));
                    result->package = PackageSource::prepare(path);
                    if (!result->package.ok()) { result->error = result->package.error; return; }
                    progress(QStringLiteral("Indexing package resources…"));
                    if (!result->resources.open(result->package.rootPath, result->error)) { return; }
                    QDirIterator iterator(result->package.rootPath, {"*.esp", "*.esm", "*.esl"}, QDir::Files, QDirIterator::Subdirectories);
                    while (iterator.hasNext() && !cancelled()) {
                        const auto pluginPath = iterator.next();
                        progress(QStringLiteral("Reading %1").arg(QFileInfo(pluginPath).fileName()));
                        result->plugins.append(EspReader::read(pluginPath, cancelled));
                    }
                    QSet<QString> readCatalogs;
                    for(const auto& plugin:result->plugins)for(const auto& master:plugin.masters) {
                        if(readCatalogs.contains(master.toLower())||cancelled())continue;
                        readCatalogs.insert(master.toLower());const auto file=QDir(data).filePath(master);
                        if(!QFileInfo::exists(file))continue;
                        progress("Reading ammunition and keyword catalog: "+master);
                        const auto catalog=EspReader::read(file,cancelled,true);
                        for(const auto& sound:catalog.sounds){SoundCue cue;cue.record=recordIdentity(catalog,sound.formId);cue.editorId=sound.editorId;cue.files=sound.files;cue.attenuationDb=sound.attenuationDb;result->referenceSounds.insert(cue.record.toLower(),cue);}
                        for(auto it=catalog.ammoNames.cbegin();it!=catalog.ammoNames.cend();++it)result->referenceNames.insert(recordIdentity(catalog,it.key()).toLower(),it.value());
                        for(auto it=catalog.keywordNames.cbegin();it!=catalog.keywordNames.cend();++it)result->referenceNames.insert(recordIdentity(catalog,it.key()).toLower(),it.value());
                    }
                }
                result->cancelled = cancelled();
            } catch (const std::exception& exception) { result->error = QString::fromUtf8(exception.what()); }
        });
        _loadThread->setParent(this);
        connect(_loadThread, &QThread::finished, this, [this, result]() {
            result->cancelled |= _loadThread->isInterruptionRequested();
            _loadThread->deleteLater(); _loadThread = nullptr;
            if (_closing) { return; }
            setLoading(false);
            if (result->cancelled) { _pendingSession={}; setStatus(QStringLiteral("Loading cancelled; previous source retained.")); return; }
            if (!result->error.isEmpty()) { _pendingSession={}; setStatus(result->error, true); return; }
            setLibraryResources(nullptr);
            clearAssetCaches();
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
            // An OMOD need not override the master KYWD that names its slot.
            for(auto& plugin:_plugins)for(const auto& mod:plugin.objectMods)
                if(!plugin.keywordNames.contains(mod.attachPoint)) {
                    const auto name=_referenceNames.value(recordIdentity(plugin,mod.attachPoint).toLower());
                    if(!name.isEmpty())plugin.keywordNames.insert(mod.attachPoint,name);
                }
            _packageText = _package.sourcePath;
            _gameDataText = _gameData;
            QSettings settings;
            settings.setValue("paths/gameData", _gameData);
            settings.setValue("paths/lastPackage", _package.sourcePath);
            _details->clear();
            for (const auto& plugin : _plugins) {
                appendDiagnostic(QStringLiteral("PLUGIN %1 weapons=%2 omods=%3").arg(plugin.fileName).arg(plugin.weapons.size()).arg(plugin.objectMods.size()));
                for (const auto& diagnostic : plugin.diagnostics) { appendDiagnostic(diagnostic); }
            }
            configureResourcePaths();
            refreshWeapons(); refreshAnimations();
            refreshSourceBar();
            setStatus(QStringLiteral("Source indexed. Select a weapon configuration and action."));
            restoreSessionSelection();
        });
        _loadThread->start();
    }


    void ReloadMakerController::refreshWeapons()
    {
        const QSignalBlocker blocker(_weaponCombo);
        _weaponCombo->clear();
        _weaponSelections.clear();
        _configurations.clear();
        for (int pluginIndex = 0; pluginIndex < _plugins.size(); ++pluginIndex) {
            const PluginContent& plugin = _plugins.at(pluginIndex);
            for (int weaponIndex = 0; weaponIndex < plugin.weapons.size(); ++weaponIndex) {
                const WeaponRecord& weapon = plugin.weapons.at(weaponIndex);
                _weaponSelections.append(WeaponSelection{ pluginIndex, weaponIndex });
                _weaponCombo->addItem(
                    QStringLiteral("%1  ·  %2  ·  %3")
                        .arg(weapon.name, weapon.editorId, _libraryMode ? plugin.origins.value(weapon.formId).plugin : plugin.fileName),
                    _weaponSelections.size() - 1);
            }
        }
        if (_libraryMode) { _weaponCombo->setCurrentIndex(-1); }
        refreshAttachments();
        refreshConfigurationChoices();
        if (_pendingSession.isEmpty() && _weaponCombo->currentIndex() >= 0 && !_gameData.isEmpty()) {
            _rebuildTimer->start();
        }
    }

    WeaponConfiguration* ReloadMakerController::configuration()
    {
        return const_cast<WeaponConfiguration*>(std::as_const(*this).configuration());
    }

    const WeaponConfiguration* ReloadMakerController::configuration() const
    {
        const int index = _weaponCombo->currentIndex();
        if (index < 0) { return nullptr; }
        auto found = _configurations.find(index);
        if (found == _configurations.end()) {
            const auto* plugin = currentPlugin();
            const auto* weapon = currentWeapon();
            if (!plugin || !weapon) { return nullptr; }
            WeaponConfiguration config;
            config.initialize(*plugin, *weapon);
            found = _configurations.insert(index, std::move(config));
        }
        return &found.value();
    }

    void ReloadMakerController::scheduleRebuild()
    {
        _rebuildTimer->start();
        updateHandPoseControls();
        setStatus(QStringLiteral("Updating selected configuration…"));
    }

    void ReloadMakerController::refreshAttachments()
    {
        // One flat workbench list: category headers, a header per slot and
        // every option under it. Slot nesting (muzzle on barrel on receiver)
        // is deliberately not shown; unavailable slots are grouped instead.
        const QSignalBlocker blocker(_partList);
        const int scroll = _partList->verticalScrollBar()->value();
        _partList->clear();
        const auto* config = configuration();
        const auto* weapon = currentWeapon();
        _loadoutTitle->setText(weapon ? (weapon->name.isEmpty() ? weapon->editorId : weapon->name) : QStringLiteral("No weapon"));
        if (!config) { _configurationSummary->setText(QStringLiteral("Choose a source and weapon in the bar above.")); return; }
        if (!config->slot(_focusedSlot) && !config->entries().isEmpty()) { _focusedSlot = config->entries().first().key; }
        const auto available = config->availableSlots();
        const QString query = _partSearch->text().trimmed();
        const auto partName = [](const ObjectModRecord& mod) { return cleanPartName(mod.name.isEmpty() ? mod.editorId : mod.name); };
        const auto addRow = [this](const QString& text, int section) {
            auto* item = new QListWidgetItem(text, _partList);
            item->setData(kSectionRole, section);
            return item;
        };
        enum class Group { Parts, Finish, Unavailable };
        const auto groupOf = [&](const ConfigurationSlot& slot) {
            return !available.contains(slot.key) ? Group::Unavailable : slot.hasModels ? Group::Parts : Group::Finish;
        };
        const std::pair<Group, QString> groups[] = {{Group::Parts, QStringLiteral("Parts")},
            {Group::Finish, QStringLiteral("Stats & finish")}, {Group::Unavailable, QStringLiteral("Needs another part installed")}};
        for (const auto& [group, title] : groups) {
            QListWidgetItem* header = nullptr;
            for (const auto& slot : config->entries()) {
                if (groupOf(slot) != group) { continue; }
                const bool slotMatch = query.isEmpty() || slot.label.contains(query, Qt::CaseInsensitive);
                QVector<const ObjectModRecord*> options;
                for (auto id : slot.choices) {
                    const auto* mod = config->mod(id);
                    if (mod && (slotMatch || partName(*mod).contains(query, Qt::CaseInsensitive)
                                || mod->editorId.contains(query, Qt::CaseInsensitive))) { options.append(mod); }
                }
                if (!slotMatch && options.isEmpty()) { continue; }
                if (!header) { header = addRow(title, kCategoryHeader); header->setFlags(Qt::ItemIsEnabled); }
                const bool enabled = group != Group::Unavailable;
                const auto selected = config->selected(slot.key);
                auto* slotRow = addRow(slot.label, kSlotHeader);
                slotRow->setData(kSlotRole, QVariant::fromValue<qulonglong>(slot.key));
                slotRow->setData(kFocusedRole, slot.key == _focusedSlot);
                slotRow->setData(kCaptionRole, enabled ? QStringLiteral("%1 options").arg(slot.choices.size()) : QStringLiteral("unavailable"));
                slotRow->setFlags(Qt::ItemIsEnabled);
                if (slot.unverified) { slotRow->setToolTip(QStringLiteral("Partial slot/association metadata; compatibility needs review.")); }
                const auto addOption = [&](std::uint32_t id, const QString& text, const QString& tip) {
                    auto* item = addRow(text, 0);
                    item->setData(kSlotRole, QVariant::fromValue<qulonglong>(slot.key));
                    item->setData(kFormIdRole, id);
                    item->setData(kInstalledRole, id == selected);
                    item->setToolTip(tip);
                    item->setFlags(enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
                };
                if (slotMatch) { addOption(0, QStringLiteral("Empty"), QStringLiteral("Remove this part. Attachments on it become unavailable; their choices are kept.")); }
                for (const auto* mod : options) {
                    addOption(mod->formId, partName(*mod), QStringLiteral("%1\n%2\n%3%4").arg(mod->editorId, formIdText(mod->formId),
                        mod->modelPath.isEmpty() ? QStringLiteral("Configuration only; no mesh") : mod->modelPath,
                        mod->metadataWarning.isEmpty() ? QString() : QStringLiteral("\n") + mod->metadataWarning));
                }
            }
        }
        _partList->verticalScrollBar()->setValue(scroll);
        const auto* plugin = currentPlugin();
        _configurationSummary->setText(QStringLiteral("%1 · %2 parts installed")
            .arg(plugin ? (_libraryMode ? plugin->origins.value(weapon->formId).plugin : plugin->fileName) : QString())
            .arg(config->activeMods().size()));
        if (_slotOverlay) { _slotOverlay->setSelected(_focusedSlot); }
    }

    void ReloadMakerController::scrollToFocusedSlot()
    {
        for (int row = 0; row < _partList->count(); ++row) {
            auto* item = _partList->item(row);
            if (item->data(kSectionRole).toInt() == kSlotHeader && item->data(kSlotRole).toULongLong() == _focusedSlot) {
                _partList->scrollToItem(item, QAbstractItemView::PositionAtTop);
                return;
            }
        }
    }

    void ReloadMakerController::refreshSlotCallouts()
    {
        QVector<SlotCallout> callouts;
        const auto* config = configuration();
        if (config) {
            const auto available = config->availableSlots();
            for (const auto& slot : config->entries()) {
                if (!slot.hasModels || !available.contains(slot.key)) { continue; }
                SlotKey anchorSlot = slot.key;
                // An empty attachment has no surface. Point to its declared
                // installed owner, explicitly marked as an owner-level anchor.
                for (int depth = 0; depth < config->entries().size() &&
                     !_partRoots.value(anchorSlot).isValid() && anchorSlot; ++depth) {
                    anchorSlot = available.value(anchorSlot);
                }
                const auto root = _partRoots.value(anchorSlot, _weaponRoot);
                if (!root.isValid()) { continue; }
                const auto* mod = config->mod(config->selected(slot.key));
                const auto* owner = config->slot(available.value(slot.key));
                const bool ownerAnchor = anchorSlot != slot.key;
                QString detail = ownerAnchor ? QStringLiteral("Empty")
                    : (mod ? (mod->name.isEmpty() ? mod->editorId : mod->name) : QStringLiteral("Installed"));
                if (ownerAnchor && mod && !mod->modelPath.isEmpty()) { detail = QStringLiteral("Unresolved part"); }
                if (owner) { detail += QStringLiteral(" · on %1").arg(owner->label); }
                const int children = int(std::count(available.cbegin(), available.cend(), slot.key));
                const QString title = slot.label + (children ? QStringLiteral("  +%1").arg(children) : QString{});
                callouts.append({slot.key, title, root, _partSurfaces.value(anchorSlot), detail,
                                 available.value(slot.key), ownerAnchor});
            }
        }
        _slotOverlay->setCallouts(std::move(callouts));
    }

    void ReloadMakerController::recordPartSurfaces(SlotKey slot, int firstBlock, int lastBlock)
    {
        auto* model = activeModel();
        QVector<QPersistentModelIndex> surfaces;
        for (int block = firstBlock; model && block < lastBlock; ++block) {
            const auto index = model->getBlockIndex(block);
            if (model->blockInherits(index, "BSTriShape") || model->blockInherits(index, "NiGeometry")) {
                surfaces.append(index);
            }
        }
        _partSurfaces.insert(slot, std::move(surfaces));
    }

    bool ReloadMakerController::remapAttachmentNodes(const ObjectModRecord& mod,int firstBlock,int lastBlock,QString& error)
    {
        auto* model=activeModel();const auto* plugin=currentPlugin();
        if(!model||!plugin){error="No assembly for attachment remapping";return false;}
        QSet<int> moved;
        for(auto keyword:mod.addedKeywords) {
            const auto remap=plugin->nodeRemaps.constFind(keyword);if(remap==plugin->nodeRemaps.cend())continue;
            QPersistentModelIndex node;
            for(int block=firstBlock;block<lastBlock;++block) {
                const auto candidate=model->getBlockIndex(block);
                if(!model->blockInherits(candidate,"NiNode")||model->get<QString>(candidate,"Name").compare(remap->node,Qt::CaseInsensitive)!=0)continue;
                if(node.isValid()){error="Ambiguous remapped branch: "+remap->node;return false;}
                node=candidate;
            }
            if(!node.isValid()){error="Missing remapped branch: "+remap->node;return false;}
            const auto target=findConnectTarget("C-"+remap->point.mid(2),nullptr,configurationSlotKey(mod));
            if(!target.valid()){error=target.error;return false;}
            const int block=model->getBlockNumber(node);
            if(moved.contains(block)){error="Conflicting remaps for branch: "+remap->node;return false;}
            auto ancestor=QModelIndex(target.parent);
            for(int depth=0;ancestor.isValid()&&depth<model->getBlockCount();++depth) {
                if(ancestor==node){error="Cyclic attachment remap: "+remap->node;return false;}
                ancestor=model->getBlockIndex(model->getParent(ancestor));
            }
            if(ancestor.isValid()){error="Cyclic remap destination ancestry";return false;}
            const QPersistentModelIndex oldParent=model->getBlockIndex(model->getParent(node));
            Transformf authored;
            if(!oldParent.isValid()||!readNodeTransform(node,authored)){error="Invalid remapped branch parent or transform";return false;}
            const Transformf connection{target.translation,target.rotation,{target.scale,target.scale,target.scale}};
            // Move only the explicitly named branch. A rail-mounted optic can
            // share this attachment with iron sights but stay on the receiver.
            delLink(model,oldParent,"Children",block);
            if(!addLink(model,target.parent,"Children",block)||!setNodeTransform(node,composed(connection,authored))) {
                error="Cannot attach remapped branch: "+remap->node;return false;
            }
            moved.insert(block);
            const auto name=plugin->keywordNames.value(keyword);
            _remappedAttachments.append({node,target.parent,name,remap->point,configurationSlotKey(mod)});
            appendDiagnostic(QString("NODE REMAP %1 · node=%2 parent=%3 via %4").arg(name).arg(block).arg(model->getBlockNumber(target.parent)).arg(remap->point));
        }
        return true;
    }

    bool ReloadMakerController::recordPartAlignment(int firstBlock, int lastBlock, int anchorBone, QString& error)
    {
        const auto* model = activeModel();
        if (!model || anchorBone < 0 || anchorBone >= _skeleton.bones.size()) {
            error = QStringLiteral("Invalid attachment alignment source");
            return false;
        }
        int anchor = -1;
        QVector<int> siblings;
        for (int block = firstBlock; block < lastBlock; ++block) {
            const auto node = model->getBlockIndex(block);
            if (!model->blockInherits(node, "NiNode")) { continue; }
            const int bone = _skeleton.boneIndex(model->get<QString>(node, "Name"));
            if (bone == anchorBone) {
                if (anchor >= 0) {
                    error = QStringLiteral("Attachment supplies multiple alignment anchors: %1").arg(_skeleton.bones.at(bone).name);
                    return false;
                }
                anchor = block;
            }
            if (bone >= 0 && _skeleton.bones.at(bone).parentIndex == _skeleton.bones.at(anchorBone).parentIndex) {
                siblings.append(block);
            }
        }
        if (anchor < 0) {
            error = QStringLiteral("Attachment's declared alignment bone is missing");
            return false;
        }
        // Align the model once using its connection, not each sibling's idle
        // placement. HK433 parks its auxiliary magazine off-weapon in idle;
        // cancelling that branch's own idle puts a second magazine in the gun.
        // Descendant cartridge tracks and other imported models stay independent.
        for (int block : siblings) { _partAlignmentAnchors.insert(block, anchor); }
        return true;
    }

    void ReloadMakerController::refreshConfigurationChoices()
    {
        refreshAnimationSetChoices();
    }

    void ReloadMakerController::refreshAnimationSetChoices()
    {
        const QSignalBlocker blocker(_animationSetCombo);
        _animationSetCombo->clear();
        _animationSubgraphSelections.clear();
        _animationSetCombo->addItem(
            QStringLiteral("Select an animation / reload set…"));
        _animationSubgraphSelections.append(AnimationSubgraphSelection{});
        _animationSetCombo->addItem(
            QStringLiteral("Automatic from selected OMOD keywords"));
        _animationSetCombo->setItemData(
            1,
            QStringLiteral(
                "Use only the weapon and OMOD keywords decoded from the plugin. "
                "This can be incomplete for complex weapon mods."),
            Qt::ToolTipRole);
        _animationSubgraphSelections.append(
            AnimationSubgraphSelection{ -1, -1 });

        const PluginContent* plugin = currentPlugin();
        const WeaponRecord* weapon = currentWeapon();
        if (!plugin || !weapon) {
            return;
        }

        int candidateCount = 0;
        int onlyCandidate = -1;
        for (int recordIndex = 0;
             recordIndex < plugin->animationSubgraphs.size();
             ++recordIndex) {
            const AnimationSubgraphRecord& record =
                plugin->animationSubgraphs.at(recordIndex);
            for (int entryIndex = 0; entryIndex < record.entries.size(); ++entryIndex) {
                const AnimationSubgraphEntry& entry = record.entries.at(entryIndex);
                if (!entry.firstPerson() ||
                    !subgraphCompatibleWithWeapon(entry, *plugin, *weapon)) {
                    continue;
                }

                QStringList keywordText;
                for (const std::uint32_t keyword : entry.requiredKeywords) {
                    keywordText.append(formIdText(keyword));
                }
                const int comboIndex = _animationSetCombo->count();
                _animationSetCombo->addItem(
                    QStringLiteral("%1  ·  %2")
                        .arg(significantAnimationRoot(entry), record.editorId));
                _animationSetCombo->setItemData(
                    comboIndex,
                    QStringLiteral(
                        "RACE: %1\nRequired keywords: %2\nBehavior: %3\nSearch order:\n  %4")
                        .arg(record.editorId,
                             keywordText.join(QStringLiteral(", ")),
                             entry.behaviorPath,
                             entry.animationSearchPaths.join(
                                 QStringLiteral("\n  "))),
                    Qt::ToolTipRole);
                _animationSubgraphSelections.append(
                    AnimationSubgraphSelection{ recordIndex, entryIndex });
                ++candidateCount;
                if (candidateCount == 1) {
                    onlyCandidate = comboIndex;
                }
            }
        }

        _animationSetCombo->setCurrentIndex(candidateCount == 1 ? onlyCandidate : 1);
    }

    void ReloadMakerController::refreshAnimations()
    {
        _animationList->clear();
        for (const AnimationAction& action : _animationActions) {
            auto* item = new QListWidgetItem(action.label, _animationList);
            item->setData(Qt::UserRole, action.resourcePath);
            item->setData(kAnimationNodeRole, action.graphNode);
            item->setData(kAnimationBaselineRole, action.baselineResourcePath);
            item->setData(kActionBlendInRole, action.blendInSeconds);
            item->setData(kActionBlendOutRole, action.blendOutSeconds);
            item->setToolTip(QStringLiteral("Graph: %1\nResolved: %2\nBaseline: %3")
                                 .arg(action.graphNode, action.resourcePath,
                                      action.baselineResourcePath));
        }
    }

    void ReloadMakerController::configureResourcePaths()
    {
        setLibraryResources(_libraryMode ? &_packageResources : nullptr);
        if (_gameData.isEmpty()) {
            return;
        }
        QStringList paths;
        if (!_libraryMode && !_package.rootPath.isEmpty()) {
            paths.append(_package.rootPath);
        }
        if (!_libraryMode) { paths.append(_gameData); }
        Game::GameManager::close_resources();
        Game::GameManager::update_folders(Game::FALLOUT_4, paths);
        Game::GameManager::update_status(Game::FALLOUT_4, true);
        Game::GameManager::update_other_games_fallback(false);
    }

    NifModel* ReloadMakerController::activeModel() const
    {
        if (!_window || !_window->getGLView() || !_window->getGLView()->getScene()) {
            return nullptr;
        }
        return const_cast<NifModel*>(_window->getGLView()->getScene()->nifModel);
    }

    ReloadMakerController::WeaponSelection ReloadMakerController::currentWeaponSelection() const
    {
        const int selectionIndex = _weaponCombo->currentIndex() < 0 ? -1 : _weaponCombo->currentData().toInt();
        return selectionIndex >= 0 && selectionIndex < _weaponSelections.size()
            ? _weaponSelections.at(selectionIndex)
            : WeaponSelection{};
    }

    const PluginContent* ReloadMakerController::currentPlugin() const
    {
        const WeaponSelection selection = currentWeaponSelection();
        return selection.pluginIndex >= 0 && selection.pluginIndex < _plugins.size()
            ? &_plugins.at(selection.pluginIndex)
            : nullptr;
    }

    const WeaponRecord* ReloadMakerController::currentWeapon() const
    {
        const WeaponSelection selection = currentWeaponSelection();
        const PluginContent* plugin = currentPlugin();
        return plugin && selection.weaponIndex >= 0 && selection.weaponIndex < plugin->weapons.size()
            ? &plugin->weapons.at(selection.weaponIndex)
            : nullptr;
    }

    const ObjectModRecord* ReloadMakerController::resolvedReceiverObjectMod() const
    {
        for (const auto* mod : configuredObjectMods()) {
            if (isReceiverObjectMod(*mod)) { return mod; }
        }
        return nullptr;
    }

    QVector<const ObjectModRecord*> ReloadMakerController::configuredObjectMods() const
    {
        QVector<const ObjectModRecord*> result;
        const auto* config = configuration();
        const auto* plugin = currentPlugin();
        if (!config || !plugin) { return result; }
        for (auto id : config->activeMods()) {
            if (const auto* mod = plugin->findObjectMod(id)) { result.append(mod); }
        }
        return result;
    }

    QVector<std::uint32_t> ReloadMakerController::activeAnimationKeywords() const
    {
        QVector<std::uint32_t> result;
        const auto append = [&result](const std::uint32_t keyword) {
            if (!result.contains(keyword)) {
                result.append(keyword);
            }
        };
        if (const WeaponRecord* weapon = currentWeapon()) {
            for (const std::uint32_t keyword : weapon->keywords) {
                append(keyword);
            }
        }
        for (const ObjectModRecord* objectMod : configuredObjectMods()) {
            if (!objectMod) {
                continue;
            }
            for (const std::uint32_t keyword : objectMod->addedKeywords) {
                append(keyword);
            }
        }
        if (const ObjectModRecord* receiver = resolvedReceiverObjectMod()) {
            for (const std::uint32_t keyword : receiver->addedKeywords) {
                append(keyword);
            }
        }
        return result;
    }

    QByteArray ReloadMakerController::readAnimationResource(
        const QString& resourcePath, QString& error) const
    {
        error.clear();
        if (_libraryMode) { return _packageResources.read(resourcePath, error); }
        if (_packageResources.contains(resourcePath)) {
            return _packageResources.read(resourcePath, error);
        }
        if (_gameAnimations.contains(resourcePath)) {
            return _gameAnimations.read(resourcePath, error);
        }
        error = QStringLiteral("Animation resource is missing: %1").arg(resourcePath);
        return {};
    }

    QString ReloadMakerController::resolveAnimationPath(
        const QString& graphAnimationPath) const
    {
        QString normalized = ResourceArchive::normalizeResourcePath(graphAnimationPath);
        if (normalized.endsWith(QStringLiteral(".hkt"), Qt::CaseInsensitive)) {
            normalized.chop(4);
            normalized.append(QStringLiteral(".hkx"));
        }
        const QString fileName = QFileInfo(normalized).fileName();
        if (fileName.isEmpty()) {
            return {};
        }

        for (const QString& searchPath : _animationSearchPaths) {
            QString directory = ResourceArchive::normalizeResourcePath(searchPath);
            if (directory.startsWith(QStringLiteral("actors/"), Qt::CaseInsensitive)) {
                directory.prepend(QStringLiteral("meshes/"));
            }
            const QString candidate = ResourceArchive::normalizeResourcePath(
                QStringLiteral("%1/%2").arg(directory, fileName));
            if (_packageResources.contains(candidate) || _gameAnimations.contains(candidate)) {
                return candidate;
            }
        }
        return {};
    }

    bool ReloadMakerController::rebuildAnimationActions(QString& error)
    {
        error.clear();
        _animationActions.clear();
        _animationSearchPaths.clear();

        const PluginContent* plugin = currentPlugin();
        const WeaponRecord* weapon = currentWeapon();
        if (!plugin || !weapon) {
            error = QStringLiteral("No weapon configuration is selected");
            return false;
        }
        const QVector<std::uint32_t> keywords = activeAnimationKeywords();
        const int selectionIndex = _animationSetCombo->currentIndex();
        if (selectionIndex < 0 ||
            selectionIndex >= _animationSubgraphSelections.size()) {
            error = QStringLiteral("Choose an animation / reload set");
            return false;
        }
        const AnimationSubgraphSelection selection =
            _animationSubgraphSelections.at(selectionIndex);
        if (selection.prompt()) {
            error = QStringLiteral(
                "Choose an explicit animation / reload set, or select the automatic keyword mode");
            return false;
        }

        const AnimationSubgraphEntry* subgraph = nullptr;
        QString subgraphAuthority;
        if (selection.automatic()) {
            subgraph = plugin->resolveFirstPersonSubgraph(keywords);
            subgraphAuthority = QStringLiteral("automatic-keywords");
        } else if (selection.recordIndex >= 0 &&
                   selection.recordIndex < plugin->animationSubgraphs.size()) {
            const AnimationSubgraphRecord& record =
                plugin->animationSubgraphs.at(selection.recordIndex);
            if (selection.entryIndex >= 0 &&
                selection.entryIndex < record.entries.size()) {
                subgraph = &record.entries.at(selection.entryIndex);
                subgraphAuthority = QStringLiteral("explicit:%1")
                                        .arg(record.editorId);
            }
        }
        if (!subgraph) {
            error = selection.automatic()
                ? QStringLiteral(
                      "The configured weapon keywords match no first-person RACE subgraph; choose an explicit animation / reload set")
                : QStringLiteral("The selected animation / reload set is no longer available");
            return false;
        }
        _animationSearchPaths = subgraph->animationSearchPaths;

        QString rootBehavior = ResourceArchive::normalizeResourcePath(subgraph->behaviorPath);
        if (rootBehavior.startsWith(QStringLiteral("actors/"), Qt::CaseInsensitive)) {
            rootBehavior.prepend(QStringLiteral("meshes/"));
        }

        HkxBehaviorGraph combined;
        QSet<QString> visited;
        std::function<bool(const QString&, int)> loadGraph;
        loadGraph = [this, &combined, &visited, &loadGraph, &error](
                        const QString& requestedPath, const int depth) -> bool {
            if (depth > 8) {
                error = QStringLiteral("Behavior graph references are nested too deeply");
                return false;
            }
            const QString path = ResourceArchive::normalizeResourcePath(requestedPath);
            if (visited.contains(path)) {
                return true;
            }
            visited.insert(path);

            QString readError;
            HkxBehaviorGraph graph;
            if (const auto* cached = _graphCache.object(path)) { graph = *cached; }
            else {
                const QByteArray bytes = readAnimationResource(path, readError);
                if (bytes.isEmpty()) { error = readError; return false; }
                graph = HkxReader::readBehaviorGraph(bytes, readError);
                if (graph.valid()) { ++_sourceMisses; _graphCache.insert(path, new HkxBehaviorGraph(graph)); }
            }
            if (!graph.valid()) {
                error = QStringLiteral("Cannot parse behavior graph '%1': %2")
                            .arg(path, readError);
                return false;
            }
            for (const HkxBehaviorClip& clip : graph.clips) {
                const bool duplicate = std::any_of(
                    combined.clips.cbegin(), combined.clips.cend(),
                    [&clip](const HkxBehaviorClip& current) {
                        return current.nodeName == clip.nodeName &&
                               current.animationPath == clip.animationPath;
                    });
                if (!duplicate) {
                    combined.clips.append(clip);
                }
            }
            if (!graph.defaultReadyAnimationPath.isEmpty()) {
                combined.defaultReadyAnimationPath = graph.defaultReadyAnimationPath;
            }

            const QString behaviorDirectory = QFileInfo(path).path();
            const QString behaviorParent = QFileInfo(behaviorDirectory).path();
            for (const QString& reference : graph.behaviorReferences) {
                QString normalizedReference = ResourceArchive::normalizeResourcePath(reference);
                QString resolvedReference;
                if (normalizedReference.startsWith(QStringLiteral("meshes/"),
                                                   Qt::CaseInsensitive)) {
                    resolvedReference = normalizedReference;
                } else if (normalizedReference.startsWith(QStringLiteral("actors/"),
                                                          Qt::CaseInsensitive)) {
                    resolvedReference = QStringLiteral("meshes/%1").arg(normalizedReference);
                } else if (normalizedReference.startsWith(QStringLiteral("behaviors/"),
                                                          Qt::CaseInsensitive)) {
                    resolvedReference = QStringLiteral("%1/%2")
                                            .arg(behaviorParent, normalizedReference);
                } else {
                    resolvedReference = QStringLiteral("%1/%2")
                                            .arg(behaviorDirectory, normalizedReference);
                }
                if (!loadGraph(resolvedReference, depth + 1)) {
                    return false;
                }
            }
            return true;
        };

        if (!loadGraph(rootBehavior, 0) || combined.clips.isEmpty()) {
            if (error.isEmpty()) {
                error = QStringLiteral("The resolved behavior graph contains no animation clips");
            }
            return false;
        }

        const auto clipForAlias = [&combined](const QStringList& aliases)
            -> const HkxBehaviorClip* {
            for (const QString& alias : aliases) {
                const auto iterator = std::find_if(
                    combined.clips.cbegin(), combined.clips.cend(),
                    [&alias](const HkxBehaviorClip& clip) {
                        QString path = clip.animationPath;
                        path.replace('\\', '/');
                        return QFileInfo(path).completeBaseName().compare(
                                   alias, Qt::CaseInsensitive) == 0;
                    });
                if (iterator != combined.clips.cend()) {
                    return &*iterator;
                }
            }
            return nullptr;
        };
        const QString readyResource = resolveAnimationPath(combined.defaultReadyAnimationPath);
        if (readyResource.isEmpty()) {
            error = QStringLiteral("The graph-selected Ready pose cannot be resolved through the RACE search stack");
            return false;
        }
        const HkxBehaviorClip* readyClip = clipForAlias(QStringList{
            QFileInfo(ResourceArchive::normalizeResourcePath(
                combined.defaultReadyAnimationPath)).completeBaseName()
        });
        _animationActions.append(AnimationAction{
            QStringLiteral("Idle / Ready"),
            readyClip ? readyClip->nodeName : QStringLiteral("Graph default Ready"),
            readyResource, readyResource, 0.0f, 0.0f, ActionFamily::Idle, false, false,
        });
        // The graph, not a fixed alias list, decides which authored actions exist.
        // Each clip is classified by base name and resolved through the RACE
        // search stack; clips absent for this weapon are skipped.
        struct Candidate { AnimationAction action; int order; };
        QVector<Candidate> candidates;
        QSet<QString> labels;
        for (const HkxBehaviorClip& clip : combined.clips) {
            QString path = clip.animationPath;
            path.replace('\\', '/');
            const auto kind = classifyWeaponClip(QFileInfo(path).completeBaseName());
            if (!kind || labels.contains(kind->label)) continue;
            QString resource = resolveAnimationPath(clip.animationPath);
            if (kind->baseReload && _glockNoExtender) {
                const auto alternative = glockNoExtenderPath();
                if (!alternative.isEmpty()) resource = alternative; else _glockNoExtender = false;
            }
            if (resource.isEmpty()) continue;
            labels.insert(kind->label);
            candidates.append({AnimationAction{kind->label, clip.nodeName, resource, readyResource,
                clip.blendInSeconds, clip.blendOutSeconds, kind->family, kind->automatic, kind->baseReload}, kind->order});
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.action.family != b.action.family ? int(a.action.family) < int(b.action.family) : a.order < b.order;
        });
        for (const auto& candidate : candidates) _animationActions.append(candidate.action);

        QStringList keywordText;
        for (const std::uint32_t keyword : keywords) {
            keywordText.append(formIdText(keyword));
        }
        appendDiagnostic(QStringLiteral("SUBGRAPH  %1  authority=%2  keywords=[%3]")
                             .arg(rootBehavior,
                                  subgraphAuthority,
                                  keywordText.join(QStringLiteral(", "))));
        for (qsizetype index = 0; index < _animationSearchPaths.size(); ++index) {
            appendDiagnostic(QStringLiteral("  SEARCH %1  %2")
                                 .arg(index + 1)
                                 .arg(_animationSearchPaths.at(index)));
        }
        appendDiagnostic(QStringLiteral("GRAPH  clips=%1 actions=%2 ready=%3")
                             .arg(combined.clips.size())
                             .arg(_animationActions.size())
                             .arg(readyResource));
        return true;
    }

    QString ReloadMakerController::normalizeNifPath(const QString& path) const
    {
        QString result = ResourceArchive::normalizeResourcePath(path);
        const qsizetype meshes = result.indexOf(QStringLiteral("meshes/"), 0,
                                                Qt::CaseInsensitive);
        if (meshes > 0) {
            result.remove(0, meshes);
        } else if (meshes < 0) {
            result.prepend(QStringLiteral("meshes/"));
        }
        if (!result.endsWith(QStringLiteral(".nif"), Qt::CaseInsensitive)) {
            result.append(QStringLiteral(".nif"));
        }
        return result;
    }

    bool ReloadMakerController::loadNif(const ResourceArchive& archive,
                                        const QString& resourcePath,
                                        NifModel& destination, QString& error) const
    {
        return loadNifBytes(nifBytes(archive, resourcePath, error), destination, error);
    }

    void ReloadMakerController::clearAssetCaches()
    {
        _scopeProfiles.clear();
        _grabLocations.clear();_pickingGrabLocation=false;_grabLocationCapturePreview.reset();
        _audio.clear();_soundTracking=false;_soundCatalog.clear();_referenceSounds.clear();_soundProfile={};_savedSounds.clear();_soundsDirty=false;
        invalidateReloadCapture();
        _referenceNames.clear();
        _weaponFitCorners.clear();
        _handPosePreview.reset();
        _partGripDraft.reset();
        _gripPreviewValid = false;
        _gripError.clear();
        _fingerDrafts = {};
        _suspendedAuthoringGrip.reset();
        _operateSuspendedGrip.reset();
        _handPoseAnchors.clear();
        _handPoseAnchor->clear();
        _playing = false; _playbackTimer->stop();
        _rebuildTimer->stop();
        _clip = {}; _readyClip = {};
        _activeAction.clear(); _activeAnimationPath.clear();
        _animationActions.clear(); _animationSearchPaths.clear();
        _nodeBindings.clear(); _sceneNodes.clear();
        _assemblyNodes.clear();
        stopMechanismPreview();
        _mechanismDraft.reset();
        _mechanismState = {};
        displayMechanism();
        _timeline->setEnabled(false);
        updateTimelineText();
        refreshFingerEditor();
        _nifCache.clear();
        _nifBytesCache.clear();
        _clipCache.clear();
        _graphCache.clear();
        _skeleton = {};
        _geometrySignature.clear();
        _renderedWeapon = -1;
        _partRoots.clear();
        _partSurfaces.clear();
        _partAlignmentAnchors.clear();
        _attachmentConnections.clear();
        _remappedAttachments.clear();
        if (_slotOverlay) { _slotOverlay->setCallouts({}); }
        if (auto* model = activeModel()) { model->clear(); }
        _window->getGLView()->flush();
        refreshPartGripChoices();
        refreshPartGripList();
        updateHandPoseControls();
    }

    QByteArray ReloadMakerController::nifBytes(const ResourceArchive& archive,
                                               const QString& resourcePath, QString& error) const
    {
        const QString path = normalizeNifPath(resourcePath);
        const QString key = (&archive == &_gameMeshes ? QStringLiteral("game:") : QStringLiteral("mod:")) + path;
        error.clear();
        if (const auto* bytes = _nifBytesCache.object(key)) { return *bytes; }
        const QByteArray bytes = archive.read(path, error);
        if (!bytes.isEmpty()) {
            _nifBytesCache.insert(key, new QByteArray(bytes), std::max(1, int((bytes.size() + 1023) / 1024)));
        }
        return bytes;
    }

    QSharedPointer<NifModel> ReloadMakerController::sourceNif(const ResourceArchive& archive,
                                                              const QString& resourcePath, QString& error)
    {
        const QString path = normalizeNifPath(resourcePath);
        const QString key = (&archive == &_gameMeshes ? QStringLiteral("game:") : QStringLiteral("mod:")) + path;
        error.clear();
        if (const auto* model = _nifCache.object(key)) { return *model; }
        QElapsedTimer decodeClock;
        decodeClock.start();
        auto model = QSharedPointer<NifModel>::create();
        if (!loadNif(archive, path, *model, error)) { return {}; }
        _decodeMilliseconds += decodeClock.elapsed();
        ++_sourceMisses;
        _nifCache.insert(key, new QSharedPointer<NifModel>(model));
        return model;
    }

    HkxClip ReloadMakerController::cachedClip(const QString& resourcePath, QString& error)
    {
        const QString path = ResourceArchive::normalizeResourcePath(resourcePath);
        error.clear();
        if (const auto* clip = _clipCache.object(path)) { return *clip; }
        const QByteArray bytes = readAnimationResource(path, error);
        if (bytes.isEmpty()) { return {}; }
        HkxClip clip = HkxReader::readClip(bytes, error);
        if (clip.valid()) { ++_sourceMisses; _clipCache.insert(path, new HkxClip(clip)); }
        return clip;
    }

    bool ReloadMakerController::loadNifBytes(const QByteArray& bytes, NifModel& destination,
                                             QString& error) const
    {
        if (bytes.isEmpty()) {
            if (error.isEmpty()) {
                error = QStringLiteral("NIF resource is empty");
            }
            return false;
        }
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly) || !destination.load(buffer)) {
            error = QStringLiteral("NifSkope could not parse the NIF resource");
            return false;
        }
        return true;
    }

    QPersistentModelIndex ReloadMakerController::pasteRoot(NifModel& source,
                                                           const QModelIndex& parent,
                                                           const QString& rootName)
    {
        const QList<int> roots = source.getRootLinks();
        if (roots.isEmpty()) {
            return {};
        }
        const QModelIndex sourceRoot = source.getBlockIndex(roots.first());
        if (!sourceRoot.isValid()) {
            return {};
        }
        // The interactive clipboard paste rebuilds every block under the live
        // model's observers. Whole single-root assets can instead be decoded in
        // isolation and transferred by NifSkope with its native link/string map.
        // Partial/multi-root imports retain branch selection semantics.
        NifModel* destination = activeModel();
        if (!destination || !parent.isValid() || source.getVersionNumber() != destination->getVersionNumber()) { return {}; }
        const int firstBlock = destination->getBlockCount();
        if (roots.size() != 1) {
            const auto root = pasteBranch(source, sourceRoot, parent, rootName);
            if (!root.isValid() || destination->getBlockNumber(root) < firstBlock) { return {}; }
            registerAttachmentModel(root, firstBlock, destination->getBlockCount());
            return root;
        }
        QElapsedTimer transferClock;
        transferClock.start();
        QByteArray bytes;
        QBuffer output(&bytes);
        if (!output.open(QIODevice::WriteOnly) || !source.save(output)) { return {}; }
        NifModel transfer;
        QString error;
        if (!loadNifBytes(bytes, transfer, error)) { return {}; }
        _copyMilliseconds += transferClock.restart();
        QPersistentModelIndex root;
        {
            // Native block transfer updates many string/link fields. Defer its
            // repeated header/link scans until the complete asset is attached;
            // structural model notifications remain enabled for Qt observers.
            const bool held = destination->holdUpdates(true);
            destination->setState(BaseModel::Processing);
            const auto restore = qScopeGuard([destination, held]() {
                destination->restoreState();
                (void)destination->holdUpdates(held);
            });
            const auto blockMap = transfer.moveAllNiBlocks(destination);
            if (!blockMap.contains(roots.first())) { return {}; }
            root = destination->getBlockIndex(blockMap.value(roots.first()));
            if (!rootName.isEmpty()) { destination->set<QString>(root, "Name", rootName); }
            blockLink(destination, parent, root);
        }
        _pasteMilliseconds += transferClock.elapsed();
        registerAttachmentModel(root, firstBlock, destination->getBlockCount());
        return root;
    }

    QPersistentModelIndex ReloadMakerController::pasteBranch(NifModel& source,
                                                             const QModelIndex& sourceRoot,
                                                             const QModelIndex& parent,
                                                             const QString& rootName)
    {
        NifModel* destination = activeModel();
        if (!sourceRoot.isValid() || !parent.isValid() || !destination || source.getVersionNumber()!=destination->getVersionNumber()) {
            return {};
        }
        const QString originalName = source.get<QString>(sourceRoot, "Name");
        if (!rootName.isEmpty()) { source.set<QString>(sourceRoot, "Name", rootName); }
        spCopyBranch copy;
        QMimeData payload;
        QElapsedTimer transferClock;
        transferClock.start();
        copy.copyData(&source, sourceRoot, payload);
        _copyMilliseconds += transferClock.restart();
        if (!rootName.isEmpty()) { source.set<QString>(sourceRoot, "Name", originalName); }
        if(payload.formats().isEmpty())return {};
        spPasteBranch paste;
        const QPersistentModelIndex result(paste.pasteData(destination, parent, payload));
        _pasteMilliseconds += transferClock.elapsed();
        return result;
    }

    QPersistentModelIndex ReloadMakerController::findNode(const QString& name,
                                                          const int firstBlock,
                                                          int lastBlock) const
    {
        NifModel* model = activeModel();
        if (!model) {
            return {};
        }
        if (lastBlock < 0 || lastBlock > model->getBlockCount()) {
            lastBlock = model->getBlockCount();
        }
        for (int block = (std::max)(0, firstBlock); block < lastBlock; ++block) {
            const QModelIndex index = model->getBlockIndex(block);
            if (index.isValid() && model->blockInherits(index, "NiNode") &&
                model->get<QString>(index, "Name").compare(name, Qt::CaseInsensitive) == 0) {
                return QPersistentModelIndex(index);
            }
        }
        return {};
    }

    QHash<QString, QPersistentModelIndex> ReloadMakerController::collectCanonicalNodes(
        const int firstBlock, int lastBlock) const
    {
        QHash<QString, QPersistentModelIndex> result;
        NifModel* model = activeModel();
        if (!model) {
            return result;
        }
        if (lastBlock < 0 || lastBlock > model->getBlockCount()) {
            lastBlock = model->getBlockCount();
        }
        for (int block = (std::max)(0, firstBlock); block < lastBlock; ++block) {
            const QModelIndex index = model->getBlockIndex(block);
            if (!index.isValid() || !model->blockInherits(index, "NiNode")) {
                continue;
            }
            const QString name = model->get<QString>(index, "Name");
            if (!name.isEmpty() && !result.contains(normalizedKey(name))) {
                result.insert(normalizedKey(name), QPersistentModelIndex(index));
            }
        }
        return result;
    }

    int ReloadMakerController::mergeWeaponModelNodes(
        const QHash<QString, QPersistentModelIndex>& nodes)
    {
        int replaced = 0;
        for (auto iterator = nodes.cbegin(); iterator != nodes.cend(); ++iterator) {
            const bool visibleWeaponPart =
                iterator.key() != QStringLiteral("weapon") &&
                iterator.key().startsWith(QStringLiteral("weapon"));
            if (visibleWeaponPart) {
                if (_canonicalNodes.contains(iterator.key()) &&
                    _canonicalNodes.value(iterator.key()) != iterator.value()) {
                    ++replaced;
                }
                _canonicalNodes.insert(iterator.key(), iterator.value());
            } else if (!_canonicalNodes.contains(iterator.key())) {
                _canonicalNodes.insert(iterator.key(), iterator.value());
            }
        }
        return replaced;
    }

    QStringList ReloadMakerController::childConnectPoints(const NifModel& source) const
    {
        QStringList result;
        for (int block = 0; block < source.getBlockCount(); ++block) {
            const QModelIndex index = source.getBlockIndex(block);
            if (!index.isValid() || source.itemName(index) != QStringLiteral("BSConnectPoint::Children")) {
                continue;
            }
            const QModelIndex points = source.getIndex(index, "Point Name");
            for (int row = 0; row < source.rowCount(points); ++row) {
                const QString point = source.get<QString>(source.getIndex(points, row));
                if (!point.isEmpty()) {
                    result.append(point);
                }
            }
        }
        return result;
    }

    void ReloadMakerController::registerAttachmentModel(const QModelIndex& root, int firstBlock, int lastBlock)
    {
        const auto* model = activeModel();
        if (!model || !root.isValid()) { return; }
        AttachmentModelScope scope;
        scope.root = model->getBlockNumber(root);
        for (int block = firstBlock; block < lastBlock; ++block) {
            const auto index = model->getBlockIndex(block);
            if (model->blockInherits(index, "NiNode")) {
                scope.nodes.append({block, model->get<QString>(index, "Name")});
            }
            if (model->itemName(index) != QStringLiteral("BSConnectPoint::Parents")) { continue; }
            const auto points = model->getIndex(index, "Connect Points");
            for (int row = 0; row < model->rowCount(points); ++row) {
                const auto point = model->getIndex(points, row);
                const auto t = model->get<Vector3>(point, "Translation");
                const auto q = model->get<Quat>(point, "Rotation");
                const float scale = model->get<float>(point, "Scale");
                scope.points.append({model->get<QString>(point, "Name"), model->get<QString>(point, "Parent"),
                    {{t[0], t[1], t[2]}, {q[1], q[2], q[3], q[0]}, {scale, scale, scale}}});
            }
        }
        if (!scope.points.isEmpty()) { _attachmentConnections.add(std::move(scope)); }
    }

    ReloadMakerController::ConnectTarget ReloadMakerController::findConnectTarget(
        const QString& childPoint, const NifModel* incoming, SlotKey slot) const
    {
        ConnectTarget result;
        const auto* model = activeModel();
        if (!model || !childPoint.startsWith(QStringLiteral("C-"), Qt::CaseInsensitive)) {
            result.error = QStringLiteral("Invalid child connection %1").arg(childPoint);
            return result;
        }
        QVector<int> preferredRoots;
        if (const auto* config = configuration()) {
            const auto available = config->availableSlots();
            SlotKey owner = available.value(slot);
            for (int depth = 0; owner && depth < config->entries().size(); ++depth) {
                const auto partRoot = _partRoots.value(owner);
                if (partRoot.isValid()) {
                    preferredRoots.append(model->getBlockNumber(partRoot));
                } else if (const auto* part = config->mod(config->selected(owner)); part && !part->modelPath.isEmpty()) {
                    // Do not steal another part's identically named connection
                    // while the declared geometric owner is still pending.
                    result.error = QStringLiteral("Waiting for installed owner %1").arg(part->editorId);
                    return result;
                }
                owner = available.value(owner);
            }
        }
        if (_weaponRoot.isValid()) { preferredRoots.append(model->getBlockNumber(_weaponRoot)); }
        const auto match = _attachmentConnections.find(QStringLiteral("P-") + childPoint.mid(2), preferredRoots, result.error);
        const auto* point = _attachmentConnections.point(match);
        if (!point) { return result; }
        result.ownerRoot = _attachmentConnections.ownerRoot(match);
        const QString& parentName = point->parentName;
        const int bone = _skeleton.boneIndex(parentName);
        bool suppliesParent = false;
        if (incoming && bone >= 0 && bone != _weaponBoneIndex && isDescendantOf(bone, _weaponBoneIndex)) {
            for (int block = 0; block < incoming->getBlockCount(); ++block) {
                const auto node = incoming->getBlockIndex(block);
                if (incoming->blockInherits(node, "NiNode") &&
                    incoming->get<QString>(node, "Name").compare(parentName, Qt::CaseInsensitive) == 0) {
                    suppliesParent = true;
                    break;
                }
            }
        }
        if (suppliesParent) {
            // Preserve incoming magazine/cartridge ownership: attach in the
            // semantic parent frame, not under a second copy of its own bone.
            const int parentBone = _skeleton.bones.at(bone).parentIndex;
            if (parentBone >= 0) {
                result.parent = _canonicalNodes.value(normalizedKey(_skeleton.bones.at(parentBone).name));
            }
            result.suppliedBone = bone;
        } else {
            const auto canonical = bone >= 0 ? _canonicalNodes.value(normalizedKey(parentName)) : QPersistentModelIndex{};
            const int anchor = _attachmentConnections.anchor(match,
                canonical.isValid() ? model->getBlockNumber(canonical) : -1, result.error);
            if (anchor >= 0) { result.parent = model->getBlockIndex(anchor); }
        }
        if (!result.parent.isValid()) {
            if (result.error.isEmpty()) { result.error = QStringLiteral("Unavailable rig parent for %1").arg(childPoint); }
            return result;
        }
        result.translation = point->local.translation;
        result.rotation = point->local.rotation;
        result.scale = point->local.scale.x;
        return result;
    }

    bool ReloadMakerController::setNodeTransform(const QModelIndex& node,
                                                 const Transformf& transform)
    {
        NifModel* model = activeModel();
        if (!model || !node.isValid()) {
            return false;
        }
        Matrix rotation;
        rotation.fromQuat(Quat(transform.rotation.w, transform.rotation.x,
                               transform.rotation.y, transform.rotation.z));
        const float scale = (transform.scale.x + transform.scale.y + transform.scale.z) / 3.0f;
        return model->set<Vector3>(node, "Translation",
                                   Vector3(transform.translation.x, transform.translation.y,
                                           transform.translation.z)) &&
               model->set<Matrix>(node, "Rotation", rotation) &&
               model->set<float>(node, "Scale", std::isfinite(scale) ? scale : 1.0f);
    }

    bool ReloadMakerController::readNodeTransform(
        const QModelIndex& node, Transformf& transform) const
    {
        NifModel* model = activeModel();
        if (!model || !node.isValid()) {
            return false;
        }
        const Vector3 translation = model->get<Vector3>(node, "Translation");
        const Matrix rotation = model->get<Matrix>(node, "Rotation");
        const Quat quaternion = rotation.toQuat();
        const float scale = model->get<float>(node, "Scale");
        if (!std::isfinite(translation[0]) ||
            !std::isfinite(translation[1]) ||
            !std::isfinite(translation[2]) ||
            !std::isfinite(quaternion[0]) ||
            !std::isfinite(quaternion[1]) ||
            !std::isfinite(quaternion[2]) ||
            !std::isfinite(quaternion[3]) ||
            !std::isfinite(scale) || std::abs(scale) <= 1.0e-6f) {
            return false;
        }
        transform = Transformf{
            Vec3f{ translation[0], translation[1], translation[2] },
            normalized(Quaternionf{
                quaternion[1], quaternion[2], quaternion[3], quaternion[0]
            }),
            Vec3f{ scale, scale, scale },
        };
        return true;
    }

    QPersistentModelIndex ReloadMakerController::createBoneNode(const int boneIndex)
    {
        NifModel* model = activeModel();
        if (!model || boneIndex < 0 || boneIndex >= _skeleton.bones.size()) {
            return {};
        }
        const HkxBone& bone = _skeleton.bones.at(boneIndex);
        if (bone.parentIndex < 0 || bone.parentIndex >= _skeleton.bones.size()) {
            return {};
        }
        const QPersistentModelIndex parent = _canonicalNodes.value(
            normalizedKey(_skeleton.bones.at(bone.parentIndex).name));
        if (!parent.isValid()) {
            return {};
        }
        const QModelIndex node = model->insertNiBlock(QStringLiteral("NiNode"), -1);
        if (!node.isValid()) {
            return {};
        }
        model->set<QString>(node, "Name", bone.name);
        (void)setNodeTransform(node, bone.referenceLocal);
        blockLink(model, parent, node);
        const QPersistentModelIndex persistent(node);
        _canonicalNodes.insert(normalizedKey(bone.name), persistent);
        return persistent;
    }

    bool ReloadMakerController::addSkinHelperBones(QString& error)
    {
        error.clear();
        const QString sourcePath = QStringLiteral(
            "meshes/actors/character/characterassets/skeleton.nif");
        const auto sourceModel = sourceNif(referenceMeshes(), sourcePath, error);
        if (!sourceModel) { return false; }
        NifModel& source = *sourceModel;

        for (const char* helperName : kSkinHelperBoneNames) {
            QModelIndex sourceNode;
            for (int block = 0; block < source.getBlockCount(); ++block) {
                const QModelIndex candidate = source.getBlockIndex(block);
                if (candidate.isValid() && source.blockInherits(candidate, "NiNode") &&
                    source.get<QString>(candidate, "Name").compare(
                        QLatin1String(helperName), Qt::CaseInsensitive) == 0) {
                    sourceNode = candidate;
                    break;
                }
            }
            if (!sourceNode.isValid()) {
                error = QStringLiteral("Full skeleton is missing helper bone '%1'.")
                            .arg(QLatin1String(helperName));
                return false;
            }

            const QModelIndex sourceParent = source.getBlockIndex(source.getParent(sourceNode));
            if (!sourceParent.isValid()) {
                error = QStringLiteral("Helper bone '%1' has no unique parent.")
                            .arg(QLatin1String(helperName));
                return false;
            }
            const QString parentName = source.get<QString>(sourceParent, "Name");
            const QPersistentModelIndex destinationParent =
                _canonicalNodes.value(normalizedKey(parentName));
            if (!destinationParent.isValid()) {
                error = QStringLiteral("First-person skeleton is missing helper parent '%1'.")
                            .arg(parentName);
                return false;
            }

            const QPersistentModelIndex pasted = pasteBranch(
                source, sourceNode, destinationParent);
            if (!pasted.isValid()) {
                error = QStringLiteral("Cannot attach helper bone '%1'.")
                            .arg(QLatin1String(helperName));
                return false;
            }
            _canonicalNodes.insert(normalizedKey(QLatin1String(helperName)), pasted);
        }
        return true;
    }

    ReloadMakerController::SkinRetargetResult ReloadMakerController::retargetSkin(
        const int firstBlock, const int lastBlock, const QModelIndex& skeletonRoot)
    {
        SkinRetargetResult result;
        NifModel* model = activeModel();
        if (!model || !skeletonRoot.isValid()) {
            return result;
        }

        const int skeletonRootBlock = model->getBlockNumber(skeletonRoot);
        const QSignalBlocker signalBlocker(model);
        const bool updatesWereHeld = model->holdUpdates(true);
        for (int block = (std::max)(0, firstBlock);
             block < (std::min)(lastBlock, model->getBlockCount()); ++block) {
            const QModelIndex skin = model->getBlockIndex(block);
            if (!skin.isValid() ||
                model->itemName(skin) != QStringLiteral("BSSkin::Instance")) {
                continue;
            }
            (void)model->setLink(skin, "Skeleton Root", skeletonRootBlock);
            const QModelIndex bones = model->getIndex(skin, "Bones");
            for (int row = 0; row < model->rowCount(bones); ++row) {
                const QModelIndex boneLink = model->getIndex(bones, row);
                const QModelIndex sourceBone = model->getBlockIndex(model->getLink(boneLink));
                if (!sourceBone.isValid()) {
                    continue;
                }
                ++result.totalBones;
                const QString name = model->get<QString>(sourceBone, "Name");
                const QPersistentModelIndex canonical = _canonicalNodes.value(normalizedKey(name));
                if (!canonical.isValid()) {
                    continue;
                }
                if (model->setLink(boneLink, model->getBlockNumber(canonical))) {
                    ++result.retargetedBones;
                }
            }
        }
        (void)model->holdUpdates(updatesWereHeld);
        return result;
    }

    bool ReloadMakerController::buildNodeBindings(QString& error)
    {
        _nodeBindings.clear();
        _sceneNodes.clear();
        NifModel* model = activeModel();
        if (!model || !_skeleton.valid()) {
            error = QStringLiteral("No scene or skeleton for binding");
            return false;
        }
        QVector<ScenePoseNode> nodes;
        QHash<int, int> blockToNode;
        QVector<int> canonical(_skeleton.bones.size(), -1);
        for (int block = 0; block < model->getBlockCount(); ++block) {
            const QModelIndex index = model->getBlockIndex(block);
            if (!model->blockInherits(index, "NiNode")) { continue; }
            Transformf rest;
            if (!readNodeTransform(index, rest)) {
                error = QStringLiteral("Invalid assembly node transform at block %1").arg(block);
                return false;
            }
            const QString name = model->get<QString>(index, "Name");
            const int bone = _skeleton.boneIndex(name);
            const bool isCanonical = bone >= 0 &&
                _canonicalNodes.value(normalizedKey(name)) == index;
            const bool part = bone >= 0 && bone != _weaponBoneIndex &&
                isDescendantOf(bone, _weaponBoneIndex);
            const int node = nodes.size();
            nodes.append(ScenePoseNode{ -1, isCanonical || part ? bone : -1, rest });
            _sceneNodes.append(QPersistentModelIndex(index));
            blockToNode.insert(block, node);
            if (isCanonical) { canonical[bone] = node; }
        }
        for (int node = 0; node < nodes.size(); ++node) {
            const int block = model->getBlockNumber(_sceneNodes.at(node));
            const auto alignment = _partAlignmentAnchors.constFind(block);
            if (alignment != _partAlignmentAnchors.cend()) {
                nodes[node].alignmentNode = blockToNode.value(alignment.value(), -1);
                if (nodes.at(node).alignmentNode < 0) {
                    error = QStringLiteral("Attachment alignment anchor disappeared before binding");
                    return false;
                }
            }
            const int parentBlock = model->getParent(_sceneNodes.at(node));
            if (parentBlock >= 0 && !blockToNode.contains(parentBlock)) {
                error = QStringLiteral("Unsupported non-node parent for assembly block %1")
                            .arg(model->getBlockNumber(_sceneNodes.at(node)));
                return false;
            }
            nodes[node].parent = blockToNode.value(parentBlock, -1);
        }
        if (!compileSceneBindings(_skeleton, _weaponBoneIndex, nodes, canonical, _nodeBindings, error)) {
            return false;
        }
        _assemblyNodes=nodes;
        int parts = 0;
        for (const auto& binding : _nodeBindings) {
            if (binding.weaponPart) {
                ++parts;
                appendDiagnostic(QStringLiteral("PART BIND %1 block=%2 parent=%3 rest=(%4,%5,%6)")
                    .arg(_skeleton.bones.at(binding.boneIndex).name)
                    .arg(model->getBlockNumber(_sceneNodes.at(binding.nodeIndex)))
                    .arg(model->getParent(_sceneNodes.at(binding.nodeIndex)))
                    .arg(binding.restInBoneParent.translation.x, 0, 'f', 4)
                    .arg(binding.restInBoneParent.translation.y, 0, 'f', 4)
                    .arg(binding.restInBoneParent.translation.z, 0, 'f', 4));
                if (binding.alignmentBoneIndex != binding.boneIndex) {
                    appendDiagnostic(QStringLiteral("PART ALIGN %1 block=%2 via attachment anchor %3; authored branch placement retained")
                        .arg(_skeleton.bones.at(binding.boneIndex).name)
                        .arg(model->getBlockNumber(_sceneNodes.at(binding.nodeIndex)))
                        .arg(_skeleton.bones.at(binding.alignmentBoneIndex).name));
                }
            }
        }
        appendDiagnostic(QStringLiteral("BIND %1 total nodes; %2 actual part bindings").arg(_nodeBindings.size()).arg(parts));
        return true;
    }

    void ReloadMakerController::rebuildWeapon()
    {
        if (_loadThread) { return; }
        invalidateCollisions();
        invalidateReloadCapture();
        _rebuildTimer->stop();
        restoreHandPosePreview();
        const auto poseControls = qScopeGuard([this]() { updateHandPoseControls(); });
        QElapsedTimer rebuildClock;
        rebuildClock.start();
        _sourceMisses = 0;
        _copyMilliseconds = _pasteMilliseconds = _decodeMilliseconds = 0;
        const QString previousAction = _activeAction;
        const bool sameWeapon = _renderedWeapon == _weaponCombo->currentIndex();
        const PluginContent* plugin = currentPlugin();
        const WeaponRecord* weapon = currentWeapon();
        if (!plugin || !weapon || !_packageResources.isOpen()) {
            return;
        }
        if (_gameData.isEmpty() || (!_libraryMode && (!_gameMeshes.isOpen() || !_gameAnimations.isOpen()))) {
            setStatus(QStringLiteral("Select the Fallout 4 VR Data folder before building the weapon."), true);
            return;
        }
        QStringList geometryParts{QString::number(_weaponCombo->currentIndex()),
                                  _bodyCombo->currentData().toString(), normalizeNifPath(weapon->baseModelPath)};
        for (const auto* mod : configuredObjectMods()) {
            if (!mod->modelPath.isEmpty()) {
                geometryParts.append(QString::number(configurationSlotKey(*mod)) + QLatin1Char(':') + normalizeNifPath(mod->modelPath));
                for(auto keyword:mod->addedKeywords)if(plugin->nodeRemaps.contains(keyword))geometryParts.append("remap:"+recordIdentity(*plugin,keyword));
            }
        }
        const QString geometrySignature = geometryParts.join(QLatin1Char('|'));
        const bool reuseGeometry = geometrySignature == _geometrySignature && !_nodeBindings.isEmpty();
        QApplication::setOverrideCursor(Qt::WaitCursor);
        _playing = false;
        _playbackTimer->stop();
        _playButton->setText(QStringLiteral("PLAY"));
        _clip = {};
        _readyClip = {};
        _handPosePreview.reset();
        _fingerDrafts = {};
        _suspendedAuthoringGrip.reset();
        _handPoseAnchors.clear();
        _handPoseAnchor->clear();
        _readyLocalPose.clear();
        _readyGlobalPose.clear();
        _activeAction.clear();
        _activeAnimationPath.clear();
        _readyAnimationPath.clear();
        _stateBaselineAnimationPath.clear();
        _animationActions.clear();
        _animationSearchPaths.clear();
        _sourceLocalPose.clear();
        _sourceGlobalPose.clear();
        _stateBaselineLocalPose.clear();
        if (!reuseGeometry) {
            _sceneNodes.clear();
            _boundLocalPose.clear();
            _boundGlobalPose.clear();
        }
        _playbackTime = 0.0f;
        _timeline->setEnabled(false);
        refreshAnimations();
        if (!reuseGeometry) {
            _canonicalNodes.clear();
            _partRoots.clear();
            _partSurfaces.clear();
            _partAlignmentAnchors.clear();
            _attachmentConnections.clear();
            _remappedAttachments.clear();
            _slotOverlay->setCallouts({});
            _nodeBindings.clear();
            _weaponRoot = QPersistentModelIndex{};
            _bodyRoot = QPersistentModelIndex{};
            _handsRoot = QPersistentModelIndex{};
            _bodyRootFlags = 0;
            _handsRootFlags = 0;
            _weaponBoneIndex = -1;
        }
        _clipBlendInSeconds = 0.0f;
        _clipBlendOutSeconds = 0.0f;

        QString error;
        if (!rebuildAnimationActions(error)) {
            appendDiagnostic(QStringLiteral("ANIMATION unavailable: %1 — configuration preview remains available").arg(error));
        }
        const QString skeletonHkxPath = QStringLiteral(
            "meshes/actors/character/_1stperson/characterassets/skeleton.hkx");
        if (!_skeleton.valid()) { _skeleton = HkxReader::readSkeleton(readAnimationResource(skeletonHkxPath, error), error); }
        if (!_skeleton.valid()) {
            QApplication::restoreOverrideCursor();
            setStatus(QStringLiteral("Cannot load first-person HKX skeleton: %1").arg(error), true);
            return;
        }
        for (const char* name : { "LArm_Collarbone", "RArm_Collarbone", "LArm_Hand", "RArm_Hand" }) {
            if (_skeleton.boneIndex(QLatin1String(name)) < 0) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("First-person skeleton is missing %1").arg(QLatin1String(name)), true);
                return;
            }
        }

        NifModel* destination = activeModel();
        if (!destination) {
            QApplication::restoreOverrideCursor();
            setStatus(QStringLiteral("NifSkope render model is unavailable."), true);
            return;
        }
        if (!reuseGeometry) {
            _geometrySignature.clear();
            bool assemblyComplete = true;
            const QString skeletonNifPath = QStringLiteral(
                "meshes/actors/character/_1stperson/characterassets/skeleton.nif");
            if (!loadNif(referenceMeshes(), skeletonNifPath, *destination, error)) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot load first-person NIF skeleton: %1").arg(error), true);
                return;
            }
            // Model reset already schedules compilation. setNif also resets the camera.
            _window->getGLView()->update();
            const int baseSkeletonBlockCount = destination->getBlockCount();
            _canonicalNodes = collectCanonicalNodes(0, baseSkeletonBlockCount);
            if (!addSkinHelperBones(error)) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot complete the first-person skin skeleton: %1")
                              .arg(error), true);
                return;
            }

            const QString bodyPath = _bodyCombo->currentData().toString() == QStringLiteral("female")
                ? QStringLiteral("meshes/actors/character/characterassets/1stpersonfemalebody.nif")
                : QStringLiteral("meshes/actors/character/characterassets/1stpersonmalebody.nif");
            const auto body = sourceNif(referenceMeshes(), bodyPath, error);
            if (!body) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot load first-person body: %1").arg(error), true);
                return;
            }
            const int bodyFirstBlock = destination->getBlockCount();
            _bodyRoot = pasteRoot(*body, destination->getBlockIndex(0));
            if (!_bodyRoot.isValid()) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot attach the first-person body to the skeleton."), true);
                return;
            }
            _bodyRootFlags = destination->get<int>(_bodyRoot, "Flags");
            const SkinRetargetResult bodySkin = retargetSkin(
                bodyFirstBlock, destination->getBlockCount(), destination->getBlockIndex(0));
            appendDiagnostic(QStringLiteral("SKIN body canonical-bones=%1/%2")
                                 .arg(bodySkin.retargetedBones)
                                 .arg(bodySkin.totalBones));
            if (bodySkin.totalBones == 0 ||
                bodySkin.retargetedBones != bodySkin.totalBones) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot retarget every first-person body skin bone."), true);
                return;
            }

            const QString handsPath = _bodyCombo->currentData().toString() == QStringLiteral("female")
                ? QStringLiteral("meshes/actors/character/characterassets/1stpersonfemalehands.nif")
                : QStringLiteral("meshes/actors/character/characterassets/1stpersonmalehands.nif");
            const auto hands = sourceNif(referenceMeshes(), handsPath, error);
            if (!hands) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot load first-person hands: %1").arg(error), true);
                return;
            }
            const int handsFirstBlock = destination->getBlockCount();
            _handsRoot = pasteRoot(
                *hands, destination->getBlockIndex(0));
            if (!_handsRoot.isValid()) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot attach the first-person hands to the skeleton."), true);
                return;
            }
            _handsRootFlags = destination->get<int>(_handsRoot, "Flags");
            const SkinRetargetResult handsSkin = retargetSkin(
                handsFirstBlock, destination->getBlockCount(), destination->getBlockIndex(0));
            appendDiagnostic(QStringLiteral("SKIN hands canonical-bones=%1/%2")
                                 .arg(handsSkin.retargetedBones)
                                 .arg(handsSkin.totalBones));
            if (handsSkin.totalBones == 0 ||
                handsSkin.retargetedBones != handsSkin.totalBones) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot retarget every first-person hand skin bone."), true);
                return;
            }

            const QVector<const ObjectModRecord*> selected = configuredObjectMods();

            const ObjectModRecord* receiver = resolvedReceiverObjectMod();
            const int weaponBone = _skeleton.boneIndex(QStringLiteral("Weapon"));
            if (weaponBone < 0 || _skeleton.bones.at(weaponBone).parentIndex < 0) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("First-person HKX skeleton has no Weapon bone."), true);
                return;
            }
            _weaponBoneIndex = weaponBone;
            QPersistentModelIndex weaponMount = _canonicalNodes.value(
                normalizedKey(_skeleton.bones.at(weaponBone).name));
            if (!weaponMount.isValid()) {
                weaponMount = createBoneNode(weaponBone);
            }
            if (!weaponMount.isValid()) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("The NIF skeleton has no usable Weapon mount."), true);
                return;
            }

            const QString basePath = normalizeNifPath(weapon->baseModelPath);
            const auto baseModel = sourceNif(_packageResources.contains(basePath) ? _packageResources : referenceMeshes(),
                                             basePath, error);
            if (weapon->baseModelPath.isEmpty() || !baseModel) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot load weapon base model '%1': %2").arg(weapon->baseModelPath, error), true);
                return;
            }
            const int baseFirstBlock = destination->getBlockCount();
            _weaponRoot = pasteRoot(*baseModel, weaponMount);
            if (!_weaponRoot.isValid()) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot attach the weapon base model to the Weapon bone."), true);
                return;
            }
            const auto baseNodes = collectCanonicalNodes(baseFirstBlock);
            const int baseWeaponNodesReplaced = mergeWeaponModelNodes(baseNodes);

            if (receiver) {
                const auto receiverSource = sourceNif(_packageResources, receiver->modelPath, error);
                if (!receiverSource) {
                    QApplication::restoreOverrideCursor();
                    setStatus(QStringLiteral("Cannot load receiver '%1': %2")
                                  .arg(receiver->modelPath, error), true);
                    return;
                }
                NifModel& receiverModel = *receiverSource;
                ConnectTarget receiverTarget;
                QString receiverPoint;
                for (const QString& point : childConnectPoints(receiverModel)) {
                    receiverTarget = findConnectTarget(point, nullptr, configurationSlotKey(*receiver));
                    if (receiverTarget.valid()) {
                        receiverPoint = point;
                        break;
                    }
                }
                if (!receiverTarget.valid()) {
                    QApplication::restoreOverrideCursor();
                    setStatus(QStringLiteral("Receiver '%1': %2").arg(receiver->modelPath, receiverTarget.error), true);
                    return;
                }
                const int receiverFirstBlock = destination->getBlockCount();
                const QPersistentModelIndex receiverRoot = pasteRoot(
                    receiverModel, receiverTarget.parent);
                if (!receiverRoot.isValid() ||
                    !setNodeTransform(receiverRoot, Transformf{
                        receiverTarget.translation,
                        receiverTarget.rotation,
                        Vec3f{ receiverTarget.scale, receiverTarget.scale, receiverTarget.scale }
                    })) {
                    QApplication::restoreOverrideCursor();
                    setStatus(QStringLiteral("Cannot attach the receiver to the weapon base model."), true);
                    return;
                }
                const auto receiverNodes = collectCanonicalNodes(receiverFirstBlock);
                _partRoots.insert(configurationSlotKey(*receiver), receiverRoot);
                recordPartSurfaces(configurationSlotKey(*receiver), receiverFirstBlock, destination->getBlockCount());
                const int receiverWeaponNodesReplaced = mergeWeaponModelNodes(receiverNodes);
                appendDiagnostic(QStringLiteral(
                    "BIND visible weapon nodes base-replaced=%1 receiver-replaced=%2")
                                     .arg(baseWeaponNodesReplaced)
                                     .arg(receiverWeaponNodesReplaced));
                appendDiagnostic(QStringLiteral("ATTACH %1 via %2")
                                     .arg(receiver->editorId, receiverPoint));
                appendDiagnostic(QStringLiteral("RECEIVER  authority=slot configuration  model=%1").arg(receiver->modelPath));
            }

            struct PendingAttachment
            {
                const ObjectModRecord* record;
                QSharedPointer<NifModel> model;
                QString connectionError;
            };
            std::vector<PendingAttachment> pending;
            QSet<QString> providedBones;
            for (const ObjectModRecord* objectMod : selected) {
                if (!objectMod || isReceiverObjectMod(*objectMod) || objectMod->modelPath.isEmpty()) { continue; }
                auto attachment = sourceNif(_packageResources, objectMod->modelPath, error);
                if (!attachment) {
                    assemblyComplete = false;
                    appendDiagnostic(QStringLiteral("SKIP %1: %2").arg(objectMod->editorId, error));
                    continue;
                }
                for (int block = 0; block < attachment->getBlockCount(); ++block) {
                    const QModelIndex index = attachment->getBlockIndex(block);
                    if (attachment->blockInherits(index, "NiNode")) {
                        providedBones.insert(normalizedKey(attachment->get<QString>(index, "Name")));
                    }
                }
                pending.push_back({ objectMod, std::move(attachment), {} });
            }
            // Never manufacture a reference-pose substitute for a bone owned by an
            // incoming model. Preserve missing-bone support for passive attachments.
            const auto createMissingBones = [this, &providedBones]() {
                int created = 0;
                for (int bone = 0; bone < _skeleton.bones.size(); ++bone) {
                    const QString key = normalizedKey(_skeleton.bones.at(bone).name);
                    if (bone != _weaponBoneIndex && isDescendantOf(bone, _weaponBoneIndex) &&
                        !_canonicalNodes.contains(key) && !providedBones.contains(key) &&
                        createBoneNode(bone).isValid()) { ++created; }
                }
                return created;
            };
            while (!pending.empty()) {
                bool progress = false;
                for (auto it = pending.begin(); it != pending.end();) {
                    NifModel& attachment = *it->model;
                    ConnectTarget target;
                    QString usedPoint;
                    for (const QString& point : childConnectPoints(attachment)) {
                        target = findConnectTarget(point, &attachment, configurationSlotKey(*it->record));
                        if (target.valid()) { usedPoint = point; break; }
                    }
                    if (!target.valid()) { it->connectionError = target.error; ++it; continue; }
                    const int firstBlock = destination->getBlockCount();
                    const QPersistentModelIndex root = pasteRoot(attachment, target.parent);
                    if (!root.isValid()) {
                        QApplication::restoreOverrideCursor();
                        setStatus(QStringLiteral("Cannot paste attachment %1").arg(it->record->editorId), true);
                        return;
                    }
                    Transformf placement{ target.translation, target.rotation,
                                          {target.scale, target.scale, target.scale} };
                    if (target.suppliedBone >= 0) {
                        Transformf authoredRoot;
                        if (!readNodeTransform(root, authoredRoot)) {
                            QApplication::restoreOverrideCursor();
                            setStatus(QStringLiteral("Cannot read authored attachment root"), true);
                            return;
                        }
                        QModelIndex anchor = findNode(_skeleton.bones.at(target.suppliedBone).name, firstBlock);
                        Transformf anchorInMount;
                        bool reachedRoot = false;
                        for (int depth = 0; depth < destination->getBlockCount() && anchor.isValid(); ++depth) {
                            Transformf local;
                            if (!readNodeTransform(anchor, local)) { break; }
                            anchorInMount = composed(local, anchorInMount);
                            if (anchor == root) { reachedRoot = true; break; }
                            anchor = destination->getBlockIndex(destination->getParent(anchor));
                        }
                        if (!reachedRoot || !suppliedAnchorPlacement(authoredRoot, anchorInMount, placement, placement)) {
                            QApplication::restoreOverrideCursor();
                            setStatus(QStringLiteral("Cannot resolve supplied attachment anchor %1")
                                          .arg(it->record->editorId), true);
                            return;
                        }
                    }
                    if (!setNodeTransform(root, placement)) {
                        QApplication::restoreOverrideCursor();
                        setStatus(QStringLiteral("Cannot place attachment %1").arg(it->record->editorId), true);
                        return;
                    }
                    if (target.suppliedBone >= 0 && !recordPartAlignment(firstBlock, destination->getBlockCount(), target.suppliedBone, error)) {
                        QApplication::restoreOverrideCursor();
                        setStatus(QStringLiteral("Cannot align attachment %1: %2").arg(it->record->editorId, error), true);
                        return;
                    }
                    if(!remapAttachmentNodes(*it->record,firstBlock,destination->getBlockCount(),error)) {
                        QApplication::restoreOverrideCursor();setStatus("Cannot remap attachment "+it->record->editorId+": "+error,true);return;
                    }
                    (void)mergeWeaponModelNodes(collectCanonicalNodes(firstBlock));
                    _partRoots.insert(configurationSlotKey(*it->record), root);
                    recordPartSurfaces(configurationSlotKey(*it->record), firstBlock, destination->getBlockCount());
                    appendDiagnostic(QStringLiteral("ATTACH %1 via %2 · owner=%3 parent=%4 root=%5 t=(%6,%7,%8)%9")
                        .arg(it->record->editorId, usedPoint).arg(target.ownerRoot)
                        .arg(destination->getBlockNumber(target.parent)).arg(destination->getBlockNumber(root))
                        .arg(placement.translation.x, 0, 'f', 3).arg(placement.translation.y, 0, 'f', 3)
                        .arg(placement.translation.z, 0, 'f', 3)
                        .arg(target.suppliedBone >= 0 ? QStringLiteral(" — owns animated connection bone") : QString{}));
                    it = pending.erase(it);
                    progress = true;
                }
                if (!progress && createMissingBones() == 0) {
                    assemblyComplete = false;
                    for (const auto& entry : pending) {
                        appendDiagnostic(QStringLiteral("SKIP %1: %2 (%3)")
                            .arg(entry.record->editorId, entry.connectionError.isEmpty() ? QStringLiteral("No child connect points") : entry.connectionError,
                                 childConnectPoints(*entry.model).join(QStringLiteral(", "))));
                    }
                    break;
                }
            }
            (void)createMissingBones();
            if (!buildNodeBindings(error)) {
                QApplication::restoreOverrideCursor();
                setStatus(QStringLiteral("Cannot bind assembled parts: %1").arg(error), true);
                return;
            }
            if (assemblyComplete) { _geometrySignature = geometrySignature; }
            _renderedWeapon = _weaponCombo->currentIndex();
        }
        setArmsVisible(_showArmsCheck->isChecked());
        refreshSlotCallouts();
        _window->getGLView()->updateScene();
        refreshAnimations();
        QListWidgetItem* initialAnimation = _animationList->count() > 0
            ? _animationList->item(0)
            : nullptr;
        if (!initialAnimation) {
            QApplication::restoreOverrideCursor();
            setStatus(QStringLiteral("The configured graph exposes no playable animation actions."), true);
            return;
        }

        _readyAnimationPath = initialAnimation->data(Qt::UserRole).toString();
        _readyClip = cachedClip(_readyAnimationPath, error);
        if (!_readyClip.valid() ||
            !samplePose(_skeleton, _readyClip, 0.0f, nullptr, _sampledTracks,
                        _readyLocalPose, _readyGlobalPose, error)) {
            QApplication::restoreOverrideCursor();
            setStatus(QStringLiteral("Cannot establish graph-selected Ready authority: %1")
                          .arg(error.isEmpty() ? QStringLiteral("pose sampling failed") : error), true);
            return;
        }
        _stateBaselineLocalPose = _readyLocalPose;
        _stateBaselineAnimationPath = _readyAnimationPath;
        // Cache the assembled weapon in Ready before any firing/reload pose.
        // Fit never includes arms, detached magazines or parked source copies.
        selectAnimation(initialAnimation);
        cacheWeaponFitBounds();
        appendDiagnostic(QStringLiteral("BASE  Ready pose=%1 sample=0.000s")
                             .arg(_readyAnimationPath));
        if (sameWeapon && !previousAction.isEmpty()) {
            for (int row = 0; row < _animationList->count(); ++row) {
                if (_animationList->item(row)->text() == previousAction) { initialAnimation = _animationList->item(row); break; }
            }
        }
        _animationList->setCurrentItem(initialAnimation);
        selectAnimation(initialAnimation);
        refreshHandPoseAnchors();
        if (!sameWeapon) { (void)fitWeapon(); }
        _window->getGLView()->update();
        QApplication::restoreOverrideCursor();
        appendDiagnostic(QStringLiteral("CONFIGURATION %1 ms · %2 · %3 source decodes (warm data retained)")
            .arg(rebuildClock.elapsed()).arg(reuseGeometry ? QStringLiteral("scene reused") : QStringLiteral("assembled from source cache"))
            .arg(_sourceMisses));
        appendDiagnostic(QStringLiteral("  COST model decode=%1 ms branch copy=%2 ms branch paste=%3 ms")
            .arg(_decodeMilliseconds).arg(_copyMilliseconds).arg(_pasteMilliseconds));
        if (!_clip.valid()) {
            setStatus(QStringLiteral("Built %1 with %2 animated node bindings.")
                          .arg(weapon->name)
                          .arg(_nodeBindings.size()));
        }
    }

    void ReloadMakerController::selectAnimation(QListWidgetItem* item)
    {
        if (_loadThread || !item || !_skeleton.valid()) { return; }
        invalidateCollisions();
        // Internal selections (Operate, reload learning) block the list's
        // signals; keep the transport's clip picker on the loaded clip.
        if (_clipCombo) { const QSignalBlocker block(_clipCombo); _clipCombo->setCurrentIndex(_animationList->row(item)); }
        const auto poseControls = qScopeGuard([this]() { updateHandPoseControls(); });
        restoreHandPosePreview();
        // A failed request must never leave the previous clip playing under the new label.
        _playing = false;
        _playbackTimer->stop();
        _playButton->setText(QStringLiteral("PLAY"));
        _clip = {};
        _activeAction.clear();
        _timeline->setEnabled(false);
        updateTimelineText();
        refreshFingerEditor();
        const QString path = item->data(Qt::UserRole).toString();
        const QString baselinePath = item->data(kAnimationBaselineRole).toString();
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
        QVector<Transformf> baselineLocal, baselineGlobal, firstLocal, firstGlobal;
        if (!baselineClip.valid() ||
            !samplePose(_skeleton, baselineClip, 0.0f,
                        baselineClip.additive ? &_readyLocalPose : nullptr,
                        _sampledTracks, baselineLocal, baselineGlobal, error) ||
            !samplePose(_skeleton, clip, 0.0f, &baselineLocal,
                        _sampledTracks, firstLocal, firstGlobal, error)) {
            setStatus(QStringLiteral("Cannot evaluate '%1' on state '%2': %3")
                          .arg(path, baselinePath, error), true);
            return;
        }
        _stateBaselineLocalPose = std::move(baselineLocal);
        _stateBaselineAnimationPath = baselinePath;
        _clip = std::move(clip);
        if(!_simulationActive)_audio.stop();cacheSourceSounds(_clip);
        _activeAction = item->text();
        _activeAnimationPath = path;
        const auto duration = [](float value) {
            return std::isfinite(value) && value > 0.0f ? value : 0.0f;
        };
        _clipBlendInSeconds = duration(item->data(kActionBlendInRole).toFloat());
        _clipBlendOutSeconds = duration(item->data(kActionBlendOutRole).toFloat());
        _playbackTime = 0.0f;
        if (!applyPose(0.0f)) { return; }
        _timeline->setEnabled(true);
        updateTimelineText();
        refreshFingerEditor();
        appendDiagnostic(QStringLiteral("\nACTION  %1  graph clip=%2\n  SOURCE %3\n  BASE %4")
                             .arg(_activeAction, item->data(kAnimationNodeRole).toString(),
                                  path, baselinePath));
        showClipDetails();
        setStatus(QStringLiteral("Loaded %1: %2 tracks, %3 annotations, %4 s; offline clip preview")
                      .arg(_activeAction).arg(_clip.transformTrackCount)
                      .arg(_clip.annotations.size()).arg(playbackDuration(), 0, 'f', 3));
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
            _playButton->setText(QStringLiteral("PAUSE"));
        } else {
            _playbackTimer->stop();
            _playButton->setText(QStringLiteral("PLAY"));
        }
    }

    void ReloadMakerController::replay()
    {
        restoreHandPosePreview();
        if (!_clip.valid()) {
            return;
        }
        _playbackTime = 0.0f;
        if (!applyPose(_playbackTime)) { return; }
        updateTimelineText();
        refreshFingerEditor();
        if (_playing) {
            _playbackClock.restart();
        }
    }

    void ReloadMakerController::advancePlayback()
    {
        if (!_playing || !_clip.valid()) {
            return;
        }
        const qint64 elapsedNanoseconds = _playbackClock.nsecsElapsed();
        _playbackClock.restart();
        const float elapsedSeconds = static_cast<float>(elapsedNanoseconds) / 1'000'000'000.0f;
        const float soundFrom=_playbackTime;
        _playbackTime += elapsedSeconds * _playbackRate;
        const float duration = playbackDuration();
        if (_playbackTime >= duration) {
            if (_loopCheck->isChecked()) {
                _playbackTime = std::fmod(_playbackTime, duration);
            } else {
                _playbackTime = duration;
                _playing = false;
                _playbackTimer->stop();
                _playButton->setText(QStringLiteral("PLAY"));
            }
        }
        if (!applyPose(_playbackTime)) { return; }
        if(_playbackTime>=soundFrom)playSourceSounds(_clip,soundFrom,_playbackTime);
        else {playSourceSounds(_clip,soundFrom,duration);playSourceSounds(_clip,-.0001f,_playbackTime);}
        updateTimelineText();
        refreshFingerEditor();
    }

    void ReloadMakerController::scrubTimeline(const int value)
    {
        invalidateCollisions();
        restoreHandPosePreview();
        if (!_clip.valid()) {
            return;
        }
        _playbackTime = playbackDuration() *
                        static_cast<float>(value) / static_cast<float>(kTimelineMaximum);
        if (!applyPose(_playbackTime)) { return; }
        updateTimelineText();
        refreshFingerEditor();
        if (_playing) {
            _playbackClock.restart();
        }
    }

    void ReloadMakerController::capturePoseDiagnostics()
    {
        if (!_clip.valid() || _globalPose.size() != _skeleton.bones.size()) { return; }
        Scene* scene = _window->getGLView()->getScene();
        NifModel* model = activeModel();
        if (!scene || !model) { return; }
        const auto text = [](const Transformf& value) {
            return QStringLiteral("t=(%1,%2,%3) q=(%4,%5,%6,%7)")
                .arg(value.translation.x, 0, 'f', 4).arg(value.translation.y, 0, 'f', 4)
                .arg(value.translation.z, 0, 'f', 4).arg(value.rotation.x, 0, 'f', 5)
                .arg(value.rotation.y, 0, 'f', 5).arg(value.rotation.z, 0, 'f', 5)
                .arg(value.rotation.w, 0, 'f', 5);
        };
        appendDiagnostic(QStringLiteral("\nPOSE time=%1 action=%2 mode=%3 transitions=%4\n  file=%5")
                             .arg(_playbackTime, 0, 'f', 4).arg(_activeAction, _motionMode->currentText(),
                                  _transitionCheck->isChecked() ? QStringLiteral("approximate") :
                                  QStringLiteral("off"), _activeAnimationPath));
        // Read rendered-node locals/worlds, not only the desired skeleton array.
        // Flush only this view's queued render request, not arbitrary UI events.
        _window->getGLView()->update();
        QCoreApplication::sendPostedEvents(_window->getGLView(), QEvent::UpdateRequest);
        for (int bindingIndex = 0; bindingIndex < _nodeBindings.size(); ++bindingIndex) {
            const ScenePoseBinding& binding = _nodeBindings.at(bindingIndex);
            const auto& nodeIndex = _sceneNodes.at(binding.nodeIndex);
            const QString& name = _skeleton.bones.at(binding.boneIndex).name;
            if (!name.startsWith(QStringLiteral("Weapon"), Qt::CaseInsensitive) &&
                !name.endsWith(QStringLiteral("Arm_Hand"), Qt::CaseInsensitive)) { continue; }
            Node* node = scene->getNode(model, nodeIndex);
            if (!node) { continue; }
            const Transform& actual = node->worldTrans();
            const Quat q = actual.rotation.toQuat();
            const Transformf rendered{
                { actual.translation[0], actual.translation[1], actual.translation[2] },
                { q[1], q[2], q[3], q[0] },
                { actual.scale, actual.scale, actual.scale }
            };
            appendDiagnostic(QStringLiteral("  %1 block=%2 parent=%3\n    SOURCE %4\n    TARGET %5\n    RENDER %6")
                                 .arg(name).arg(model->getBlockNumber(nodeIndex))
                                 .arg(node->parentNode() ? node->parentNode()->id() : -1)
                                 .arg(text(_sourceGlobalPose.at(binding.boneIndex)),
                                      text(_boundGlobalPose.at(bindingIndex)), text(rendered)));
        }
    }

    void ReloadMakerController::updatePlaybackRate(const int index)
    {
        _playbackRate = _speedCombo->itemData(index).toFloat();
        if (!(_playbackRate > 0.0f)) {
            _playbackRate = 1.0f;
        }
    }

    void ReloadMakerController::setArmsVisible(const bool visible)
    {
        NifModel* model = activeModel();
        Scene* scene = _window ? _window->getGLView()->getScene() : nullptr;
        if (!model || !scene) {
            return;
        }

        const auto updateRoot = [model, scene, visible](
                                    const QPersistentModelIndex& root,
                                    const int originalFlags) {
            if (!root.isValid()) {
                return;
            }
            const int flags = visible
                ? originalFlags
                : originalFlags | 1;
            if (!model->set<int>(root, "Flags", flags)) {
                return;
            }
            if (Node* node = scene->getNode(model, root)) {
                node->update(model, root);
            }
        };

        const QSignalBlocker signalBlocker(model);
        updateRoot(_bodyRoot, _bodyRootFlags);
        updateRoot(_handsRoot, _handsRootFlags);
        _window->getGLView()->update();
        appendDiagnostic(visible
            ? QStringLiteral("VIEW  arms and hands visible")
            : QStringLiteral("VIEW  arms and hands hidden; weapon animation remains active"));
    }

    float ReloadMakerController::playbackDuration() const noexcept
    {
        return _clip.valid()
            ? _clip.durationSeconds + (_transitionCheck->isChecked() ? _clipBlendOutSeconds : 0.0f)
            : 0.0f;
    }

    bool ReloadMakerController::sampleActionPose(
        const float timeSeconds, QVector<Transformf>& localPose,
        QVector<Transformf>& globalPose)
    {
        if (!_clip.valid()) {
            return false;
        }
        const float clipTime = std::clamp(
            timeSeconds, 0.0f, _clip.durationSeconds);
        if (!samplePose(_skeleton, _clip, clipTime, &_stateBaselineLocalPose,
                        _sampledTracks, localPose, globalPose, _poseError)) {
            return false;
        }
        if (_stateBaselineLocalPose.size() != localPose.size()) {
            return false;
        }

        // Learned reloads and the interactive simulation use exact source poses; only
        // free clip preview applies the approximate entry/exit blend.
        const float actionWeight = (_reloadWorkspaceActive || _simulationActive) ? 1.0f : actionBlendWeight(timeSeconds);
        if (actionWeight < 1.0f) {
            for (qsizetype bone = 0; bone < localPose.size(); ++bone) {
                localPose[bone] = interpolatedTransform(
                    _stateBaselineLocalPose.at(bone), localPose.at(bone),
                    actionWeight);
            }
        }
        return actionWeight >= 1.0f || buildPoseGlobals(_skeleton, localPose, globalPose, _poseError);
    }

    float ReloadMakerController::actionBlendWeight(
        const float timeSeconds) const noexcept
    {
        if (!_transitionCheck->isChecked()) { return 1.0f; }
        if (_clipBlendInSeconds > 0.0f &&
            timeSeconds < _clipBlendInSeconds) {
            const float progress = std::clamp(
                timeSeconds / _clipBlendInSeconds, 0.0f, 1.0f);
            return progress * progress * (3.0f - 2.0f * progress);
        }
        if (_clipBlendOutSeconds > 0.0f &&
            timeSeconds > _clip.durationSeconds) {
            const float progress = std::clamp(
                (timeSeconds - _clip.durationSeconds) /
                    _clipBlendOutSeconds,
                0.0f, 1.0f);
            const float smoothProgress = progress * progress *
                (3.0f - 2.0f * progress);
            return 1.0f - smoothProgress;
        }
        return 1.0f;
    }

    bool ReloadMakerController::isDescendantOf(
        const int boneIndex, const int ancestorIndex) const
    {
        if (boneIndex < 0 || boneIndex >= _skeleton.bones.size() ||
            ancestorIndex < 0 || ancestorIndex >= _skeleton.bones.size()) {
            return false;
        }

        int current = boneIndex;
        for (int depth = 0; depth < _skeleton.bones.size() && current >= 0; ++depth) {
            if (current == ancestorIndex) {
                return true;
            }
            current = _skeleton.bones.at(current).parentIndex;
        }
        return false;
    }

    bool ReloadMakerController::applyPose(const float timeSeconds)
    {
        NifModel* model = activeModel();
        Scene* scene = _window->getGLView()->getScene();
        const auto fail = [this]() {
            stopMechanismPreview();
            _playing = false;
            _playbackTimer->stop();
            _playButton->setText(QStringLiteral("PLAY"));
            _timeline->setEnabled(false);
            _clip = {};
            _handPosePreview.reset();
            _partGripDraft.reset();
            _gripPreviewValid = false;
            updateHandPoseControls();
            setStatus(QStringLiteral("Pose evaluation stopped: %1").arg(_poseError), true);
            return false;
        };
        _poseError = QStringLiteral("Missing model, skeleton, scene or binding");
        if (!model || !scene || !_skeleton.valid() || _nodeBindings.isEmpty() ||
            !sampleActionPose(timeSeconds, _sourceLocalPose, _sourceGlobalPose)) { return fail(); }

        _localPose = _sourceLocalPose;
        const bool editingFingers = _workspace == 2 && _handPosePages->currentIndex() == 0;
        if (editingFingers && !_mechanismPreview) for (const auto& draft : _fingerDrafts) {
            if (draft && !applyHandPoseFingers(_skeleton, *draft, _localPose, _poseError)) return fail();
        }
        // Preview is a presentation override only. Preserve the native clip
        // and weapon-part inputs so restoring/scrubbing is non-destructive.
        if (editingFingers && _handPosePreview && !_mechanismPreview && !applyHandPoseFingers(_skeleton, *_handPosePreview, _localPose, _poseError)) {
            return fail();
        }
        if (!buildPoseGlobals(_skeleton, _localPose, _globalPose, _poseError)) { return fail(); }
        // Fixed mode removes this frame's complete Weapon world transform from
        // every branch. Source mode removes only the Ready world frame: Weapon
        // is allowed to translate/rotate, with the very same local articulation.
        // Reload authoring must honor the fixed Weapon frame for hands and
        // internal parts alike. Firing retains its native recoil presentation.
        const bool simulationFire = _simulationActive && _simulation.action == SimulationAction::Fire;
        const Transformf frame = _motionMode->currentIndex() == 0 && !simulationFire
            ? _globalPose.at(_weaponBoneIndex) : _readyGlobalPose.at(_weaponBoneIndex);
        if (!reframePose(_skeleton, frame, Transformf{}, _localPose, _globalPose, _poseError)) {
            return fail();
        }
        if (!evaluateSceneBindings(_skeleton, _nodeBindings, _sourceLocalPose, _stateBaselineLocalPose,
                                   _globalPose, _boundLocalPose, _boundGlobalPose, _poseError)) {
            return fail();
        }
        if (_mechanismPreview && !applyMechanismPreview()) { return fail(); }
        if (!applySimulationPose()) { return fail(); }
        if (!applyOperatingGripPose()) { return fail(); }
        if (!applyMagazineGrabPose()) { return fail(); }
        const bool gripWorkspace = (_workspace == 2 && _handPosePages->currentIndex() == 1) || mechanismPageActive() || (_reloadWorkspaceActive && _reloadHeld);
        const bool showGrip=gripWorkspace && _partGripDraft && (!_mechanismPreview || ((_mechanismTeaching||(_mechanismHeld&&_mechanismPoseOnGrab->isChecked())) && _mechanismDraft && sameMechanismPart(_partGripDraft->pose.placement,_mechanismDraft->anchor)));
        if (showGrip && (!_reloadWorkspaceActive||_reloadHeld) && !applyPartGripPreview()) { return fail(); }
        if (_mechanismPreview && showGrip && !applyMechanismPreview()) { return fail(); }
        if (_mechanismRestorePending && !_mechanismPreview) {
            MechanismScenePlan native;
            if (!compileMechanismScene(_assemblyNodes,_nodeBindings,{},native,_poseError) ||
                !nativeMechanismScene(native,_nodeBindings,_boundLocalPose,_mechanismLocal,_mechanismWorld,_poseError)) { return fail(); }
        }
        const QSignalBlocker signalBlocker(model);
        if (_mechanismPreview || _mechanismRestorePending) {
            for (int i=0;i<_sceneNodes.size();++i) {
                if (!_sceneNodes[i].isValid() || !setNodeTransform(_sceneNodes[i],_mechanismLocal[i])) {
                    _poseError=QStringLiteral("Cannot write mechanism scene node %1").arg(i);return fail();
                }
                if (Node* node=scene->getNode(model,_sceneNodes[i])) node->update(model,_sceneNodes[i]);
            }
            _mechanismRestorePending=false;
            _window->getGLView()->update();return true;
        }
        for (int bindingIndex = 0; bindingIndex < _nodeBindings.size(); ++bindingIndex) {
            const ScenePoseBinding& binding = _nodeBindings.at(bindingIndex);
            const auto& nodeIndex = _sceneNodes.at(binding.nodeIndex);
            if (!nodeIndex.isValid() ||
                !setNodeTransform(nodeIndex, _boundLocalPose.at(bindingIndex))) {
                _poseError = QStringLiteral("Cannot write bound scene node for bone %1").arg(binding.boneIndex);
                return fail();
            }
            if (Node* node = scene->getNode(model, nodeIndex)) { node->update(model, nodeIndex); }
        }
        applySimulationVisibility();
        _window->getGLView()->update();
        return true;
    }

    void ReloadMakerController::updateTimelineText()
    {
        if (!_clip.valid()) {
            _timeLabel->setText(QStringLiteral("0.000 / 0.000 s"));
            return;
        }
        const QSignalBlocker blocker(_timeline);
        const float duration = playbackDuration();
        _timeline->setValue(static_cast<int>(std::round(
            static_cast<double>(_playbackTime / duration) * kTimelineMaximum)));
        _timeLabel->setText(QStringLiteral("%1 / %2 s")
                                .arg(_playbackTime, 0, 'f', 3)
                                .arg(duration, 0, 'f', 3));
    }

    void ReloadMakerController::showClipDetails()
    {
        if (!_clip.valid()) {
            return;
        }
        appendDiagnostic(QStringLiteral("\nCLIP  duration=%1s frames=%2 tracks=%3 type=%4 skeleton=%5")
                             .arg(_clip.durationSeconds, 0, 'f', 3)
                             .arg(_clip.frameCount)
                             .arg(_clip.transformTrackCount)
                             .arg(_clip.additive ? QStringLiteral("additive") : QStringLiteral("normal"))
                             .arg(_clip.skeletonName));
        if (_clipBlendInSeconds > 0.0f) {
            appendDiagnostic(QStringLiteral(
                "  ENTRY approximate blend=%1s")
                                 .arg(_clipBlendInSeconds, 0, 'f', 3));
        }
        if (_clipBlendOutSeconds > 0.0f) {
            appendDiagnostic(QStringLiteral(
                "  EXIT  approximate blend=%1s total=%2s")
                                 .arg(_clipBlendOutSeconds, 0, 'f', 3)
                                 .arg(playbackDuration(), 0, 'f', 3));
        }
        appendDiagnostic(QStringLiteral(
            "  AUTHORITY %1; native hands/fingers; common pose-frame conversion\n"
            "  GRAPH clip selection only; masks/modifiers/conditions are not evaluated\n"
            "  TRANSITIONS %2")
                             .arg(_motionMode->currentText(),
                                  _transitionCheck->isChecked() ? QStringLiteral("approximate state blend") :
                                  QStringLiteral("off — authored clip timing")));
        for (const HkxAnnotation& annotation : _clip.annotations) {
            appendDiagnostic(QStringLiteral("  %1  %2  %3")
                                 .arg(annotation.timeSeconds, 7, 'f', 3)
                                 .arg(annotation.trackName, -24)
                                 .arg(annotation.text));
        }
    }

    void ReloadMakerController::setStatus(const QString& message, const bool error)
    {
        _lastError = error ? message : QString();
        _window->statusBar()->setStyleSheet(error
            ? QStringLiteral("color:#ff8a80")
            : QStringLiteral("color:#aeb4ba"));
        _window->statusBar()->showMessage(message);
        if (error) {
            if(_workshopHint&&_handPosePages&&_handPosePages->currentIndex()==2)_workshopHint->setText(message);
            appendDiagnostic(QStringLiteral("ERROR  %1").arg(message));
        }
    }

    void ReloadMakerController::appendDiagnostic(const QString& message)
    {
        _details->appendPlainText(message);
    }
}
