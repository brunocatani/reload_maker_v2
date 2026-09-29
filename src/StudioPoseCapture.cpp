#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUuid>
#include <algorithm>

namespace reloadmaker {
bool ReloadMakerController::authorReloadGrips() {
    if(_loadThread||_reloadCapturing||!_simulationRig||!_reloadRecipe) {
        setStatus("Wait for the configured reload to finish preparing.",true);return false;
    }
    const auto magazine=_reloadRecipe->tracks[_simulationRig->magazine].anchor;
    const float seconds=_simulationActive&&_simulation.action==SimulationAction::Reload?_simulation.seconds:0.f;
    int anchor=-1;
    for(int i=0;i<_handPoseAnchors.size();++i)if(sameMechanismPart(_handPoseAnchors[i].description,magazine)) {
        if(anchor>=0){setStatus("The magazine frame is ambiguous in this assembly.",true);return false;}anchor=i;
    }
    if(anchor<0){setStatus("The configured magazine has no capture frame.",true);return false;}
    setWorkspace(2);restoreHandPosePreview();_handPosePages->setCurrentIndex(0);
    _motionMode->setCurrentIndex(0);
    if(!selectSimulationClip(false,true)){setStatus(_simulationError,true);return false;}
    _playbackTime=std::min(seconds,playbackDuration());
    _handPoseAnchor->setCurrentIndex(_handPoseAnchor->findData(anchor));
    _handPoseSide->setCurrentIndex(_handPoseSide->findData(int(PoseHand::Left)));
    _showArmsCheck->setChecked(true);
    if(!applyPose(_playbackTime))return false;
    updateTimelineText();refreshFingerEditor();updateHandPoseControls();
    setStatus("Weapon fixed. Scrub to the hand pose you want and save it. Select a saved pose to place it on a weapon part.");
    return true;
}

void ReloadMakerController::applySavedPoseToPart() {
    const auto* pose=selectedHandPose();
    if(!pose){setStatus("Select a saved hand pose first.",true);return;}
    const auto name=pose->name;
    restoreHandPosePreview();openPartGripEditor();_gripName->setText(name);
    beginGripSurfacePick();
}

int ReloadMakerController::magazineGrabAnchor(const HandPosePlacement& placement)const {
    if(placement.anchorName!="WeaponMagazine"&&placement.anchorName!="WeaponExtra3")return -1;
    for(int i=0;i<_handPoseAnchors.size();++i) {
        const auto& candidate=_handPoseAnchors[i].description;
        if(magazineGrabSupported(candidate)&&candidate.ownerRecord.compare(placement.ownerRecord,Qt::CaseInsensitive)==0&&
            normalizeNifPath(candidate.ownerModel)==normalizeNifPath(placement.ownerModel))return i;
    }
    return -1;
}

bool ReloadMakerController::useMagazineGrabGrip() {
    const int anchor=_partGripDraft?magazineGrabAnchor(_partGripDraft->pose.placement):-1;
    if(anchor<0) {
        setStatus("Place the pose on the moving magazine before assigning it. Glock drums are excluded.",true);return false;
    }
    updatePartGripControls();if(!_gripSave->isEnabled()){setStatus("Place the hand within reach and name the grip before assigning it.",true);return false;}
    PartGrip grip=*_partGripDraft;
    Transformf weaponInverse;
    if(!invertUniformTransform(_globalPose[_weaponBoneIndex],weaponInverse)){setStatus("The weapon reference is not invertible.",true);return false;}
    const auto handInWeapon=composed(weaponInverse,composed(gripReferenceWorld(),grip.pose.placement.handInAnchor));
    const auto& target=_handPoseAnchors[anchor];
    if(!sameMechanismPart(grip.pose.placement,target.description)) {
        // The native Glock reload uses an auxiliary magazine body. Convert
        // between the two authored model frames, never between their current
        // animated positions (the fresh copy may be parked off-weapon).
        int source=-1;
        for(int i=0;i<_handPoseAnchors.size();++i)if(sameMechanismPart(_handPoseAnchors[i].description,grip.pose.placement)){source=i;break;}
        if(source<0){setStatus("The captured magazine frame is no longer available.",true);return false;}
        const auto rest=[&](int node){Transformf result;for(int depth=0;node>=0&&depth<_assemblyNodes.size();++depth){result=composed(_assemblyNodes[node].restLocal,result);node=_assemblyNodes[node].parent;}return result;};
        const int sourceNode=_sceneNodes.indexOf(_handPoseAnchors[source].node),targetNode=_sceneNodes.indexOf(target.node);
        Transformf inverse;if(sourceNode<0||targetNode<0||!invertUniformTransform(rest(targetNode),inverse)){setStatus("Cannot convert the magazine's authored frames.",true);return false;}
        const auto bridge=composed(inverse,rest(sourceNode));
        const auto placement=composed(bridge,grip.pose.placement.handInAnchor);
        grip.pose.placement=target.description;grip.pose.placement.handInAnchor=placement;
        if(grip.surfaceContact)grip.surfaceContact->pointInAnchor=composed(bridge,Transformf{grip.surfaceContact->pointInAnchor}).translation;
        grip.mechanismId.clear();
    }
    grip.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    grip.pose.placement.handInWeapon=handInWeapon;
    grip.pose.name=_gripName->text().trimmed();grip.pose.createdUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    grip.pose.source=currentPoseSource();
    QString error;if(!_partGripLibrary.save(grip,error)){setStatus(error,true);return false;}
    _magazineGrabGrips[magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord)]=grip.pose.id;
    if(_reloadRecipe)for(auto& window:_reloadRecipe->windows)for(auto& member:window.members)
        if(sameMechanismPart(_reloadRecipe->tracks[member.track].anchor,grip.pose.placement))member.gripId=grip.pose.id;
    refreshPartGripLibrary();
    setStatus("“"+grip.pose.name+"” is assigned to this magazine's grab and reload. Save the project to retain the assignment.");
    return true;
}

void ReloadMakerController::resolveMagazineGrabGrip() {
    _magazineGrabGrip.reset();_magazineGrabBinding=-1;_magazineGrabError.clear();
    const QSignalBlocker block(_simulationMagazineGrip);
    _simulationMagazineGrip->clear();_simulationMagazineGrip->addItem("Magazine grip · native hand",QString());
    if(!_simulationRig||!_reloadRecipe)return;
    const auto& anchor=_reloadRecipe->tracks[_simulationRig->magazine].anchor;
    if(!magazineGrabSupported(anchor))return;
    const auto id=_magazineGrabGrips.value(magazineGrabKey(poseRecordKey(currentWeapon()->formId),anchor.ownerRecord));
    for(const auto& grip:_partGrips)if(grip.weaponRecord.compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)==0&&sameMechanismPart(grip.pose.placement,anchor))
        _simulationMagazineGrip->addItem(grip.pose.name+" · "+(grip.pose.hand==PoseHand::Left?"Left":"Right"),grip.pose.id);
    const int selectedIndex=_simulationMagazineGrip->findData(id);if(selectedIndex>=0)_simulationMagazineGrip->setCurrentIndex(selectedIndex);
    if(id.isEmpty())return;
    const PartGrip* selected=nullptr;
    for(const auto& grip:_partGrips)if(grip.pose.id==id) {
        if(selected){_magazineGrabError="Duplicate magazine grip ID.";return;}selected=&grip;
    }
    if(!selected||selected->weaponRecord.compare(poseRecordKey(currentWeapon()->formId),Qt::CaseInsensitive)!=0||!sameMechanismPart(selected->pose.placement,anchor)) {
        _magazineGrabError="The assigned magazine grip is missing or targets a different part.";return;
    }
    for(int i=0;i<_nodeBindings.size();++i)if(_nodeBindings[i].nodeIndex==_reloadTrackNodes[_simulationRig->magazine]){_magazineGrabBinding=i;break;}
    if(_magazineGrabBinding<0){_magazineGrabError="The magazine grip has no live binding.";return;}
    _magazineGrabGrip=*selected;
    for(auto& window:_reloadRecipe->windows)for(auto& member:window.members)
        if(member.track==_simulationRig->magazine&&member.gripId.isEmpty())member.gripId=id;
}

bool ReloadMakerController::applyMagazineGrabPose() {
    if(!_simulationActive||!_magazineGrabGrip)return true;
    if(!_simulation.magazine.carried()){_magazineGrabError.clear();return true;}
    const auto target=composed(_boundGlobalPose[_magazineGrabBinding],_magazineGrabGrip->pose.placement.handInAnchor);
    if(!solvePartGrip(_skeleton,_localPose,_globalPose,_magazineGrabGrip->pose,target,_localPose,_globalPose,_magazineGrabError,GripReach::FollowTarget))return true;
    // Arm composition preserves the weapon; restore its manually driven part
    // targets after evaluating the updated hand skeleton bindings.
    return evaluateSceneBindings(_skeleton,_nodeBindings,_sourceLocalPose,_stateBaselineLocalPose,
        _globalPose,_boundLocalPose,_boundGlobalPose,_poseError)&&applySimulationPose();
}
}
