#include "ReloadMakerController.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include "AuthoringProgress.h"
#include "AuthoringPaths.h"
#include "SourcePolicy.h"
#include "StudioPaths.h"
#include "SourceEvidence.h"
#include "ReloadTemplates.h"
#include "ReloadBundle.h"
#include "StudioWindow.h"
#include "glview.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>
#include <QJsonDocument>
#include <QListWidget>
#include <QLineEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QTabWidget>
#include <algorithm>

namespace reloadmaker {
QJsonObject ReloadMakerController::animationSourceIdentity() const {
    QJsonObject identity{{"mode","omod-keywords"}};
    const auto* plugin=currentPlugin();if(!plugin||!currentWeapon())return identity;
    const auto* selected=plugin->resolveFirstPersonSubgraph(activeAnimationKeywords());if(!selected)return identity;
    for(const auto& record:plugin->animationSubgraphs)for(const auto& entry:record.entries)if(&entry==selected) {
        identity["record"]=recordIdentity(*plugin,record.formId);
        identity["paths"]=QJsonArray::fromStringList(entry.animationSearchPaths);identity["behavior"]=entry.behaviorPath;
        return identity;
    }
    return identity;
}
QString ReloadMakerController::suggestedSessionPath() const {
    if(!currentWeapon())return studioDataPath("workspace/projects/new.project.json");
    const auto normalize=[](QString value){return value.replace('\\','/').toLower();};
    QJsonObject source{{"data",normalize(_gameData)}};
    if(_libraryMode) {
        source["mode"]="mo2-profile";source["instance"]=normalize(_launchSnapshot.instancePath);source["profile"]=_launchSnapshot.profileName.toLower();
    }else {
        source["mode"]=_package.resourceData.isEmpty()?"ordered-packages":"explicit-data";
        if(!_package.plugins.isEmpty()){QJsonArray inputs;for(const auto& path:_package.plugins)inputs.append(normalize(path));source["plugins"]=inputs;}
        else source["package"]=normalize(_package.sourcePath);
        if(!_package.resourceData.isEmpty())source["resourceData"]=normalize(_package.resourceData);
    }
    const auto relative=authoringProjectPath(poseRecordKey(currentWeapon()->formId),source);
    return relative.isEmpty()?QString():studioDataPath(relative);
}
bool ReloadMakerController::beginWeaponProject(int selectedIndex) {
    const int previousIndex=_renderedWeapon;
    if(_loadThread||!_pendingSession.isEmpty()||previousIndex<0||selectedIndex<0||previousIndex==selectedIndex)return false;
    QString oldPath;
    // The current project is saved under the previous weapon first.
    _selectedWeapon=validWeaponIndex(previousIndex);
    if(_reloadCapturing||_packProcess){setStatus("Finish the current capture or pack job before changing weapons.",true);return true;}
    if(!saveSession())return true;
    oldPath=_sessionPath;
    _selectedWeapon=validWeaponIndex(selectedIndex);
    const auto next=suggestedSessionPath();_sessionPath.clear();
    if(QFileInfo(next).isFile()) {
        const bool all=allWeaponsSelected();
        openSession(next);
        if(all&&!_pendingSession.isEmpty())_pendingSession["browserPlugin"]=kAllWeaponsSource;
        if(!_loadThread&&_pendingSession.isEmpty()) {
            _selectedWeapon=validWeaponIndex(previousIndex);_sessionPath=oldPath;refreshWeaponPicker();
        }
        return true;
    }
    return false;
}
bool ReloadMakerController::saveSession(const QString& requested) {
    if (!currentWeapon() || _loadThread) return true;
    const QString path = requested.isEmpty() ? (_sessionPath.isEmpty() ? suggestedSessionPath() : _sessionPath) : requested;
    if(path.isEmpty()){setStatus("Cannot identify this weapon's originating plugin for saving.",true);return false;}
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!absolute.startsWith(QDir(studioRoot()).absolutePath()+"/", Qt::CaseInsensitive)) { setStatus("Save projects inside the Reload Maker V2 workspace.", true); return false; }
    QJsonObject record{{"format",1},{"kind","reload-maker-v2-project"},{"package",_package.sourcePath},{"gameData",_gameData},
        {"weaponName",currentWeapon()->name.isEmpty()?currentWeapon()->editorId:currentWeapon()->name},{"weaponEditorId",currentWeapon()->editorId},
        {"library",_libraryMode},{"mo2Instance",_mo2Instance},{"mo2Profile",_libraryMode?_launchSnapshot.profileName:QString()},{"weapon",poseRecordKey(currentWeapon()->formId)},
        {"body",_armsBody},{"animationSource",animationSourceIdentity()},
        {"animation",_activeAnimationPath},{"seconds",_playbackTime},{"motionMode",_weaponFixedMotion?0:1},
        {"transitions",_approximateTransitions},{"showArms",_armsShown},{"workspace",_workspace},
        {"hand",int(_editedHand)},{"fingerJoint",_fingerJointIndex},
        {"selectedHandPose",selectedHandPose()?selectedHandPose()->id:QString()}};
    QJsonObject choices;
    const auto sourcePlugins=selectionPlugins(_selectedWeapon);
    if(allWeaponsSelected())record["browserPlugin"]=kAllWeaponsSource;
    else if(!sourcePlugins.isEmpty())record["browserPlugin"]=sourcePlugins.contains(_browserPlugin,Qt::CaseInsensitive)?_browserPlugin:sourcePlugins.first();
    if (const auto* config = configuration()) for (const auto& slot : config->entries()) choices[QString::number(slot.key)] = QString::number(config->selected(slot.key));
    record["choices"] = choices;
    record["authoringRoadmaps"]=_authoringRoadmaps;
    record["reloadTemplates"]=_reloadTemplateAssignments;
    record["sourcePolicies"]=_sourcePolicies;
    if(const auto* config=configuration())record["recordChoices"]=config->savedChoices(*currentPlugin());
    record["simulationSpringReturn"] = _slideSpringReturn;
    record["camera"] = _window->getGLView()->studioCameraState();
    record["glockReloadNoExtender"] = _glockNoExtender;
    QJsonObject magazineGrabs;for(auto it=_magazineGrabGrips.cbegin();it!=_magazineGrabGrips.cend();++it)magazineGrabs[it.key()]=it.value();
    record["magazineGrabGrips"]=magazineGrabs;
    record["handsPage"]=int(_handEditorPage);
    record["interactionHand"]=_interactionHand==PoseHand::Left?"left":"right";
    QJsonObject sounds;for(auto it=_savedSounds.cbegin();it!=_savedSounds.cend();++it)sounds[it.key()]=soundProfileJson(it.value());
    if(!_soundProfile.weapon.isEmpty())sounds[soundProfileKey()]=soundProfileJson(_soundProfile);
    record["soundProfiles"]=sounds;
    QJsonObject locations;for(auto it=_grabLocations.cbegin();it!=_grabLocations.cend();++it)locations[it.key()]=grabLocationsJson(it.value());record["grabLocations"]=locations;
    QJsonObject optics;for(auto it=_scopeProfiles.cbegin();it!=_scopeProfiles.cend();++it)optics[it.key()]=scopeProfileJson(it.value());record["scopeProfiles"]=optics;
    record["scopeSelection"]=currentScopeKey();
    QJsonObject operating;for(auto it=_savedOperatingGrips.cbegin();it!=_savedOperatingGrips.cend();++it)operating[it.key()]=it.value();record["operatingGrips"]=operating;
    QJsonObject magazineDefaults;
    for(auto it=_magazineGrabDefaults.cbegin();it!=_magazineGrabDefaults.cend();++it)magazineDefaults[it.key()]=it.value();
    record["magazineGrabDefaults"]=magazineDefaults;
    if(_simulationRig&&_reloadRecipe)record["simulationSetup"]=simulationRigJson(*_reloadRecipe,*_simulationRig,_slideSpringReturn);
    if(_magazineRail)record["magazineInteraction"]=magazineRailJson(*_magazineRail);
    if(_simulationAmmo.valid())record["ammunitionConfiguration"]=_simulationAmmo.json();
    QString error;
    rememberMagazineGrip();
    QJsonObject magazineDrafts;
    for(auto it=_magazineGripDrafts.cbegin();it!=_magazineGripDrafts.cend();++it) {
        const auto bytes=encodePartGrip(it.value(),error);
        if(bytes.isEmpty()){setStatus("Cannot save magazine draft: "+error,true);return false;}
        magazineDrafts[it.key()]=QJsonDocument::fromJson(bytes).object();
    }
    record["magazineGripDrafts"]=magazineDrafts;
    QJsonArray fingers;
    for (const auto& draft : _fingerDrafts) {
        if (!draft) { fingers.append(QJsonValue()); continue; }
        const auto bytes = encodeHandPose(*draft,error);
        if (!error.isEmpty()) { setStatus(error,true); return false; }
        fingers.append(QJsonDocument::fromJson(bytes).object());
    }
    record["fingerDrafts"] = fingers;
    if (_handPosePreview) {
        const auto bytes=encodeHandPose(*_handPosePreview,error);
        if(!error.isEmpty()){setStatus(error,true);return false;}
        record["handPreview"]=QJsonDocument::fromJson(bytes).object();
    }
    const auto& authoredGrip=_reloadWorkspaceActive?_suspendedAuthoringGrip:simulationWorkspace(_workspace)?_operateSuspendedGrip:_partGripDraft;
    if (authoredGrip) {
        const auto bytes = encodePartGrip(*authoredGrip,error);
        if (!error.isEmpty()) { setStatus("Finish the grip before saving: "+error,true); return false; }
        record["gripDraft"] = QJsonDocument::fromJson(bytes).object();
    }
    if (_mechanismDraft && !_mechanismDraft->stages.isEmpty()) {
        const auto bytes = encodeMechanism(*_mechanismDraft,error);
        if (!error.isEmpty()) { setStatus("Finish the mechanism step before saving: "+error,true); return false; }
        record["mechanismDraft"] = QJsonDocument::fromJson(bytes).object();
    }
    if (_reloadRecipe && !_reloadCapturing) {
        const auto bytes = encodeReloadRecipe(*_reloadRecipe,error);
        if (!error.isEmpty()) { setStatus(error,true); return false; }
        record["reloadDraft"] = QJsonDocument::fromJson(bytes).object();
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const auto bytes = QJsonDocument(record).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { setStatus("Project save failed: "+file.errorString(),true); return false; }
    _sessionPath = absolute;
    if(!_libraryMode&&!currentTestbed().isEmpty())QSettings().setValue("testbeds/"+currentTestbed(),absolute);
    QSettings().setValue("projects/lastProject",absolute);
    setStatus("Project saved · "+QFileInfo(path).fileName());
    return true;
}
void ReloadMakerController::openSession(const QString& path) {
    if (_loadThread || _reloadCapturing) { setStatus("Finish or cancel the current load first.",true); return; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64*1024*1024) { setStatus("Cannot read this project.",true); return; }
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto record = parsed.object();
    if (parseError.error != QJsonParseError::NoError || record["format"] != 1 || record["kind"] != "reload-maker-v2-project") { setStatus("Unsupported or invalid Reload Maker project.",true); return; }
    if (!QFileInfo(record["gameData"].toString()).isDir() || (!record["library"].toBool() && !QFileInfo(record["package"].toString()).exists())) { setStatus("Project source is missing. Restore its original package/Data path before opening.",true); return; }
    // Validate all authored data before changing source or replacing a draft.
    QString error;
    if(record.contains("reloadTemplates")) {
        const auto templates=record["reloadTemplates"].toObject();
        if(!record["reloadTemplates"].isObject()||templates.size()>512){setStatus("Invalid reload template assignments.",true);return;}
        for(auto it=templates.begin();it!=templates.end();++it)if(it.key().isEmpty()||it.key().size()>512||!templateMechanism(it.value().toString())){setStatus("Unsupported reload template assignment.",true);return;}
    }
    if(record.contains("sourcePolicies")) {
        const auto policies=record["sourcePolicies"].toObject();
        if(!record["sourcePolicies"].isObject()||policies.size()>512){setStatus("Invalid saved source policies.",true);return;}
        for(auto it=policies.begin();it!=policies.end();++it)if(it.key().isEmpty()||it.key().size()>1024||!it.value().isObject()||!validSourcePolicy(it.value().toObject(),error)){setStatus("Invalid saved source policy: "+error,true);return;}
    }
    if(record.contains("authoringRoadmaps")) {
        const auto plans=record["authoringRoadmaps"].toObject();
        if(!record["authoringRoadmaps"].isObject()||plans.size()>512){setStatus("Invalid authoring roadmap collection.",true);return;}
        for(const auto& value:plans){const auto plan=value.toObject();const int step=plan["step"].toInt(-1);
            OperatingSetup setup;MagazineGuide guide;MagazineSetup magazine;MagazineReference reference;
            if(!value.isObject()||step<0||step>11||(plan.contains("sources")&&!validSourceEvidence(plan["sources"].toObject(),error))||
                 (plan.contains("operating")&&!readOperatingSetup(plan["operating"].toObject(),setup,error))||
                 (plan.contains("magazineGuide")&&!readMagazineGuide(plan["magazineGuide"].toObject(),guide,error))||
                 (plan.contains("magazineReference")&&!readMagazineReference(plan["magazineReference"].toObject(),reference,error))||
                 (plan.contains("reviews")&&(!plan["reviews"].isObject()||!validAuthoringReviews(plan["reviews"].toObject())))||
                 (plan.contains("collisionDrafts")&&(!plan["collisionDrafts"].isObject()||!validCollisionDrafts(plan["collisionDrafts"].toObject())))||
                 (plan.contains("magazineSetup")&&!readMagazineSetup(plan["magazineSetup"].toObject(),magazine,error))){setStatus("Invalid saved authoring roadmap: "+error,true);return;}}
    }
    if(record.contains("recordChoices")) {
        const auto choices=record["recordChoices"].toObject();
        if(!record["recordChoices"].isObject()||choices.size()>1024){setStatus("Invalid saved attachment choices.",true);return;}
        for(auto it=choices.begin();it!=choices.end();++it)if(!it.value().isString()||it.key().size()>1024||it.value().toString().size()>1024){setStatus("Invalid saved attachment identity.",true);return;}
    }
    if(record.contains("scopeProfiles")) {
        if(!record["scopeProfiles"].isObject()||record["scopeProfiles"].toObject().size()>128){setStatus("Invalid scope profiles.",true);return;}
        const auto optics=record["scopeProfiles"].toObject();for(auto it=optics.begin();it!=optics.end();++it){ScopeProfile p;
            if(!it.value().isObject()||!readScopeProfile(it.value().toObject(),p,error)||p.model!=it.key()){setStatus("Invalid saved scope profile: "+error,true);return;}}
    }
    if(record.contains("grabLocations")) {
        if(!record["grabLocations"].isObject()||record["grabLocations"].toObject().size()>128){setStatus("Invalid grab locations.",true);return;}
        const auto locations=record["grabLocations"].toObject();for(auto it=locations.begin();it!=locations.end();++it){OperatingGrabLocations g;
            if(!it.value().isObject()||!readGrabLocations(it.value().toObject(),g,error)||it.key()!=grabLocationsKey(g)){setStatus("Invalid grab locations: "+error,true);return;}}
    }
    const auto magazineDefaults=record["magazineGrabDefaults"].toObject();
    if(record.contains("soundProfiles")){
        if(!record["soundProfiles"].isObject()||record["soundProfiles"].toObject().size()>128){setStatus("Invalid sound profiles.",true);return;}
        const auto sounds=record["soundProfiles"].toObject();for(auto it=sounds.begin();it!=sounds.end();++it){SoundProfile profile;
            if(!it.value().isObject()||!readSoundProfile(it.value().toObject(),profile,error)||it.key()!=(profile.weapon+"|"+profile.animation).toLower()){setStatus("Invalid saved sound profile: "+error,true);return;}}
    }
    if((record.contains("magazineGrabDefaults")&&!record["magazineGrabDefaults"].isObject())||magazineDefaults.size()>512){setStatus("Invalid magazine defaults.",true);return;}
    for(auto it=magazineDefaults.begin();it!=magazineDefaults.end();++it)if(it.key().isEmpty()||it.key().size()>512||!it.value().isString()||QUuid(it.value().toString()).isNull()){setStatus("Invalid magazine default identity.",true);return;}
    const auto operating=record["operatingGrips"].toObject();
    if((record.contains("operatingGrips")&&!record["operatingGrips"].isObject())||operating.size()>64){setStatus("Invalid operating grips.",true);return;}
    for(auto it=operating.begin();it!=operating.end();++it)if(it.key().size()>1024||!it.key().contains('|')||QUuid(it.value().toString()).isNull()){setStatus("Invalid operating grip identity.",true);return;}
    const auto magazineDrafts=record["magazineGripDrafts"].toObject();
    if((record.contains("magazineGripDrafts")&&!record["magazineGripDrafts"].isObject())||magazineDrafts.size()>512){setStatus("Invalid magazine drafts.",true);return;}
    for(auto it=magazineDrafts.begin();it!=magazineDrafts.end();++it) {
        PartGrip grip;
        if(!decodePartGrip(QJsonDocument(it.value().toObject()).toJson(),grip,error)||(it.key()!=magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord)&&it.key()!=handAssignmentKey(magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord),grip.pose.hand))){setStatus("Invalid magazine draft: "+error,true);return;}
    }
    const auto fingerDrafts=record["fingerDrafts"].toArray();
    if(record.contains("magazineGrabGrips")&&!record["magazineGrabGrips"].isObject()){setStatus("Invalid magazine grip assignments.",true);return;}
    const auto magazineGrabs=record["magazineGrabGrips"].toObject();
    if(magazineGrabs.size()>512){setStatus("Too many magazine grip assignments.",true);return;}
    for(auto it=magazineGrabs.begin();it!=magazineGrabs.end();++it)if(!it.key().contains('|')||it.key().size()>1024||!it.value().isString()||QUuid(it.value().toString()).isNull()) {
        setStatus("Invalid magazine grip assignment identity.",true);return;
    }
    if(fingerDrafts.size()>2){setStatus("Invalid physical-hand draft count.",true);return;}
    for (int i=0;i<fingerDrafts.size();++i) {
        const auto v=fingerDrafts[i];
        if (v.isNull()) continue;
        HandPose draft;
        if (!decodeHandPose(QJsonDocument(v.toObject()).toJson(),draft,error)) { setStatus(error,true); return; }
        if(int(draft.hand)!=i){setStatus("Finger draft belongs to a different physical hand.",true);return;}
    }
    if(record.contains("handPreview")){HandPose v;if(!decodeHandPose(QJsonDocument(record["handPreview"].toObject()).toJson(),v,error)){setStatus(error,true);return;}}
    if (record.contains("gripDraft")) { PartGrip v; if (!decodePartGrip(QJsonDocument(record["gripDraft"].toObject()).toJson(),v,error)) {setStatus(error,true);return;} }
    if (record.contains("mechanismDraft")) { PartMechanism v; if (!decodeMechanism(QJsonDocument(record["mechanismDraft"].toObject()).toJson(),v,error)) {setStatus(error,true);return;} }
    if (record.contains("reloadDraft")) { ReloadRecipe v; if (!decodeReloadRecipe(QJsonDocument(record["reloadDraft"].toObject()).toJson(),v,error)) {setStatus(error,true);return;} }
    _pendingSession = record;
    _pendingReloadDraft = {};
    _sessionPath = QFileInfo(path).absoluteFilePath();
    _gameDataText = record["gameData"].toString();
    if (!record["mo2Instance"].toString().isEmpty()) _mo2Instance = record["mo2Instance"].toString();
    if(record["library"].toBool()&&!record["mo2Profile"].toString().isEmpty())_requestedMo2Profile=record["mo2Profile"].toString();
    if (record["library"].toBool()) startLibraryLoad();
    else startPackageLoad(record["package"].toString(), record["gameData"].toString());
}
void ReloadMakerController::restoreSessionSelection() {
    if (_pendingSession.isEmpty()) return;
    const auto record = _pendingSession;
    _pendingSession = {};
    int selected = -1;
    for (int i = 0; i < _weaponSelections.size(); ++i) {
        _selectedWeapon = i;
        if (currentWeapon() && poseRecordKey(currentWeapon()->formId) == record["weapon"].toString()&&
            (record["browserPlugin"].toString().isEmpty()||(_libraryMode&&record["browserPlugin"]==kAllWeaponsSource)||selectionPlugins(i).contains(record["browserPlugin"].toString(),Qt::CaseInsensitive))) { selected = i; break; }
    }
    if (selected < 0) {
        _selectedWeapon = -1;
        refreshWeaponPicker();
        setStatus("The project weapon is absent or excluded from this source's player-weapon library. See Diagnostics for exclusion reasons.",true);return;
    }
    alignPluginToWeapon();refreshWeaponPicker();
    _armsBody = isArmsBody(record["body"].toString()) ? record["body"].toString() : QStringLiteral("male");
    showArmsBody();
    refreshAttachments();
    if (auto* config = configuration()) {
        if(record.contains("recordChoices")) {
            for(const auto& missing:config->restoreChoices(*currentPlugin(),record["recordChoices"].toObject()))
                appendDiagnostic("PROJECT: unavailable saved record choice "+missing);
        } else {
        const auto choices = record["choices"].toObject();
        for (auto it = choices.begin(); it != choices.end(); ++it) {
            if (!config->select(it.key().toULongLong(),it.value().toString().toUInt())) appendDiagnostic("PROJECT: unavailable saved slot choice "+it.key());
        }
        }
    }
    refreshAttachments();
    _glockNoExtender=record["glockReloadNoExtender"].toBool(false);
    _authoringRoadmaps=record["authoringRoadmaps"].toObject();
    _reloadTemplateAssignments=record["reloadTemplates"].toObject();
    _sourcePolicies=record["sourcePolicies"].toObject();
    _savedSounds.clear();const auto sounds=record["soundProfiles"].toObject();for(auto it=sounds.begin();it!=sounds.end();++it){SoundProfile profile;QString error;if(readSoundProfile(it.value().toObject(),profile,error))_savedSounds.insert(it.key(),profile);}
    _scopeProfiles.clear();const auto optics=record["scopeProfiles"].toObject();for(auto it=optics.begin();it!=optics.end();++it){ScopeProfile p;QString error;if(readScopeProfile(it.value().toObject(),p,error))_scopeProfiles.insert(it.key(),p);}
    _interactionHand=record["interactionHand"].toString(isM1AConfiguration()?"right":"left")=="right"&&!isGlockConfiguration()?PoseHand::Right:PoseHand::Left;
    _grabLocations.clear();const auto locations=record["grabLocations"].toObject();for(auto it=locations.begin();it!=locations.end();++it){OperatingGrabLocations g;QString e;if(readGrabLocations(it.value().toObject(),g,e))_grabLocations[it.key()]=g;}
    _savedOperatingGrips.clear();const auto operating=record["operatingGrips"].toObject();for(auto it=operating.begin();it!=operating.end();++it)_savedOperatingGrips[it.key()]=it.value().toString();
    _magazineGrabGrips.clear();const auto magazineGrabs=record["magazineGrabGrips"].toObject();
    for(auto it=magazineGrabs.begin();it!=magazineGrabs.end();++it)_magazineGrabGrips[it.key().toLower()]=it.value().toString();
    _magazineGrabDefaults.clear();const auto magazineDefaults=record["magazineGrabDefaults"].toObject();
    for(auto it=magazineDefaults.begin();it!=magazineDefaults.end();++it)_magazineGrabDefaults[it.key().toLower()]=it.value().toString();
    _partGripDraft.reset();_operateSuspendedGrip.reset();_suspendedAuthoringGrip.reset();_editingMagazine=false;
    _magazineGripDrafts.clear();const auto magazineDrafts=record["magazineGripDrafts"].toObject();
    for(auto it=magazineDrafts.begin();it!=magazineDrafts.end();++it){PartGrip grip;QString e;if(decodePartGrip(QJsonDocument(it.value().toObject()).toJson(),grip,e))_magazineGripDrafts[handAssignmentKey(magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord),grip.pose.hand)]=grip;}
    rebuildWeapon();
    if (!_clip.valid()) return;
    if (const int action = actionIndex(record["animation"].toString()); action >= 0) selectAction(action);
    setWeaponFixedMotion(record["motionMode"].toInt()==0);
    setApproximateTransitions(record["transitions"].toBool());
    setArmsShown(record["showArms"].toBool(true));
    _playbackTime = std::clamp(float(record["seconds"].toDouble()),0.f,playbackDuration());
    QString error;
    const auto fingers = record["fingerDrafts"].toArray();
    for (int i = 0; i < std::min(2,int(fingers.size())); ++i) if (fingers[i].isObject()) {
        HandPose pose; if (decodeHandPose(QJsonDocument(fingers[i].toObject()).toJson(),pose,error)) _fingerDrafts[i] = pose;
    }
    if(record.contains("handPreview")){HandPose v;if(decodeHandPose(QJsonDocument(record["handPreview"].toObject()).toJson(),v,error))_handPosePreview=v;}
    setEditedHand(record["hand"].toInt(int(PoseHand::Right))==int(PoseHand::Left)?PoseHand::Left:PoseHand::Right);
    setFingerJoint(std::clamp(record["fingerJoint"].toInt(),0,14));
    if(handPoseListed(record["selectedHandPose"].toString()))selectHandPose(record["selectedHandPose"].toString());
    if (record.contains("mechanismDraft")) {
        PartMechanism part; if (decodeMechanism(QJsonDocument(record["mechanismDraft"].toObject()).toJson(),part,error)) { _mechanismDraft=part;displayMechanism(); }
    }
    if (record.contains("gripDraft")) {
        PartGrip grip;
        if (decodePartGrip(QJsonDocument(record["gripDraft"].toObject()).toJson(),grip,error)) {
            QVector<HandPosePlacement> anchors; for (const auto& a : _handPoseAnchors) anchors.append(a.description);
            const int anchor = resolveGripAnchor(grip,poseRecordKey(currentWeapon()->formId),anchors,error);
            if (anchor >= 0 && preparePartGripFrame(anchor,error)) {
                _partGripDraft=grip;_gripName->setText(grip.pose.name);
                const QSignalBlocker poseBlock(_gripPoseCombo),partBlock(_gripReferenceCombo);
                _gripPoseCombo->setCurrentIndex(_gripPoseCombo->findData(grip.sourcePoseId));
                _gripReferenceCombo->setCurrentIndex(anchor);
                _editingMagazine=magazineGrabAnchor(grip.pose.placement)>=0;
                if(record["workspace"].toInt()==GripsWorkspace)setHandEditorPage(HandEditorPage::PartGrip);
                showPartGripValues();
            }
        }
    }
    (void)applyPose(_playbackTime); updateTimelineText(); refreshFingerEditor();
    setSlideSpringReturn(record["simulationSpringReturn"].toBool(true));
    const int workspace=record["workspace"].toInt(OperateWorkspace);
    setWorkspace(validWorkspace(workspace)?workspace:OperateWorkspace);
    if(record.contains("scopeSelection")){const auto model=record["scopeSelection"].toString();for(int i=0;i<_scopePart->count();++i){
        const auto anchor=_scopePart->itemData(i).toInt();if(anchor>0&&anchor<_handPoseAnchors.size()&&scopeModelKey(_handPoseAnchors[anchor].description.ownerModel)==model){_scopePart->setCurrentIndex(i);break;}}}
    if(workspace==GripsWorkspace&&record.contains("handsPage"))setHandEditorPage(static_cast<HandEditorPage>(std::clamp(record["handsPage"].toInt(),0,3)));
    if(record.contains("camera")&&!_window->getGLView()->studioRestoreCamera(record["camera"].toObject()))appendDiagnostic("PROJECT: invalid saved camera ignored.");
    if (record.contains("reloadDraft")) {
        // Native recapture establishes live node bindings before restoring the
        // saved trajectory/phase document. Source identity must still match.
        _pendingReloadDraft = record["reloadDraft"].toObject();
    }
    ++_revision;
    if(workspace==GripsWorkspace&&_handEditorPage==HandEditorPage::GrabLocations)prepareGrabLocationFrame();
    setStatus(error.isEmpty() ? "Project restored · "+QFileInfo(_sessionPath).fileName() : "Project restored with a grip warning: "+error,!error.isEmpty());
}
}
