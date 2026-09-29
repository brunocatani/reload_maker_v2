#include "ReloadMakerController.h"
#include "StudioPaths.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidget>
#include <QLineEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>

namespace reloadmaker {
bool ReloadMakerController::saveSession(const QString& requested) {
    if (!currentWeapon() || _loadThread) return true;
    const QString path = requested.isEmpty() ? (_sessionPath.isEmpty() ? studioDataPath("workspace/current.project.json") : _sessionPath) : requested;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!absolute.startsWith(QDir(studioRoot()).absolutePath()+"/", Qt::CaseInsensitive)) { setStatus("Save projects inside the Reload Maker V2 workspace.", true); return false; }
    QJsonObject record{{"format",1},{"kind","reload-maker-v2-project"},{"package",_package.sourcePath},{"gameData",_gameData},
        {"library",_libraryMode},{"mo2Instance",_mo2Path->text()},{"weapon",poseRecordKey(currentWeapon()->formId)},
        {"body",_bodyCombo->currentData().toString()},{"animationSet",_animationSetCombo->currentText()},
        {"animation",_activeAnimationPath},{"seconds",_playbackTime},{"motionMode",_motionMode->currentIndex()},
        {"transitions",_transitionCheck->isChecked()},{"showArms",_showArmsCheck->isChecked()},{"workspace",_workspace},
        {"hand",_handPoseSide->currentData().toInt()},{"fingerJoint",_fingerJoint->currentIndex()},
        {"selectedHandPose",selectedHandPose()?selectedHandPose()->id:QString()}};
    QJsonObject choices;
    if (const auto* config = configuration()) for (const auto& slot : config->entries()) choices[QString::number(slot.key)] = QString::number(config->selected(slot.key));
    record["choices"] = choices;
    record["simulationSpringReturn"] = _simulationSpring->isChecked();
    if(_simulationRig&&_reloadRecipe)record["simulationSetup"]=simulationRigJson(*_reloadRecipe,*_simulationRig,_simulationSpring->isChecked());
    if(_magazineRail)record["magazineInteraction"]=magazineRailJson(*_magazineRail);
    if(_simulationAmmo.valid())record["ammunitionConfiguration"]=_simulationAmmo.json();
    QString error;
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
    const auto& authoredGrip=_reloadWorkspaceActive?_suspendedAuthoringGrip:_partGripDraft;
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
    const auto fingerDrafts=record["fingerDrafts"].toArray();
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
    _gameDataPath->setText(record["gameData"].toString());
    _mo2Path->setText(record["mo2Instance"].toString());
    if (record["library"].toBool()) startLibraryLoad();
    else startPackageLoad(record["package"].toString(), record["gameData"].toString());
}
void ReloadMakerController::restoreSessionSelection() {
    if (_pendingSession.isEmpty()) return;
    const auto record = _pendingSession;
    _pendingSession = {};
    int selected = -1;
    for (int i = 0; i < _weaponSelections.size(); ++i) {
        const QSignalBlocker block(_weaponCombo); _weaponCombo->setCurrentIndex(i);
        if (currentWeapon() && poseRecordKey(currentWeapon()->formId) == record["weapon"].toString()) { selected = i; break; }
    }
    if (selected < 0) { setStatus("The project weapon is absent from this source.",true); return; }
    { const QSignalBlocker block(_bodyCombo); _bodyCombo->setCurrentIndex(std::max(0,_bodyCombo->findData(record["body"].toString()))); }
    refreshAttachments(); refreshConfigurationChoices();
    if (auto* config = configuration()) {
        const auto choices = record["choices"].toObject();
        for (auto it = choices.begin(); it != choices.end(); ++it) {
            if (!config->select(it.key().toULongLong(),it.value().toString().toUInt())) appendDiagnostic("PROJECT: unavailable saved slot choice "+it.key());
        }
    }
    refreshAttachments(); refreshConfigurationChoices();
    const int animationSet = _animationSetCombo->findText(record["animationSet"].toString());
    if (animationSet < 0 && _animationSetCombo->count() > 0) { setStatus("The project's animation set no longer exists.",true); return; }
    { const QSignalBlocker block(_animationSetCombo); _animationSetCombo->setCurrentIndex(animationSet); }
    rebuildWeapon();
    if (!_clip.valid()) return;
    for (int i = 0; i < _animationList->count(); ++i) if (_animationList->item(i)->data(Qt::UserRole).toString() == record["animation"].toString()) { _animationList->setCurrentRow(i); selectAnimation(_animationList->item(i)); break; }
    _motionMode->setCurrentIndex(record["motionMode"].toInt());
    _transitionCheck->setChecked(record["transitions"].toBool());
    _showArmsCheck->setChecked(record["showArms"].toBool(true));
    _playbackTime = std::clamp(float(record["seconds"].toDouble()),0.f,playbackDuration());
    QString error;
    const auto fingers = record["fingerDrafts"].toArray();
    for (int i = 0; i < std::min(2,int(fingers.size())); ++i) if (fingers[i].isObject()) {
        HandPose pose; if (decodeHandPose(QJsonDocument(fingers[i].toObject()).toJson(),pose,error)) _fingerDrafts[i] = pose;
    }
    if(record.contains("handPreview")){HandPose v;if(decodeHandPose(QJsonDocument(record["handPreview"].toObject()).toJson(),v,error))_handPosePreview=v;}
    _handPoseSide->setCurrentIndex(std::max(0,_handPoseSide->findData(record["hand"].toInt(int(PoseHand::Right)))));
    _fingerJoint->setCurrentIndex(std::clamp(record["fingerJoint"].toInt(),0,14));
    for(int i=0;i<_handPoseList->count();++i)if(_handPoseList->item(i)->data(Qt::UserRole).toString()==record["selectedHandPose"].toString())_handPoseList->setCurrentRow(i);
    if (record.contains("mechanismDraft")) {
        PartMechanism part; if (decodeMechanism(QJsonDocument(record["mechanismDraft"].toObject()).toJson(),part,error)) { _mechanismDraft=part;displayMechanism(); }
    }
    if (record.contains("gripDraft")) {
        PartGrip grip;
        if (decodePartGrip(QJsonDocument(record["gripDraft"].toObject()).toJson(),grip,error)) {
            QVector<HandPosePlacement> anchors; for (const auto& a : _handPoseAnchors) anchors.append(a.description);
            const int anchor = resolveGripAnchor(grip,poseRecordKey(currentWeapon()->formId),anchors,error);
            if (anchor >= 0 && preparePartGripFrame(anchor,error)) { _partGripDraft=grip;showPartGripValues(); }
        }
    }
    (void)applyPose(_playbackTime); updateTimelineText(); refreshFingerEditor();
    _simulationSpring->setChecked(record["simulationSpringReturn"].toBool(true));
    setWorkspace(std::clamp(record["workspace"].toInt(),0,5));
    if (record.contains("reloadDraft")) {
        // Native recapture establishes live node bindings before restoring the
        // saved trajectory/phase document. Source identity must still match.
        _pendingReloadDraft = record["reloadDraft"].toObject();
    }
    ++_revision;
    setStatus(error.isEmpty() ? "Project restored · "+QFileInfo(_sessionPath).fileName() : "Project restored with a grip warning: "+error,!error.isEmpty());
}
}
