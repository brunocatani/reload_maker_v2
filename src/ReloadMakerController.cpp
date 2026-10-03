#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "ReloadTemplates.h"
#include "PoseEvaluator.h"
#include "SlotOverlay.h"
#include "viewport/PartPicker.h"
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
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QScopeGuard>
#include <QScopedValueRollback>
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

        [[nodiscard]] QString formIdText(const std::uint32_t value)
        {
            return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
        }
    }

    ReloadMakerController::ReloadMakerController(StudioWindow* window)
        : QObject(window), _window(window), _playbackTimer(new QTimer(this))
    {
        _requestedMo2Profile=QCoreApplication::instance()->property("requestedMo2Profile").toString();
        if(insideMo2VirtualFilesystem())_launchSnapshot=readLoadOrderSnapshot(
            QCoreApplication::instance()->property("launchMo2Instance").toString(),
            QCoreApplication::instance()->property("launchGameData").toString(),_requestedMo2Profile);
        _rebuildTimer = new QTimer(this);
        _rebuildTimer->setSingleShot(true);
        _rebuildTimer->setInterval(60);
        connect(_rebuildTimer, &QTimer::timeout, this, &ReloadMakerController::rebuildWeapon);
        createUi();
        // Qt runs the most recently installed event filter first. The picker is
        // installed before the slot labels so a click on a label stays the
        // label's; the picker only sees clicks that reach the model.
        _partPicker = new PartPicker(_window->getGLView(), this);
        _partPicker->selected = [this](SlotKey slot) { focusSlot(slot); };
        _partPicker->activated = [this](SlotKey slot) { showLoadoutChoices(slot); };
        _partPicker->contextRequested = [this](SlotKey slot, QPoint position) { showPartMenu(slot, position); };
        _partPicker->describe = [this](SlotKey slot) { return describePart(slot); };
        _slotOverlay = new SlotOverlay(_window->getGLView(), this);
        _slotOverlay->selected = [this](SlotKey slot) { focusSlot(slot); };
        createGripViewport();
        createReloadViewport();
        createSimulationViewport();
        applyStyle();
        setWorkspace(OperateWorkspace);
        startCommandServer();
        _window->prepareClose = [this] { if(_packProcess){setStatus("Finish or cancel the pack build before closing.",true);return false;}return saveSession(); };
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

    void ReloadMakerController::refreshAttachments()
    {
        // One button-like row per slot. Its choices live in the slot dialog.
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
                const auto* installed=config->mod(selected);
                slotRow->setData(kSelectionRole,installed?partName(*installed):QStringLiteral("Empty"));
                slotRow->setFlags(enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
                slotRow->setToolTip(enabled?QStringLiteral("Choose %1").arg(slot.label):QStringLiteral("Install the required parent part to use this slot."));
                if (slot.unverified) { slotRow->setToolTip(QStringLiteral("Partial slot/association metadata; compatibility needs review.")); }
            }
        }
        _partList->verticalScrollBar()->setValue(scroll);
        const auto* plugin = currentPlugin();
        _configurationSummary->setText(QStringLiteral("%1 · %2 parts installed")
            .arg(plugin ? (_libraryMode ? plugin->origins.value(weapon->formId).plugin : plugin->fileName) : QString())
            .arg(config->activeMods().size()));
        if (_slotOverlay) { _slotOverlay->setSelected(_focusedSlot); }
    }

    void ReloadMakerController::showLoadoutChoices(SlotKey key)
    {
        if(_loadThread||_reloadCapturing||_rebuildTimer->isActive())return;
        const auto* config=configuration();const auto* slot=config?config->slot(key):nullptr;
        if(!slot||!config->availableSlots().contains(key))return;
        const auto weapon=selectionRecordKey(_selectedWeapon);const auto source=_browserSourceKey;
        _focusedSlot=key;if(_slotOverlay)_slotOverlay->setSelected(key);
        QDialog dialog(_window);dialog.setObjectName("LoadoutChoices");dialog.setWindowTitle("Choose "+slot->label);dialog.resize(560,520);
        auto* layout=new QVBoxLayout(&dialog);auto* title=new QLabel(slot->label,&dialog);title->setObjectName("Section");layout->addWidget(title);
        auto* search=new QLineEdit(&dialog);search->setPlaceholderText("Filter "+slot->label.toLower()+" options…");search->setClearButtonEnabled(true);layout->addWidget(search);
        auto* choices=new QListWidget(&dialog);choices->setObjectName("LoadoutOptions");choices->setItemDelegate(new LoadoutDelegate(choices));layout->addWidget(choices,1);
        const auto add=[&](std::uint32_t id,const QString& text,const QString& tip){
            auto* item=new QListWidgetItem(text,choices);item->setData(kFormIdRole,id);item->setData(kInstalledRole,id==config->selected(key));item->setToolTip(tip);
            if(id==config->selected(key))choices->setCurrentItem(item);
        };
        add(0,"Empty","Remove this part. Dependent slots become unavailable; their choices are kept.");
        for(auto id:slot->choices)if(const auto* mod=config->mod(id))add(id,cleanPartName(mod->name.isEmpty()?mod->editorId:mod->name),
            QString("%1\n%2\n%3\n%4").arg(mod->editorId,formIdText(id),mod->modelPath,mod->metadataWarning));
        auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);buttons->button(QDialogButtonBox::Ok)->setText("Use selected");layout->addWidget(buttons);
        connect(search,&QLineEdit::textChanged,&dialog,[choices,buttons](const QString& query){
            for(int i=0;i<choices->count();++i){auto* item=choices->item(i);item->setHidden(!item->text().contains(query,Qt::CaseInsensitive)&&!item->toolTip().contains(query,Qt::CaseInsensitive));}
            buttons->button(QDialogButtonBox::Ok)->setEnabled(choices->currentItem()&&!choices->currentItem()->isHidden());
        });
        connect(choices,&QListWidget::currentItemChanged,&dialog,[buttons](QListWidgetItem* current){buttons->button(QDialogButtonBox::Ok)->setEnabled(current&&!current->isHidden());});
        connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        connect(choices,&QListWidget::itemDoubleClicked,&dialog,&QDialog::accept);
        if(dialog.exec()!=QDialog::Accepted||!choices->currentItem()||choices->currentItem()->isHidden())return;
        auto* current=configuration();
        if(source!=_browserSourceKey||weapon!=selectionRecordKey(_selectedWeapon)||!current||!current->availableSlots().contains(key))return;
        const auto id=choices->currentItem()->data(kFormIdRole).toUInt();
        if(current->selected(key)==id)return;
        if(current->select(key,id)){refreshAttachments();scheduleRebuild();++_revision;}
    }

    // Shared by the slot labels, clicks on the model and the focus_slot command.
    void ReloadMakerController::focusSlot(SlotKey key)
    {
        _focusedSlot = key;
        _partSearch->clear();
        refreshAttachments();
        scrollToFocusedSlot();
    }

    void ReloadMakerController::showPartMenu(SlotKey key, QPoint globalPosition)
    {
        const auto* config = configuration();
        const auto* slot = config ? config->slot(key) : nullptr;
        if (!slot) {
            return;
        }
        focusSlot(key);
        QMenu menu(_window);
        auto* title = menu.addAction(describePart(key).section('\n', 0, 0));
        title->setEnabled(false);
        menu.addSeparator();
        auto* change = menu.addAction(QStringLiteral("Change %1…").arg(slot->label.toLower()));
        // Same rule as showLoadoutChoices: no loadout edits while the scene is rebuilding.
        change->setEnabled(!_loadThread && !_reloadCapturing && !_rebuildTimer->isActive());
        if (menu.exec(globalPosition) == change) {
            showLoadoutChoices(key);
        }
    }

    QString ReloadMakerController::describePart(SlotKey key) const
    {
        const auto* config = configuration();
        const auto* slot = config ? config->slot(key) : nullptr;
        if (!slot) {
            return {};
        }
        const auto* mod = config->mod(config->selected(key));
        const QString part = mod ? cleanPartName(mod->name.isEmpty() ? mod->editorId : mod->name) : QStringLiteral("Empty");
        return QStringLiteral("%1: %2\nDouble-click to change").arg(slot->label, part);
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

    // The renderer identifies each shape by its NIF block number.
    void ReloadMakerController::refreshPartPicking()
    {
        auto* model = activeModel();
        QVector<PartSurfaces> parts;
        for (auto it = _partSurfaces.cbegin(); model && it != _partSurfaces.cend(); ++it) {
            PartSurfaces part{ it.key(), {} };
            for (const auto& surface : it.value()) {
                if (surface.isValid() && surface.model() == model) {
                    part.blocks.append(model->getBlockNumber(surface));
                }
            }
            parts.append(std::move(part));
        }
        _partPicker->setParts(std::move(parts));
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
        setLibraryResources(_libraryMode||!_package.plugins.isEmpty() ? &_packageResources : nullptr);
        if (_gameData.isEmpty()) {
            return;
        }
        QStringList paths;
        if (!_libraryMode && _package.resourceData.isEmpty() && !_package.rootPath.isEmpty()) {
            paths.append(_package.rootPath);
        }
        if (!_libraryMode&&_package.resourceData.isEmpty()) { paths.append(_gameData); }
        Game::GameManager::close_resources();
        Game::GameManager::update_folders(Game::FALLOUT_4, paths);
        Game::GameManager::update_status(Game::FALLOUT_4, true);
        Game::GameManager::update_other_games_fallback(false);
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
        const auto read=[&](const ResourceArchive& archive){
            auto bytes=archive.read(resourcePath,error);const auto key=ResourceArchive::normalizeResourcePath(resourcePath);
            if(error.isEmpty()&&!bytes.isEmpty()&&!_animationReadHashes.contains(key))
                _animationReadHashes.insert(key,QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()));
            return bytes;
        };
        if (_libraryMode||!_package.resourceData.isEmpty()) { return read(_packageResources); }
        if (_packageResources.contains(resourcePath)) {
            return read(_packageResources);
        }
        if (_gameAnimations.contains(resourcePath)) {
            return read(_gameAnimations);
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
            if (_packageResources.contains(candidate) || (!_libraryMode&&_package.resourceData.isEmpty()&&_gameAnimations.contains(candidate))) {
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
        const auto* subgraph = plugin->resolveFirstPersonSubgraph(keywords);
        const QString subgraphAuthority = QStringLiteral("automatic-keywords");
        if (!subgraph) {
            error = QStringLiteral("The equipped weapon and OMOD keywords match no first-person animation subgraph.");
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
        const QScopedValueRollback<bool> viewOnly(_viewRefresh, true);
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
                             .arg(_playbackTime, 0, 'f', 4).arg(_activeAction, motionModeName(),
                                  _approximateTransitions ? QStringLiteral("approximate") :
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
            ? _clip.durationSeconds + (_approximateTransitions ? _clipBlendOutSeconds : 0.0f)
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
        if (!_approximateTransitions) { return 1.0f; }
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
        if(_simulationActive&&_simulation.action==SimulationAction::Held) {
            const auto& fingers=_operatingFingers[int(_interactionHand)];
            if(fingers&&!applyHandPoseFingers(_skeleton,*fingers,_localPose,_poseError))return fail();
        }
        if (!buildPoseGlobals(_skeleton, _localPose, _globalPose, _poseError)) { return fail(); }
        // Fixed mode removes this frame's complete Weapon world transform from
        // every branch. Source mode removes only the Ready world frame: Weapon
        // is allowed to translate/rotate, with the very same local articulation.
        // Reload authoring must honor the fixed Weapon frame for hands and
        // internal parts alike. Firing retains its native recoil presentation.
        const bool simulationFire = _simulationActive && _simulation.action == SimulationAction::Fire;
        const Transformf frame = _weaponFixedMotion && !simulationFire
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
                             .arg(motionModeName(),
                                  _approximateTransitions ? QStringLiteral("approximate state blend") :
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
