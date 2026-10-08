// Weapon records: one weapon's complete editor state as JSON. The ESP
// project (EspProject.cpp) keeps one per weapon and commits them; an explicit
// import validates an earlier project's record before anything changes.
#include "ReloadMakerController.h"
#include "CollisionDraftPolicy.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include "ReloadBundle.h"
#include "ReloadTemplates.h"
#include "SourcePolicy.h"
#include "StudioPaths.h"
#include "StudioWindow.h"
#include "glview.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace reloadmaker
{
    namespace
    {
        template <typename Map>
        QJsonObject stringMapJson(const Map& map)
        {
            QJsonObject json;
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                json[it.key()] = it.value();
            }
            return json;
        }

        // Template, source policy, authoring record and attachment choice collections.
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
            if (record.contains("reloadAxes")) {
                const auto weapons = record["reloadAxes"].toObject();
                if (!record["reloadAxes"].isObject() || weapons.size() > 512) {
                    message = "Invalid reload parts.";
                    return false;
                }
                for (auto it = weapons.begin(); it != weapons.end(); ++it) {
                    const auto axes = it.value().toArray();
                    if (it.key().isEmpty() || it.key().size() > 512 || !it.value().isArray() || axes.size() > 3) {
                        message = "Invalid reload parts.";
                        return false;
                    }
                    for (const auto& value : axes) {
                        const auto role = value.toObject()["role"].toString();
                        const auto slot = value.toObject()["slot"].toString();
                        if ((role != "receiver" && role != "operatingPart" && role != "magazine") || slot.isEmpty() || slot.size() > 512) {
                            message = "Invalid reload parts.";
                            return false;
                        }
                    }
                }
            }
            if (record.contains("weaponProperties")) {
                const auto properties = record["weaponProperties"].toObject();
                if (!record["weaponProperties"].isObject() || properties.size() > 512) {
                    message = "Invalid weapon properties.";
                    return false;
                }
                for (auto it = properties.begin(); it != properties.end(); ++it) {
                    WeaponOverrides overrides;
                    if (it.key().isEmpty() || it.key().size() > 512 || !it.value().isObject() || !readWeaponOverrides(it.value().toObject(), overrides, error)) {
                        message = "Invalid weapon properties: " + error;
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
                    message = "Invalid authoring record collection.";
                    return false;
                }
                for (const auto& value : plans) {
                    const auto plan = value.toObject();
                    OperatingSetup setup;
                    MagazineGuide guide;
                    MagazineSetup magazine;
                    MagazineReference reference;
                    if (!value.isObject() ||
                        (plan.contains("operating") && !readOperatingSetup(plan["operating"].toObject(), setup, error)) ||
                        (plan.contains("magazineGuide") && !readMagazineGuide(plan["magazineGuide"].toObject(), guide, error)) ||
                        (plan.contains("magazineReference") && !readMagazineReference(plan["magazineReference"].toObject(), reference, error)) ||
                        (plan.contains("checklist") && (!plan["checklist"].isObject() || !validChecklistEntries(plan["checklist"].toObject()))) ||
                        (plan.contains("collisionDrafts") &&
                         (!plan["collisionDrafts"].isObject() || !validCollisionDrafts(plan["collisionDrafts"].toObject()))) ||
                        (plan.contains("magazineSetup") && !readMagazineSetup(plan["magazineSetup"].toObject(), magazine, error))) {
                        message = "Invalid saved authoring record: " + error;
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

        bool validProjectPins(const QJsonObject& record, QString& message)
        {
            QVector<Pin> pins;
            QString error;
            if (!readPins(record["annotations"], pins, error)) {
                message = "Invalid saved pins: " + error;
                return false;
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
            // Finished world models: weapon|magazine -> full and empty paths.
            const auto models = record["magazineModels"].toObject();
            if ((record.contains("magazineModels") && !record["magazineModels"].isObject()) || models.size() > 512) {
                message = "Invalid magazine model assignments.";
                return false;
            }
            for (auto it = models.begin(); it != models.end(); ++it) {
                const auto pair = it.value().toObject();
                if (!it.key().contains('|') || it.key().size() > 1024 || !pair["full"].isString() || !pair["empty"].isString()) {
                    message = "Invalid magazine model assignment.";
                    return false;
                }
            }
            // Round choices: magazine record|model -> piece path -> round.
            const auto rounds = record["magazineRounds"].toObject();
            if ((record.contains("magazineRounds") && !record["magazineRounds"].isObject()) || rounds.size() > 512) {
                message = "Invalid magazine round choices.";
                return false;
            }
            for (auto it = rounds.begin(); it != rounds.end(); ++it) {
                const auto pieces = it.value().toObject();
                bool valid = it.key().contains('|') && it.key().size() <= 1024 && it.value().isObject() && !pieces.isEmpty() && pieces.size() <= 256;
                for (auto piece = pieces.begin(); valid && piece != pieces.end(); ++piece) {
                    valid = !piece.key().isEmpty() && piece.key().size() <= 4096 && piece.value().isBool();
                }
                if (!valid) {
                    message = "Invalid magazine round choice.";
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
        record["authoringRoadmaps"] = _authoringRecords;
        record["reloadTemplates"] = _reloadTemplateAssignments;
        record["reloadAxes"] = _reloadAxes;
        QJsonObject weaponProperties;
        for (auto it = _weaponOverrides.cbegin(); it != _weaponOverrides.cend(); ++it) {
            if (!it->empty()) {
                weaponProperties[it.key()] = weaponOverridesJson(*it);
            }
        }
        if (!weaponProperties.isEmpty()) {
            record["weaponProperties"] = weaponProperties;
        }
        record["sourcePolicies"] = _sourcePolicies;
        if (!_pins.isEmpty()) {
            record["annotations"] = pinsJson();
        }
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
        // The shown profile is authoring once edited; read from the
        // animation it is not (applied ones are in _savedSounds).
        if (!_soundProfile.weapon.isEmpty() && _soundsDirty) {
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
        if (!_magazineModels.isEmpty()) {
            record["magazineModels"] = stringMapJson(_magazineModels);
        }
        if (!_magazineRounds.isEmpty()) {
            QJsonObject rounds;
            for (auto it = _magazineRounds.cbegin(); it != _magazineRounds.cend(); ++it) {
                rounds[it.key()] = it.value();
            }
            record["magazineRounds"] = rounds;
        }
        return record;
    }

    // The open magazine grip is remembered first. A draft that cannot be
    // encoded fails the save with `message`.
    bool ReloadMakerController::appendProjectDrafts(QJsonObject& record, QString& message) const
    {
        QString error;
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
        if (authoredGrip && partGripSnapshot(*authoredGrip) != _partGripOpened) {
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
            if (bytes == _mechanismOpened) {
                return true;
            }
            record["mechanismDraft"] = QJsonDocument::fromJson(bytes).object();
        }
        return true;
    }

    namespace
    {
        // What a reload, grab point or scope is made of: only these mark a
        // weapon unsaved and only these enter a saved version. Finger, grip,
        // mechanism and magazine-grip drafts are unsaved work and count once
        // they differ from what their editor opened with.
        constexpr const char* kAuthoringKeys[] = {
            "format", "kind", "weapon", "weaponName", "weaponEditorId",
            "authoringRoadmaps", "reloadTemplates", "reloadAxes", "weaponProperties", "sourcePolicies", "annotations",
            "simulationSpringReturn", "glockReloadNoExtender", "soundProfiles", "grabLocations", "scopeProfiles",
            "operatingGrips", "magazineGrabGrips", "magazineGrabDefaults", "magazineModels", "magazineRounds",
            "magazineGripDrafts", "fingerDrafts", "gripDraft", "mechanismDraft", "unfinishedDraft"
        };
        // Where the author left the weapon: loadout, arms, camera, page, clip
        // and hand. Kept automatically per weapon outside every version
        // (sessionFile); it never marks a weapon unsaved.
        constexpr const char* kSessionKeys[] = {
            "body", "choices", "recordChoices", "browserPlugin", "camera", "workspace", "handsPage", "animation", "seconds",
            "motionMode", "transitions", "showArms", "hand", "fingerJoint", "selectedHandPose", "interactionHand", "scopeSelection"
        };

        template <std::size_t N>
        QJsonObject onlyKeys(const QJsonObject& record, const char* const (&keys)[N])
        {
            QJsonObject part;
            for (const char* key : keys) {
                if (const auto value = record.value(QLatin1String(key)); !value.isUndefined()) {
                    part[QLatin1String(key)] = value;
                }
            }
            return part;
        }
    }

    // The grip as the editor shows it, with its name from the form.
    QByteArray ReloadMakerController::partGripSnapshot(const PartGrip& grip) const
    {
        auto named = grip;
        named.pose.name = _gripDraftName.trimmed();
        QString error;
        return encodePartGrip(named, error);
    }

    void ReloadMakerController::markPartGripOpened()
    {
        _partGripOpened = _partGripDraft ? partGripSnapshot(*_partGripDraft) : QByteArray();
    }

    void ReloadMakerController::markMechanismOpened()
    {
        QString error;
        _mechanismOpened = _mechanismDraft ? encodeMechanism(*_mechanismDraft, error) : QByteArray();
    }

    QJsonObject ReloadMakerController::authoredPart(QJsonObject record)
    {
        return onlyKeys(record, kAuthoringKeys);
    }

    QJsonObject ReloadMakerController::sessionPart(const QJsonObject& record)
    {
        return onlyKeys(record, kSessionKeys);
    }

    // A saved version holds the authoring and, as metadata beside it, the
    // source the weapon was read from.
    QJsonObject ReloadMakerController::versionRecord(const QJsonObject& record)
    {
        auto version = authoredPart(record);
        for (const char* key : { "package", "gameData", "library", "mo2Instance", "mo2Profile" }) {
            if (record.contains(QLatin1String(key))) {
                version[QLatin1String(key)] = record[QLatin1String(key)];
            }
        }
        return version;
    }

    // The project record without its view state, with the drafts a save
    // would keep (an unfinished one counts as content).
    QJsonObject ReloadMakerController::authoredRecord() const
    {
        if (!currentWeapon() || _loadThread) {
            return {};
        }
        auto record = projectRecord();
        QString message;
        if (!appendProjectDrafts(record, message)) {
            record["unfinishedDraft"] = message;
        }
        return authoredPart(record);
    }

    bool ReloadMakerController::validWeaponRecord(const QJsonObject& record, QString& message)
    {
        if (record["format"] != 1 || (record["kind"] != "reload-maker-v2-project" && record["kind"] != "reload-maker-v2-weapon-authoring") ||
            record["weapon"].toString().isEmpty()) {
            message = QStringLiteral("Unsupported or invalid weapon record.");
            return false;
        }
        return validProjectAssignments(record, message) && validProjectPins(record, message) && validProjectProfiles(record, message) &&
               validProjectGrips(record, message) && validProjectDrafts(record, message);
    }

    bool ReloadMakerController::documentDirty() const
    {
        return currentWeapon() && !_loadThread && authoredRecord() != _savedAuthoring;
    }

    void ReloadMakerController::markDocumentClean()
    {
        _savedAuthoring = authoredRecord();
        refreshDocumentModified();
    }

    void ReloadMakerController::refreshDocumentModified()
    {
        if (_window) {
            _window->setWindowModified(documentDirty());
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

    // Authoring records, templates, source policies, sound and optic profiles, the
    // interaction hand, grab locations and every grip assignment and draft.
    // Unsaved drafts from the previous project are dropped.
    void ReloadMakerController::restoreProjectAuthoring(const QJsonObject& record)
    {
        // "step" and "reviews" belonged to the retired 12-step roadmap
        // (2026-10-04); nothing reads them, so they are not kept.
        _authoringRecords = {};
        const auto records = record["authoringRoadmaps"].toObject();
        for (auto it = records.begin(); it != records.end(); ++it) {
            auto saved = it.value().toObject();
            saved.remove(QStringLiteral("step"));
            saved.remove(QStringLiteral("reviews"));
            _authoringRecords.insert(it.key(), saved);
        }
        _reloadTemplateAssignments = record["reloadTemplates"].toObject();
        _reloadAxes = record["reloadAxes"].toObject();
        restorePins(record);
        _weaponOverrides.clear();
        const auto weaponProperties = record["weaponProperties"].toObject();
        for (auto it = weaponProperties.begin(); it != weaponProperties.end(); ++it) {
            WeaponOverrides overrides;
            QString error;
            if (readWeaponOverrides(it.value().toObject(), overrides, error)) {
                _weaponOverrides.insert(it.key(), overrides);
            }
        }
        // Rules were once keyed by weapon and source context; one ESP
        // project now owns them by weapon alone.
        _sourcePolicies = {};
        const auto policies = record["sourcePolicies"].toObject();
        for (auto it = policies.begin(); it != policies.end(); ++it) {
            const auto weapon = it.key().section('|', 0, 0).toLower();
            if (!_sourcePolicies.contains(weapon) || !it.key().contains('|')) {
                _sourcePolicies[weapon] = it.value();
            }
        }
        _magazineModels.clear();
        const auto models = record["magazineModels"].toObject();
        for (auto it = models.begin(); it != models.end(); ++it) {
            if (it.key().contains('|') && it.value().isObject()) {
                _magazineModels[it.key().toLower()] = it.value().toObject();
            }
        }
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
        const QString hand = record["interactionHand"].toString(defaultMagazineHand() == PoseHand::Right ? "right" : "left");
        const PoseHand wanted = hand == "right" ? PoseHand::Right : PoseHand::Left;
        _interactionHand = magazineHandAllowed(wanted) ? wanted : defaultMagazineHand();
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
        _magazineRounds.clear();
        const auto rounds = record["magazineRounds"].toObject();
        for (auto it = rounds.begin(); it != rounds.end(); ++it) {
            _magazineRounds[it.key().toLower()] = it.value().toObject();
        }
        _partGripDraft.reset();
        _operateSuspendedGrip.reset();
        _editingMagazine = false;
        _magazineGripDrafts.clear();
        const auto magazineDrafts = record["magazineGripDrafts"].toObject();
        for (auto it = magazineDrafts.begin(); it != magazineDrafts.end(); ++it) {
            PartGrip grip;
            QString error;
            if (!decodePartGrip(QJsonDocument(it.value().toObject()).toJson(), grip, error)) {
                continue;
            }
            // Opening the editor once stored the fit it showed as a draft;
            // such a copy of a saved fit is not unsaved work.
            const auto saved = std::find_if(_partGrips.cbegin(), _partGrips.cend(), [&](const auto& s) { return s.pose.id == grip.pose.id; });
            if (saved == _partGrips.cend() || !sameGripFit(grip, *saved)) {
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

    // Once a source is indexed: selects the requested weapon and opens its
    // authoring from the ESP project. An explicit import then replaces that
    // weapon's working state with the imported record, unsaved.
    void ReloadMakerController::restoreSessionSelection()
    {
        if (_pendingSession.isEmpty()) {
            return;
        }
        const auto request = std::exchange(_pendingSession, {});
        StageClock clock(_loadTimings, "project.restore");
        if (!selectProjectWeapon(request)) {
            return;
        }
        openWeaponAuthoring();
        if (request.contains("import") && _esp) {
            auto& work = _esp->weapons[weaponRecordKey()];
            const auto baseline = work.committed;
            restoreWeaponRecord(request["import"].toObject(), baseline);
            work.imported = true;
            setStatus(documentDirty() ? QStringLiteral("Imported as unsaved edits. Save adds them to %1 as a new version; leaving discards them.").arg(_esp->plugin)
                                      : QStringLiteral("Imported project matches the saved authoring of %1.").arg(_esp->plugin));
        }
        if (request.contains("workspace")) {
            const int workspace = request["workspace"].toInt(OperateWorkspace);
            setWorkspace(validWorkspace(workspace) ? workspace : OperateWorkspace);
        }
        ++_revision;
    }
}
