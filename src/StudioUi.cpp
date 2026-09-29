#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "SlotOverlay.h"
#include "glview.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QJsonArray>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace reloadmaker {
namespace {
constexpr int kFormIdRole = Qt::UserRole + 1;
constexpr int kTimelineMaximum = 100000;
QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setObjectName("Hint");
    return label;
}
QDockWidget* dock(StudioWindow* window, const QString& title, Qt::DockWidgetArea area) {
    auto* result = new QDockWidget(title, window);
    result->setObjectName(title);
    result->setFeatures(QDockWidget::NoDockWidgetFeatures);
    result->setAllowedAreas(area);
    window->addDockWidget(area, result);
    return result;
}
}

void ReloadMakerController::createUi() {
    auto* header = new QToolBar("Workflow", _window);
    header->setMovable(false);
    header->setObjectName("WorkflowHeader");
    _window->addToolBar(Qt::TopToolBarArea, header);
    auto* brand = new QLabel("RELOAD <span style='color:#7f9699'>/ V2</span>", header);
    brand->setObjectName("Brand");
    header->addWidget(brand);
    _workspaceButtons = new QButtonGroup(this);
    const QStringList workspaces{"Setup", "Preview", "Hands & grips", "Mechanisms", "Reload", "Simulate"};
    for (int i = 0; i < workspaces.size(); ++i) {
        auto* button = new QPushButton(workspaces[i], header);
        button->setCheckable(true);
        button->setObjectName("WorkspaceButton");
        _workspaceButtons->addButton(button, i);
        header->addWidget(button);
    }
    auto* spacer = new QWidget(header);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(spacer);
    auto* openProject = new QPushButton("Open project", header);
    auto* saveProject = new QToolButton(header);
    saveProject->setText("Save project");
    saveProject->setPopupMode(QToolButton::MenuButtonPopup);
    auto* saveMenu = new QMenu(saveProject);
    auto* saveAs = saveMenu->addAction("Save project as…");
    saveAs->setShortcut(QKeySequence::SaveAs);
    saveProject->setMenu(saveMenu);
    connect(saveAs,&QAction::triggered,this,[this] {
        const auto path=QFileDialog::getSaveFileName(_window,"Save Reload Maker project",studioDataPath("workspace/reload.project.json"),"Reload Maker project (*.json)");
        if(!path.isEmpty())saveSession(path);
    });
    auto* importRecords = new QPushButton("Import", header);
    auto* exportRecords = new QToolButton(header);
    exportRecords->setText("Export");
    exportRecords->setPopupMode(QToolButton::InstantPopup);
    auto* exportMenu = new QMenu(exportRecords);
    for(const auto& pair : {std::pair{"Saved hand pose","hand"},std::pair{"Current part grip","grip"},std::pair{"Current mechanism","mechanism"},std::pair{"Current reload recipe","reload"},std::pair{"Reload + grips (self-contained)","reload_bundle"},std::pair{"Current slide for DIAMOND","slide"}}) {
        connect(exportMenu->addAction(pair.first),&QAction::triggered,this,[this,kind=QString(pair.second)] {
            try {const auto result=executeCommand({{"method","export"},{"params",QJsonObject{{"kind",kind}}}});setStatus("Exported · "+result["path"].toString());}
            catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
        });
    }
    exportRecords->setMenu(exportMenu);
    auto* diagnostics = new QPushButton("Diagnostics", header);
    diagnostics->setCheckable(true);
    for (QWidget* b : std::initializer_list<QWidget*>{openProject, saveProject, importRecords, diagnostics}) header->addWidget(b);
    header->addWidget(exportRecords);
    connect(importRecords,&QPushButton::clicked,this,[this] {
        const auto paths=QFileDialog::getOpenFileNames(_window,"Import existing poses, grips, mechanisms or reload recipes",studioDataPath("workspace"),"Authoring records (*.json)");
        if(paths.isEmpty())return;
        QJsonArray files;for(const auto& path:paths)files.append(path);
        try {const auto result=executeCommand({{"method","import_records"},{"params",QJsonObject{{"paths",files}}}});setStatus(QString("Imported %1 authored records; originals were preserved.").arg(result["imported"].toInt()));}
        catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });

    _packageDock = dock(_window, "SOURCE & CONFIGURATION", Qt::LeftDockWidgetArea);
    _packageDock->setMinimumWidth(310);
    _packageDock->setMaximumWidth(440);
    auto* packagePanel = new QWidget(_packageDock);
    auto* packageLayout = new QVBoxLayout(packagePanel);
    packageLayout->setContentsMargins(12, 12, 12, 12);
    packageLayout->setSpacing(10);
    _packageGroup = new QGroupBox("Mod source", packagePanel);
    auto* sourceLayout = new QVBoxLayout(_packageGroup);
    _packagePath = new QLineEdit(_packageGroup);
    _packagePath->setPlaceholderText("Folder or ZIP containing a weapon mod");
    sourceLayout->addWidget(_packagePath);
    auto* sourceButtons = new QHBoxLayout;
    auto* folderButton = new QPushButton("Open folder", _packageGroup);
    auto* zipButton = new QPushButton("Open ZIP", _packageGroup);
    auto* libraryButton = new QPushButton("Installed library", _packageGroup);
    for (auto* b : {folderButton, zipButton, libraryButton}) sourceButtons->addWidget(b);
    sourceLayout->addLayout(sourceButtons);
    packageLayout->addWidget(_packageGroup);
    _referenceGroup = new QGroupBox("Reference game data", packagePanel);
    auto* gameLayout = new QHBoxLayout(_referenceGroup);
    _gameDataPath = new QLineEdit(_referenceGroup);
    _gameDataPath->setPlaceholderText("Fallout 4 VR / Data");
    auto* gameButton = new QPushButton("Browse", _referenceGroup);
    gameLayout->addWidget(_gameDataPath, 1);
    gameLayout->addWidget(gameButton);
    packageLayout->addWidget(_referenceGroup);
    auto* weaponForm = new QFormLayout;
    _weaponCombo = new QComboBox(packagePanel);
    _weaponCombo->setMinimumContentsLength(16);
    _weaponCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    weaponForm->addRow("Weapon", _weaponCombo);
    packageLayout->addLayout(weaponForm);
    _pages = new QTabWidget(packagePanel);
    _pages->tabBar()->hide();
    packageLayout->addWidget(_pages, 1);

    auto* setup = new QWidget(_pages);
    auto* setupLayout = new QVBoxLayout(setup);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    _configurationSummary = hint("Open a mod to assemble its weapon and attachments.", setup);
    setupLayout->addWidget(_configurationSummary);
    auto* slotSplitter = new QSplitter(Qt::Vertical, setup);
    _slotTree = new QTreeWidget(slotSplitter);
    _slotTree->setHeaderLabels({"Attachment slot", "Installed part"});
    _slotTree->setIndentation(14);
    _slotTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _slotTree->header()->setStretchLastSection(true);
    auto* choices = new QWidget(slotSplitter);
    auto* choicesLayout = new QVBoxLayout(choices);
    choicesLayout->setContentsMargins(0, 8, 0, 0);
    _slotDescription = hint("Select a slot to see compatible parts.", choices);
    _childSlotCombo = new QComboBox(choices);
    _partSearch = new QLineEdit(choices);
    _partSearch->setPlaceholderText("Filter compatible parts…");
    _partSearch->setClearButtonEnabled(true);
    _partList = new QListWidget(choices);
    for (QWidget* w : std::initializer_list<QWidget*>{static_cast<QWidget*>(_slotDescription), _childSlotCombo, _partSearch, _partList}) choicesLayout->addWidget(w);
    slotSplitter->setStretchFactor(0, 3);
    slotSplitter->setStretchFactor(1, 2);
    setupLayout->addWidget(slotSplitter, 1);
    _pages->addTab(setup, "Configuration");

    auto* preview = new QWidget(_pages);
    auto* previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    auto* animationForm = new QFormLayout;
    _bodyCombo = new QComboBox(preview);
    _bodyCombo->addItem("Male arms", "male");
    _bodyCombo->addItem("Female arms", "female");
    _animationSetCombo = new QComboBox(preview);
    _animationSetCombo->setMinimumContentsLength(18);
    _animationSetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _animationSetCombo->setMaxVisibleItems(24);
    animationForm->addRow("Body", _bodyCombo);
    animationForm->addRow("Animation set", _animationSetCombo);
    previewLayout->addLayout(animationForm);
    previewLayout->addWidget(hint("Choose an action. Pause at any frame to capture a hand or start authoring a part.", preview));
    _animationList = new QListWidget(preview);
    previewLayout->addWidget(_animationList, 1);
    _pages->addTab(preview, "Actions");
    createLibraryPage();
    _loadingLabel = hint({}, packagePanel);
    _loadingProgress = new QProgressBar(packagePanel);
    _cancelLoading = new QPushButton("Cancel load", packagePanel);
    for (QWidget* w : std::initializer_list<QWidget*>{static_cast<QWidget*>(_loadingLabel), _loadingProgress, _cancelLoading}) { packageLayout->addWidget(w); w->hide(); }
    connect(_cancelLoading, &QPushButton::clicked, this, [this] {
        if (_loadThread) _loadThread->requestInterruption();
        _cancelLoading->setEnabled(false);
        _loadingLabel->setText("Cancelling; keeping the previous source…");
    });
    auto* reloadButton = _reloadSource = new QPushButton("Refresh source files", packagePanel);
    packageLayout->addWidget(reloadButton);
    _packageDock->setWidget(packagePanel);

    _detailsDock = dock(_window, "DIAGNOSTICS", Qt::BottomDockWidgetArea);
    _details = new QPlainTextEdit(_detailsDock);
    _details->setReadOnly(true);
    _details->setMaximumBlockCount(3000);
    _details->setMaximumHeight(165);
    _detailsDock->setWidget(_details);
    _detailsDock->hide();

    _timelineDock = dock(_window, "PLAYBACK", Qt::BottomDockWidgetArea);
    _timelineDock->setMaximumHeight(108);
    _timelineDock->setTitleBarWidget(new QWidget(_timelineDock));
    auto* timelinePanel = new QWidget(_timelineDock);
    auto* timelineLayout = new QVBoxLayout(timelinePanel);
    timelineLayout->setContentsMargins(12, 8, 12, 10);
    auto* transport = new QHBoxLayout;
    _playButton = new QPushButton("Play", timelinePanel);
    _playButton->setObjectName("PrimaryButton");
    auto* replayButton = new QPushButton("Restart", timelinePanel);
    _loopCheck = new QCheckBox("Loop", timelinePanel);
    _loopCheck->setChecked(true);
    _speedCombo = new QComboBox(timelinePanel);
    for (float rate : {.25f, .5f, 1.f, 1.5f, 2.f}) _speedCombo->addItem(QString::number(rate)+"×", rate);
    _speedCombo->setCurrentIndex(2);
    _timeline = new QSlider(Qt::Horizontal, timelinePanel);
    _timeline->setRange(0, kTimelineMaximum);
    _timeline->setEnabled(false);
    _timeLabel = new QLabel("0.000 / 0.000 s", timelinePanel);
    transport->addWidget(_playButton); transport->addWidget(replayButton); transport->addWidget(_loopCheck); transport->addWidget(_speedCombo);
    transport->addWidget(_timeline, 1); transport->addWidget(_timeLabel);
    timelineLayout->addLayout(transport);
    auto* viewOptions = new QHBoxLayout;
    _motionMode = new QComboBox(timelinePanel);
    _motionMode->addItems({"Weapon fixed", "Full native motion"});
    _transitionCheck = new QCheckBox("Approximate entry / exit blends", timelinePanel);
    _transitionCheck->setChecked(true);
    _transitionCheck->setToolTip("Graph-derived transition preview; not full Havok behavior emulation.");
    _showArmsCheck = new QCheckBox("Show arms & hands", timelinePanel);
    _showArmsCheck->setChecked(true);
    viewOptions->addWidget(_motionMode); viewOptions->addWidget(_transitionCheck); viewOptions->addWidget(_showArmsCheck); viewOptions->addStretch();
    viewOptions->addWidget(hint("Drag: orbit   ·   Middle drag: pan   ·   Wheel: zoom", timelinePanel));
    timelineLayout->addLayout(viewOptions);
    _timelineDock->setWidget(timelinePanel);

        connect(folderButton, &QPushButton::clicked, this, &ReloadMakerController::browsePackage);
        connect(zipButton, &QPushButton::clicked, this, [this]() {
            const QString file = QFileDialog::getOpenFileName(
                _window, QStringLiteral("Open mod ZIP"), _packagePath->text(),
                QStringLiteral("ZIP archives (*.zip)"));
            if (!file.isEmpty()) {
                _packagePath->setText(file);
                openPackage(file);
            }
        });
        connect(_packagePath, &QLineEdit::returnPressed,
                this, &ReloadMakerController::openPackageFromField);
        connect(gameButton, &QPushButton::clicked, this, &ReloadMakerController::browseGameData);
        connect(_gameDataPath, &QLineEdit::returnPressed, this, [this]() {
            setGameData(_gameDataPath->text());
        });
        connect(_weaponCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    refreshAttachments();
                    refreshConfigurationChoices();
                    scheduleRebuild();
                });
        connect(_bodyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { rebuildWeapon(); });
        connect(_animationSetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { scheduleRebuild(); });
        connect(_slotTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            _focusedSlot = item ? item->data(0, kFormIdRole).toULongLong() : 0;
            _partSearch->clear();
            refreshSlotChoices();
            if (_slotOverlay) { _slotOverlay->setSelected(_focusedSlot); }
        });
        connect(_partSearch, &QLineEdit::textChanged, this, [this]() { refreshSlotChoices(); });
        connect(_childSlotCombo, qOverload<int>(&QComboBox::activated), this, [this](int index) {
            const auto key = _childSlotCombo->itemData(index).toULongLong();
            if (!key) { return; }
            _focusedSlot = key;
            _partSearch->clear();
            refreshAttachments();
        });
        const auto choosePart = [this](QListWidgetItem* item) {
            auto* config = configuration();
            if (item && config && config->select(_focusedSlot, item->data(kFormIdRole).toUInt())) {
                refreshAttachments();
                scheduleRebuild();
            }
        };
        connect(_partList, &QListWidget::itemClicked, this, choosePart);
        connect(_partList, &QListWidget::itemActivated, this, choosePart);
        connect(_pages, &QTabWidget::currentChanged, this, [this](int index) {
            if (_playing) { togglePlayback(); }
            _timelineDock->setVisible(index == 1);
            if (_slotOverlay) { _slotOverlay->setActive(index == 0 && _showPartLabels); }
            updateGripViewport();
            // Both pages share one live scene and one configuration.
            _window->getGLView()->update();
        });
        connect(_animationList, &QListWidget::itemActivated,
                this, &ReloadMakerController::selectAnimation);
        connect(_animationList, &QListWidget::itemClicked,
                this, &ReloadMakerController::selectAnimation);
        connect(reloadButton, &QPushButton::clicked, this, [this]() {
            if (_libraryMode) { startLibraryLoad(); }
            else { openPackageFromField(); }
        });
        _timelineDock->hide();
        connect(_playButton, &QPushButton::clicked, this, &ReloadMakerController::togglePlayback);
        connect(replayButton, &QPushButton::clicked, this, &ReloadMakerController::replay);
        connect(_timeline, &QSlider::valueChanged, this, &ReloadMakerController::scrubTimeline);
        connect(_timeline, &QSlider::sliderReleased,
                this, &ReloadMakerController::capturePoseDiagnostics);
        connect(_speedCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &ReloadMakerController::updatePlaybackRate);
        const auto refreshPoseMode = [this]() {
            if (!_clip.valid()) { return; }
            _playbackTime = std::min(_playbackTime, playbackDuration());
            if (applyPose(_playbackTime)) {
                updateTimelineText();
                showClipDetails();
                setStatus(QStringLiteral("%1 — %2 — %3 s; offline clip preview")
                              .arg(_activeAction, _motionMode->currentText()).arg(playbackDuration(), 0, 'f', 3));
            }
        };
        connect(_motionMode, qOverload<int>(&QComboBox::currentIndexChanged), this, refreshPoseMode);
        connect(_transitionCheck, &QCheckBox::toggled, this, refreshPoseMode);
        connect(_showArmsCheck, &QCheckBox::toggled,
                this, &ReloadMakerController::setArmsVisible);
        createHandPoseUi();
        createReloadUi();
        createSimulationUi();


    _handPoseDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _handPoseDock->setMinimumWidth(340);
    _handPoseDock->setMaximumWidth(450);
    _window->removeDockWidget(_handPoseDock);
    _window->addDockWidget(Qt::RightDockWidgetArea, _handPoseDock);
    _handPosePages->setTabText(0, "Finger poses");
    _handPosePages->setTabText(1, "Part grip");
    _handPosePages->setTabText(2, "Mechanism");
    createFingerEditor();
    // Short controls and scrollable inspectors remain usable on smaller screens.
    for (int page : {0, 1}) {
        QWidget* content = _handPosePages->widget(page);
        const QString title = _handPosePages->tabText(page);
        _handPosePages->removeTab(page);
        auto* scroll = new QScrollArea(_handPosePages);
        scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        _handPosePages->insertTab(page, scroll, title);
    }
    _reloadInspectorDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _reloadInspectorDock->setMinimumWidth(320);
    _reloadInspectorDock->setMaximumWidth(440);
    _reloadGraphDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _reloadGraphDock->setMinimumHeight(170);
    _reloadGraphDock->setMaximumHeight(360);
    auto* camera = new QToolBar("Camera", _window);
    camera->setMovable(false); _window->addToolBarBreak(); _window->addToolBar(camera);
    camera->addWidget(new QLabel("  VIEW  ", camera));
    const std::pair<const char*, GLView::ViewState> views[]{{"Front",GLView::ViewFront},{"Side",GLView::ViewRight},{"Top",GLView::ViewTop},{"Perspective",GLView::ViewDefault}};
    for (const auto& [label, view] : views) {
        auto* action = camera->addAction(label);
        connect(action, &QAction::triggered, this, [this, view] {
            _window->getGLView()->setOrientation(view, false);
            if(view==GLView::ViewDefault)_window->getGLView()->setRotation(-65,0,135);
        });
    }
    connect(camera->addAction("Fit weapon"), &QAction::triggered, this, [this] { (void)fitWeapon(); });
    auto* grid = new QCheckBox("Grid", camera);
    camera->addWidget(grid);
    connect(grid,&QCheckBox::toggled,this,[this](bool show) {
        auto* view=_window->getGLView();
        if(show)view->getScene()->options|=Scene::ShowGrid;else view->getScene()->options&=~Scene::ShowGrid;
        view->update();
    });
    auto* labels = new QCheckBox("Part labels", camera);
    camera->addWidget(labels);
    connect(labels,&QCheckBox::toggled,this,[this](bool show) {
        _showPartLabels=show;if(_slotOverlay)_slotOverlay->setActive(show&&_workspace==0);
    });
    connect(_workspaceButtons, &QButtonGroup::idClicked, this, &ReloadMakerController::setWorkspace);
    connect(libraryButton, &QPushButton::clicked, this, [this] { setWorkspace(0); _pages->setCurrentIndex(2); });
    connect(diagnostics, &QPushButton::toggled, _detailsDock, &QWidget::setVisible);
    connect(saveProject, &QToolButton::clicked, this, [this] { saveSession(); });
    connect(openProject, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(_window, "Open Reload Maker project", studioDataPath("workspace"), "Reload Maker project (*.json)");
        if (!path.isEmpty()) openSession(path);
    });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, _window);
    connect(saveShortcut, &QShortcut::activated, this, [this] { saveSession(); });
    auto* fitShortcut = new QShortcut(QKeySequence("F"), _window);
    connect(fitShortcut, &QShortcut::activated, this, [this] { (void)fitWeapon(); });
    for (int i = 0; i < workspaces.size(); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i+1)), _window);
        connect(shortcut, &QShortcut::activated, this, [this, i] { setWorkspace(i); });
    }
}

void ReloadMakerController::setWorkspace(int workspace) {
    if (workspace < 0 || workspace > 5 || _loadThread) return;
    if (_workspace == 5 && workspace != 5) stopSimulation();
    _workspace = workspace;
    _workspaceButtons->button(workspace)->setChecked(true);
    _pages->setCurrentIndex(workspace == 0 ? 0 : workspace >= 4 ? 3 : 1);
    _packageDock->setVisible(workspace != 5);
    _simulationDock->setVisible(workspace == 5);
    _reloadGraphDock->setVisible(workspace == 4);
    _reloadInspectorDock->setVisible(workspace == 4);
    if (workspace == 4) startReloadCapture();
    _packageGroup->setVisible(workspace == 0);
    _referenceGroup->setVisible(workspace == 0);
    _reloadSource->setVisible(workspace == 0);
    _handPoseDock->setVisible(workspace == 2 || workspace == 3);
    _handPosePages->setTabVisible(0, workspace == 2);
    _handPosePages->setTabVisible(1, workspace == 2);
    _handPosePages->setTabVisible(2, workspace == 3);
    _handPosePages->tabBar()->setVisible(workspace == 2);
    if (workspace == 3) _handPosePages->setCurrentIndex(2);
    else if (workspace == 2 && _handPosePages->currentIndex() == 2) _handPosePages->setCurrentIndex(0);
    _timelineDock->setVisible(workspace != 0 && workspace < 4);
    _packageDock->setWindowTitle(workspace == 0 ? "SOURCE & CONFIGURATION" : workspace == 4 ? "RELOAD PHASES" : "ANIMATION SOURCE");
    _handPoseDock->setWindowTitle(workspace == 3 ? "PART MECHANISM" : "HANDS & GRIPS");
    if (_slotOverlay) _slotOverlay->setActive(workspace == 0 && _showPartLabels);
    updateGripViewport();
    if(_clip.valid()&&!_reloadCapturing)(void)applyPose(_playbackTime);
    refreshFingerEditor();
    if (workspace == 5) prepareSimulation();
    ++_revision;
}

void ReloadMakerController::applyStyle() {
    _window->setStyleSheet(R"(
        QWidget { font-family:'Segoe UI'; font-size:12px; color:#dce7e7; background:#171d23; }
        QMainWindow, QToolBar { background:#11171c; }
        QToolBar { border:0; spacing:7px; padding:7px; }
        QLabel#Brand { font-size:20px; font-weight:700; padding:0 20px 0 5px; }
        QLabel#Hint { color:#91a4ae; }
        QDockWidget::title { background:#202a32; color:#93b8b9; padding:9px 12px; font-weight:600; }
        QGroupBox { border:1px solid #344149; border-radius:5px; margin-top:12px; padding:10px 6px 5px; }
        QGroupBox::title { subcontrol-origin:margin; left:10px; color:#a6bdc5; padding:0 4px; }
        QLineEdit, QComboBox, QDoubleSpinBox, QListWidget, QTreeWidget, QPlainTextEdit { background:#10171d; border:1px solid #35434c; border-radius:4px; padding:5px; selection-background-color:#275452; }
        QComboBox { min-height:23px; }
        QComboBox::drop-down { width:20px; border:0; }
        QAbstractItemView { outline:0; }
        QListWidget::item, QTreeWidget::item { padding:7px 3px; }
        QListWidget::item:selected, QTreeWidget::item:selected { background:#254e4d; color:#f0ffff; }
        QHeaderView::section { background:#25323a; color:#9ab1bc; border:0; padding:7px; }
        QPushButton, QToolButton { background:#27343d; border:1px solid #3b4d58; border-radius:4px; padding:7px 10px; }
        QPushButton:hover { background:#324751; border-color:#6ba6a8; }
        QPushButton:pressed { background:#152c30; }
        QPushButton:disabled { color:#637078; background:#202830; border-color:#2c3942; }
        QPushButton:checked, QPushButton#PrimaryButton { color:#efffff; background:#235854; border-color:#4c928b; }
        QPushButton#WorkspaceButton { min-width:96px; padding:10px; border-color:transparent; background:transparent; }
        QPushButton#WorkspaceButton:checked { background:#26423f; color:#91ddd0; border-bottom:2px solid #74c4b3; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { background:#1d2931; color:#98adb8; padding:10px 14px; }
        QTabBar::tab:selected { background:#2a4646; color:#d8fff5; }
        QSlider::groove:horizontal { height:5px; background:#344751; border-radius:2px; }
        QSlider::handle:horizontal { width:13px; margin:-5px 0; background:#88c8ba; border-radius:6px; }
        QCheckBox { spacing:6px; }
        QProgressBar { border:1px solid #344751; border-radius:3px; text-align:center; }
        QProgressBar::chunk { background:#467f78; }
        QScrollBar:vertical { width:10px; background:#161f26; }
        QScrollBar::handle:vertical { background:#40545f; min-height:30px; border-radius:4px; }
        QStatusBar { background:#11191f; color:#9bb5bc; border-top:1px solid #2c3a43; }
        QToolTip { color:#e1eeee; background:#293d44; border:1px solid #62858b; padding:5px; }
    )");
}
}
