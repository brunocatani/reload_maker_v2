#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "InstallationSetup.h"
#include "SlotOverlay.h"
#include "glview.h"
#include "LoadoutView.h"
#include "MagazineViewport.h"
#include "areas/operate/PlaybackTimeline.h"
#include "design/DesignWidgets.h"
#include "design/Icons.h"
#include "design/Theme.h"
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
#include <QStackedWidget>
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
    using namespace design;
    // Bottom docks span the viewport only; the side panels run full height.
    _window->setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    _window->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    _window->setDockOptions(QMainWindow::AnimatedDocks);

    // ---- Header: wordmark, weapon, configuration, mode switch, menu ----
    auto* header = new QToolBar("Header", _window);
    header->setMovable(false);
    header->setFloatable(false);
    header->setObjectName("AppHeader");
    header->setContextMenuPolicy(Qt::PreventContextMenu);
    _window->addToolBar(Qt::TopToolBarArea, header);
    auto* wordmark = new QLabel("Reload Maker", header);
    wordmark->setObjectName("Wordmark");
    header->addWidget(wordmark);
    _weaponPill = new QPushButton("Choose a weapon", header);
    _weaponPill->setObjectName("WeaponPill");
    _weaponPill->setIcon(icon(Icon::ChevronDown, color::textMuted));
    _weaponPill->setLayoutDirection(Qt::RightToLeft);
    _weaponPill->setToolTip("Source, plugin and weapon");
    _weaponPill->setCursor(Qt::PointingHandCursor);
    header->addWidget(_weaponPill);
    connect(_weaponPill, &QPushButton::clicked, this, &ReloadMakerController::showSourcePicker);
    _headerSummary = new QLabel(header);
    _headerSummary->setObjectName("HeaderSummary");
    header->addWidget(_headerSummary);
    _presetChip = new QLabel(header);
    _presetChip->setObjectName("PresetChip");
    _presetChip->setFixedHeight(24);
    _presetChip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    header->addWidget(_presetChip);
    auto* spacer = new QWidget(header);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(spacer);
    // The mode switch shows pages; each page or builder step opens a stable,
    // persisted workspace id (src/areas/shell/RailPages.cpp).
    auto* modes = new SegmentedControl("ModeSwitch", header);
    _modeSwitch = modes;
    _railButtons = modes->group();
    const std::pair<QString, RailPage> pages[]{{"Reload builder", RailPage::Builder}, {"Operate", RailPage::Operate},
        {"Poses library", RailPage::Poses}, {"Sounds", RailPage::Sounds}, {"Scopes", RailPage::Scopes}};
    for (const auto& [title, page] : pages) {
        modes->addSegment(title, int(page));
    }
    header->addWidget(modes);
    auto* menuButton = new QToolButton(header);
    menuButton->setObjectName("MenuButton");
    menuButton->setIcon(icon(Icon::Menu, color::textSecondary));
    menuButton->setIconSize(QSize(18, 18));
    menuButton->setToolTip("Menu");
    menuButton->setAccessibleName("Menu");
    menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* mainMenu = new QMenu(menuButton);
    mainMenu->setObjectName("MainMenu");
    menuButton->setMenu(mainMenu);
    header->addWidget(menuButton);

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
    connect(_sourceMenu->addAction("Reload source files"),&QAction::triggered,this,&ReloadMakerController::reloadSourceFiles);
    connect(_sourceMenu->addAction("Compatibility rules…"),&QAction::triggered,this,&ReloadMakerController::showSourcePolicy);
    connect(_sourceMenu,&QMenu::aboutToShow,this,[this,dynamicSource,developmentSource]{dynamicSource->setChecked(_libraryMode);developmentSource->setChecked(!_libraryMode&&!_package.sourcePath.isEmpty());});
    mainMenu->addSeparator();
    connect(mainMenu->addAction("Setup && tools…"),&QAction::triggered,this,&ReloadMakerController::showSetup);
    connect(mainMenu->addAction("Weapon collision…"),&QAction::triggered,this,[this]{setWorkspace(WeaponCollisionWorkspace);});
    connect(mainMenu->addAction("Home"),&QAction::triggered,this,&ReloadMakerController::showHome);
    connect(mainMenu->addAction("Reload checklist"),&QAction::triggered,this,&ReloadMakerController::showChecklist);
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
    createSourcePicker();

    // ---- Left panel: the builder checklist and the loadout ----
    _packageDock = dock(_window, "Left panel", Qt::LeftDockWidgetArea);
    _packageDock->setTitleBarWidget(new QWidget(_packageDock));
    _packageDock->setMinimumWidth(300);
    _packageDock->setMaximumWidth(400);
    auto* left = new QWidget(_packageDock);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 6, 0, 10);
    leftLayout->setSpacing(0);
    _leftTabs = new QTabBar(left);
    _leftTabs->setExpanding(true);
    _leftTabs->setDrawBase(false);
    _leftTabs->addTab("Checklist");
    _leftTabs->addTab("Loadout");
    leftLayout->addWidget(_leftTabs);
    _leftStack = new QStackedWidget(left);
    leftLayout->addWidget(_leftStack, 1);

    createChecklistPanel();
    _leftStack->addWidget(_checklistPanel);

    auto* loadoutPanel = new QWidget(_leftStack);
    auto* packageLayout = new QVBoxLayout(loadoutPanel);
    packageLayout->setContentsMargins(12, 14, 12, 0);
    packageLayout->setSpacing(8);
    _pages = new QTabWidget(loadoutPanel);
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
    _configurationSummary = hint("Choose a source and weapon with the weapon button above.", setup);
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
    _leftStack->addWidget(loadoutPanel);
    // One native clip list; the learned reload shows it, the hand editor uses
    // the transport's clip picker over the same model.
    _animationList = new QListWidget(_window);
    _animationList->hide();
    _loadingLabel = hint({}, left);
    _loadingProgress = new QProgressBar(left);
    _cancelLoading = new QPushButton("Cancel load", left);
    for (QWidget* w : std::initializer_list<QWidget*>{static_cast<QWidget*>(_loadingLabel), _loadingProgress, _cancelLoading}) { leftLayout->addWidget(w); w->hide(); }
    connect(_cancelLoading, &QPushButton::clicked, this, [this] {
        if (_loadThread) _loadThread->requestInterruption();
        _cancelLoading->setEnabled(false);
        _loadingLabel->setText("Cancelling; keeping the previous source…");
    });
    _packageDock->setWidget(left);
    connect(_leftTabs, &QTabBar::currentChanged, this, [this](int index) { _leftStack->setCurrentIndex(index); });

    _detailsDock = dock(_window, "Diagnostics", Qt::BottomDockWidgetArea);
    _detailsDock->setTitleBarWidget(new PanelHeader("Diagnostics", {}, _detailsDock));
    _details = new QPlainTextEdit(_detailsDock);
    _details->setReadOnly(true);
    _details->setFont(monoFont(kSmallPx));
    _details->setMaximumBlockCount(kMaximumDiagnosticLines);
    _details->setMaximumHeight(165);
    _detailsDock->setWidget(_details);
    _detailsDock->hide();

    // ---- Timeline: the clip under the viewport ----
    _timelineDock = dock(_window, "Timeline", Qt::BottomDockWidgetArea);
    _timelineDock->setMaximumHeight(96);
    _timelineDock->setTitleBarWidget(new QWidget(_timelineDock));
    auto* timelinePanel = new QWidget(_timelineDock);
    timelinePanel->setObjectName("Timeline");
    timelinePanel->setAttribute(Qt::WA_StyledBackground);
    auto* timelineLayout = new QVBoxLayout(timelinePanel);
    timelineLayout->setContentsMargins(16, 10, 16, 10);
    timelineLayout->setSpacing(8);
    auto* transport = new QHBoxLayout;
    transport->setSpacing(10);
    _playButton = new QPushButton(timelinePanel);
    _playButton->setObjectName("PlayButton");
    _playButton->setIcon(icon(Icon::Play, color::onPrimary));
    _playButton->setToolTip("Play");
    _playButton->setAccessibleName("Play");
    auto* replayButton = new QPushButton(timelinePanel);
    replayButton->setObjectName("IconButton");
    replayButton->setIcon(icon(Icon::Restart, color::textSecondary));
    replayButton->setToolTip("Restart");
    replayButton->setAccessibleName("Restart");
    _clipCombo = new QComboBox(timelinePanel);
    _clipCombo->setModel(_animationList->model());
    _clipCombo->setMinimumContentsLength(18);
    _clipCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _clipCombo->setToolTip("Native clip shown on the timeline");
    _clipPathLabel = new QLabel(timelinePanel);
    _clipPathLabel->setObjectName("ClipPath");
    _clipPathLabel->setFont(monoFont(kSmallPx));
    _clipPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // A long resource path must never widen the timeline past the viewport.
    _clipPathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _clipPathLabel->setMinimumWidth(40);
    _loopCheck = new QCheckBox("Loop", timelinePanel);
    _loopCheck->setChecked(_loopPlayback);
    _speedCombo = new QComboBox(timelinePanel);
    for (float rate : {.25f, .5f, 1.f, 1.5f, 2.f}) _speedCombo->addItem(QString::number(rate)+"×", rate);
    _speedCombo->setCurrentIndex(2);
    _speedCombo->setToolTip("Playback speed");
    _transitionCheck = new QCheckBox("Approximate blends", timelinePanel);
    _transitionCheck->setChecked(_approximateTransitions);
    _transitionCheck->setToolTip("Graph-derived entry and exit blends; not full Havok behavior emulation.");
    _timeLabel = new QLabel("0.000 / 0.000 s", timelinePanel);
    _timeLabel->setObjectName("TimeLabel");
    _timeLabel->setFont(monoFont(kBodyPx));
    transport->addWidget(_playButton); transport->addWidget(replayButton); transport->addWidget(_clipCombo);
    transport->addWidget(_clipPathLabel, 1); transport->addWidget(_transitionCheck); transport->addWidget(_loopCheck);
    transport->addWidget(_speedCombo); transport->addWidget(_timeLabel);
    timelineLayout->addLayout(transport);
    _timeline = new QSlider(Qt::Horizontal, timelinePanel);
    _timeline->setRange(0, kTimelineMaximum);
    _timeline->setEnabled(false);
    timelineLayout->addWidget(_timeline);
    _timelineDock->setWidget(timelinePanel);
    // The motion frame and arm visibility controls live in the view bar and
    // show the document fields (areas/operate/PreviewSettings.cpp).
    _motionMode = new QComboBox(_window);
    _motionMode->setObjectName("ViewChip");
    _motionMode->addItems({"Weapon fixed", "Full native motion"});
    showWeaponFixedMotion();
    _motionMode->setToolTip("Weapon fixed holds the weapon frame still while hands and moving parts keep their native relative motion.");
    _showArmsCheck = new QCheckBox("Arms", _window);
    _showArmsCheck->setChecked(_armsShown);
    _bodyCombo = new QComboBox(_window);_bodyCombo->setObjectName("ViewChip");_bodyCombo->setAccessibleName("Hand model");
    _bodyCombo->addItem("Male arms", "male");_bodyCombo->addItem("Female arms", "female");

        connect(_bodyCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            setArmsBody(_bodyCombo->currentData().toString());
            rebuildWeapon();
        });
        connect(_partSearch, &QLineEdit::textChanged, this, [this]() { refreshAttachments(); });
        const auto choosePart = [this](QListWidgetItem* item) {
            if(item&&item->data(kSectionRole).toInt()==kSlotHeader&&(item->flags()&Qt::ItemIsEnabled))
                showLoadoutChoices(item->data(kSlotRole).toULongLong());
        };
        connect(_partList, &QListWidget::itemClicked, this, choosePart);
        connect(_partList, &QListWidget::itemActivated, this, choosePart);
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
                this, [this](int index) { setPlaybackRate(_speedCombo->itemData(index).toFloat()); });
        connect(_loopCheck, &QCheckBox::toggled, this, [this](bool loop) { setLoopPlayback(loop); });
        connect(_motionMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int index) { setWeaponFixedMotion(index == 0); });
        connect(_transitionCheck, &QCheckBox::toggled, this, [this](bool enabled) { setApproximateTransitions(enabled); });
        connect(_showArmsCheck, &QCheckBox::toggled, this, [this](bool shown) { setArmsShown(shown); });
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
    // The Poses library page and the Reload builder steps choose the page.
    _handPosePages->tabBar()->hide();
    _reloadGraphDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _reloadGraphDock->setMinimumHeight(170);
    _reloadGraphDock->setMaximumHeight(360);

    // Every panel opens with the canvas heading instead of a dock caption.
    _handPoseHeader = new PanelHeader("Reload builder", {}, _handPoseDock);
    _handPoseDock->setTitleBarWidget(_handPoseHeader);
    _collisionsHeader = new PanelHeader("Reload builder", "Magazines", _collisionsDock);
    _collisionsDock->setTitleBarWidget(_collisionsHeader);
    _reloadDock->setTitleBarWidget(new PanelHeader("Reload builder", "Learned reload", _reloadDock));
    _simulationDock->setTitleBarWidget(new PanelHeader("Operate", "Work the weapon", _simulationDock));
    _soundsDock->setTitleBarWidget(new PanelHeader("Sounds", "Reload sounds", _soundsDock));
    _scopesDock->setTitleBarWidget(new PanelHeader("Scopes", "Scope profile", _scopesDock));
    _reloadGraphDock->setTitleBarWidget(new PanelHeader("When each part moves", {}, _reloadGraphDock));
    for (auto* side : {_handPoseDock, _collisionsDock, _reloadDock, _simulationDock, _soundsDock, _scopesDock}) {
        side->setMinimumWidth(360);
    }

    createViewBar();
    connect(_railButtons, &QButtonGroup::idToggled, this, [this](int page, bool checked) { if (checked) showRailPage(RailPage(page)); });
    connect(_builderStepButtons, &QButtonGroup::idToggled, this, [this](int step, bool checked) { if (checked) showBuilderStep(BuilderStep(step)); });
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
    for (int i = 0; i < int(std::size(pages)); ++i) {
        auto* shortcut = new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i+1)), _window);
        connect(shortcut, &QShortcut::activated, this, [this, page = pages[i].second] { showRailPage(page); });
    }
    refreshHeader();
}

// ---- View bar: camera, framing and what the viewport shows ----
// The viewport is a native GL window, so these sit in a bar above it.
void ReloadMakerController::createViewBar() {
    using namespace design;
    auto* bar = new QWidget(_window);
    bar->setObjectName("ViewBar");
    bar->setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(6);
    const auto chip = [bar, layout](const QString& text, const QString& tip) {
        auto* button = new QPushButton(text, bar);
        button->setObjectName("ViewChip");
        button->setToolTip(tip);
        layout->addWidget(button);
        return button;
    };
    const std::pair<const char*, GLView::ViewState> views[]{{"Front",GLView::ViewFront},{"Side",GLView::ViewRight},{"Top",GLView::ViewTop},{"3D",GLView::ViewDefault}};
    for (const auto& [label, view] : views) {
        auto* button = chip(label, QString("%1 view").arg(label == QString("3D") ? QString("Perspective") : QString(label)));
        connect(button, &QPushButton::clicked, this, [this, view] {
            _window->getGLView()->setOrientation(view, false);
            if(view==GLView::ViewDefault)_window->getGLView()->setRotation(-65,0,135);
        });
    }
    auto* fit = chip("Fit", "Frame the weapon (F)");
    fit->setIcon(icon(Icon::Fit, color::textSecondary));
    connect(fit, &QPushButton::clicked, this, [this] { (void)fitWeapon(); });
    auto* grid = chip("Grid", "Show the ground grid");
    grid->setCheckable(true);
    connect(grid,&QPushButton::toggled,this,[this](bool show) {
        auto* view=_window->getGLView();
        if(show)view->getScene()->options|=Scene::ShowGrid;else view->getScene()->options&=~Scene::ShowGrid;
        view->update();
    });
    auto* labels = chip("Part labels", "Label the installed parts on the model");
    labels->setCheckable(true);
    connect(labels,&QPushButton::toggled,this,[this](bool show) {
        _showPartLabels=show;if(_slotOverlay)_slotOverlay->setActive(show&&loadoutPageActive());
    });
    layout->addStretch(1);
    layout->addWidget(_motionMode);
    _showArmsCheck->setParent(bar);
    layout->addWidget(_showArmsCheck);
    layout->addWidget(_bodyCombo);
    _window->setViewportBar(bar);
    // How to use the viewport stays in the footer, beside the status.
    auto* navigation = new QLabel("Click a part to select it · double-click to change it · right-click for actions · drag to orbit · middle-drag to pan · wheel to zoom", _window);
    navigation->setObjectName("ViewHint");
    _window->statusBar()->addPermanentWidget(navigation);
}

bool ReloadMakerController::validWorkspace(int workspace) {
    return workspace == GripsWorkspace || workspace == PhasesWorkspace || collisionWorkspace(workspace) || workspace == ScopesWorkspace || simulationWorkspace(workspace);
}

bool ReloadMakerController::mechanismPageActive() const {
    return _workspace == GripsWorkspace && _handEditorPage == HandEditorPage::Mechanism;
}

// Part labels and model picking belong to the loadout page: outside Reload,
// and not while a source loads.
bool ReloadMakerController::loadoutPageActive() const {
    return !_reloadWorkspaceActive && !_sourceLoading;
}

void ReloadMakerController::setWorkspace(int workspace) {
    if (!validWorkspace(workspace) || _loadThread) return;
    if(_workspace==CollisionsWorkspace&&workspace!=CollisionsWorkspace&&_magazineWorldPreview){stopSimulation();_magazineWorldPreview=false;}
    if(_magazineFitPreview&&workspace!=GripsWorkspace){rememberMagazineGrip();stopSimulation();}
    // Operate replaces clips and poses freely; an in-progress authoring grip is
    // parked while it is open and returned when leaving.
    if (simulationWorkspace(_workspace) && !simulationWorkspace(workspace)) {
        if((_magazineFitPreview&&workspace==GripsWorkspace)||(workspace==CollisionsWorkspace&&_magazineWorldPreview)) {
            _simulationTimer->stop();_magazineViewport->setActive(false);
            _simulationDrive->setActive(false,false);
        } else stopSimulation();
        _partGripDraft = _operateSuspendedGrip; _operateSuspendedGrip.reset();
    } else if (!simulationWorkspace(_workspace) && simulationWorkspace(workspace)) {
        _operateSuspendedGrip = _partGripDraft;
        _partGripDraft.reset();
    }
    _workspace = workspace;
    if(workspace!=GripsWorkspace)_magazineFitPreview=false;
    syncRail();
    // The learned reload opens its inspector and the graph of its moving
    // parts; every other page keeps the loadout in the left panel.
    const bool phases = workspace == PhasesWorkspace;
    setReloadWorkspace(phases);
    _reloadDock->setVisible(phases);
    _simulationDock->setVisible(workspace == OperateWorkspace);
    _soundsDock->setVisible(workspace==SoundsWorkspace);
    _collisionsDock->setVisible(collisionWorkspace(workspace));
    _scopesDock->setVisible(workspace==ScopesWorkspace);
    if(workspace==ScopesWorkspace){if(_playing)togglePlayback();_reloadPlaying=false;_reloadPreviewTimer->stop();refreshScopeTargets();}
    if(collisionWorkspace(workspace)){if(_playing)togglePlayback();_reloadPlaying=false;_reloadPreviewTimer->stop();showCollisionSubject();refreshCollisions();}
    _reloadGraphDock->setVisible(phases);
    _handPoseDock->setVisible(workspace == GripsWorkspace);
    // The hand editor needs a frame to capture from; Reload shows the transport
    // only while a non-reload clip is being previewed.
    _timelineDock->setVisible(workspace == GripsWorkspace && !_magazineFitPreview);
    if (_slotOverlay) _slotOverlay->setActive(loadoutPageActive() && _showPartLabels);
    updateGripViewport();
    if(_clip.valid()&&!_reloadCapturing)(void)applyPose(_playbackTime);
    refreshFingerEditor();
    // Operate needs the learned reload for its rig; learning no longer depends
    // on the Reload page being visible.
    if (simulationWorkspace(workspace)) { if(!_simulationActive){startReloadCapture();prepareSimulation();}refreshSoundsUi(); }
    ++_revision;
}

void ReloadMakerController::previewNativeClip(int row) {
    if (row < 0 || row >= _animationActions.size()) return;
    // Previewing a native clip leaves the interactive weapon state; the next
    // Operate action starts a fresh simulation from Ready.
    if (_simulationActive) { stopSimulation(); updateSimulationControls(); }
    if (_reloadWorkspaceActive) {
        // On the Reload page the reload itself belongs to its graph; other
        // clips are inspected on the transport until the graph or PLAY RELOAD
        // returns to the reload.
        const bool reload = _animationActions[row].resourcePath == _reloadExpectedPath;
        _reloadPreviewTimer->stop(); _reloadPlaying = false; _reloadPlay->setText("Play reload");
        if (reload) { restoreReloadClip(); return; }
        _timelineDock->setVisible(true);
    }
    selectAction(row);
}

void ReloadMakerController::restoreReloadClip() {
    if (!_reloadWorkspaceActive) return;
    if (_workspace == PhasesWorkspace) _timelineDock->setVisible(false);
    if (_activeAnimationPath != _reloadExpectedPath || !_clip.valid()) startReloadCapture();
}

void ReloadMakerController::applyStyle() {
    design::applyTheme();
}
}
