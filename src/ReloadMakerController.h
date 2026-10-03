#pragma once

#include "EspReader.h"
#include "HkxReader.h"
#include "PackageSource.h"
#include "ScenePoseBinding.h"
#include "AttachmentConnections.h"
#include "WeaponConfiguration.h"
#include "LoadOrderLibrary.h"
#include "HandPoseLibrary.h"
#include "PartGrip.h"
#include "PartMechanism.h"
#include "MechanismScene.h"
#include "ReloadRecipe.h"
#include "ReloadDriveViewport.h"
#include "WeaponSimulation.h"
#include "OperatingSetup.h"
#include "MagazineSetup.h"
#include "WeaponAmmunition.h"
#include "ActionCatalog.h"
#include "BoltCycle.h"
#include "SoundProfile.h"
#include "AudioPreview.h"
#include "OperatingGrabLocations.h"
#include "CollisionAuthoring.h"
#include "ScopeProfile.h"
#include "InstallationSetup.h"
#include "areas/sources/SourceBrowserRows.h"
#include "commands/CommandRegistry.h"

#include <QHash>
#include <QJsonArray>
#include <QCache>
#include <QSharedPointer>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QSet>
#include <QVector>

#include <cstdint>
#include <optional>

class NifModel;
class StudioWindow;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTimer;
class QTabWidget;
class QTreeWidget;
class QThread;
class QProgressBar;
class QGroupBox;
class QDoubleSpinBox;
class QButtonGroup;
class QToolButton;
class QToolBar;
class QAction;
class QMenu;
class QProcess;
class QLocalSocket;

namespace reloadmaker
{
    class ReloadGraph;
    class SlotOverlay;
    class PartPicker;
    class GripViewport;
    class MagazineViewport;
    class CollisionPanel;
    class ReloadMakerController final : public QObject
    {
        Q_OBJECT

    public:
        explicit ReloadMakerController(StudioWindow* window);
        ~ReloadMakerController() override;
        void showHome();

        void openInitialPaths(const QString& packagePath, const QString& gameDataPath,
                              const QString& mo2Instance = {}, bool library = false);
        QJsonObject executeCommand(const QJsonObject& request);

    signals:
        void packJobChanged();

    private slots:
        void browsePackage();
        void rebuildWeapon();
        void selectAnimation(QListWidgetItem* item);
        void togglePlayback();
        void replay();
        void advancePlayback();
        void scrubTimeline(int value);
        void capturePoseDiagnostics();
        void updatePlaybackRate(int index);
        void setArmsVisible(bool visible);

    private:
        struct WeaponSelection
        {
            int pluginIndex{ -1 };
            int weaponIndex{ -1 };
        };

        struct ConnectTarget
        {
            QPersistentModelIndex parent;
            Vec3f translation{};
            Quaternionf rotation{};
            float scale{ 1.0f };
            int suppliedBone{ -1 };
            int ownerRoot{ -1 };
            QString error;

            [[nodiscard]] bool valid() const noexcept { return parent.isValid(); }
        };

        struct SkinRetargetResult
        {
            int retargetedBones{ 0 };
            int totalBones{ 0 };
        };

        // Steps of rebuildWeapon (areas/loadout/WeaponAssembly.cpp). Each
        // returns false with the user-facing status in `error`.
        [[nodiscard]] QString assemblyGeometrySignature(const PluginContent& plugin, const WeaponRecord& weapon) const;
        void resetAssemblyState(bool reuseGeometry);
        bool loadFirstPersonSkeleton(QString& error);
        bool assembleScene(NifModel& destination, const WeaponRecord& weapon, const QString& geometrySignature, QString& error);
        bool attachFirstPersonArms(NifModel& destination, QString& error);
        bool attachArmsModel(NifModel& destination, const QString& path, const QString& name, QPersistentModelIndex& root,
                             int& flags, QString& error);
        bool attachWeaponModels(NifModel& destination, const WeaponRecord& weapon, QString& error);
        bool attachReceiver(NifModel& destination, const ObjectModRecord& receiver, int baseNodesReplaced, QString& error);
        bool attachObjectMods(NifModel& destination, bool& complete, QString& error);
        bool placeAttachment(NifModel& destination, const ObjectModRecord& record, NifModel& attachment,
                             const ConnectTarget& target, const QString& point, QString& error);
        bool showReadyPose(bool sameWeapon, const QString& previousAction, QString& error);

        struct AnimationAction
        {
            QString label;
            QString graphNode;
            QString resourcePath;
            QString baselineResourcePath;
            float blendInSeconds{ 0.0f };
            float blendOutSeconds{ 0.0f };
            ActionFamily family{ ActionFamily::Idle };
            bool automatic{ false };
            bool baseReload{ false };
        };

        // Stable ids: persisted in projects and used by MCP clients (DIAMOND's
        // importer uses OperateWorkspace = 5). Retired ids (0 Source, 1 Preview,
        // 3 Mechanisms) are rejected; projects saved on them reopen in Operate.
        enum Workspace : int { GripsWorkspace = 2, PhasesWorkspace = 4, OperateWorkspace = 5, SoundsWorkspace = 6, CollisionsWorkspace = 7, ScopesWorkspace = 8 };
        static bool simulationWorkspace(int value) { return value==OperateWorkspace||value==SoundsWorkspace; }
        static constexpr int kLoadoutPage = 0, kReloadPage = 1;
        void createSourceBar(QToolBar* bar);
        void refreshSourceBar();
        void refreshWeaponPicker();
        void restoreReloadClip();
        [[nodiscard]] bool mechanismPageActive() const;
        [[nodiscard]] static bool validWorkspace(int workspace);
        void createUi();
        void setWorkspace(int workspace);
        void previewNativeClip(int row);
        [[nodiscard]] bool reloadCaptureAllowed() const noexcept;
        void createCollisionsUi();
        void createScopesUi();
        void refreshScopeTargets();
        void refreshScopeControls();
        void updateScopeViewport();
        QVector<int> scopeShapeBlocks() const;
        QString currentScopeKey() const;
        bool scopeFrame(Transformf& world) const;
        bool pickScopeLens(QPointF pixel);
        void editScopeWorld(const Transformf& world);
        QJsonObject scopeState() const;
        QJsonObject configureScope(const QJsonObject& params);
        QJsonObject exportScopeProfile();
        QHash<QString,ScopeProfile> _scopeProfiles;
        QDockWidget* _scopesDock{};
        QComboBox* _scopePart{};
        QLineEdit* _scopeName{};
        QDoubleSpinBox* _scopePosition[3]{};
        QDoubleSpinBox* _scopeRotation[3]{};
        QDoubleSpinBox* _scopeRadius{},*_scopeAspect{},*_scopeRelief{},*_scopeCamera{};
        QLabel* _scopeStatus{};
        GripViewport* _scopeViewport{};
        bool _editingScopeUi{false};
        void refreshCollisions();
        void invalidateCollisions();
        CollisionProfile collisionSnapshot(int target,bool referenceOnly,bool captureRender=false);
        void paintCollisions();
        QVector<int> _collisionTargets;
        CollisionPanel* _collisions{};
        QDockWidget* _collisionsDock{};
        void createSoundsUi();
        void prepareSounds();
        void refreshSoundsUi();
        void editSoundCue();
        bool saveSounds();
        bool playSoundCue(int index);
        void emitSoundEvent(const QString& event);
        void updateSimulationSounds();
        void resetSoundTracking(bool stop=true);
        void playSourceSounds(const HkxClip&,float from,float to);
        void cacheSourceSounds(const HkxClip&);
        bool cacheSound(const SoundCue&);
        bool playSound(const SoundCue&);
        QJsonObject soundState() const;
        QJsonObject exportSounds() const;
        QString soundProfileKey() const;
        QHash<QString,SoundCue> _referenceSounds,_soundCatalog;
        QHash<QString,SoundProfile> _savedSounds;
        SoundProfile _soundProfile;
        SoundCueGate _soundGate;
        AudioPreview _audio;
        QString _soundError,_lastSound;
        quint64 _soundEventCount{0};
        bool _soundTracking{false},_soundsMuted{false},_soundsDirty{false},_editingSoundUi{false};
        bool _simulationTriggerDown{false};
        WeaponSimulation _soundPrevious;
        QDockWidget* _soundsDock{};
        QListWidget* _soundCueList{};
        QComboBox* _soundChoice{};
        QComboBox* _soundTrigger{};
        QDoubleSpinBox* _soundProgress{};
        QDoubleSpinBox* _soundGain{};
        QCheckBox* _soundEnabled{};
        QCheckBox* _soundMute{};
        QLabel* _soundInfo{};
        QLabel* _soundStatus{};
        void createFingerEditor();
        void refreshFingerEditor();
        void editFinger(int joint, const Quaternionf& rotation);
        bool saveSession(const QString& path = {});
        QString suggestedSessionPath() const;
        bool beginWeaponProject(int selectedIndex);
        // Selected weapon: an index into _weaponSelections, -1 when none.
        // selectWeapon() is the user-facing change: it may switch projects and
        // reassembles. Restores assign the field directly.
        int _selectedWeapon{ -1 };
        void selectWeapon(int index);
        // `index` when it names a weapon selection, otherwise -1.
        [[nodiscard]] int validWeaponIndex(int index) const;
        // First-person arms body, "male" or "female". The arms picker shows it.
        QString _armsBody{ "male" };
        // Preview and Operate settings saved with the project (areas/operate/PreviewSettings.cpp).
        bool _slideSpringReturn{ true };      // the slide, handle or bolt returns when released
        bool _weaponFixedMotion{ true };      // the preview holds the weapon frame still
        bool _approximateTransitions{ true }; // clip entry and exit blends
        bool _armsShown{ true };
        [[nodiscard]] QString motionModeName() const;
        void setSlideSpringReturn(bool enabled);
        void setWeaponFixedMotion(bool fixed);
        void showWeaponFixedMotion();
        void setApproximateTransitions(bool enabled);
        void setArmsShown(bool shown);
        void refreshPoseMode();
        [[nodiscard]] static bool isArmsBody(const QString& body);
        void showArmsBody();
        QString sourcePolicyKey() const;
        [[nodiscard]] bool sourcePluginLoaded(const QString& plugin) const;
        QJsonObject sourcePolicyState() const;
        QJsonObject configureSourcePolicy(const QJsonObject&);
        void showSourcePolicy();
        QJsonObject _sourcePolicies;
        void openSession(const QString& path);
        void restoreSessionSelection();

        // Command pipe and registry; definitions live in src/commands by area.
        CommandRegistry _commands;
        void startCommandServer();
        void acceptCommandClient(QLocalSocket* socket);
        QJsonObject answerCommand(const QByteArray& line);
        void requireIdle() const;
        void registerCommands();
        void registerDocumentCommands();
        void registerSourceCommands();
        void registerLoadoutCommands();
        void registerSimulationCommands();
        void registerHandCommands();
        void registerMagazineCommands();
        void registerOperatingCommands();
        void registerReloadCommands();
        void registerSoundCommands();
        void registerGrabLocationCommands();
        void registerCollisionCommands();
        void registerScopeCommands();
        void registerPackCommands();
        QJsonObject diagnosticsState() const;
        QJsonObject poseState() const;
        QJsonObject importAuthoredRecords(const QJsonArray& paths);
        QJsonObject exportRecord(const QString& kind);
        QByteArray exportReloadBundle(QString& error);
        void configureLoadout(const QJsonObject& params);
        void controlPlayback(const QJsonObject& params);
        void arrangeView(const QJsonObject& params);
        QJsonObject renderView(const QJsonObject& params);
        QJsonObject simulateOperation(const QJsonObject& params);
        void moveSimulatedMagazine(const QJsonObject& params);
        void editFingerJoints(const QJsonObject& params);
        void saveHandPoseFromCommand(const QJsonObject& params);
        void setGripPlacement(const QJsonObject& params);
        QJsonObject mechanismOperation(const QJsonObject& params);
        void setMechanismStage(const QJsonObject& params);
        void editReloadWindow(const QJsonObject& params);
        void updateReloadWindow(ReloadRecipe& recipe, int index, const QJsonObject& params) const;
        void soundOperation(const QJsonObject& params);
        void editSoundCueFromCommand(int index, const QJsonObject& params);

        QJsonObject studioState() const;
        QJsonObject installationState() const;
        bool configureInstallation(const QJsonObject&);
        void showSetup();
        void showPackBuilder();
        void showPackExporter();
        void showCollisionCooker();
        QJsonObject capturePreparedPart(const QString& output);
        QJsonObject captureMagazineWorld(bool empty);
        std::optional<CollisionProfile> _magazineWorldSnapshot;
        bool _magazineWorldPreview{false};
        QJsonObject _preparedPart;
        void showAuthoringRoadmap();
        QJsonObject homeState() const;
        QJsonObject chooseHomeRole(const QString&);
        bool openBaseGame();
        QString roadmapKey() const;
        QJsonObject roadmapState() const;
        QJsonArray roadmapProgress() const;
        void rememberCollisionDraft();
        QString assignedTemplate() const;
        QJsonObject roadmapCommand(const QJsonObject&);
        QJsonObject sourceEvidence() const;
        QJsonObject auditSources(const QJsonObject&) const;
        QJsonObject _authoringRoadmaps;
        QJsonObject _reloadTemplateAssignments;
        std::optional<WeaponMechanism> _assignedMechanism;
        mutable QHash<QString,QString> _animationReadHashes;
        QJsonObject packCommand(const QJsonObject&);
        QProcess* _packProcess{};
        QJsonObject _packJob;
        QString _packCancelFile;
        bool connectInstalledProfile();
        bool handOffToMo2(const InstallationSetup& setup, const InstallationCheck& check);
        QJsonObject animationSourceIdentity() const;
        void cacheWeaponFitBounds();
        bool fitWeapon();
        QVector<QVector3D> _weaponFitCorners;
        void createSimulationUi();
        bool authorReloadGrips();
        void createSimulationViewport();
        bool isGlockConfiguration() const;
        bool isAr15Configuration() const;
        bool isMk18Configuration() const;
        bool isSrepConfiguration() const;
        bool isSrepEmbeddedMagazine() const;
        bool isMk18OverhaulConfiguration() const;
        bool isM1AConfiguration() const;
        bool isThanatosConfiguration() const;
        bool isL118Configuration() const;
        WeaponMechanism simulationMechanism() const;
        bool isBoltActionConfiguration() const;
        bool prepareBoltActionMotion();
        void applyBoltActionTargets(QHash<int,Transformf>& targets) const;
        QJsonObject exportBoltActionMotion() const;
        BoltCycle _boltCycle;
        QVector<Transformf> _boltFiredPose;
        QJsonObject _boltCycleSource;
        bool isRifleConfiguration() const;
        QString currentTestbed() const;
        QString testbedProject(const QString& name) const;
        bool prepareRifleMotion();
        bool buildCurrentSimulationRig(SimulationRig&,QString&);
        Transformf rifleBoltPose(float progress) const;
        Transformf rifleFireBoltPose() const;
        float simulationPoseSeconds() const;
        void advanceSimulation(float delta);
        void setSimulationTrigger(bool pressed);
        bool selectTestbed(const QString& name);
        QJsonArray _testbedSources;
        bool _testbedSourcesLoaded{false};
        void refreshDevelopmentSources();
        bool selectSourcePlugin(const QString& plugin);
        bool selectSourceWeapon(int index);
        bool selectSourceMode(const QString& mode);
        bool allWeaponsSelected() const;
        QStringList selectionPlugins(int index) const;
        QStringList sourcePluginOrder() const;
        // Browser entries: kAllWeaponsSource first for an MO2 profile, then the plugins.
        [[nodiscard]] QStringList browserPluginNames() const;
        // The listed entry matching `name` case-insensitively, or empty.
        [[nodiscard]] QString browserPluginEntry(const QString& name) const;
        [[nodiscard]] PluginDependencies pluginDependencies() const;
        [[nodiscard]] QVector<SourcePluginRow> sourcePluginRows() const;
        [[nodiscard]] QVector<SourceWeaponRow> sourceWeaponRows() const;
        void resolveBrowserPlugin();
        // Moves the browser selection to a plugin the open weapon comes from.
        void alignBrowserPlugin();
        void alignPluginToWeapon();
        void renderPluginPicker();
        [[nodiscard]] SourceBrowserProjection sourceBrowserProjection() const;
        QJsonObject sourceBrowserState() const;
        // Selected source browser entry, always one of browserPluginNames() or
        // empty. The plugin picker only shows it.
        QString _browserPlugin;
        QString _browserSourceKey;
        QVector<Transformf> _rifleBoltCycle;
        int _rifleBoltOpen{0};
        float _rifleBoltDuration{0},_rifleFireInterval{0},_rifleSingleDuration{0};
        QString _rifleBoltSource;
        SlotKey _simulationMagazineSlot{0};
        int _m1aEmptyMagazineFrame{0};
        int _m1aLoadedMagazineFrame{0};
        HandPosePlacement simulationMagazineAnchor() const;
        Transformf _magazineGrabBridge{};
        QString glockNoExtenderPath() const;
        void prepareSimulation();
        void stopSimulation();
        void updateSimulationControls();
        void runSimulationAction(const QString& action);
        bool selectSimulationClip(bool fire, bool reload);
        bool applySimulationPose();
        void applySimulationVisibility();
        void tickSimulation();
        bool beginSimulationGrab(QPointF point, ReloadDriveCurve& curve);
        void dragSimulation(float progress);
        void releaseSimulation(bool cancel);
        QJsonObject simulationState() const;
        QJsonObject exportSimulation() const;
    QJsonObject exportRuntimeConfiguration() const;
    QString runtimeTrackPath(int track) const;
        QJsonObject operatingSetupState();
        QJsonObject configureOperatingSetup(const QJsonObject&);
        void showOperatingSetup();
        QJsonObject magazineGuideState() const;
        QJsonObject configureMagazineGuide(const QJsonObject&);
        void showMagazineGuide();
        QJsonObject magazineSetupState() const;
        QJsonObject magazineReferenceState() const;
        QJsonObject configureMagazineReference(const QJsonObject&);
        void showMagazineReference();
        bool magazineReferenceCandidate(int anchor) const;
        QJsonObject configureMagazineSetup(const QJsonObject&);
        void showMagazineSetup();
        QJsonObject packCoverageState(bool refresh=false);
        void showPackCoverage();
        QString selectionRecordKey(int selection) const;
        QString coverageLabel(const QString& record) const;
        void updatePackCoverage(const QJsonObject&);
        QJsonObject _packCoverage;
        QJsonObject _weaponLibraryReport;
        QHash<QString,QString> _coverageLabels;
        QString magazinePiecePath(const QModelIndex&) const;
        bool sampledMagazinePayload() const;
        bool buildSourceSimulationRig(SimulationRig&,QString&);
        bool buildConfiguredSimulationRig(const OperatingSetup&,SimulationRig&,QString&);
        QJsonObject saveConfiguredReload(const QJsonObject& request);
        void showConfigurationSaver();
        QJsonObject exportSlideProfile() const;
        void prepareMagazineInteraction();
        void moveFreeMagazine(QVector3D offset);
        void moveMagazineRail(float distance);
        void cancelMagazine();
        void updateMagazinePose();
        std::optional<MagazineRail> _magazineRail;
        QString _magazineError;
        struct MagazineVisual {QPersistentModelIndex node;int flags{0};bool empty{false};int track{-1};bool cartridge{false};bool nativeOnly{false};};
        QVector<MagazineVisual> _magazineVisuals;
        // Steps of the Operate simulation (src/areas/operate/).
        bool buildSrepSimulationRig(SimulationRig& rig, QString& error);
        bool buildMk18SimulationRig(SimulationRig& rig, QString& error);
        bool measureRifleFireInterval(const HkxClip& single, const AnimationAction* automatic, QString& error);
        bool sampleRifleFireBolt(const HkxClip& base, const HkxClip& clip, QString& error);
        bool useReloadBoltStroke(const HkxClip& singleFire);
        bool resolveBoltCycleTracks();
        bool resolveSimulationMagazineRoots();
        bool prepareMechanismMotion();
        bool runMagazineAction(const QString& action);
        bool startNativeAction(bool fire);
        [[nodiscard]] QString simulationRejection(const QString& action) const;
        void startSimulationClock();
        void setBoltDriveCurve(const Vec3f& hit, ReloadDriveCurve& curve) const;
        bool setStrokeDriveCurve(const Vec3f& hit, ReloadDriveCurve& curve, QString& error);
        void addSlideTargets(QHash<int, Transformf>& targets, bool holdOpen) const;
        void addAr15Targets(QHash<int, Transformf>& targets, bool holdOpen) const;
        [[nodiscard]] Transformf ar15BoltPose(bool holdOpen) const;
        void addM1ATargets(QHash<int, Transformf>& targets, bool holdOpen) const;
        bool addMagazineRoundTargets(QHash<int, Transformf>& targets);
        void addDetachedMagazineTargets(QHash<int, Transformf>& targets, QSet<int>& sceneTargets) const;
        bool poseSimulationTargets(const QHash<int, Transformf>& targets, const QSet<int>& sceneTargets,
                                   QVector<Transformf>& local, QVector<Transformf>& world);
        [[nodiscard]] bool magazineVisualHidden(const MagazineVisual& visual) const;
        [[nodiscard]] bool reloadTrackCulled(int track) const;
        [[nodiscard]] QJsonObject glockReloadProfile() const;
        [[nodiscard]] QJsonArray simulationPartsJson() const;
        [[nodiscard]] QJsonArray magazineRootsJson() const;
        [[nodiscard]] QJsonArray magazineShapesJson() const;
        bool addRifleExport(QJsonObject& result) const;
        std::optional<MagazineSetup> _magazineSetup;
        MagazineViewport* _magazineViewport{nullptr};
        QPushButton* _magazineOut{nullptr};
        QPushButton* _magazineIn{nullptr};
        QPushButton* _magazineGrab{nullptr};
        QDockWidget* _simulationDock{nullptr};
        QPushButton* _simulationFire{nullptr};
        QPushButton* _simulationReload{nullptr};
        QPushButton* _simulationDrop{nullptr};
        QPushButton* _simulationReset{nullptr};
        QLabel* _simulationStatus{nullptr};
        QLabel* _simulationParts{nullptr};
        QLabel* _simulationInstructions{nullptr};
        QCheckBox* _simulationNoExtender{nullptr};
        QLabel* _simulationSource{nullptr};
        bool _glockNoExtender{false};
        float _glockReloadRackAt{-1}, _glockReloadFeedAt{-1};
        QCheckBox* _simulationSpring{nullptr};
        QTimer* _simulationTimer{nullptr};
        QElapsedTimer _simulationClock;
        ReloadDriveViewport* _simulationDrive{nullptr};
        std::optional<SimulationRig> _simulationRig;
        WeaponSimulation _simulation;
        bool _simulationActive{false};
        QString _simulationError;
        WeaponAmmunition _simulationAmmo;
        QHash<QString,QString> _referenceNames;
        QPushButton* _simulationSlideRelease{nullptr};
        QVector<std::pair<QPersistentModelIndex,int>> _simulationVisibility;
        QButtonGroup* _workspaceButtons{nullptr};
        int _workspace{0};
        bool _showPartLabels{false};
        quint64 _revision{0};
        // True while a timer only advances what is shown (playback, previews,
        // simulated time). Widget changes made then are not document edits and
        // do not advance _revision.
        bool _viewRefresh{false};
        // Where the revision moved between commands, so a stale request can
        // say what changed: a widget the user (or a deferred update) changed,
        // or an unattributed bump outside the command pipe.
        bool _executingCommand{false};
        quint64 _revisionAfterCommand{0};
        QString _revisionChangedOutside;
        [[nodiscard]] QString staleRevisionMessage() const;
        // Advances the revision for a document change that is not a view
        // refresh, remembering `source` when it happened outside a command.
        void noteDocumentChange(const QString& source);
        QJsonObject _pendingSession;
        QJsonObject _pendingReloadDraft;
        QString _sessionPath;
        QString _lastError;
        QComboBox* _fingerJoint{nullptr};
        std::array<QDoubleSpinBox*,3> _fingerAngles{};
        bool _fingerRefreshing{false};
        std::array<std::optional<HandPose>,2> _fingerDrafts;
        void createHandPoseUi();
        void createPartGripUi();
        void createMechanismUi();
        void createReloadUi();
        void createReloadViewport();
        void setReloadWorkspace(bool active);
        void invalidateReloadCapture();
        void startReloadCapture();
        void advanceReloadCapture();
        void refreshReloadWindows();
        void selectReloadWindow(int window,int track=-1);
        void refreshReloadMembers();
        void selectReloadMember();
        void applyReloadGrip();
        void seekReload(float seconds);
        void tickReloadPreview();
        void releaseReloadGrab(bool cancel);
        void saveReloadRecipe();
        [[nodiscard]] bool beginReloadGrab(QPointF point,ReloadDriveCurve& curve);
        void selectWorkshopPart(int anchorIndex,std::optional<Vec3f> point=std::nullopt,const QString& surface={});
        void refreshWorkshopChoices();
        void applyWorkshopPose();
        void beginWorkshopPick(bool contactOnly=false);
        void beginWorkshopPivotPick();
        void pickWorkshopPart(QPointF point);
        void beginMechanismTeaching();
        void startMechanismPreview(bool teaching);
        void beginTeachingDrag(int axis,bool rotation);
        void captureMechanismRest();
        void saveWorkshop();
        void editWorkshopHand(bool enabled);
        [[nodiscard]] bool pickWeaponPart(QPointF point,int& automatic,Vec3f& world,QString& surface,QVector<int>& candidates,QString& error);
        void refreshMechanisms();
        void refreshMechanismChoices();
        void openMechanismForPart();
        void loadMechanism();
        void saveMechanism();
        void displayMechanism();
        void readMechanismControls();
        void analyzeMechanismAnimation();
        void beginMechanismPreview();
        void updateMechanismControls();
        void stopMechanismPreview();
        void releaseMechanismGrab();
        void advanceMechanismPreview();
        [[nodiscard]] bool grabMechanismAt(QPointF point);
        void beginMechanismGrab();
        void dragMechanismBy(float value);
        void cancelMechanismGrab();
        [[nodiscard]] bool applyMechanismPreview();
        [[nodiscard]] bool assignGripPart(int anchorIndex,QString& error);
        [[nodiscard]] int resolveMechanismAnchor(const HandPosePlacement& anchor,QString& error) const;
        [[nodiscard]] Transformf gripReferenceWorld() const;
        void createGripViewport();
        void updateGripViewport();
        void beginGripSurfacePick();
        void pickGripSurface(QPointF point);
        void editGripWorld(const Transformf& world);
        void refreshPartGripLibrary();
        void refreshPartGripChoices();
        void refreshPartGripList();
        void updatePartGripControls();
        void openPartGripEditor();
        void startPartGrip(bool capturedOffset);
        void loadPartGrip();
        bool savePartGrip();
        void showPartGripValues();
        [[nodiscard]] bool preparePartGripFrame(int anchorIndex, QString& error);
        [[nodiscard]] bool applyPartGripPreview();
        [[nodiscard]] const PartGrip* selectedPartGrip() const;
        [[nodiscard]] QJsonObject currentPoseSource() const;
        void refreshHandPoseLibrary();
        void refreshHandPoseList();
        void refreshHandPoseAnchors();
        void updateHandPoseControls();
        bool saveCurrentHandPose();
        void applySavedPoseToPart();
        bool useMagazineGrabGrip(bool allMagazines=false);
        bool editMagazineGrip(const QString& savedId={});
        bool loadGripRecord(const PartGrip& grip);
        int currentMagazineAnchor() const;
        void updateMagazineGripUi();
        void rememberMagazineGrip();
        bool useDefaultMagazineGrip();
        QJsonObject magazineEditorState() const;
        int magazineGrabAnchor(const HandPosePlacement& placement) const;
        void resolveMagazineGrabGrip();
        bool applyMagazineGrabPose();
        PoseHand _interactionHand{PoseHand::Left};
        QComboBox* _interactionHandPicker{nullptr};
        QComboBox* _magazineHandPicker{nullptr};
        // Assignments supply finger locals only; operating wrists stay live.
        QHash<QString,QString> _savedOperatingGrips;
        std::array<std::optional<HandPose>,2> _operatingFingers;
        QString _operatingFingerError;
        QComboBox* _operatingFingerPicker{};
        QString operatingGripKey(PoseHand hand) const;
        void resolveOperatingFingers();
        bool useOperatingFingers(const QString& id);
        QLabel* _operatingGripStatus{nullptr};
        bool setInteractionHand(PoseHand hand);
        void createGrabLocationsUi();
        bool openGrabLocations();
        void prepareGrabLocationFrame();
        std::optional<std::pair<QString,float>> _grabLocationCapturePreview;
        OperatingGrabLocations currentGrabLocations() const;
        void refreshGrabLocationsUi();
        bool beginGrabLocationPick(bool replace=false);
        bool pickGrabLocation(QPointF);
        void editGrabLocationWorld(const Transformf&);
        bool saveGrabLocations();
        QJsonObject grabLocationsState() const;
        QJsonObject exportGrabLocations() const;
        QJsonObject configureGrabLocation(const QJsonObject&);
        QHash<QString,OperatingGrabLocations> _grabLocations;
        QListWidget* _grabLocationList{};
        QLineEdit* _grabLocationName{};
        QDoubleSpinBox* _grabLocationPosition[3]{};
        QDoubleSpinBox* _grabLocationRadius{};
        QComboBox* _grabLocationHands{};
        QLabel* _grabLocationStatus{};
        bool _editingGrabLocationUi{},_pickingGrabLocation{},_replaceGrabLocation{};
        QString operatingGroup() const;
        int operatingAnchor() const;
        bool mirrorMagazineGrip();
        void updateInteractionGripUi();
        QJsonObject interactionGripState() const;
        QJsonObject exportInteractionGrips() const;
        bool operatingGrabAvailable() const;
        std::optional<QVector3D> hoverOperatingPart(QPointF point);
        bool _operatingHover{false};
        QHash<QString,QString> _magazineGrabGrips;
        QHash<QString,QString> _magazineGrabDefaults;
        QHash<QString,PartGrip> _magazineGripDrafts;
        bool _editingMagazine{false};
        bool _magazineFitPreview{false};
        QLabel* _magazineFitStatus{nullptr};
        QPushButton* _editMagazineGrip{nullptr};
        QPushButton* _saveAllMagazineGrips{nullptr};
        QPushButton* _useDefaultMagazineGrip{nullptr};
        QCheckBox* _gripHistory{nullptr};
        QComboBox* _simulationMagazineGrip{nullptr};
        std::optional<PartGrip> _magazineGrabGrip;
        int _magazineGrabBinding{-1};
        QString _magazineGrabError;
        QPushButton* _useMagazineGrip{nullptr};
        void previewSavedHandPose();
        void restoreHandPosePreview();
        [[nodiscard]] const HandPose* selectedHandPose() const;
        [[nodiscard]] bool sceneNodeWorld(const QModelIndex& node, Transformf& world, QString& error) const;
        [[nodiscard]] QString poseRecordKey(std::uint32_t formId) const;
        void startLibraryLoad();
        void installLibrary(LoadOrderLibrary& library);
        void setLoading(bool loading, const QString& message = {});
        void startPackageLoad(const QString& path, const QString& dataPath,bool newDocument=false);
        [[nodiscard]] const ResourceArchive& referenceMeshes() const;
        void applyStyle();
        void setStatus(const QString& message, bool error = false);
        void appendDiagnostic(const QString& message);
        void refreshWeapons();
        void refreshAttachments();
        void showLoadoutChoices(SlotKey key);
        void focusSlot(SlotKey key);
        void showPartMenu(SlotKey key, QPoint globalPosition);
        [[nodiscard]] QString describePart(SlotKey key) const;
        void scrollToFocusedSlot();
        void refreshSlotCallouts();
        void refreshPartPicking();
        [[nodiscard]] QJsonObject partAtPoint(const QJsonObject& params);
        void recordPartSurfaces(SlotKey slot, int firstBlock, int lastBlock);
        bool remapAttachmentNodes(const ObjectModRecord& mod, int firstBlock, int lastBlock, QString& error,bool completeAssembly=false);
        struct RemappedAttachment {QPersistentModelIndex node, parent;QString keyword, point;SlotKey slot{0};};
        QVector<RemappedAttachment> _remappedAttachments;
        [[nodiscard]] bool recordPartAlignment(int firstBlock, int lastBlock, int anchorBone, QString& error);
        void scheduleRebuild();
        void clearAssetCaches();
        [[nodiscard]] WeaponConfiguration* configuration();
        [[nodiscard]] const WeaponConfiguration* configuration() const;
        void refreshAnimations();
        void configureResourcePaths();
        void openPackage(const QString& path);
        void setGameData(const QString& path);

        [[nodiscard]] NifModel* activeModel() const;
        [[nodiscard]] WeaponSelection currentWeaponSelection() const;
        [[nodiscard]] const PluginContent* currentPlugin() const;
        [[nodiscard]] const WeaponRecord* currentWeapon() const;
        [[nodiscard]] const ObjectModRecord* resolvedReceiverObjectMod() const;
        [[nodiscard]] QVector<const ObjectModRecord*> configuredObjectMods() const;
        [[nodiscard]] QVector<std::uint32_t> activeAnimationKeywords() const;
        [[nodiscard]] QByteArray readAnimationResource(const QString& resourcePath,
                                                       QString& error) const;
        [[nodiscard]] QString resolveAnimationPath(const QString& graphAnimationPath) const;
        [[nodiscard]] bool rebuildAnimationActions(QString& error);
        [[nodiscard]] QString normalizeNifPath(const QString& path) const;
        [[nodiscard]] bool loadNif(const ResourceArchive& archive, const QString& resourcePath,
                                   NifModel& destination, QString& error) const;
        [[nodiscard]] QSharedPointer<NifModel> sourceNif(const ResourceArchive& archive,
                                                       const QString& path, QString& error);
        [[nodiscard]] QByteArray nifBytes(const ResourceArchive& archive,
                                         const QString& path, QString& error) const;
        [[nodiscard]] HkxClip cachedClip(const QString& path, QString& error);
        [[nodiscard]] bool loadNifBytes(const QByteArray& bytes, NifModel& destination,
                                        QString& error) const;
        [[nodiscard]] QPersistentModelIndex pasteRoot(NifModel& source,
                                                      const QModelIndex& parent,
                                                      const QString& rootName = {});
        [[nodiscard]] QPersistentModelIndex pasteBranch(NifModel& source,
                                                        const QModelIndex& sourceRoot,
                                                        const QModelIndex& parent,
                                                        const QString& rootName = {});
        [[nodiscard]] QPersistentModelIndex findNode(const QString& name,
                                                     int firstBlock = 0,
                                                     int lastBlock = -1) const;
        [[nodiscard]] QHash<QString, QPersistentModelIndex> collectCanonicalNodes(
            int firstBlock, int lastBlock = -1) const;
        [[nodiscard]] int mergeWeaponModelNodes(
            const QHash<QString, QPersistentModelIndex>& nodes);
        [[nodiscard]] QStringList childConnectPoints(const NifModel& source) const;
        void registerAttachmentModel(const QModelIndex& root, int firstBlock, int lastBlock);
        [[nodiscard]] ConnectTarget findConnectTarget(const QString& childPoint,
                                                       const NifModel* incoming, SlotKey slot) const;
        [[nodiscard]] bool readNodeTransform(
            const QModelIndex& node, Transformf& transform) const;
        [[nodiscard]] bool setNodeTransform(const QModelIndex& node, const Transformf& transform);
        [[nodiscard]] QPersistentModelIndex createBoneNode(int boneIndex);
        [[nodiscard]] bool addSkinHelperBones(QString& error);
        [[nodiscard]] SkinRetargetResult retargetSkin(int firstBlock, int lastBlock,
                                                      const QModelIndex& skeletonRoot);
        [[nodiscard]] bool buildNodeBindings(QString& error);
        [[nodiscard]] bool sampleActionPose(
            float timeSeconds, QVector<Transformf>& localPose,
            QVector<Transformf>& globalPose);
        [[nodiscard]] float actionBlendWeight(float timeSeconds) const noexcept;
        [[nodiscard]] float playbackDuration() const noexcept;
        [[nodiscard]] bool isDescendantOf(int boneIndex, int ancestorIndex) const;
        [[nodiscard]] bool applyPose(float timeSeconds);
        void updateTimelineText();
        void showClipDetails();

        StudioWindow* _window{ nullptr };
        QDockWidget* _packageDock{ nullptr };
        QDockWidget* _detailsDock{ nullptr };
        QDockWidget* _timelineDock{ nullptr };
        QDockWidget* _handPoseDock{nullptr};
        QLineEdit* _handPoseName{nullptr};
        QPlainTextEdit* _handPoseNotes{nullptr};
        QComboBox* _handPoseSide{nullptr};
        QComboBox* _handPoseAnchor{nullptr};
        QPushButton* _saveHandPose{nullptr};
        QPushButton* _previewHandPose{nullptr};
        QPushButton* _restoreHandPose{nullptr};
        QLineEdit* _handPoseSearch{nullptr};
        QListWidget* _handPoseList{nullptr};
        QPlainTextEdit* _handPoseDetails{nullptr};
        QLabel* _handPoseStatus{nullptr};
        struct HandPoseAnchor {
            HandPosePlacement description;
            QPersistentModelIndex node;
        };
        QVector<HandPoseAnchor> _handPoseAnchors;
        HandPoseLibrary _handPoseLibrary{HandPoseLibrary::defaultDirectory()};
        QVector<HandPose> _savedHandPoses;
        std::optional<HandPose> _handPosePreview;
        QTabWidget* _handPosePages{nullptr};
        QComboBox* _gripPoseCombo{nullptr};
        QComboBox* _gripReferenceCombo{nullptr};
        QLineEdit* _gripName{nullptr};
        std::array<QDoubleSpinBox*,3> _gripPosition{};
        std::array<QDoubleSpinBox*,3> _gripAngles{};
        QPushButton* _gripCapturedStart{nullptr};
        QPushButton* _gripCurrentStart{nullptr};
        QPushButton* _gripSave{nullptr};
        QPushButton* _gripLoad{nullptr};
        QPushButton* _gripEnd{nullptr};
        QPushButton* _gripPick{nullptr};
        GripViewport* _gripViewport{nullptr};
        QListWidget* _gripList{nullptr};
        QLabel* _gripStatus{nullptr};
        PartGripLibrary _partGripLibrary{PartGripLibrary::defaultDirectory()};
        QVector<PartGrip> _partGrips;
        std::optional<PartGrip> _partGripDraft;
        std::optional<PartGrip> _suspendedAuthoringGrip;
        std::optional<PartGrip> _operateSuspendedGrip;
        struct PartGripFrame {
            bool weapon{false};
            int binding{-1};
            Transformf bridge{};
            int node{-1};
        } _partGripFrame;
        bool _gripPreviewValid{false};
        QString _gripError;
        bool _reloadWorkspaceActive{false},_reloadCapturing{false},_reloadCaptureQueued{false},_reloadUpdating{false};
        bool _reloadHeld{false},_reloadPlaying{false},_reloadReturning{false};
        bool _reloadIgnoreSavedOnce{false};
        bool _reloadOldWeaponFixed{true};
        int _reloadWindow{-1},_reloadTrack{-1},_reloadCaptureFrame{0};
        float _reloadGrabStart{0};
        QDockWidget* _reloadGraphDock{nullptr};
        QDockWidget* _reloadInspectorDock{nullptr};
        ReloadGraph* _reloadGraph{nullptr};
        ReloadDriveViewport* _reloadDrive{nullptr};
        QTimer* _reloadCaptureTimer{nullptr};
        QTimer* _reloadPreviewTimer{nullptr};
        QElapsedTimer _reloadClock;
        QLabel* _reloadSourceLabel{nullptr};
        QLabel* _reloadSummary{nullptr};
        QProgressBar* _reloadProgress{nullptr};
        QPushButton* _reloadAnalyze{nullptr};
        QPushButton* _reloadPlay{nullptr};
        QPushButton* _reloadTest{nullptr};
        QListWidget* _reloadWindows{nullptr};
        QListWidget* _reloadMembers{nullptr};
        QLineEdit* _reloadWindowName{nullptr};
        QLineEdit* _reloadPartName{nullptr};
        QComboBox* _reloadGripChoice{nullptr};
        QCheckBox* _reloadReturn{nullptr};
        QSlider* _reloadWindowProgress{nullptr};
        QPushButton* _reloadSave{nullptr};
        QLabel* _reloadWindowInfo{nullptr};
        ReloadRecipeLibrary _reloadRecipeLibrary;
        std::optional<ReloadRecipe> _reloadRecipe;
        HkxClip _reloadClip;
        QVector<Transformf> _reloadBase;
        MechanismScenePlan _reloadScenePlan;
        QVector<int> _reloadTrackNodes;
        QString _reloadExpectedPath;
        QComboBox* _mechanismList{nullptr};
        QComboBox* _workshopPart{nullptr};
        QComboBox* _workshopPose{nullptr};
        QPushButton* _workshopPick{nullptr};
        QPushButton* _workshopContact{nullptr};
        QPushButton* _workshopHand{nullptr};
        QPushButton* _workshopTeach{nullptr};
        QPushButton* _workshopMove{nullptr};
        QPushButton* _workshopRotate{nullptr};
        QPushButton* _workshopPivot{nullptr};
        QPushButton* _workshopAddStage{nullptr};
        QWidget* _workshopStageFields{nullptr};
        QWidget* _workshopTestControls{nullptr};
        QLabel* _workshopHint{nullptr};
        bool _mechanismTeaching{false},_workshopPicking{false},_workshopContactOnly{false},_workshopHandEditing{false};
        bool _workshopPivotPicking{false};
        bool _workshopRefreshing{false},_workshopGripDirty{false};
        MechanismStage _teachingDragStart;
        int _teachingAxis{0};
        bool _teachingRotation{false};
        std::optional<Vec3f> _workshopContactPoint;
        QString _workshopSurface;
        QLineEdit* _mechanismName{nullptr};
        QListWidget* _mechanismStages{nullptr};
        QLineEdit* _stageForward{nullptr};
        QLineEdit* _stageReverse{nullptr};
        QComboBox* _stageMotion{nullptr};
        QComboBox* _stageRelease{nullptr};
        QDoubleSpinBox* _stageFrom{nullptr};
        QDoubleSpinBox* _stageTo{nullptr};
        QDoubleSpinBox* _stageRate{nullptr};
        std::array<QDoubleSpinBox*,3> _stagePivot{};
        QListWidget* _mechanismFollowers{nullptr};
        QLabel* _mechanismStatus{nullptr};
        QPushButton* _mechanismSave{nullptr};
        QPushButton* _mechanismStart{nullptr};
        QCheckBox* _mechanismPoseOnGrab{nullptr};
        QSlider* _mechanismSlider{nullptr};
        QTimer* _mechanismTimer{nullptr};
        QElapsedTimer _mechanismClock;
        MechanismLibrary _mechanismLibrary{MechanismLibrary::defaultDirectory()};
        QVector<PartMechanism> _mechanisms;
        std::optional<PartMechanism> _mechanismDraft;
        MechanismState _mechanismState;
        bool _mechanismUpdating{false},_mechanismPreview{false},_mechanismRestorePending{false},_mechanismHeld{false};
        float _mechanismGrabPath{0},_mechanismGrabScale{1};
        int _mechanismDirection{1};
        QVector<ScenePoseNode> _assemblyNodes;
        MechanismScenePlan _mechanismScene;
        QVector<Transformf> _mechanismLocal,_mechanismWorld;
        // Requested source paths (the loaded ones live in _package/_gameData).
        QString _packageText;
        QString _gameDataText;
        QString _mo2Instance;
        QString _requestedMo2Profile;
        QString _profileHandoff;
        QMenu* _sourceMenu{nullptr};
        QToolButton* _pluginSearch{nullptr};
        QLabel* _sourceLabel{nullptr};
        QComboBox* _weaponPicker{nullptr};
        QComboBox* _clipCombo{nullptr};
        QLabel* _loadoutTitle{nullptr};
        QComboBox* _bodyCombo{ nullptr };
        QTabWidget* _pages{ nullptr };
        QComboBox* _pluginFilter{nullptr};
        QLabel* _loadingLabel{nullptr};
        QProgressBar* _loadingProgress{nullptr};
        QPushButton* _cancelLoading{nullptr};
        QThread* _loadThread{nullptr};
        bool _libraryMode{false};
        bool _closing{false};
        int _pageBeforeLoad{0};
        LoadOrderSnapshot _launchSnapshot;
        QVector<LibraryPlugin> _libraryPlugins;
        QListWidget* _partList{ nullptr };
        QLineEdit* _partSearch{ nullptr };
        QLabel* _configurationSummary{ nullptr };
        QTimer* _rebuildTimer{ nullptr };
        SlotOverlay* _slotOverlay{ nullptr };
        PartPicker* _partPicker{ nullptr };
        QHash<SlotKey, QPersistentModelIndex> _partRoots;
        QHash<SlotKey, QVector<QPersistentModelIndex>> _partSurfaces;
        // Append-only assembly block IDs, discarded on source/config rebuild.
        // Only models explicitly supplying their connection bone populate this.
        QHash<int, int> _partAlignmentAnchors;
        AttachmentConnections _attachmentConnections;
        QListWidget* _animationList{ nullptr };
        QPlainTextEdit* _details{ nullptr };
        QPushButton* _playButton{ nullptr };
        QCheckBox* _loopCheck{ nullptr };
        QCheckBox* _showArmsCheck{ nullptr };
        QComboBox* _motionMode{ nullptr };
        QCheckBox* _transitionCheck{ nullptr };
        QComboBox* _speedCombo{ nullptr };
        QSlider* _timeline{ nullptr };
        QLabel* _timeLabel{ nullptr };
        QTimer* _playbackTimer{ nullptr };

        PreparedPackage _package;
        ResourceArchive _packageResources;
        ResourceArchive _gameMeshes;
        ResourceArchive _gameAnimations;
        ResourceArchive _gameSounds;
        QVector<PluginContent> _plugins;
        QVector<WeaponSelection> _weaponSelections;
        mutable QHash<int, WeaponConfiguration> _configurations;
        QString _gameData;
        HkxSkeleton _skeleton;
        // GUI-thread caches; source models are never attached or mutated in place.
        QCache<QString, QSharedPointer<NifModel>> _nifCache{64};
        mutable QCache<QString, QByteArray> _nifBytesCache{128 * 1024};
        QCache<QString, HkxClip> _clipCache{64};
        QCache<QString, HkxBehaviorGraph> _graphCache{32};
        QString _geometrySignature;
        int _renderedWeapon{-1};
        int _sourceMisses{0};
        qint64 _copyMilliseconds{0};
        qint64 _pasteMilliseconds{0};
        qint64 _decodeMilliseconds{0};
        SlotKey _focusedSlot{0};
        HkxClip _clip;
        HkxClip _readyClip;
        QString _readyAnimationPath;
        QString _stateBaselineAnimationPath;
        QVector<AnimationAction> _animationActions;
        QStringList _animationSearchPaths;
        QVector<Transformf> _sampledTracks;
        QVector<Transformf> _localPose;
        QVector<Transformf> _globalPose;
        QVector<Transformf> _sourceLocalPose;
        QVector<Transformf> _sourceGlobalPose;
        QVector<Transformf> _readyLocalPose;
        QVector<Transformf> _readyGlobalPose;
        QString _poseError;
        QString _activeAction;
        QString _activeAnimationPath;
        QVector<Transformf> _stateBaselineLocalPose;
        QVector<ScenePoseBinding> _nodeBindings;
        QVector<QPersistentModelIndex> _sceneNodes;
        QVector<Transformf> _boundLocalPose;
        QVector<Transformf> _boundGlobalPose;
        QHash<QString, QPersistentModelIndex> _canonicalNodes;
        QPersistentModelIndex _weaponRoot;
        QPersistentModelIndex _bodyRoot;
        QPersistentModelIndex _handsRoot;
        int _bodyRootFlags{ 0 };
        int _handsRootFlags{ 0 };
        int _weaponBoneIndex{ -1 };
        float _clipBlendInSeconds{ 0.0f };
        float _clipBlendOutSeconds{ 0.0f };
        float _playbackTime{ 0.0f };
        float _playbackRate{ 1.0f };
        QElapsedTimer _playbackClock;
        bool _playing{ false };
    };
}
