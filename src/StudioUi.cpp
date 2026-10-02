#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "InstallationSetup.h"
#include "SlotOverlay.h"
#include "glview.h"
#include "LoadoutView.h"
#include "MagazineViewport.h"
#include <QCompleter>
#include <QAbstractItemModel>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
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
    auto* menuButton = new QToolButton(header);
    menuButton->setText("MENU");menuButton->setObjectName("MainMenuButton");menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* mainMenu=new QMenu(menuButton);mainMenu->setObjectName("MainMenu");menuButton->setMenu(mainMenu);header->addWidget(menuButton);
    _sourceMenu=mainMenu->addMenu("Source");_sourceMenu->setObjectName("SourceMenu");
    auto* dynamicSource=_sourceMenu->addAction("Dynamic source · MO2");dynamicSource->setCheckable(true);
    auto* developmentSource=_sourceMenu->addAction("Development source · Testbeds");developmentSource->setCheckable(true);
    connect(dynamicSource,&QAction::triggered,this,[this]{(void)selectSourceMode("dynamic");});
    connect(developmentSource,&QAction::triggered,this,[this]{(void)selectSourceMode("development");});
    _sourceMenu->addSeparator();
    connect(_sourceMenu->addAction("Open ESP / ESL / ESM…"),&QAction::triggered,this,[this]{const auto path=QFileDialog::getOpenFileName(_window,"Open source plugin",_packageText,"Bethesda plugins (*.esp *.esl *.esm)");if(!path.isEmpty())openPackage(path);});
    connect(_sourceMenu->addAction("Open mod folder…"),&QAction::triggered,this,&ReloadMakerController::browsePackage);
    connect(_sourceMenu->addAction("Open mod ZIP…"),&QAction::triggered,this,[this]{const auto path=QFileDialog::getOpenFileName(_window,"Open mod ZIP",_packageText,"ZIP archives (*.zip)");if(!path.isEmpty())openPackage(path);});
    connect(_sourceMenu->addAction("Open ordered source set…"),&QAction::triggered,this,[this]{const auto path=QFileDialog::getOpenFileName(_window,"Open ordered source set",studioDataPath("workspace"),"Source set (*.reloadsources.json)");if(!path.isEmpty())openPackage(path);});
    connect(_sourceMenu->addAction("Open base game and loose Data"),&QAction::triggered,this,[this]{(void)openBaseGame();});
    _sourceMenu->addSeparator();
    connect(_sourceMenu->addAction("Reload source files"),&QAction::triggered,this,[this]{if(_libraryMode)startLibraryLoad();else openPackage(_packageText);});
    connect(_sourceMenu,&QMenu::aboutToShow,this,[this,dynamicSource,developmentSource]{dynamicSource->setChecked(_libraryMode);developmentSource->setChecked(!_libraryMode&&!_package.sourcePath.isEmpty());});
    _workspaceButtons = new QButtonGroup(this);
    // Stable workspace ids are persisted in projects and used by MCP clients;
    // display order is independent of the id.
    const std::pair<QString, int> workspaces[]{{"Operate", OperateWorkspace},
        {"Hands && grips", GripsWorkspace}, {"Reload phases", PhasesWorkspace},{"Sounds",SoundsWorkspace},{"Collisions",CollisionsWorkspace},{"Scopes",ScopesWorkspace}};
    for (const auto& [title, id] : workspaces) {
        auto* button = new QPushButton(title, header);
        button->setCheckable(true);
        button->setObjectName("WorkspaceButton");
        _workspaceButtons->addButton(button, id);
        header->addWidget(button);
    }
    auto* spacer = new QWidget(header);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(spacer);
    mainMenu->addSeparator();
    connect(mainMenu->addAction("Setup && tools…"),&QAction::triggered,this,&ReloadMakerController::showSetup);
    connect(mainMenu->addAction("Home"),&QAction::triggered,this,&ReloadMakerController::showHome);
    connect(mainMenu->addAction("Author reload…"),&QAction::triggered,this,&ReloadMakerController::showAuthoringRoadmap);
    connect(mainMenu->addAction("Reload library…"),&QAction::triggered,this,&ReloadMakerController::showPackCoverage);
    mainMenu->addSeparator();
    auto* openProject=mainMenu->addAction("Open project…");
    auto* saveProject=mainMenu->addAction("Save project");
    auto* saveAs = mainMenu->addAction("Save project as…");
    saveAs->setShortcut(QKeySequence::SaveAs);
    connect(saveAs,&QAction::triggered,this,[this] {
        const auto path=QFileDialog::getSaveFileName(_window,"Save Reload Maker project",suggestedSessionPath(),"Reload Maker project (*.json)");
        if(!path.isEmpty())saveSession(path);
    });
    auto* importRecords = mainMenu->addAction("Import…");
    auto* exportMenu = mainMenu->addMenu("Export");
    connect(exportMenu->addAction("Save selected configuration to pack…"),&QAction::triggered,this,&ReloadMakerController::showConfigurationSaver);
    connect(exportMenu->addAction("Complete DIAMOND reload pack…"),&QAction::triggered,this,&ReloadMakerController::showPackExporter);
    exportMenu->addSeparator();
    connect(exportMenu->addAction("Current configured reload for DIAMOND"),&QAction::triggered,this,[this]{
        try{const auto result=executeCommand({{"method","export"},{"params",QJsonObject{{"kind","runtime_configuration"}}}});setStatus("Captured configured reload · "+result["path"].toString());}
        catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });
    for(const auto& pair : {std::pair{"Saved hand pose","hand"},std::pair{"Current part grip","grip"},std::pair{"Current mechanism","mechanism"},std::pair{"Current reload recipe","reload"},std::pair{"Reload + grips (self-contained)","reload_bundle"},std::pair{"Simulation setup","simulation"},std::pair{"Current slide for DIAMOND","slide"},std::pair{"Operating grab locations","grab_locations"}}) {
        connect(exportMenu->addAction(pair.first),&QAction::triggered,this,[this,kind=QString(pair.second)] {
            try {const auto result=executeCommand({{"method","export"},{"params",QJsonObject{{"kind",kind}}}});setStatus("Exported · "+result["path"].toString());}
            catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
        });
    }
    connect(exportMenu->addAction("True Scopes profile"),&QAction::triggered,this,[this]{
        try{(void)exportScopeProfile();}catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });
    connect(exportMenu->addAction("Physical reload sounds"),&QAction::triggered,this,[this]{
        try{const auto result=executeCommand({{"method","export"},{"params",QJsonObject{{"kind","sounds"}}}});setStatus("Exported · "+result["path"].toString());}
        catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });
    mainMenu->addSeparator();
    auto* diagnostics = mainMenu->addAction("Diagnostics");
    diagnostics->setCheckable(true);
    connect(importRecords,&QAction::triggered,this,[this] {
        const auto paths=QFileDialog::getOpenFileNames(_window,"Import existing poses, grips, mechanisms or reload recipes",studioDataPath("workspace"),"Authoring records (*.json)");
        if(paths.isEmpty())return;
        QJsonArray files;for(const auto& path:paths)files.append(path);
        try {const auto result=executeCommand({{"method","import_records"},{"params",QJsonObject{{"paths",files}}}});setStatus(QString("Imported %1 authored records; originals were preserved.").arg(result["imported"].toInt()));}
        catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });

    // ---- Loadout rail: the weapon as a workbench ----
    _packageDock = dock(_window, "LOADOUT", Qt::LeftDockWidgetArea);
    _packageDock->setMinimumWidth(320);
    _packageDock->setMaximumWidth(440);
    auto* packagePanel = new QWidget(_packageDock);
    auto* packageLayout = new QVBoxLayout(packagePanel);
    packageLayout->setContentsMargins(12, 12, 12, 12);
    packageLayout->setSpacing(8);
    // Hidden selection authority: index into _weaponSelections. The visible
    // header pickers filter and drive it.
    _weaponCombo = new QComboBox(_window);
    _weaponCombo->hide();
    _pages = new QTabWidget(packagePanel);
    _pages->tabBar()->hide();
    packageLayout->addWidget(_pages, 1);

    auto* setup = new QWidget(_pages);
    auto* setupLayout = new QVBoxLayout(setup);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    setupLayout->setSpacing(6);
    _loadoutTitle = new QLabel("No weapon", setup);
    _loadoutTitle->setObjectName("LoadoutTitle");
    _loadoutTitle->setWordWrap(true);
    setupLayout->addWidget(_loadoutTitle);
    _configurationSummary = hint("Choose a source and weapon in the bar above.", setup);
    setupLayout->addWidget(_configurationSummary);
    auto* delegate = new LoadoutDelegate(this);
    _partSearch = new QLineEdit(setup);
    _partSearch->setPlaceholderText("Search parts or slots…");
    _partSearch->setClearButtonEnabled(true);
    setupLayout->addWidget(_partSearch);
    _partList = new QListWidget(setup);
    _partList->setItemDelegate(delegate);
    _partList->setMouseTracking(true);
    _partList->setObjectName("Loadout");
    _partList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setupLayout->addWidget(_partList, 1);
    _pages->addTab(setup, "Loadout");
    // One native clip list; Reload phases shows it, Hands & grips uses the
    // transport's clip picker over the same model.
    _animationList = new QListWidget(_window);
    _animationList->hide();
    _loadingLabel = hint({}, packagePanel);
    _loadingProgress = new QProgressBar(packagePanel);
    _cancelLoading = new QPushButton("Cancel load", packagePanel);
    for (QWidget* w : std::initializer_list<QWidget*>{static_cast<QWidget*>(_loadingLabel), _loadingProgress, _cancelLoading}) { packageLayout->addWidget(w); w->hide(); }
    connect(_cancelLoading, &QPushButton::clicked, this, [this] {
        if (_loadThread) _loadThread->requestInterruption();
        _cancelLoading->setEnabled(false);
        _loadingLabel->setText("Cancelling; keeping the previous source…");
    });
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
    _clipCombo = new QComboBox(timelinePanel);
    _clipCombo->setModel(_animationList->model());
    _clipCombo->setMinimumContentsLength(16);
    _clipCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _clipCombo->setToolTip("Native clip shown on the timeline.");
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
    transport->addWidget(_clipCombo); transport->addWidget(_playButton); transport->addWidget(replayButton); transport->addWidget(_loopCheck); transport->addWidget(_speedCombo);
    transport->addWidget(_timeline, 1); transport->addWidget(_timeLabel);
    timelineLayout->addLayout(transport);
    auto* viewOptions = new QHBoxLayout;
    _transitionCheck = new QCheckBox("Approximate entry / exit blends", timelinePanel);
    _transitionCheck->setChecked(true);
    _transitionCheck->setToolTip("Graph-derived transition preview; not full Havok behavior emulation.");
    viewOptions->addWidget(_transitionCheck); viewOptions->addStretch();
    viewOptions->addWidget(hint("Drag: orbit   ·   Middle drag: pan   ·   Wheel: zoom", timelinePanel));
    timelineLayout->addLayout(viewOptions);
    _timelineDock->setWidget(timelinePanel);
    // One authority each for the motion frame and arm visibility; both live
    // in the always-visible view toolbar.
    _motionMode = new QComboBox(_window);
    _motionMode->addItems({"Weapon fixed", "Full native motion"});
    _motionMode->setToolTip("Weapon fixed holds the weapon frame still while hands and moving parts keep their native relative motion.");
    _showArmsCheck = new QCheckBox("Arms && hands", _window);
    _showArmsCheck->setChecked(true);
    _bodyCombo = new QComboBox(_window);_bodyCombo->setObjectName("ArmsBodyPicker");_bodyCombo->setAccessibleName("Hand model");
    _bodyCombo->addItem("Male arms", "male");_bodyCombo->addItem("Female arms", "female");

        connect(_weaponCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int selectedIndex) {
                    alignPluginToWeapon();
                    if(beginWeaponProject(selectedIndex))return;
                    refreshWeaponPicker();
                    refreshAttachments();

                    scheduleRebuild();
                });
        connect(_bodyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this](int) { rebuildWeapon(); });
        connect(_partSearch, &QLineEdit::textChanged, this, [this]() { refreshAttachments(); });
        const auto choosePart = [this](QListWidgetItem* item) {
            if(item&&item->data(kSectionRole).toInt()==kSlotHeader&&(item->flags()&Qt::ItemIsEnabled))
                showLoadoutChoices(item->data(kSlotRole).toULongLong());
        };
        connect(_partList, &QListWidget::itemClicked, this, choosePart);
        connect(_partList, &QListWidget::itemActivated, this, choosePart);
        connect(_pages, &QTabWidget::currentChanged, this, [this](int index) {
            if (_playing) { togglePlayback(); }
            if (_slotOverlay) { _slotOverlay->setActive(index == kLoadoutPage && _showPartLabels); }
            updateGripViewport();
            // Both pages share one live scene and one configuration.
            _window->getGLView()->update();
        });
        connect(_animationList, &QListWidget::itemActivated,
                this, [this](QListWidgetItem* item) { previewNativeClip(_animationList->row(item)); });
        connect(_animationList, &QListWidget::itemClicked,
                this, [this](QListWidgetItem* item) { previewNativeClip(_animationList->row(item)); });
        connect(_clipCombo, qOverload<int>(&QComboBox::activated), this, [this](int row) { previewNativeClip(row); });
        connect(_animationList, &QListWidget::currentRowChanged, this, [this](int row) {
            const QSignalBlocker block(_clipCombo); _clipCombo->setCurrentIndex(row);
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
        createSoundsUi();
    createCollisionsUi();
    createScopesUi();
        createGrabLocationsUi();


    _handPoseDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _handPoseDock->setMinimumWidth(400);
    _handPoseDock->setMaximumWidth(480);
    _window->removeDockWidget(_handPoseDock);
    _window->addDockWidget(Qt::RightDockWidgetArea, _handPoseDock);
    _handPoseDock->setWindowTitle("HANDS && GRIPS");
    _handPosePages->setTabText(0, "Finger poses");
    _handPosePages->setTabText(1, "Part grip");
    _handPosePages->setTabText(2, "Mechanism");
    createFingerEditor();
    // Short controls and scrollable inspectors remain usable on smaller screens.
    for (int page : {0, 1, 3}) {
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

    // ---- Context bar: source, plugin and weapon, then the view controls ----
    auto* camera = new QToolBar("Context", _window);
    camera->setMovable(false); _window->addToolBarBreak(); _window->addToolBar(camera);
    createSourceBar(camera);
    camera->addSeparator();
    camera->addWidget(new QLabel("  VIEW  ", camera));
    const std::pair<const char*, GLView::ViewState> views[]{{"Front",GLView::ViewFront},{"Side",GLView::ViewRight},{"Top",GLView::ViewTop},{"Perspective",GLView::ViewDefault}};
    for (const auto& [label, view] : views) {
        auto* action = camera->addAction(label);
        connect(action, &QAction::triggered, this, [this, view] {
            _window->getGLView()->setOrientation(view, false);
            if(view==GLView::ViewDefault)_window->getGLView()->setRotation(-65,0,135);
        });
    }
    connect(camera->addAction("Fit"), &QAction::triggered, this, [this] { (void)fitWeapon(); });
    auto* grid = new QCheckBox("Grid", camera);
    camera->addWidget(grid);
    connect(grid,&QCheckBox::toggled,this,[this](bool show) {
        auto* view=_window->getGLView();
        if(show)view->getScene()->options|=Scene::ShowGrid;else view->getScene()->options&=~Scene::ShowGrid;
        view->update();
    });
    auto* labels = new QCheckBox("Part labels", camera);
    camera->addWidget(labels);
    camera->addSeparator();
    camera->addWidget(_motionMode);
    camera->addWidget(_showArmsCheck);
    camera->addWidget(_bodyCombo);
    connect(labels,&QCheckBox::toggled,this,[this](bool show) {
        _showPartLabels=show;if(_slotOverlay)_slotOverlay->setActive(show&&_pages->currentIndex()==kLoadoutPage);
    });
    connect(_workspaceButtons, &QButtonGroup::idClicked, this, &ReloadMakerController::setWorkspace);
    connect(diagnostics, &QAction::toggled, _detailsDock, &QWidget::setVisible);
    connect(_detailsDock,&QDockWidget::visibilityChanged,diagnostics,&QAction::setChecked);
    connect(saveProject, &QAction::triggered, this, [this] { saveSession(); });
    connect(openProject, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(_window, "Open Reload Maker project", studioDataPath("workspace"), "Reload Maker project (*.json)");
        if (!path.isEmpty()) openSession(path);
    });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, _window);
    connect(saveShortcut, &QShortcut::activated, this, [this] { saveSession(); });
    auto* fitShortcut = new QShortcut(QKeySequence("F"), _window);
    connect(fitShortcut, &QShortcut::activated, this, [this] { (void)fitWeapon(); });
    for (int i = 0; i < int(std::size(workspaces)); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i+1)), _window);
        connect(shortcut, &QShortcut::activated, this, [this, id = workspaces[i].second] { setWorkspace(id); });
    }
}

bool ReloadMakerController::validWorkspace(int workspace) {
    return workspace == GripsWorkspace || workspace == PhasesWorkspace || workspace == CollisionsWorkspace || workspace == ScopesWorkspace || simulationWorkspace(workspace);
}

bool ReloadMakerController::mechanismPageActive() const {
    return _workspace == GripsWorkspace && _handPosePages && _handPosePages->currentIndex() == 2;
}

void ReloadMakerController::setWorkspace(int workspace) {
    if (!validWorkspace(workspace) || _loadThread) return;
    if(_workspace==CollisionsWorkspace&&workspace!=CollisionsWorkspace&&_magazineWorldPreview){stopSimulation();_magazineWorldPreview=false;}
    if(_magazineFitPreview&&workspace!=GripsWorkspace){rememberMagazineGrip();stopSimulation();}
    // Operate replaces clips and poses freely; an in-progress authoring grip is
    // parked while it is open and returned when leaving (as Reload phases does).
    if (simulationWorkspace(_workspace) && !simulationWorkspace(workspace)) {
        if((_magazineFitPreview&&workspace==GripsWorkspace)||(workspace==CollisionsWorkspace&&_magazineWorldPreview)) {
            _simulationTimer->stop();_magazineViewport->setActive(false);
            _simulationDrive->setActive(false,false);
        } else stopSimulation();
        _partGripDraft = _operateSuspendedGrip; _operateSuspendedGrip.reset();
    } else if (!simulationWorkspace(_workspace) && simulationWorkspace(workspace)) {
        _operateSuspendedGrip = _reloadWorkspaceActive ? _suspendedAuthoringGrip : _partGripDraft;
        if (_reloadWorkspaceActive) _suspendedAuthoringGrip.reset();
        _partGripDraft.reset();
    }
    _workspace = workspace;
    if(workspace!=GripsWorkspace)_magazineFitPreview=false;
    _workspaceButtons->button(workspace)->setChecked(true);
    // Reload phases replaces the loadout rail with its phase list and the
    // native clip list; Operate and Hands & grips keep the loadout.
    const bool phases = workspace == PhasesWorkspace;
    _pages->setCurrentIndex(phases ? kReloadPage : kLoadoutPage);
    _packageDock->setWindowTitle(phases ? "RELOAD PHASES" : "LOADOUT");
    _simulationDock->setVisible(workspace == OperateWorkspace);
    _soundsDock->setVisible(workspace==SoundsWorkspace);
    _collisionsDock->setVisible(workspace==CollisionsWorkspace);
    _scopesDock->setVisible(workspace==ScopesWorkspace);
    if(workspace==ScopesWorkspace){if(_playing)togglePlayback();_reloadPlaying=false;_reloadPreviewTimer->stop();refreshScopeTargets();}
    if(workspace==CollisionsWorkspace){if(_playing)togglePlayback();_reloadPlaying=false;_reloadPreviewTimer->stop();refreshCollisions();}
    _reloadGraphDock->setVisible(phases);
    _reloadInspectorDock->setVisible(phases);
    _handPoseDock->setVisible(workspace == GripsWorkspace);
    // Hands & grips needs a frame to capture from; phases shows the transport
    // only while a non-reload clip is being previewed.
    _timelineDock->setVisible(workspace == GripsWorkspace && !_magazineFitPreview);
    if (_slotOverlay) _slotOverlay->setActive(_pages->currentIndex() == kLoadoutPage && _showPartLabels);
    updateGripViewport();
    if(_clip.valid()&&!_reloadCapturing)(void)applyPose(_playbackTime);
    refreshFingerEditor();
    // Operate needs the learned reload for its rig; learning no longer depends
    // on the Reload phases page being visible.
    if (simulationWorkspace(workspace)) { if(!_simulationActive){startReloadCapture();prepareSimulation();}refreshSoundsUi(); }
    ++_revision;
}

void ReloadMakerController::previewNativeClip(int row) {
    if (row < 0 || row >= _animationList->count()) return;
    // Previewing a native clip leaves the interactive weapon state; the next
    // Operate action starts a fresh simulation from Ready.
    if (_simulationActive) { stopSimulation(); updateSimulationControls(); }
    if (_reloadWorkspaceActive) {
        // In Reload phases the reload itself belongs to the phase graph; other
        // clips are inspected on the transport until a phase is selected again.
        const bool reload = _animationList->item(row)->data(Qt::UserRole).toString() == _reloadExpectedPath;
        _reloadPreviewTimer->stop(); _reloadPlaying = false; _reloadPlay->setText("PLAY RELOAD");
        if (reload) { restoreReloadClip(); return; }
        _timelineDock->setVisible(true);
    }
    _animationList->setCurrentRow(row);
    selectAnimation(_animationList->item(row));
}

void ReloadMakerController::restoreReloadClip() {
    if (!_reloadWorkspaceActive) return;
    if (_workspace == PhasesWorkspace) _timelineDock->setVisible(false);
    if (_activeAnimationPath != _reloadExpectedPath || !_clip.valid()) startReloadCapture();
}

void ReloadMakerController::applyStyle() {
    _window->setStyleSheet(R"(
        QWidget { font-family:'Segoe UI'; font-size:12px; color:#dce7e7; background:#171d23; }
        QMainWindow, QToolBar { background:#11171c; }
        QToolBar { border:0; spacing:7px; padding:7px; }
        QLabel#Brand { font-size:20px; font-weight:700; padding:0 20px 0 5px; }
        QLabel#Hint { color:#91a4ae; }
        QLabel#Section { color:#7fb3ac; font-weight:700; letter-spacing:1px; padding-top:6px; }
        QLabel#LoadoutTitle { font-size:19px; font-weight:700; color:#e6f5f2; }
        QLabel#SourceLabel { color:#a6c9c4; padding:0 6px; }
        QToolButton#SourceButton { font-weight:700; color:#91ddd0; }
        QTreeWidget#Loadout, QListWidget#Loadout { background:transparent; border:0; }
        QTreeWidget#Loadout::item, QListWidget#Loadout::item { padding:0; }
        QLabel#StatusCard { padding:10px 12px; background:#1f3533; color:#d3f7ec; border-radius:5px; }
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
