#include "ReloadMakerController.h"
#include "InstallationSetup.h"
#include "ReloadTemplates.h"
#include "PoseEvaluator.h"
#include "SlotOverlay.h"
#include "viewport/PartPicker.h"
#include "LibraryResources.h"
#include "LoadoutView.h"
#include "areas/operate/PlaybackTimeline.h"

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
        constexpr int kPlaybackIntervalMilliseconds = 16;
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

    // Shows the catalog in the clip list. A new catalog has nothing selected.
    void ReloadMakerController::refreshAnimations()
    {
        _selectedAction = -1;
        _animationList->clear();
        for (const AnimationAction& action : _animationActions) {
            auto* item = new QListWidgetItem(action.label, _animationList);
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
            if(_workshopHint&&_handEditorPage==HandEditorPage::Mechanism)_workshopHint->setText(message);
            appendDiagnostic(QStringLiteral("ERROR  %1").arg(message));
        }
    }

    // The log keeps as many lines as the details dock shows.
    void ReloadMakerController::appendDiagnostic(const QString& message)
    {
        _diagnosticLines += message.split(QLatin1Char('\n'));
        if (_diagnosticLines.size() > kMaximumDiagnosticLines) {
            _diagnosticLines.erase(_diagnosticLines.begin(), _diagnosticLines.end() - kMaximumDiagnosticLines);
        }
        _details->appendPlainText(message);
    }

    void ReloadMakerController::clearDiagnostics()
    {
        _diagnosticLines.clear();
        _details->clear();
    }
}
