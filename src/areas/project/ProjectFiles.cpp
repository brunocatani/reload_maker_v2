// Project files: one weapon's authoring session saved as a JSON record inside
// the workspace. Opening one validates every authored value first, loads its
// source, and restores the weapon, its authored data, the pose and the view
// once the source is indexed.
#include "ReloadMakerController.h"
#include "AuthoringPaths.h"
#include "AuthoringProgress.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include "ReloadBundle.h"
#include "ReloadTemplates.h"
#include "SourceEvidence.h"
#include "SourcePolicy.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "glview.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <algorithm>

namespace reloadmaker
{
    namespace
    {
        constexpr qint64 kMaximumProjectBytes = 64 * 1024 * 1024;

        // Source identities compare with forward slashes and in lower case.
        QString normalizedPath(QString value)
        {
            return value.replace('\\', '/').toLower();
        }

        template <typename Map>
        QJsonObject stringMapJson(const Map& map)
        {
            QJsonObject json;
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                json[it.key()] = it.value();
            }
            return json;
        }

        // Template, source policy, roadmap and attachment choice collections.
        bool validProjectAssignments(const QJsonObject& record, QString& message)
        {
            QString error;
            if (record.contains("reloadTemplates")) {
                const auto templates = record["reloadTemplates"].toObject();
                if (!record["reloadTemplates"].isObject() || templates.size() > 512) {
                    message = "Invalid reload template assignments.";
                    return false;
                }
                for (auto it = templates.begin(); it != templates.end(); ++it) {
                    if (it.key().isEmpty() || it.key().size() > 512 || !templateMechanism(it.value().toString())) {
                        message = "Unsupported reload template assignment.";
                        return false;
                    }
                }
            }
            if (record.contains("sourcePolicies")) {
                const auto policies = record["sourcePolicies"].toObject();
                if (!record["sourcePolicies"].isObject() || policies.size() > 512) {
                    message = "Invalid saved source policies.";
                    return false;
                }
                for (auto it = policies.begin(); it != policies.end(); ++it) {
                    if (it.key().isEmpty() || it.key().size() > 1024 || !it.value().isObject() || !validSourcePolicy(it.value().toObject(), error)) {
                        message = "Invalid saved source policy: " + error;
                        return false;
                    }
                }
            }
            if (record.contains("authoringRoadmaps")) {
                const auto plans = record["authoringRoadmaps"].toObject();
                if (!record["authoringRoadmaps"].isObject() || plans.size() > 512) {
                    message = "Invalid authoring roadmap collection.";
                    return false;
                }
                for (const auto& value : plans) {
                    const auto plan = value.toObject();
                    const int step = plan["step"].toInt(-1);
                    OperatingSetup setup;
                    MagazineGuide guide;
                    MagazineSetup magazine;
                    MagazineReference reference;
                    if (!value.isObject() || step < 0 || step > 11 ||
                        (plan.contains("sources") && !validSourceEvidence(plan["sources"].toObject(), error)) ||
                        (plan.contains("operating") && !readOperatingSetup(plan["operating"].toObject(), setup, error)) ||
                        (plan.contains("magazineGuide") && !readMagazineGuide(plan["magazineGuide"].toObject(), guide, error)) ||
                        (plan.contains("magazineReference") && !readMagazineReference(plan["magazineReference"].toObject(), reference, error)) ||
                        (plan.contains("reviews") && (!plan["reviews"].isObject() || !validAuthoringReviews(plan["reviews"].toObject()))) ||
                        (plan.contains("checklist") && (!plan["checklist"].isObject() || !validChecklistConfirmations(plan["checklist"].toObject()))) ||
                        (plan.contains("collisionDrafts") &&
                         (!plan["collisionDrafts"].isObject() || !validCollisionDrafts(plan["collisionDrafts"].toObject()))) ||
                        (plan.contains("magazineSetup") && !readMagazineSetup(plan["magazineSetup"].toObject(), magazine, error))) {
                        message = "Invalid saved authoring roadmap: " + error;
                        return false;
                    }
                }
            }
            if (record.contains("recordChoices")) {
                const auto choices = record["recordChoices"].toObject();
                if (!record["recordChoices"].isObject() || choices.size() > 1024) {
                    message = "Invalid saved attachment choices.";
                    return false;
                }
                for (auto it = choices.begin(); it != choices.end(); ++it) {
                    if (!it.value().isString() || it.key().size() > 1024 || it.value().toString().size() > 1024) {
                        message = "Invalid saved attachment identity.";
                        return false;
                    }
                }
            }
            return true;
        }

        // Optic, grab location and sound profiles, each stored under its own key.
        bool validProjectProfiles(const QJsonObject& record, QString& message)
        {
            QString error;
            if (record.contains("scopeProfiles")) {
                if (!record["scopeProfiles"].isObject() || record["scopeProfiles"].toObject().size() > 128) {
                    message = "Invalid scope profiles.";
                    return false;
                }
                const auto optics = record["scopeProfiles"].toObject();
                for (auto it = optics.begin(); it != optics.end(); ++it) {
                    ScopeProfile profile;
                    if (!it.value().isObject() || !readScopeProfile(it.value().toObject(), profile, error) || profile.model != it.key()) {
                        message = "Invalid saved scope profile: " + error;
                        return false;
                    }
                }
            }
            if (record.contains("grabLocations")) {
                if (!record["grabLocations"].isObject() || record["grabLocations"].toObject().size() > 128) {
                    message = "Invalid grab locations.";
                    return false;
                }
                const auto locations = record["grabLocations"].toObject();
                for (auto it = locations.begin(); it != locations.end(); ++it) {
                    OperatingGrabLocations grabs;
                    if (!it.value().isObject() || !readGrabLocations(it.value().toObject(), grabs, error) || it.key() != grabLocationsKey(grabs)) {
                        message = "Invalid grab locations: " + error;
                        return false;
                    }
                }
            }
            if (record.contains("soundProfiles")) {
                if (!record["soundProfiles"].isObject() || record["soundProfiles"].toObject().size() > 128) {
                    message = "Invalid sound profiles.";
                    return false;
                }
                const auto sounds = record["soundProfiles"].toObject();
                for (auto it = sounds.begin(); it != sounds.end(); ++it) {
                    SoundProfile profile;
                    if (!it.value().isObject() || !readSoundProfile(it.value().toObject(), profile, error) ||
                        it.key() != (profile.weapon + "|" + profile.animation).toLower()) {
                        message = "Invalid saved sound profile: " + error;
                        return false;
                    }
                }
            }
            return true;
        }

        // Magazine defaults, operating grips, magazine grip drafts and magazine
        // grip assignments.
        bool validProjectGrips(const QJsonObject& record, QString& message)
        {
            QString error;
            const auto magazineDefaults = record["magazineGrabDefaults"].toObject();
            if ((record.contains("magazineGrabDefaults") && !record["magazineGrabDefaults"].isObject()) || magazineDefaults.size() > 512) {
                message = "Invalid magazine defaults.";
                return false;
            }
            for (auto it = magazineDefaults.begin(); it != magazineDefaults.end(); ++it) {
                if (it.key().isEmpty() || it.key().size() > 512 || !it.value().isString() || QUuid(it.value().toString()).isNull()) {
                    message = "Invalid magazine default identity.";
                    return false;
                }
            }
            const auto operating = record["operatingGrips"].toObject();
            if ((record.contains("operatingGrips") && !record["operatingGrips"].isObject()) || operating.size() > 64) {
                message = "Invalid operating grips.";
                return false;
            }
            for (auto it = operating.begin(); it != operating.end(); ++it) {
                if (it.key().size() > 1024 || !it.key().contains('|') || QUuid(it.value().toString()).isNull()) {
                    message = "Invalid operating grip identity.";
                    return false;
                }
            }
            const auto magazineDrafts = record["magazineGripDrafts"].toObject();
            if ((record.contains("magazineGripDrafts") && !record["magazineGripDrafts"].isObject()) || magazineDrafts.size() > 512) {
                message = "Invalid magazine drafts.";
                return false;
            }
            for (auto it = magazineDrafts.begin(); it != magazineDrafts.end(); ++it) {
                PartGrip grip;
                if (!decodePartGrip(QJsonDocument(it.value().toObject()).toJson(), grip, error) ||
                    (it.key() != magazineGrabKey(grip.weaponRecord, grip.pose.placement.ownerRecord) &&
                     it.key() != handAssignmentKey(magazineGrabKey(grip.weaponRecord, grip.pose.placement.ownerRecord), grip.pose.hand))) {
                    message = "Invalid magazine draft: " + error;
                    return false;
                }
            }
            if (record.contains("magazineGrabGrips") && !record["magazineGrabGrips"].isObject()) {
                message = "Invalid magazine grip assignments.";
                return false;
            }
            const auto magazineGrabs = record["magazineGrabGrips"].toObject();
            if (magazineGrabs.size() > 512) {
                message = "Too many magazine grip assignments.";
                return false;
            }
            for (auto it = magazineGrabs.begin(); it != magazineGrabs.end(); ++it) {
                if (!it.key().contains('|') || it.key().size() > 1024 || !it.value().isString() || QUuid(it.value().toString()).isNull()) {
                    message = "Invalid magazine grip assignment identity.";
                    return false;
                }
            }
            return true;
        }

        // Unsaved finger, hand preview, grip and mechanism drafts.
        bool validProjectDrafts(const QJsonObject& record, QString& message)
        {
            QString error;
            const auto fingerDrafts = record["fingerDrafts"].toArray();
            if (fingerDrafts.size() > 2) {
                message = "Invalid physical-hand draft count.";
                return false;
            }
            for (int i = 0; i < fingerDrafts.size(); ++i) {
                const auto value = fingerDrafts[i];
                if (value.isNull()) {
                    continue;
                }
                HandPose draft;
                if (!decodeHandPose(QJsonDocument(value.toObject()).toJson(), draft, error)) {
                    message = error;
                    return false;
                }
                if (int(draft.hand) != i) {
                    message = "Finger draft belongs to a different physical hand.";
                    return false;
                }
            }
            if (record.contains("handPreview")) {
                HandPose preview;
                if (!decodeHandPose(QJsonDocument(record["handPreview"].toObject()).toJson(), preview, error)) {
                    message = error;
                    return false;
                }
            }
            if (record.contains("gripDraft")) {
                PartGrip grip;
                if (!decodePartGrip(QJsonDocument(record["gripDraft"].toObject()).toJson(), grip, error)) {
                    message = error;
                    return false;
                }
            }
            if (record.contains("mechanismDraft")) {
                PartMechanism part;
                if (!decodeMechanism(QJsonDocument(record["mechanismDraft"].toObject()).toJson(), part, error)) {
                    message = error;
                    return false;
                }
            }
            return true;
        }
    }

    // ---- Saving ----

    // The first-person animation subgraph the equipped modifications select.
    QJsonObject ReloadMakerController::animationSourceIdentity() const
    {
        QJsonObject identity{ { "mode", "omod-keywords" } };
        const auto* plugin = currentPlugin();
        if (!plugin || !currentWeapon()) {
            return identity;
        }
        const auto* selected = plugin->resolveFirstPersonSubgraph(activeAnimationKeywords());
        if (!selected) {
            return identity;
        }
        for (const auto& record : plugin->animationSubgraphs) {
            for (const auto& entry : record.entries) {
                if (&entry == selected) {
                    identity["record"] = recordIdentity(*plugin, record.formId);
                    identity["paths"] = QJsonArray::fromStringList(entry.animationSearchPaths);
                    identity["behavior"] = entry.behaviorPath;
                    return identity;
                }
            }
        }
        return identity;
    }

    // The weapon's project path under the workspace, from its record and the
    // source it was loaded from; empty when the originating plugin is unknown.
    QString ReloadMakerController::suggestedSessionPath() const
    {
        if (!currentWeapon()) {
            return studioDataPath("workspace/projects/new.project.json");
        }
        const auto relative = authoringProjectPath(poseRecordKey(currentWeapon()->formId), sessionSourceContext());
        return relative.isEmpty() ? QString() : studioDataPath(relative);
    }

    // The loaded source, as weapon project paths identify it.
    QJsonObject ReloadMakerController::sessionSourceContext() const
    {
        QJsonObject source{ { "data", normalizedPath(_gameData) } };
        if (_libraryMode) {
            source["mode"] = "mo2-profile";
            source["instance"] = normalizedPath(_launchSnapshot.instancePath);
            source["profile"] = _launchSnapshot.profileName.toLower();
        } else {
            source["mode"] = _package.resourceData.isEmpty() ? "ordered-packages" : "explicit-data";
            if (!_package.plugins.isEmpty()) {
                QJsonArray inputs;
                for (const auto& path : _package.plugins) {
                    inputs.append(normalizedPath(path));
                }
                source["plugins"] = inputs;
            } else {
                source["package"] = normalizedPath(_package.sourcePath);
            }
            if (!_package.resourceData.isEmpty()) {
                source["resourceData"] = normalizedPath(_package.resourceData);
            }
        }
        return source;
    }

    // Changing weapons saves the current project under the previous weapon,
    // then opens the next weapon's project when one exists. Returns whether
    // the change was handled here.
    bool ReloadMakerController::beginWeaponProject(int selectedIndex)
    {
        const int previousIndex = _renderedWeapon;
        if (_loadThread || !_pendingSession.isEmpty() || previousIndex < 0 || selectedIndex < 0 || previousIndex == selectedIndex) {
            return false;
        }
        _selectedWeapon = validWeaponIndex(previousIndex);
        if (_reloadCapturing || _packProcess) {
            setStatus("Finish the current capture or pack job before changing weapons.", true);
            return true;
        }
        if (!saveSession()) {
            return true;
        }
        const QString oldPath = _sessionPath;
        _selectedWeapon = validWeaponIndex(selectedIndex);
        const auto next = suggestedSessionPath();
        _sessionPath.clear();
        if (!QFileInfo(next).isFile()) {
            return false;
        }
        const bool all = allWeaponsSelected();
        openSession(next);
        if (all && !_pendingSession.isEmpty()) {
            _pendingSession["browserPlugin"] = kAllWeaponsSource;
        }
        if (!_loadThread && _pendingSession.isEmpty()) {
            _selectedWeapon = validWeaponIndex(previousIndex);
            _sessionPath = oldPath;
            refreshWeaponPicker();
        }
        return true;
    }

    // Source, weapon, settings and authored data; the drafts are added by
    // appendProjectDrafts.
    QJsonObject ReloadMakerController::projectRecord() const
    {
        const auto* weapon = currentWeapon();
        QJsonObject record{ { "format", 1 },
                            { "kind", "reload-maker-v2-project" },
                            { "package", _package.sourcePath },
                            { "gameData", _gameData },
                            { "weaponName", weapon->name.isEmpty() ? weapon->editorId : weapon->name },
                            { "weaponEditorId", weapon->editorId },
                            { "library", _libraryMode },
                            { "mo2Instance", _mo2Instance },
                            { "mo2Profile", _libraryMode ? _launchSnapshot.profileName : QString() },
                            { "weapon", poseRecordKey(weapon->formId) },
                            { "body", _armsBody },
                            { "animationSource", animationSourceIdentity() },
                            { "animation", _activeAnimationPath },
                            { "seconds", _playbackTime },
                            { "motionMode", _weaponFixedMotion ? 0 : 1 },
                            { "transitions", _approximateTransitions },
                            { "showArms", _armsShown },
                            { "workspace", _workspace },
                            { "hand", int(_editedHand) },
                            { "fingerJoint", _fingerJointIndex },
                            { "selectedHandPose", selectedHandPose() ? selectedHandPose()->id : QString() } };
        const auto sourcePlugins = selectionPlugins(_selectedWeapon);
        if (allWeaponsSelected()) {
            record["browserPlugin"] = kAllWeaponsSource;
        } else if (!sourcePlugins.isEmpty()) {
            record["browserPlugin"] = sourcePlugins.contains(_browserPlugin, Qt::CaseInsensitive) ? _browserPlugin : sourcePlugins.first();
        }
        QJsonObject choices;
        if (const auto* config = configuration()) {
            for (const auto& slot : config->entries()) {
                choices[QString::number(slot.key)] = QString::number(config->selected(slot.key));
            }
        }
        record["choices"] = choices;
        record["authoringRoadmaps"] = _authoringRoadmaps;
        record["reloadTemplates"] = _reloadTemplateAssignments;
        record["sourcePolicies"] = _sourcePolicies;
        if (const auto* config = configuration()) {
            record["recordChoices"] = config->savedChoices(*currentPlugin());
        }
        record["simulationSpringReturn"] = _slideSpringReturn;
        record["camera"] = _window->getGLView()->studioCameraState();
        record["glockReloadNoExtender"] = _glockNoExtender;
        record["magazineGrabGrips"] = stringMapJson(_magazineGrabGrips);
        record["handsPage"] = int(_handEditorPage);
        record["interactionHand"] = _interactionHand == PoseHand::Left ? "left" : "right";
        QJsonObject sounds;
        for (auto it = _savedSounds.cbegin(); it != _savedSounds.cend(); ++it) {
            sounds[it.key()] = soundProfileJson(it.value());
        }
        if (!_soundProfile.weapon.isEmpty()) {
            sounds[soundProfileKey()] = soundProfileJson(_soundProfile);
        }
        record["soundProfiles"] = sounds;
        QJsonObject locations;
        for (auto it = _grabLocations.cbegin(); it != _grabLocations.cend(); ++it) {
            locations[it.key()] = grabLocationsJson(it.value());
        }
        record["grabLocations"] = locations;
        QJsonObject optics;
        for (auto it = _scopeProfiles.cbegin(); it != _scopeProfiles.cend(); ++it) {
            optics[it.key()] = scopeProfileJson(it.value());
        }
        record["scopeProfiles"] = optics;
        record["scopeSelection"] = currentScopeKey();
        record["operatingGrips"] = stringMapJson(_savedOperatingGrips);
        record["magazineGrabDefaults"] = stringMapJson(_magazineGrabDefaults);
        if (_simulationRig && _reloadRecipe) {
            record["simulationSetup"] = simulationRigJson(*_reloadRecipe, *_simulationRig, _slideSpringReturn);
        }
        if (_magazineRail) {
            record["magazineInteraction"] = magazineRailJson(*_magazineRail);
        }
        if (_simulationAmmo.valid()) {
            record["ammunitionConfiguration"] = _simulationAmmo.json();
        }
        return record;
    }

    // The open magazine grip is remembered first. A draft that cannot be
    // encoded fails the save with `message`.
    bool ReloadMakerController::appendProjectDrafts(QJsonObject& record, QString& message)
    {
        QString error;
        rememberMagazineGrip();
        QJsonObject magazineDrafts;
        for (auto it = _magazineGripDrafts.cbegin(); it != _magazineGripDrafts.cend(); ++it) {
            const auto bytes = encodePartGrip(it.value(), error);
            if (bytes.isEmpty()) {
                message = "Cannot save magazine draft: " + error;
                return false;
            }
            magazineDrafts[it.key()] = QJsonDocument::fromJson(bytes).object();
        }
        record["magazineGripDrafts"] = magazineDrafts;
        QJsonArray fingers;
        for (const auto& draft : _fingerDrafts) {
            if (!draft) {
                fingers.append(QJsonValue());
                continue;
            }
            const auto bytes = encodeHandPose(*draft, error);
            if (!error.isEmpty()) {
                message = error;
                return false;
            }
            fingers.append(QJsonDocument::fromJson(bytes).object());
        }
        record["fingerDrafts"] = fingers;
        if (_handPosePreview) {
            const auto bytes = encodeHandPose(*_handPosePreview, error);
            if (!error.isEmpty()) {
                message = error;
                return false;
            }
            record["handPreview"] = QJsonDocument::fromJson(bytes).object();
        }
        const auto& authoredGrip = simulationWorkspace(_workspace) ? _operateSuspendedGrip : _partGripDraft;
        if (authoredGrip) {
            const auto bytes = encodePartGrip(*authoredGrip, error);
            if (!error.isEmpty()) {
                message = "Finish the grip before saving: " + error;
                return false;
            }
            record["gripDraft"] = QJsonDocument::fromJson(bytes).object();
        }
        if (_mechanismDraft && !_mechanismDraft->stages.isEmpty()) {
            const auto bytes = encodeMechanism(*_mechanismDraft, error);
            if (!error.isEmpty()) {
                message = "Finish the mechanism step before saving: " + error;
                return false;
            }
            record["mechanismDraft"] = QJsonDocument::fromJson(bytes).object();
        }
        return true;
    }

    // Saves to `requested`, else the open project, else the weapon's project
    // path. Nothing to save (no weapon, or a source loading) counts as saved.
    bool ReloadMakerController::saveSession(const QString& requested)
    {
        if (!currentWeapon() || _loadThread) {
            return true;
        }
        const QString path = requested.isEmpty() ? (_sessionPath.isEmpty() ? suggestedSessionPath() : _sessionPath) : requested;
        if (path.isEmpty()) {
            setStatus("Cannot identify this weapon's originating plugin for saving.", true);
            return false;
        }
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (!absolute.startsWith(QDir(studioRoot()).absolutePath() + "/", Qt::CaseInsensitive)) {
            setStatus("Save projects inside the Reload Maker V2 workspace.", true);
            return false;
        }
        StageClock clock(_loadTimings, "project.save");
        auto record = projectRecord();
        QString message;
        if (!appendProjectDrafts(record, message)) {
            setStatus(message, true);
            return false;
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        const auto bytes = QJsonDocument(record).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            setStatus("Project save failed: " + file.errorString(), true);
            return false;
        }
        _sessionPath = absolute;
        if (!_libraryMode && !currentTestbed().isEmpty()) {
            QSettings().setValue("testbeds/" + currentTestbed(), absolute);
        }
        QSettings().setValue("projects/lastProject", absolute);
        writeWeaponStatus(absolute);
        setStatus("Project saved · " + QFileInfo(path).fileName());
        return true;
    }

    // ---- Opening ----

    // Validates every authored value before the source or a draft changes,
    // then loads the project's source. restoreSessionSelection finishes once
    // the source is indexed.
    void ReloadMakerController::openSession(const QString& path)
    {
        if (_loadThread || _reloadCapturing) {
            setStatus("Finish or cancel the current load first.", true);
            return;
        }
        StageClock clock(_loadTimings, "project.read");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumProjectBytes) {
            setStatus("Cannot read this project.", true);
            return;
        }
        QJsonParseError parseError;
        const auto parsed = QJsonDocument::fromJson(file.readAll(), &parseError);
        const auto record = parsed.object();
        if (parseError.error != QJsonParseError::NoError || record["format"] != 1 || record["kind"] != "reload-maker-v2-project") {
            setStatus("Unsupported or invalid Reload Maker project.", true);
            return;
        }
        if (!QFileInfo(record["gameData"].toString()).isDir() ||
            (!record["library"].toBool() && !QFileInfo(record["package"].toString()).exists())) {
            setStatus("Project source is missing. Restore its original package/Data path before opening.", true);
            return;
        }
        QString message;
        if (!validProjectAssignments(record, message) || !validProjectProfiles(record, message) || !validProjectGrips(record, message) ||
            !validProjectDrafts(record, message)) {
            setStatus(message, true);
            return;
        }
        _pendingSession = record;
        _sessionPath = QFileInfo(path).absoluteFilePath();
        _gameDataText = record["gameData"].toString();
        if (!record["mo2Instance"].toString().isEmpty()) {
            _mo2Instance = record["mo2Instance"].toString();
        }
        if (record["library"].toBool() && !record["mo2Profile"].toString().isEmpty()) {
            _requestedMo2Profile = record["mo2Profile"].toString();
        }
        if (record["library"].toBool()) {
            startLibraryLoad(true);
        } else {
            startPackageLoad(record["package"].toString(), record["gameData"].toString(), false, true);
        }
    }

    // Finds the project's weapon in the indexed source, restricted to the
    // plugin it was saved from unless every weapon was listed.
    bool ReloadMakerController::selectProjectWeapon(const QJsonObject& record)
    {
        const QString plugin = record["browserPlugin"].toString();
        int selected = -1;
        for (int i = 0; i < _weaponSelections.size(); ++i) {
            _selectedWeapon = i;
            if (currentWeapon() && poseRecordKey(currentWeapon()->formId) == record["weapon"].toString() &&
                (plugin.isEmpty() || (_libraryMode && record["browserPlugin"] == kAllWeaponsSource) ||
                 selectionPlugins(i).contains(plugin, Qt::CaseInsensitive))) {
                selected = i;
                break;
            }
        }
        if (selected < 0) {
            _selectedWeapon = -1;
            refreshWeaponPicker();
            setStatus("The project weapon is absent or excluded from this source's player-weapon library. See Diagnostics for exclusion reasons.",
                      true);
            return false;
        }
        alignPluginToWeapon();
        refreshWeaponPicker();
        return true;
    }

    // Arms body, attachment choices (by record identity, or by slot for older
    // projects) and the NoExtender reload.
    void ReloadMakerController::restoreProjectLoadout(const QJsonObject& record)
    {
        _armsBody = isArmsBody(record["body"].toString()) ? record["body"].toString() : QStringLiteral("male");
        showArmsBody();
        refreshAttachments();
        if (auto* config = configuration()) {
            if (record.contains("recordChoices")) {
                for (const auto& missing : config->restoreChoices(*currentPlugin(), record["recordChoices"].toObject())) {
                    appendDiagnostic("PROJECT: unavailable saved record choice " + missing);
                }
            } else {
                const auto choices = record["choices"].toObject();
                for (auto it = choices.begin(); it != choices.end(); ++it) {
                    if (!config->select(it.key().toULongLong(), it.value().toString().toUInt())) {
                        appendDiagnostic("PROJECT: unavailable saved slot choice " + it.key());
                    }
                }
            }
        }
        refreshAttachments();
        _glockNoExtender = record["glockReloadNoExtender"].toBool(false);
    }

    // Roadmaps, templates, source policies, sound and optic profiles, the
    // interaction hand, grab locations and every grip assignment and draft.
    // Unsaved drafts from the previous project are dropped.
    void ReloadMakerController::restoreProjectAuthoring(const QJsonObject& record)
    {
        _authoringRoadmaps = record["authoringRoadmaps"].toObject();
        _reloadTemplateAssignments = record["reloadTemplates"].toObject();
        _sourcePolicies = record["sourcePolicies"].toObject();
        _savedSounds.clear();
        const auto sounds = record["soundProfiles"].toObject();
        for (auto it = sounds.begin(); it != sounds.end(); ++it) {
            SoundProfile profile;
            QString error;
            if (readSoundProfile(it.value().toObject(), profile, error)) {
                _savedSounds.insert(it.key(), profile);
            }
        }
        _scopeProfiles.clear();
        const auto optics = record["scopeProfiles"].toObject();
        for (auto it = optics.begin(); it != optics.end(); ++it) {
            ScopeProfile profile;
            QString error;
            if (readScopeProfile(it.value().toObject(), profile, error)) {
                _scopeProfiles.insert(it.key(), profile);
            }
        }
        const QString hand = record["interactionHand"].toString(isM1AConfiguration() ? "right" : "left");
        _interactionHand = hand == "right" && !isGlockConfiguration() ? PoseHand::Right : PoseHand::Left;
        _grabLocations.clear();
        const auto locations = record["grabLocations"].toObject();
        for (auto it = locations.begin(); it != locations.end(); ++it) {
            OperatingGrabLocations grabs;
            QString error;
            if (readGrabLocations(it.value().toObject(), grabs, error)) {
                _grabLocations[it.key()] = grabs;
            }
        }
        _savedOperatingGrips.clear();
        const auto operating = record["operatingGrips"].toObject();
        for (auto it = operating.begin(); it != operating.end(); ++it) {
            _savedOperatingGrips[it.key()] = it.value().toString();
        }
        _magazineGrabGrips.clear();
        const auto magazineGrabs = record["magazineGrabGrips"].toObject();
        for (auto it = magazineGrabs.begin(); it != magazineGrabs.end(); ++it) {
            _magazineGrabGrips[it.key().toLower()] = it.value().toString();
        }
        _magazineGrabDefaults.clear();
        const auto magazineDefaults = record["magazineGrabDefaults"].toObject();
        for (auto it = magazineDefaults.begin(); it != magazineDefaults.end(); ++it) {
            _magazineGrabDefaults[it.key().toLower()] = it.value().toString();
        }
        _partGripDraft.reset();
        _operateSuspendedGrip.reset();
        _editingMagazine = false;
        _magazineGripDrafts.clear();
        const auto magazineDrafts = record["magazineGripDrafts"].toObject();
        for (auto it = magazineDrafts.begin(); it != magazineDrafts.end(); ++it) {
            PartGrip grip;
            QString error;
            if (decodePartGrip(QJsonDocument(it.value().toObject()).toJson(), grip, error)) {
                _magazineGripDrafts[handAssignmentKey(magazineGrabKey(grip.weaponRecord, grip.pose.placement.ownerRecord), grip.pose.hand)] = grip;
            }
        }
    }

    // Clip, motion settings, time, finger drafts, hand editor values and the
    // mechanism and grip drafts. A draft that no longer decodes or fits is
    // reported through `error`.
    void ReloadMakerController::restoreProjectPose(const QJsonObject& record, QString& error)
    {
        if (const int action = actionIndex(record["animation"].toString()); action >= 0) {
            selectAction(action);
        }
        setWeaponFixedMotion(record["motionMode"].toInt() == 0);
        setApproximateTransitions(record["transitions"].toBool());
        setArmsShown(record["showArms"].toBool(true));
        _playbackTime = std::clamp(float(record["seconds"].toDouble()), 0.f, playbackDuration());
        const auto fingers = record["fingerDrafts"].toArray();
        for (int i = 0; i < std::min(2, int(fingers.size())); ++i) {
            if (!fingers[i].isObject()) {
                continue;
            }
            HandPose pose;
            if (decodeHandPose(QJsonDocument(fingers[i].toObject()).toJson(), pose, error)) {
                _fingerDrafts[i] = pose;
            }
        }
        if (record.contains("handPreview")) {
            HandPose preview;
            if (decodeHandPose(QJsonDocument(record["handPreview"].toObject()).toJson(), preview, error)) {
                _handPosePreview = preview;
            }
        }
        setEditedHand(record["hand"].toInt(int(PoseHand::Right)) == int(PoseHand::Left) ? PoseHand::Left : PoseHand::Right);
        setFingerJoint(std::clamp(record["fingerJoint"].toInt(), 0, 14));
        if (handPoseListed(record["selectedHandPose"].toString())) {
            selectHandPose(record["selectedHandPose"].toString());
        }
        if (record.contains("mechanismDraft")) {
            PartMechanism part;
            if (decodeMechanism(QJsonDocument(record["mechanismDraft"].toObject()).toJson(), part, error)) {
                _mechanismDraft = part;
                displayMechanism();
            }
        }
        if (record.contains("gripDraft")) {
            PartGrip grip;
            if (decodePartGrip(QJsonDocument(record["gripDraft"].toObject()).toJson(), grip, error)) {
                QVector<HandPosePlacement> anchors;
                for (const auto& anchor : _handPoseAnchors) {
                    anchors.append(anchor.description);
                }
                const int anchor = resolveGripAnchor(grip, poseRecordKey(currentWeapon()->formId), anchors, error);
                if (anchor >= 0 && preparePartGripFrame(anchor, error)) {
                    _partGripDraft = grip;
                    setGripDraftName(grip.pose.name);
                    followGripInForm(grip.sourcePoseId, anchor);
                    _editingMagazine = magazineGrabAnchor(grip.pose.placement) >= 0;
                    if (record["workspace"].toInt() == GripsWorkspace) {
                        setHandEditorPage(HandEditorPage::PartGrip);
                    }
                    showPartGripValues();
                }
            }
        }
    }

    // The posed frame, spring return, workspace, optic, hand editor page and
    // camera.
    void ReloadMakerController::restoreProjectView(const QJsonObject& record)
    {
        (void)applyPose(_playbackTime);
        updateTimelineText();
        refreshFingerEditor();
        setSlideSpringReturn(record["simulationSpringReturn"].toBool(true));
        const int workspace = record["workspace"].toInt(OperateWorkspace);
        setWorkspace(validWorkspace(workspace) ? workspace : OperateWorkspace);
        if (record.contains("scopeSelection")) {
            const auto model = record["scopeSelection"].toString();
            for (int i = 0; i < _scopeTargets.size(); ++i) {
                const auto anchor = _scopeTargets[i].anchor;
                if (anchor > 0 && anchor < _handPoseAnchors.size() && scopeModelKey(_handPoseAnchors[anchor].description.ownerModel) == model) {
                    selectScopeTarget(i);
                    break;
                }
            }
        }
        if (workspace == GripsWorkspace && record.contains("handsPage")) {
            setHandEditorPage(static_cast<HandEditorPage>(std::clamp(record["handsPage"].toInt(), 0, 3)));
        }
        if (record.contains("camera") && !_window->getGLView()->studioRestoreCamera(record["camera"].toObject())) {
            appendDiagnostic("PROJECT: invalid saved camera ignored.");
        }
        ++_revision;
        if (workspace == GripsWorkspace && _handEditorPage == HandEditorPage::GrabLocations) {
            prepareGrabLocationFrame();
        }
    }

    // Finishes opening a project once its source is indexed.
    void ReloadMakerController::restoreSessionSelection()
    {
        if (_pendingSession.isEmpty()) {
            return;
        }
        const auto record = _pendingSession;
        _pendingSession = {};
        StageClock clock(_loadTimings, "project.restore");
        if (!selectProjectWeapon(record)) {
            return;
        }
        restoreProjectLoadout(record);
        restoreProjectAuthoring(record);
        rebuildWeapon();
        if (!_clip.valid()) {
            return;
        }
        QString error;
        restoreProjectPose(record, error);
        restoreProjectView(record);
        setStatus(error.isEmpty() ? "Project restored · " + QFileInfo(_sessionPath).fileName() : "Project restored with a grip warning: " + error,
                  !error.isEmpty());
    }
}
