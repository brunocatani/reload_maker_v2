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
#include "areas/collisions/CollisionWorkshop.h"
#include "areas/sources/SourceBrowserRows.h"
#include "commands/CommandRegistry.h"
#include "domain/checklist/Checklist.h"
#include "domain/detectors/DefaultSoundCues.h"
#include "viewport/Pins.h"
#include "domain/detectors/MagazinePieces.h"
#include "domain/weapon/WeaponOverrides.h"
#include "LoadTimings.h"

#include <QHash>
#include <QJsonArray>
#include <QCache>
#include <QSharedPointer>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QSet>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

class NifModel;
class Scene;
class StudioWindow;
class QCheckBox;
class QComboBox;
class QGridLayout;
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
class QStackedWidget;
class QTabBar;
class QFrame;
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
    class PinOverlay;
    struct PinMarker;
    class GripViewport;
    class MagazineViewport;
    class MagazineAuthoringViewport;
    struct GuideShape;
    struct MagazineGuide;
    class CollisionPanel;
    namespace design { class PanelHeader; class ChecklistRow; class IconTile; class TileChoice; class ElidedLabel; }
    struct PackageLoad;
    struct WeaponPreset;
    class ReloadMakerController final : public QObject
    {
        Q_OBJECT

    public:
        explicit ReloadMakerController(StudioWindow* window);
        ~ReloadMakerController() override;
        void showHome();
        // Reopens the source and weapon last worked on; their authoring comes
        // from the ESP library (areas/project/EspProject.cpp).
        void resumeLastSession();

        void openInitialPaths(const QString& packagePath, const QString& gameDataPath,
                              const QString& mo2Instance = {}, bool library = false);
        QJsonObject executeCommand(const QJsonObject& request);

    signals:
        void packJobChanged();

    private slots:
        void browsePackage();
        void rebuildWeapon();
        void togglePlayback();
        void replay();
        void advancePlayback();
        void scrubTimeline(int value);
        void capturePoseDiagnostics();
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
            QPersistentModelIndex connectionParent;
            Vec3f translation{};
            Quaternionf rotation{};
            float scale{ 1.0f };
            int suppliedBone{ -1 };
            int mountedBone{ -1 }; // incoming copy stays a static bridge to this parent bone
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
        bool placeReceiver(NifModel& destination, const ObjectModRecord& receiver, NifModel& receiverModel, const ConnectTarget& target,
                           const QString& point, QString& error);
        int _baseNodesReplaced{ 0 };
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
        // CollisionsWorkspace is the builder's loose magazine world models;
        // WeaponCollisionWorkspace the whole weapon and its installed parts;
        // MagazinesWorkspace the builder's magazine pieces and insertion guide,
        // on Operate's simulation without its drag controls.
        enum Workspace : int { GripsWorkspace = 2, PhasesWorkspace = 4, OperateWorkspace = 5, SoundsWorkspace = 6, CollisionsWorkspace = 7, ScopesWorkspace = 8, WeaponCollisionWorkspace = 9, MagazinesWorkspace = 10 };
        static bool simulationWorkspace(int value) { return value==OperateWorkspace||value==SoundsWorkspace||value==MagazinesWorkspace; }
        static bool collisionWorkspace(int value) { return value == CollisionsWorkspace || value == WeaponCollisionWorkspace; }
        static constexpr int kMaximumDiagnosticLines = 3000;
        // Hands editor pages, in tab order; saved in projects as handsPage.
        enum class HandEditorPage : int { FingerPoses = 0, PartGrip = 1, Mechanism = 2, GrabLocations = 3 };
        // Rail pages and Reload builder steps, derived from the workspace and
        // hand editor page (src/areas/shell/RailPages.cpp).
        enum class RailPage : int { Operate, Builder, Poses, Sounds, Scopes };
        enum class BuilderStep : int { Reload, PartGrips, Mechanisms, GrabPoints, Magazines };
        // Reload builder › Magazines sub-steps (StudioMagazines.cpp): Pieces and
        // Insertion guide are MagazinesWorkspace; Hand fit is the magazine's part
        // grip editor and World models the collisions page. Also MCP's
        // workspace magazineStep.
        enum class MagazineStep : int { Pieces, Guide, HandFit, WorldModels };
        [[nodiscard]] std::optional<RailPage> railPage() const;
        [[nodiscard]] std::optional<BuilderStep> builderStep() const;
        [[nodiscard]] std::optional<MagazineStep> magazineStep() const;
        void showMagazineStep(MagazineStep step);
        void showRailPage(RailPage page);
        void showBuilderStep(BuilderStep step);
        void syncRail();
        static constexpr int kPoseNameLength = 128;
        // The weapon pill's popover: source, plugin and weapon pickers.
        void createSourcePicker();
        void showSourcePicker();
        void refreshSourceBar();
        // Header pill, configuration summary and preset chip.
        void refreshHeader();
        void createViewBar();
        void refreshWeaponPicker();
        void offerPluginChoice();
        void restoreReloadClip();
        [[nodiscard]] bool mechanismPageActive() const;
        [[nodiscard]] static bool validWorkspace(int workspace);
        void createUi();
        void setWorkspace(int workspace);
        [[nodiscard]] bool loadoutPageActive() const;
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
        void showCollisionSubject();
        void invalidateCollisions();
        CollisionProfile collisionSnapshot(int target,bool referenceOnly,bool captureRender=false);
        void paintCollisions();
        QVector<int> _collisionTargets;
        CollisionPanel* _collisions{};
        // The collision document (src/areas/collisions); _collisions shows it.
        CollisionWorkshop _collisionWorkshop;
        QDockWidget* _collisionsDock{};
        void createSoundsUi();
        void prepareSounds();
        // The game's own fire sound for the configured weapon (FireSound.cpp).
        void resolveFireSound();
        bool playFireSound();
        QJsonObject fireSoundState() const;
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
        // Package masters' sound descriptors; every descriptor in reach, and
        // the cue catalog derived from them, by lower-case record identity.
        QHash<QString,SoundDescriptor> _referenceSounds,_soundDescriptors;
        QHash<QString,SoundCue> _soundCatalog;
        // Package masters' sound keyword mappings and Sound keywords, by
        // lower-case record identity (the MO2 library holds them in _plugins).
        QVector<SoundMapping> _referenceSoundMappings;
        QSet<QString> _referenceSoundKeywords;
        // Played once per shot in Operate; never part of the exported pack,
        // because the game plays its own fire sound.
        FireSoundPlan _fireSound;
        QString _fireSoundMapping,_fireSoundError;
        QStringList _fireSoundContext;
        void playShotSound();
        void endFireLoop();
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
        QLabel* _fireSoundInfo{};
        void createFingerEditor();
        void refreshFingerEditor();
        void editFinger(int joint, const Quaternionf& rotation);
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
        // Operate draws parts whose authored motion differs from the source
        // clip a second time at their native pose (rework 03 §5). A view
        // option, not part of the document.
        bool _nativeGhost{ false };
        // Native weapon-local poses of the authored operating tracks posed this
        // frame, by scene node; written while the targets are collected.
        mutable QHash<int, Transformf> _ghostNatives;
        QHash<int, Transformf> _simulationStaticLocalPose;
        bool _weaponFixedMotion{ true };      // the preview holds the weapon frame still
        bool _approximateTransitions{ true }; // clip entry and exit blends
        bool _armsShown{ true };
        [[nodiscard]] QString motionModeName() const;
        void setSlideSpringReturn(bool enabled);
        void setNativeGhost(bool enabled);
        void setWeaponFixedMotion(bool fixed);
        void showWeaponFixedMotion();
        void setApproximateTransitions(bool enabled);
        void setArmsShown(bool shown);
        void setGlockNoExtender(bool enabled);
        void refreshPoseMode();
        [[nodiscard]] static bool isArmsBody(const QString& body);
        void setArmsBody(const QString& body);
        void showArmsBody();
        QString sourcePolicyKey() const;
        [[nodiscard]] bool sourcePluginLoaded(const QString& plugin) const;
        QJsonObject sourcePolicyState() const;
        QJsonObject configureSourcePolicy(const QJsonObject&);
        void showSourcePolicy();
        QJsonObject _sourcePolicies;
        // Weapon records (src/areas/project/ProjectFiles.cpp). One weapon's
        // complete editor state: its authored part, drafts and view state.
        // Explicit saves only (owner, 2026-10-05): the authored part, never
        // the camera, part selection or playback, is compared with what the
        // committed version produced.
        [[nodiscard]] QJsonObject authoredRecord() const;
        [[nodiscard]] static QJsonObject authoredPart(QJsonObject record);
        [[nodiscard]] static QJsonObject sessionPart(const QJsonObject& record);
        [[nodiscard]] static QJsonObject versionRecord(const QJsonObject& record);
        // Per-weapon session (src/areas/project/WeaponSession.cpp).
        [[nodiscard]] QString sessionFile(const QString& plugin) const;
        [[nodiscard]] QJsonObject weaponSession(const QString& record) const;
        void keepWeaponSession();
        [[nodiscard]] bool documentDirty() const;
        void markDocumentClean();
        void refreshDocumentModified();
        QJsonObject _savedAuthoring;
        bool _modifiedRefreshQueued{ false };
        QJsonObject animationSourceIdentity() const;
        [[nodiscard]] QJsonObject projectRecord() const;
        bool appendProjectDrafts(QJsonObject& record, QString& message) const;
        [[nodiscard]] static bool validWeaponRecord(const QJsonObject& record, QString& message);
        // After a source is indexed: select the requested weapon, then open
        // its authoring (and apply an explicitly imported record).
        void restoreSessionSelection();
        bool selectProjectWeapon(const QJsonObject& record);
        void restoreWeaponRecord(const QJsonObject& record, const QJsonObject& baseline);
        void restoreProjectLoadout(const QJsonObject& record);
        void restoreProjectAuthoring(const QJsonObject& record);
        void restorePins(const QJsonObject& record);
        [[nodiscard]] QJsonArray pinsJson() const;
        void restoreProjectPose(const QJsonObject& record, QString& error);
        void restoreProjectView(const QJsonObject& record);

        // The ESP-owned working project (src/areas/project/EspProject.cpp).
        // Every authored weapon belongs to the plugin that defines its record.
        // Entering a plugin's weapons loads that ESP's latest committed
        // version once; switching between its weapons keeps each weapon's
        // unsaved edits in memory. Only a deliberate Save commits a new
        // version, and it commits every weapon of the ESP together.
        struct EspWeaponWork
        {
            QJsonObject record;    // the complete editor record while off screen; empty until visited
            QJsonObject committed; // the authored part as committed: the dirty baseline
            QJsonObject status;    // checklist progress per configuration
            bool dirty{ false };
            bool imported{ false }; // replaced by an explicit import; unsaved until Save
            bool unreadable{ false }; // its committed authoring could not be read; Save keeps that entry
        };
        struct EspWork
        {
            QString plugin;        // the originating plugin, as its file is spelled
            QString base;          // the committed version these edits start from; empty for none
            QJsonObject snapshot;  // that version
            QHash<QString, EspWeaponWork> weapons; // by lower-case record
        };
        std::optional<EspWork> _esp;
        bool _openSettling{ false }; // a weapon opened from a saved version is still assembling
        // Capturing the edited weapons' configurations, then committing.
        struct EspSaveJob
        {
            QString id;
            QString folder;
            QString note;
            QStringList queue;     // weapons still to capture
            QString returnWeapon;  // on screen when the save started
            QHash<QString, QJsonArray> exceptions;
            QSet<QString> captured;
            QHash<QString, QString> captureFolders;
            QJsonObject sent;      // record -> the authored part handed to the save
            bool returned{ false }; // back on the author's weapon after the captures
            int started{ 0 };       // weapons whose captures began
            bool committing{ false };
        };
        std::optional<EspSaveJob> _espSave;
        // Run after the Save chosen in a Save / Discard / Cancel prompt succeeds.
        std::function<void()> _afterSave;
        [[nodiscard]] QString weaponRecordKey() const;
        [[nodiscard]] bool espBusy() const;
        [[nodiscard]] bool espDirty() const;
        [[nodiscard]] QStringList espDirtyWeapons() const;
        bool changeWeapon(int index);
        bool showWorkingWeapon(const QString& record, bool stash = true);
        void openWeaponAuthoring();
        void stashDisplayedWeapon();
        void applyEspModelOverrides();
        void settleOpenedWeapon(const QString& weapon);
        [[nodiscard]] QJsonObject mergedWeaponStatus(QJsonObject status) const;
        // Leaving the working project: unsaved edits ask Save / Discard /
        // Cancel in the editor (Save runs `retry` once it has committed) and
        // are discarded in an unattended run, whose scripts save explicitly.
        bool leaveDocument(const QString& action, std::function<void()> retry = {});
        QJsonObject espState() const;
        QJsonObject startEspSave(const QJsonObject& request);
        void continueEspSave();
        void commitEspSave();
        void finishEspSave(bool success, const QJsonObject& result);
        QJsonObject loadEspVersion(const QString& save);
        QJsonObject deleteEspVersion(const QString& save);
        void finishEspDelete(bool success, const QJsonObject& result);
        QJsonObject importLegacyProject(const QString& path);
        QJsonObject exportWeaponProject(const QString& path);
        QJsonObject assignMagazineModels(const QJsonObject& request);
        QJsonObject espExport(const QJsonObject& request);
        void rememberLastSession();
        void showEspVersions();
        void showMagazineModels();
        mutable QHash<QString, QJsonObject> _espIndexCache;
        QJsonObject espIndex(const QString& plugin) const;

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
        void registerAnnotationCommands();
        QJsonObject diagnosticsState() const;
        QJsonObject poseState() const;
        QJsonObject importAuthoredRecords(const QJsonArray& paths);
        QJsonObject exportRecord(const QString& kind);
        QByteArray exportReloadBundle(QString& error);
        void configureLoadout(const QJsonObject& params);
        void controlPlayback(const QJsonObject& params);
        void arrangeView(const QJsonObject& params);
        QJsonObject renderView(const QJsonObject& params);
        QJsonObject captureWindow();
        QJsonObject simulateOperation(const QJsonObject& params);
        void moveSimulatedMagazine(const QJsonObject& params);
        void editFingerJoints(const QJsonObject& params);
        void saveHandPoseFromCommand(const QJsonObject& params);
        void setGripPlacement(const QJsonObject& params);
        QJsonObject mechanismOperation(const QJsonObject& params);
        void setMechanismStage(const QJsonObject& params);
        void soundOperation(const QJsonObject& params);
        void editSoundCueFromCommand(int index, const QJsonObject& params);

        QJsonObject studioState() const;
        QJsonObject installationState() const;
        bool configureInstallation(const QJsonObject&);
        void showSetup();
        // Which saved projects an export window exports: the ones ticked in
        // its list, the ESP open in the picker, or every saved ESP project
        // with weapons in the loaded source (reloads and scope profiles).
        enum class ExportScope { Chosen, OpenEsp, Everything };
        void showPackBuilder(ExportScope scope = ExportScope::Chosen);
        void showCollisionCooker();
        QJsonObject capturePreparedPart(const QString& output);
        QJsonObject captureMagazineWorld(bool empty);
        std::optional<CollisionProfile> _magazineWorldSnapshot;
        bool _magazineWorldPreview{false};
        QJsonObject _preparedPart;
        // Opens the reload builder on its checklist (Main menu, Home).
        void showChecklist();
    public:
        // The weapon's checklist (areas/checklist): items from its preset's
        // weapon model, detected and confirmed. `evaluated` is false until the
        // weapon, its learned reload and its operating parts are available.
        struct ChecklistView
        {
            const WeaponPreset* preset{ nullptr };
            QVector<ChecklistItemStatus> items;
            ChecklistSummary summary;
            bool evaluated{ false };
            QString problem;
        };
        ChecklistView checklistView() const;
        QJsonObject checklistState() const;
        QJsonObject weaponPropertiesState() const;
        QJsonObject checklistCommand(const QJsonObject& request);
    private:
        const WeaponPreset* checklistPreset() const;
        QHash<QString, ChecklistObservation> checklistObservations(const WeaponPreset& preset) const;
        void prepareChecklistEvidence();
        bool buildOperatingRig(QString& error);
        // The learned reload whose rig could not be built, and why; the
        // checklist does not retry it until the reload is learned again.
        QString _checklistRigRecipe, _checklistRigError;
        void createChecklistPanel();
        void refreshChecklistUi();
        void runChecklistCommand(const QJsonObject& request);
        QString checklistRefreshKey() const;
        // Checklist progress by lower-case weapon record: the working
        // project's weapons, then each loaded plugin's latest version, so the
        // weapon list shows complete, draft and untouched weapons.
        QHash<QString, WeaponProgress> weaponProgressIndex() const;
        QWidget* _checklistList{nullptr};
        QLabel* _checklistCount{nullptr};
        QProgressBar* _checklistProgress{nullptr};
        QLabel* _checklistBanner{nullptr};
        QLabel* _checklistFooter{nullptr}; // status footer counts while the checklist shows
        QButtonGroup* _checklistItemButtons{nullptr};
        std::array<design::ChecklistRow*, 6> _checklistSectionRows{};
        std::array<QWidget*, 6> _checklistSectionBoxes{};
        QHash<QString, design::ChecklistRow*> _checklistItemRows;
        QStringList _checklistItemIds;
        QString _selectedChecklistItem;
        bool _checklistFireOpen{ false };
        QString _checklistShownKey;
        QTimer* _checklistTimer{nullptr};
        QFrame* _checklistCard{nullptr};
        QLabel* _checklistCardSection{nullptr};
        QLabel* _checklistCardTitle{nullptr};
        QLabel* _checklistCardState{nullptr};
        QLabel* _checklistCardOwner{nullptr};
        QLabel* _checklistCardValue{nullptr};
        QLabel* _checklistCardEvidence{nullptr};
        QLabel* _checklistCardMessage{nullptr};
        QComboBox* _checklistPresetChoice{nullptr};
        QPushButton* _checklistUsePreset{nullptr};
        QPushButton* _checklistSkip{nullptr};
        QPushButton* _checklistUndo{nullptr};
        QPushButton* _checklistOpen{nullptr};
        QJsonObject homeState() const;
        QJsonObject chooseHomeRole(const QString&);
        bool openBaseGame();
        // Authoring records (areas/project/AuthoringRecords.cpp): one per
        // reload configuration, keyed by the weapon and its reload parts
        // (receiver, separate operating part, magazine; _reloadAxes).
        // Projects save them under "authoringRoadmaps", the name of the retired
        // 12-step roadmap that first kept them.
        QString authoringRecordKey() const;
        bool savedOperatingSetup(OperatingSetup& setup, QString& error) const;
        void rememberCollisionDraft();
        QString assignedTemplate() const;
        void assignReloadPreset(const QString& id);
        QJsonObject _authoringRecords;
        QJsonObject _reloadTemplateAssignments;
        std::optional<WeaponMechanism> _assignedMechanism;
        // Weapon properties (areas/properties): the author's per-weapon
        // overrides by weapon record, and the values the code reads.
        QHash<QString, WeaponOverrides> _weaponOverrides;
        // The preset Maker proposes from the records until one is assigned,
        // kept for the configuration it was read from.
        struct ProposedPreset
        {
            QString configuration, preset, evidence, problem;
        };
        mutable ProposedPreset _presetProposal;
        const ProposedPreset& presetProposal() const;
        const WeaponPreset* proposedPreset() const;
        WeaponOverrides currentWeaponOverrides() const;
        InsertionPath magazineInsertionPath() const;
        bool magazineHandAllowed(PoseHand hand) const;
        PoseHand defaultMagazineHand() const;
        QString magazineHandRule() const;
        void setWeaponProperty(const QString& name, const QJsonValue& value);
        QJsonObject packCommand(const QJsonObject&);
        QProcess* _packProcess{};
        QJsonObject _packJob;
        QString _packCancelFile;
        bool connectInstalledProfile();
        bool handOffToMo2(const InstallationSetup& setup, const InstallationCheck& check);
        void cacheWeaponFitBounds();
        bool fitWeapon();
        QVector<QVector3D> _weaponFitCorners;
        void createSimulationUi();
        bool authorReloadGrips();
        void createSimulationViewport();
        bool isAr15Configuration() const;
        bool isM1AConfiguration() const;
        WeaponMechanism simulationMechanism() const;
        bool isBoltActionConfiguration() const;
        bool prepareBoltActionMotion();
        void applyBoltActionTargets(QHash<int,Transformf>& targets) const;
        QJsonObject exportBoltActionMotion() const;
        BoltCycle _boltCycle;
        QVector<Transformf> _boltFiredPose;
        QJsonObject _boltCycleSource;
        bool isRifleConfiguration() const;
        QString currentTestbed();
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
        // The fire cycle replays the reload's bolt stroke (the fire clip has none).
        bool _rifleBoltFromReloadStroke{ false };
        SlotKey _simulationMagazineSlot{0};
        int _m1aEmptyMagazineFrame{0};
        int _m1aLoadedMagazineFrame{0};
        HandPosePlacement simulationMagazineAnchor() const;
        bool magazineAnchorDecides() const;
        Transformf _magazineGrabBridge{};
        QString noExtenderReloadPath() const;
        void prepareSimulation();
        void stopSimulation();
        void updateSimulationControls();
        void runSimulationAction(const QString& action);
        bool selectSimulationClip(bool fire, bool reload);
        bool applySimulationPose();
        void refreshNativeGhost(const QHash<int, Transformf>& targets);
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
        QJsonObject magazineSetupState() const;
        QJsonObject magazineReferenceState() const;
        QJsonObject configureMagazineReference(const QJsonObject&);
        void showMagazineReference();
        bool magazineReferenceCandidate(int anchor) const;
        QJsonObject configureMagazineSetup(const QJsonObject&);
        // Rounds hidden when the magazine is empty (src/areas/magazine/
        // MagazineRounds.cpp): per magazine record and model, the pieces whose
        // round role the author changed from the detected one, keyed by their
        // path from the full carrier. A saved magazine setup decides instead.
        QHash<QString,QJsonObject> _magazineRounds;
        // Finished full/empty world models assigned to a magazine, by
        // lower-case "weapon|magazine". Saving copies them into the ESP project.
        QHash<QString,QJsonObject> _magazineModels;
        QString magazineRoundsKey() const;
        QString magazineRoundPath(const QModelIndex& node, const QModelIndex& carrier) const;
        QJsonArray magazineRoundPieces() const;
        void applyMagazineRounds(const MagazineRail& rail);
        QJsonObject magazinePresentationJson(const QJsonObject& setup) const;
        QJsonObject magazineRoundsState() const;
        QJsonObject configureMagazineRounds(const QJsonObject&);
        // The magazine at the end of its guide, full or empty, on any
        // simulation page.
        void presentMagazineOutside(bool empty);
        // Reload builder › Magazines (StudioMagazines.cpp, areas/magazine/
        // MagazineAuthoring.cpp).
        void createMagazinesUi();
        QWidget* createMagazineStepBar(QWidget* parent);
        void syncMagazineStepBars();
        void refreshMagazinesUi();
        void presentMagazineStep();
        void updateMagazineAuthoring();
        // The magazine's pieces with role, presentation, block and name.
        QJsonArray magazinePieceRows() const;
        // Throws when the role cannot be used; also MCP's magazine_setup role.
        void setMagazinePieceRole(const QString& path, const QString& role);
        void setMagazineAdvanced(const QJsonObject& changes);
        void restoreMagazinePieces();
        [[nodiscard]] std::optional<GuideShape> magazineGuideShape() const;
        void editMagazineGuide(const GuideShape& shape, bool commit);
        // A typed guide (direction in the connection's frame): applied live,
        // or saved and prepared again when there is no rail. Also MCP's
        // magazine_guide edit.
        void applyMagazineGuideEdit(const MagazineGuide& guide);
        void typeMagazineGuide();
        void testMagazineGuide();
        void leaveMagazineGripEditing();
        MagazineStep _magazineStep{ MagazineStep::Pieces }; // view memory, not saved
        bool _magazineShowsEmpty{false};
        QString _selectedMagazinePiece;
        QDockWidget* _magazinesDock{nullptr};
        design::PanelHeader* _magazinesHeader{nullptr};
        QStackedWidget* _magazinePages{nullptr};
        QVector<QButtonGroup*> _magazineStepGroups;
        QListWidget* _magazinePieceList{nullptr};
        QButtonGroup* _magazinePresentation{nullptr};
        QButtonGroup* _magazinePieceRole{nullptr};
        QLabel* _magazinePiecesStatus{nullptr};
        QComboBox* _magazineEmptyBody{nullptr};
        QComboBox* _magazineMotion{nullptr};
        QComboBox* _magazineMovement{nullptr};
        QPushButton* _magazineRestorePieces{nullptr};
        std::array<QDoubleSpinBox*,3> _guideDirection{};
        QDoubleSpinBox* _guideTravel{nullptr};
        QDoubleSpinBox* _guidePickup{nullptr};
        QDoubleSpinBox* _guideRelease{nullptr};
        QLabel* _guideStatus{nullptr};
        QPushButton* _guideTest{nullptr};
        QPushButton* _guideRestore{nullptr};
        QWidget* _partGripMagazineSteps{nullptr};
        MagazineAuthoringViewport* _magazineAuthoring{nullptr};
        // The guide as a handle drag found it; a drag that ends there writes nothing.
        QJsonObject _guideEditStart;
        // The library's ESP projects with their latest versions' weapons
        // (StudioCoverage.cpp); read again after every save, delete or export.
        QJsonObject packCoverageState(bool refresh=false);
        void showPackCoverage();
        QString selectionRecordKey(int selection) const;
        QString coverageLabel(const QString& record) const;
        void updatePackCoverage();
        QJsonObject _packCoverage;
        QJsonObject _weaponLibraryReport;
        QHash<QString,QString> _coverageLabels;
        QString magazinePiecePath(const QModelIndex&) const;
        bool sampledMagazinePayload() const;
        bool buildSourceSimulationRig(SimulationRig&,QString&);
        bool buildConfiguredSimulationRig(const OperatingSetup&,SimulationRig&,QString&);
        QJsonObject exportSlideProfile() const;
        void prepareMagazineInteraction();
        // Steps of prepareMagazineInteraction (src/areas/magazine).
        struct MagazineConnection
        {
            QString name;
            Transformf frame;
            bool authored{ false };
        };
        struct MagazineShape
        {
            QPersistentModelIndex node;
            int track{ -1 };
        };
        bool loadMagazineSetup();
        bool resolveMagazineConnection(MagazineConnection& connection);
        bool collectMagazineGeometry(QVector<QVector3D>& body, QVector<QVector3D>& frame, QVector<MagazineShape>& shapes);
        // How the magazine sits in the weapon (rework 14, items 5 and 7): a
        // part attached inside the carrier bone, a magazine model that brings
        // its own carrier bone, or geometry built into the receiver's carrier.
        enum class MagazineMount { Installed, SuppliesCarrier, BuiltIn };
        struct MagazineMountInfo
        {
            MagazineMount mount{ MagazineMount::SuppliesCarrier };
            QString evidence;
        };
        MagazineMountInfo magazineMount(const QModelIndex& carrier) const;
        bool magazineBuiltIn() const;
        QString parkedMagazinePath() const;
        PieceRoleProposal magazinePieceProposal(const QModelIndex& shape) const;
        bool buildMagazineMotion(const QVector<QVector3D>& body, MagazineRail& rail);
        bool buildNativeMagazineMotion(const QVector<QVector3D>& body, MagazineRail& rail);
        void classifyMagazineShapes(const QVector<MagazineShape>& shapes, const MagazineRail& rail, bool& emptyBody, bool& fullBody);
        bool applyMagazinePieceRoles(const MagazineRail& rail);
        bool magazineSettingsBusy() const;
        void checkMagazineSetup(const MagazineSetup& value) const;
        void moveFreeMagazine(QVector3D offset);
        void moveMagazineRail(float distance);
        void cancelMagazine();
        void updateMagazinePose();
        std::optional<MagazineRail> _magazineRail;
        QString _magazineError;
        // parked: a native spare outside the magazine that the runtime hides.
        struct MagazineVisual {QPersistentModelIndex node;int flags{0};bool empty{false};int track{-1};bool cartridge{false};bool nativeOnly{false};bool parked{false};};
        QVector<MagazineVisual> _magazineVisuals;
        // Steps of the Operate simulation (src/areas/operate/).
        bool buildBoltDrivenRig(SimulationRig& rig, QString& error);
        bool manualBoltFromReload() const;
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
        Vec3f operatingControllerStroke() const;
        void addSlideTargets(QHash<int, Transformf>& targets, bool holdOpen) const;
        [[nodiscard]] Transformf operatingTrackTarget(int track, float progress) const;
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
        // Movement timing and provenance an operating setup mode supplies.
        struct ConfiguredMotion
        {
            float duration{ 0 };
            float ejectSeconds{ -1 };
            QJsonObject provenance;
        };
        bool sampleSetupClip(const OperatingSetup& setup, SimulationParts& parts, ConfiguredMotion& motion, QString& error);
        bool composeManualMovements(const OperatingSetup& setup, SimulationParts& parts, ConfiguredMotion& motion, QString& error);
        bool composeManualMovement(const OperatingSetup& setup, int track, const QString& movementId, SimulationParts& parts,
                                   QJsonArray& definitions, QString& error);
        bool reverseSetupWindow(SimulationParts& parts, ConfiguredMotion& motion, QString& error);
        bool readReloadSource(const QString& expected, ReloadRecipe& recipe, QString& error);
        bool addReloadTracks(ReloadRecipe& recipe, QString& error);
        [[nodiscard]] QSet<int> weaponGeometryBlocks() const;
        void finishReloadCapture(QString& error);
        void resetReloadPage();
        void showReloadCaptureStarted(const QString& expected);
        std::optional<MagazineSetup> _magazineSetup;
        MagazineViewport* _magazineViewport{nullptr};
        design::IconTile* _magazineOut{nullptr};
        design::IconTile* _magazineIn{nullptr};
        design::IconTile* _magazineGrab{nullptr};
        QDockWidget* _simulationDock{nullptr};
        QGridLayout* _simulationActionGrid{nullptr};
        QGridLayout* _simulationOptionGrid{nullptr};
        design::IconTile* _simulationFire{nullptr};
        design::IconTile* _simulationReload{nullptr};
        design::IconTile* _simulationDrop{nullptr};
        design::IconTile* _simulationReset{nullptr};
        QLabel* _simulationStatus{nullptr};
        QLabel* _simulationParts{nullptr};
        QLabel* _simulationInstructions{nullptr};
        design::IconTile* _simulationNoExtender{nullptr};
        QLabel* _simulationSource{nullptr};
        bool _glockNoExtender{false};
        float _glockReloadRackAt{-1}, _glockReloadFeedAt{-1};
        design::IconTile* _simulationSpring{nullptr};
        design::IconTile* _simulationGhost{nullptr};
        QTimer* _simulationTimer{nullptr};
        QElapsedTimer _simulationClock;
        ReloadDriveViewport* _simulationDrive{nullptr};
        std::optional<SimulationRig> _simulationRig;
        WeaponSimulation _simulation;
        bool _simulationActive{false};
        QString _simulationError;
        WeaponAmmunition _simulationAmmo;
        QHash<QString,QString> _referenceNames;
        design::IconTile* _simulationSlideRelease{nullptr};
        QVector<std::pair<QPersistentModelIndex,int>> _simulationVisibility;
        QButtonGroup* _railButtons{nullptr};
        QButtonGroup* _builderStepButtons{nullptr};
        QWidget* _modeSwitch{nullptr};
        QPushButton* _weaponPill{nullptr};
        design::ElidedLabel* _headerSummary{nullptr};
        design::ElidedLabel* _presetChip{nullptr};
        QFrame* _sourcePicker{nullptr};
        // Left panel: Checklist | Loadout tabs in Reload builder, else the loadout.
        QTabBar* _leftTabs{nullptr};
        QStackedWidget* _leftStack{nullptr};
        QWidget* _checklistPanel{nullptr};
        QDockWidget* _reloadDock{nullptr};
        design::PanelHeader* _handPoseHeader{nullptr};
        design::PanelHeader* _collisionsHeader{nullptr};
        QLabel* _clipPathLabel{nullptr};
        BuilderStep _lastBuilderStep{ BuilderStep::Reload }; // view memory, not saved
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
        // What to select once a source is indexed: {weapon, browserPlugin,
        // workspace} and, for an explicit import, the imported record.
        QJsonObject _pendingSession;
        QString _lastError;
        QComboBox* _fingerJoint{nullptr};
        std::array<QDoubleSpinBox*,3> _fingerAngles{};
        bool _fingerRefreshing{false};
        std::array<std::optional<HandPose>,2> _fingerDrafts;
        void createHandPoseUi();
        void createPartGripUi();
        void createMechanismUi();
        void createReloadUi();
        void setReloadWorkspace(bool active);
        void invalidateReloadCapture();
        void startReloadCapture();
        void advanceReloadCapture();
        void showReloadRecipe();
        void setReloadPlaying(bool playing);
        void seekReload(float seconds);
        void tickReloadPreview();
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
        design::TileChoice* _interactionHandPicker{nullptr};
        design::TileChoice* _magazineHandPicker{nullptr};
        // Assignments supply finger locals only; operating wrists stay live.
        QHash<QString,QString> _savedOperatingGrips;
        std::array<std::optional<HandPose>,2> _operatingFingers;
        QString _operatingFingerError;
        QComboBox* _operatingFingerPicker{};
        QString operatingGripKey(PoseHand hand, bool modelOverride) const;
        void resolveOperatingFingers();
        bool useOperatingFingers(const QString& id);
        QLabel* _operatingGripStatus{nullptr};
        bool setInteractionHand(PoseHand hand);
        void createGrabLocationsUi();
        bool openGrabLocations();
        void prepareGrabLocationFrame();
        // The learned reload and the operating rig, built without Operate's
        // simulation; false (with the reason in the status) while unavailable.
        bool ensureOperatingRig();
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
        design::TileChoice* _grabLocationHands{};
        QLabel* _grabLocationStatus{};
        bool _editingGrabLocationUi{},_pickingGrabLocation{},_replaceGrabLocation{};
        QString operatingGroup() const;
        int operatingAnchor() const;
        bool operatingSurfaceCandidate(int anchor) const;
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
        // Hands editor document (src/areas/hands).
        // The hand editor opens on the part workshop (Mechanisms), the page the assembled
        // tabs have always left current.
        HandEditorPage _handEditorPage{ HandEditorPage::Mechanism };
        PoseHand _editedHand{ PoseHand::Right };
        int _fingerJointIndex{ 0 };
        int _handCaptureAnchor{ -1 };
        QString _handCaptureName;
        QString _handCaptureNotes;
        QString _selectedHandPoseId;
        // Saved poses are listed when their name, hand, weapon or notes
        // contain this text.
        QString _handPoseFilter;
        void setHandEditorPage(HandEditorPage page);
        void handEditorPageChanged();
        void setEditedHand(PoseHand hand);
        void setFingerJoint(int joint);
        void setHandCaptureAnchor(int anchor);
        void setHandCaptureName(const QString& name);
        void setHandCaptureNotes(const QString& notes);
        void selectHandPose(const QString& id);
        void setHandPoseFilter(const QString& text);
        [[nodiscard]] bool handPoseMatchesFilter(const HandPose& pose) const;
        [[nodiscard]] bool handPoseListed(const QString& id) const;
        void clearHandPoseAnchors();
        [[nodiscard]] bool handPoseFrameReady() const;
        [[nodiscard]] bool partGripPreviewActive() const;
        [[nodiscard]] bool canSaveHandPose() const;
        [[nodiscard]] bool canPreviewHandPose() const;
        bool collectHandPoseAnchors();
        void addAnimatedPartAnchor(const ScenePoseBinding& binding);
        void addHandPoseAnchor(const QModelIndex& node, const QString& kind, const QString& label, const QString& modelPath,
                               const QString& record, const QModelIndex& owner);
        [[nodiscard]] HandPose captureFingerDraft(PoseHand hand) const;
        // Hands editor views.
        void showHandEditorPage();
        void showEditedHand();
        void showFingerJoint();
        void showHandPoseAnchors();
        void showHandCaptureAnchor();
        void showHandCaptureName();
        void showHandCaptureNotes();
        void showSelectedHandPose();
        void showHandPoseFilter();
        // Part grip form document (src/areas/hands).
        QString _gripPoseId;
        int _gripAnchor{ -1 };
        QString _gripDraftName;
        QString _selectedGripId;
        // Older snapshots and magazine copies in no binding are listed only
        // on request.
        bool _showGripHistory{ false };
        void setGripPose(const QString& id);
        void setGripAnchor(int anchor);
        void setGripDraftName(const QString& name);
        void selectPartGrip(const QString& id);
        void setShowGripHistory(bool show);
        [[nodiscard]] bool partGripShown(const PartGrip& grip) const;
        [[nodiscard]] bool partGripListed(const QString& id) const;
        void followGripInForm(const QString& poseId, int anchor);
        [[nodiscard]] QString knownHandPoseId(const QString& id) const;
        [[nodiscard]] const HandPose* gripPose() const;
        [[nodiscard]] bool partGripFrameReady() const;
        [[nodiscard]] bool canStartPartGrip() const;
        [[nodiscard]] bool canPickGripSurface() const;
        [[nodiscard]] bool partGripEditable() const;
        [[nodiscard]] bool canSavePartGrip() const;
        [[nodiscard]] bool canLoadPartGrip() const;
        void applyGripPoseToDraft();
        void setGripPosition(int axis, float value);
        void setGripRotation(const QVector3D& eulerDegrees);
        [[nodiscard]] QVector<int> visibleWeaponShapes() const;
        void appendAncestorAnchor(QModelIndex node, QVector<int>& candidates) const;
        // Part grip form views.
        void showPartGripChoices();
        void showGripPose();
        void showGripAnchor();
        void showGripDraftName();
        void showSelectedPartGrip();
        void showGripHistory();
        void showGripStatus(const QString& text);
        bool convertToMagazineFrame(PartGrip& grip, int targetAnchor);
        void showMagazineGrabGrip(const PartGrip* selected);
        void showOperatingFingerChoices();
        // Mechanism workshop document (src/areas/mechanisms).
        int _mechanismStage{ -1 };
        QString _workshopPoseChoice;
        QString _selectedMechanismId;
        bool _mechanismHandShownOnGrab{ true };
        struct WorkshopPoseChoice
        {
            QString label;
            QString key;
        };
        // The definition form: name, the selected step's fields (motion is
        // the axis plus 3 for rotation) and the checked follower anchors.
        struct MechanismForm
        {
            QString name;
            QString forwardName;
            QString reverseName;
            int motion{ 0 };
            int release{ 0 };
            float from{ 0 };
            float to{ 0 };
            float returnRate{ 0 };
            Vec3f pivot{};
            QVector<int> followers;
        };
        // Whether a part keeps a fixed relation to the definition's part
        // over the clip, by anchor index.
        struct FollowerObservation
        {
            bool visible{ false };
            bool related{ false };
            float distance{ 0 };
            float angle{ 0 };
        };
        void selectMechanismStage(int stage);
        void normalizeMechanismStage();
        void setWorkshopPoseChoice(const QString& choice);
        void chooseWorkshopPose(const QString& choice);
        [[nodiscard]] QVector<WorkshopPoseChoice> workshopPoseChoices() const;
        [[nodiscard]] bool workshopPoseListed(const QString& choice) const;
        void selectMechanism(const QString& id);
        void setMechanismHandShownOnGrab(bool shown);
        void applyMechanismForm(const MechanismForm& form);
        [[nodiscard]] bool workshopFrameReady() const;
        [[nodiscard]] bool canSaveMechanism() const;
        [[nodiscard]] QVector<const PartMechanism*> weaponMechanisms() const;
        [[nodiscard]] bool mechanismListed(const QString& id) const;
        bool compileMechanismPreview(QString& error);
        [[nodiscard]] bool mechanismSkinOwned() const;
        void holdMechanismSlider();
        void slideMechanism(int value);
        void changeMechanismStep(int direction);
        void endMechanismTest();
        bool placeWorkshopFingerPose(const QString& id);
        void pickWorkshopPivot(const Vec3f& world);
        void pickWorkshopContact(const Vec3f& world, const QString& surface, const QVector<int>& candidates);
        void addMechanismStage();
        void removeMechanismStage();
        void moveMechanismStage(int delta);
        void chooseTeachingTool(bool rotation);
        void applyFollowerChange(const MechanismForm& form);
        void suggestMechanismFollowers();
        // Mechanism workshop views.
        [[nodiscard]] MechanismForm mechanismForm() const;
        void readMechanismForm();
        void showMechanismStageSummary(int stage);
        void showTaughtStage(int stage);
        void showFollowerObservations(const QVector<FollowerObservation>& observations);
        void showWorkshopPoseChoice();
        void showWorkshopHint(const QString& text);
        void showMechanismStage();
        void showMechanismChoices();
        void showSelectedMechanism();
        void showMechanismHandShownOnGrab();
        // Grab locations document (src/areas/grabpoints).
        int _selectedGrabLocation{ -1 };
        void selectGrabLocation(int index);
        void normalizeGrabLocationSelection();
        void editGrabLocationFields(OperatingGrabLocations& g, int i, const QJsonObject& p) const;
        void showSelectedGrabLocation();
        void showGrabLocationStatus(const QString& text);
        // Sounds document (src/areas/sounds).
        int _selectedSoundCue{ -1 };
        // The cue form: the original sound's record, the trigger, movement
        // progress and volume as fractions, and whether the cue plays.
        struct SoundCueForm
        {
            QString record;
            QString trigger;
            float progress{ 0 };
            float gain{ 1 };
            bool enabled{ false };
        };
        // The clips the default cues take their source times from.
        struct SoundCueSources
        {
            QString animation;
            QString cyclePath;
            HkxClip cycle;
        };
        void selectSoundCue(int index);
        void normalizeSoundCueSelection();
        void applySoundCueForm(const SoundCueForm& form);
        void setSoundsMuted(bool muted);
        void buildSoundCatalog();
        SoundCue findSoundByEditorId(const QString& edid);
        void addSoundCue(const SoundCueSources& sources, const char* id, const char* label, const char* trigger, float progress,
                         const QString& edid, bool bolt = false);
        [[nodiscard]] DefaultSoundCues defaultSoundCues(const SoundCueSources& sources) const;
        void addDefaultSoundCues(const SoundCueSources& sources, const DefaultSoundCues& defaults);
        void resolveSoundDescriptors();
        // Sounds views.
        [[nodiscard]] SoundCueForm soundCueForm() const;
        void showSoundsMuted();
        // Collision capture and world models (src/areas/collisions).
        [[nodiscard]] CollisionProfile collisionProfileIdentity(int target) const;
        void collectCollisionSources(CollisionProfile& p, int target, bool captureRender);
        bool findShapeOwner(const QModelIndex& node, const QModelIndex& targetNode, QModelIndex& owner, QStringList& path) const;
        [[nodiscard]] CollisionMesh collisionSource(const QModelIndex& node, const QModelIndex& owner, const QStringList& path,
                                                    const QString& weaponRecord) const;
        [[nodiscard]] CollisionProfile magazineWorldIdentity(bool empty, const HandPosePlacement& anchor, const QMatrix4x4& root) const;
        void collectMagazineWorldSources(CollisionProfile& captured, bool empty, const QModelIndex& carrierNode, const HandPosePlacement& anchor);
        [[nodiscard]] QJsonArray preparedPartMeshes(const CollisionProfile& profile, const QMatrix4x4& inverse) const;
        [[nodiscard]] QString preparedPartForCollision() const;
        void startCollisionCook(const QString& geometry, const QString& output);
        // Pack jobs and the runtime configuration export (src/areas/packs).
        [[nodiscard]] QStringList packToolArguments(QString& python, QString& id);
        void cancelPackJob();
        // Save all configurations (rework 06, src/areas/packs/SaveAllConfigurations.cpp).
        enum class ConfigurationRole { Receiver, OperatingPart, Magazine };
        struct ConfigurationChoice
        {
            ConfigurationRole role{ ConfigurationRole::Receiver };
            SlotKey slot{ 0 };
            std::uint32_t mod{ 0 };
        };
        using ConfigurationPlan = QVector<QVector<ConfigurationChoice>>;
        // The job runs on the GUI thread between events: each step assembles,
        // prepares or captures one configuration, then yields.
        struct SaveAllJob
        {
            QJsonObject request;                    // project, expected, id
            QString folder;                         // the captures, named in plan order
            ConfigurationPlan plan;
            int next{ 0 };
            bool assembled{ false };                // plan[next] is selected
            QSet<QString> captured;                 // selections already captured
            QHash<SlotKey, std::uint32_t> original; // the author's selection, restored at the end
            QJsonArray exceptions;
            // The weapon's checklist progress, evaluated again on every
            // configuration the save prepares.
            QJsonObject status;
            int captures{ 0 };
            bool cancelled{ false };
            // The ESP save's continuation; without one the job reports alone.
            std::function<void(SaveAllJob&)> finished;
        };
        std::optional<SaveAllJob> _saveAll;
        [[nodiscard]] ConfigurationPlan saveAllPlan() const;
        // The weapon's reload parts as slots: the receiver, a separate
        // operating part and the magazine. Authoring keys, Save all and the
        // pack all use them (areas/project/AuthoringRecords.cpp).
        using ReloadAxes = QVector<QPair<ConfigurationRole, SlotKey>>;
        [[nodiscard]] ReloadAxes reloadAxes() const;
        [[nodiscard]] ReloadAxes liveReloadAxes() const;
        void recordReloadAxes();
        [[nodiscard]] static QString configurationRoleName(ConfigurationRole role);
        // Weapon record -> [{role, slot id}], saved as "reloadAxes".
        QJsonObject _reloadAxes;
        void continueSaveAll();
        void captureSaveAllConfiguration(const QVector<ConfigurationChoice>& combination);
        void finishSaveAll();
        bool selectConfigurationRecords(const QStringList& records);
        [[nodiscard]] bool assemblyBusy() const;
        void startPackProcess(const QString& python, const QStringList& arguments, const QJsonObject& job);
        [[nodiscard]] QJsonObject runtimeOperatingProfile() const;
        [[nodiscard]] QVector<int> runtimeOperatingTracks() const;
        [[nodiscard]] QJsonObject runtimeOperatingTrack(int index) const;
        [[nodiscard]] bool runtimeFullTrack(int index) const;
        [[nodiscard]] bool runtimePayloadTrack(int index) const;
        [[nodiscard]] QJsonObject runtimeMagazine(const HandPosePlacement& anchor, const QString& weapon) const;
        void addRuntimeMagazineGrips(QJsonObject& magazine, const HandPosePlacement& anchor, const QString& weapon) const;
        void addRuntimeMagazineReference(QJsonObject& magazine, const HandPosePlacement& anchor) const;
        void addRuntimeMagazineParts(QJsonObject& magazine) const;
        void addRuntimeMagazineHiding(QJsonObject& magazine) const;
        [[nodiscard]] QJsonObject runtimeProfileHeader(const QString& weapon, bool concreteReceiver, const QString& receiverKey,
                                                       const QString& magazineKey, const QJsonObject& selection) const;
        // Scopes document (src/areas/scopes): the optics offered and the
        // selected one.
        struct ScopeTarget
        {
            int anchor{ 0 };
            QString label;
        };
        QVector<ScopeTarget> _scopeTargets;
        int _selectedScopeTarget{ -1 };
        void selectScopeTarget(int index);
        [[nodiscard]] int scopeTargetAnchor() const;
        [[nodiscard]] QString scopeTargetLabel() const;
        void importScopeProfile(const QString& path);
        // Scopes views.
        void showScopeTargets();
        void showSelectedScopeTarget();
        void startLibraryLoad(bool reuse = false);
        void installLibrary(LoadOrderLibrary& library, bool keepTextures = false);
        void setLoading(bool loading, const QString& message = {});
        // From setLoading(true) until setLoading(false); the load thread
        // starts just after the first and ends just before the second.
        bool _sourceLoading{ false };
        void startPackageLoad(const QString& path, const QString& dataPath, bool reuse = false);
        // Resident sources (areas/sources/ResidentSources.cpp).
        void takeCurrentPackage(PackageLoad& load);
        void parkCurrentPackage();
        std::shared_ptr<PackageLoad> takeResidentPackage(const QString& key);
        void evictResidentSources();
        void clearResidentSources();
        void reloadSourceFiles();
        QJsonObject residentState() const;
        QHash<QString, std::shared_ptr<PackageLoad>> _residentPackages; // by packageSourceKey
        QStringList _residentOrder;                                   // least recently used first
        QByteArray _packageStamp, _libraryStamp;                      // of the installed source
        [[nodiscard]] const ResourceArchive& referenceMeshes() const;
        void applyStyle();
        void setStatus(const QString& message, bool error = false);
        void appendDiagnostic(const QString& message);
        void clearDiagnostics();
        void refreshWeapons();
        void refreshAttachments();
        void showLoadoutChoices(SlotKey key);
        void focusSlot(SlotKey key);
        void showPartMenu(SlotKey key, QPoint globalPosition, QPointF viewportPoint);

        // Point-and-note pins (src/areas/project/Annotations.cpp).
        [[nodiscard]] std::optional<Pin> pinAnchorAt(QPointF point, QString& error) const;
        [[nodiscard]] QPersistentModelIndex pinShape(const Pin& pin) const;
        [[nodiscard]] PinMarker pinMarker(int index) const;
        [[nodiscard]] int pinIndex(const QString& id) const;
        void refreshPins();
        bool addPin(Pin anchor, const QString& text, QString& error);
        bool editPin(const QString& id, const QString& text, QString& error);
        bool setPinResolved(const QString& id, bool resolved, const QString& resolution, QString& error);
        bool removePin(const QString& id, QString& error);
        void selectPin(const QString& id);
        void showPinMenu(const QString& id, QPoint globalPosition);
        void askNewPin(const Pin& anchor);
        [[nodiscard]] QJsonObject annotationsState() const;
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
        // keepTextures: the same source is installed again, so its textures stay valid.
        void clearAssetCaches(bool keepTextures = false);
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
        [[nodiscard]] const ObjectModRecord* reloadHostObjectMod() const;
        [[nodiscard]] const ObjectModRecord* reloadReceiverObjectMod() const;
        [[nodiscard]] QVector<const ObjectModRecord*> configuredObjectMods() const;
        [[nodiscard]] QVector<std::uint32_t> activeAnimationKeywords() const;
        [[nodiscard]] QByteArray readAnimationResource(const QString& resourcePath,
                                                       QString& error) const;
        [[nodiscard]] QString resolveAnimationPath(const QString& graphAnimationPath) const;
        [[nodiscard]] QString animationSetRelativePath(const QString& path) const;
        [[nodiscard]] bool rebuildAnimationActions(QString& error);
        bool loadBehaviorGraph(const QString& requestedPath, int depth, HkxBehaviorGraph& combined,
                               QSet<QString>& visited, QString& error);
        QVector<AnimationAction> authoredGraphActions(const HkxBehaviorGraph& graph, const QString& readyResource);
        [[nodiscard]] QString normalizeNifPath(const QString& path) const;
        [[nodiscard]] bool loadNif(const ResourceArchive& archive, const QString& resourcePath,
                                   NifModel& destination, QString& error) const;
        [[nodiscard]] QSharedPointer<NifModel> sourceNif(const ResourceArchive& archive,
                                                       const QString& path, QString& error, bool firstPerson = false);
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
        [[nodiscard]] ConnectTarget findConnectTarget(const QString& childPoint, const NifModel* incoming, SlotKey slot,
                                                       bool waitForOwner = true) const;
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
        bool failPose();
        bool poseSkeleton();
        bool applyFingerOverrides();
        bool applyPartOverrides();
        bool writeScenePose(NifModel* model, Scene* scene);
        void updateTimelineText();
        void showClipDetails();

        StudioWindow* _window{ nullptr };
        QDockWidget* _packageDock{ nullptr };
        QDockWidget* _detailsDock{ nullptr };
        QDockWidget* _timelineDock{ nullptr };
        QDockWidget* _handPoseDock{nullptr};
        QLineEdit* _handPoseName{nullptr};
        QPlainTextEdit* _handPoseNotes{nullptr};
        design::TileChoice* _handPoseSide{nullptr};
        QComboBox* _handPoseAnchor{nullptr};
        QPushButton* _saveHandPose{nullptr};
        QPushButton* _previewHandPose{nullptr};
        QPushButton* _restoreHandPose{nullptr};
        QLineEdit* _handPoseSearch{nullptr};
        QListWidget* _handPoseList{nullptr};
        QPlainTextEdit* _handPoseDetails{nullptr};
        QLabel* _handPoseStatus{nullptr};
        // A reference part a hand can be placed against, with its label and
        // tooltip as every part list shows them.
        struct HandPoseAnchor {
            HandPosePlacement description;
            QPersistentModelIndex node;
            QString label;
            QString tooltip;
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
        std::optional<PartGrip> _operateSuspendedGrip;
        struct PartGripFrame {
            bool weapon{false};
            int binding{-1};
            Transformf bridge{};
            int node{-1};
        } _partGripFrame;
        bool _gripPreviewValid{false};
        QString _gripError;
        bool _reloadWorkspaceActive{false},_reloadCapturing{false},_reloadCaptureQueued{false};
        bool _reloadPlaying{false};
        bool _reloadOldWeaponFixed{true};
        int _reloadCaptureFrame{0};
        QDockWidget* _reloadGraphDock{nullptr};
        ReloadGraph* _reloadGraph{nullptr};
        QTimer* _reloadCaptureTimer{nullptr};
        QTimer* _reloadPreviewTimer{nullptr};
        QElapsedTimer _reloadClock;
        QLabel* _reloadSourceLabel{nullptr};
        QLabel* _reloadSummary{nullptr};
        QProgressBar* _reloadProgress{nullptr};
        QPushButton* _reloadAnalyze{nullptr};
        QPushButton* _reloadPlay{nullptr};
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
        QPushButton* _mechanismSuggest{nullptr};
        design::TileChoice* _operatingHandPicker{nullptr};
        QWidget* _magazineSetupPanel{nullptr};
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
        LoadOrderSnapshot _launchSnapshot;
        QVector<LibraryPlugin> _libraryPlugins;
        QListWidget* _partList{ nullptr };
        QLineEdit* _partSearch{ nullptr };
        QLabel* _configurationSummary{ nullptr };
        QTimer* _rebuildTimer{ nullptr };
        QTimer* _sessionTimer{ nullptr };
        // What the grip and mechanism editors opened with (an assigned or
        // saved grip, a pose placed on a part, a saved definition). A draft
        // equal to it is not unsaved work; opening an editor changes nothing.
        QByteArray _partGripOpened;
        QByteArray _mechanismOpened;
        [[nodiscard]] QByteArray partGripSnapshot(const PartGrip& grip) const;
        void markPartGripOpened();
        void markMechanismOpened();
        // The session last written for a weapon (keepWeaponSession).
        QString _keptSessionWeapon;
        QJsonObject _keptSession;
        SlotOverlay* _slotOverlay{ nullptr };
        PartPicker* _partPicker{ nullptr };
        PinOverlay* _pinOverlay{ nullptr };
        QVector<Pin> _pins;          // saved with the project, in display order
        QString _selectedPin;        // view state only
        QHash<SlotKey, QPersistentModelIndex> _partRoots;
        QHash<SlotKey, QVector<QPersistentModelIndex>> _partSurfaces;
        // Append-only assembly block IDs, discarded on source/config rebuild.
        // Only models explicitly supplying their connection bone populate this.
        QHash<int, int> _partAlignmentAnchors;
        QHash<int, QString> _attachmentRuntimeNames;
        QSet<int> _attachmentBoneAliases;
        QSet<int> _auxiliaryReloadNodes;
        AttachmentConnections _attachmentConnections;
        QListWidget* _animationList{ nullptr };
        QPlainTextEdit* _details{ nullptr };
        // The diagnostics log the details dock shows, oldest line first.
        QStringList _diagnosticLines;
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
        // Stage times of loads, rebuilds, reload learning and Operate
        // preparation (MCP load_timings).
        LoadTimings _loadTimings;
        QElapsedTimer _reloadCaptureClock;
        qint64 _reloadSamplingMilliseconds{0};
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
        // Bones createBoneNode added from the skeleton: absent from every
        // loaded model, so they do not exist in the game.
        QSet<QPersistentModelIndex> _synthesizedBones;
        QPersistentModelIndex _weaponRoot;
        QPersistentModelIndex _bodyRoot;
        QPersistentModelIndex _handsRoot;
        int _bodyRootFlags{ 0 };
        int _handsRootFlags{ 0 };
        int _weaponBoneIndex{ -1 };
        float _clipBlendInSeconds{ 0.0f };
        float _clipBlendOutSeconds{ 0.0f };
        float _playbackTime{ 0.0f };
        float _posedSeconds{ 0.0f };  // clip time of the pose last applied, so a view option can redraw that frame
        float _playbackRate{ 1.0f };
        QElapsedTimer _playbackClock;
        bool _playing{ false };
        // The catalog action on the timeline (index in _animationActions, -1 for
        // none). The clip list and the transport's clip picker only show it.
        int _selectedAction{ -1 };
        bool _loopPlayback{ true };
        void selectAction(int index);
        void showSelectedAction();
        // Index of the catalog action with this resource path, or -1.
        [[nodiscard]] int actionIndex(const QString& resourcePath) const;
        void stopPlayback();
        void showPlaybackState();
        void setPlaybackRate(float rate);
        void setLoopPlayback(bool loop);
    };
}
