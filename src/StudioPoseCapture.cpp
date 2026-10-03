#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include <QComboBox>
#include <QDateTime>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUuid>
#include <algorithm>

namespace reloadmaker {
void ReloadMakerController::applySavedPoseToPart() {
    _editingMagazine=false;
    const auto* pose=selectedHandPose();
    if(!pose){setStatus("Select a saved hand pose first.",true);return;}
    const auto name=pose->name;
    restoreHandPosePreview();openPartGripEditor();_gripName->setText(name);
    beginGripSurfacePick();
}

int ReloadMakerController::magazineGrabAnchor(const HandPosePlacement& placement)const {
    if(isRifleConfiguration()||_authoringRoadmaps.value(roadmapKey()).toObject().contains("magazineReference")) {
        const int index=currentMagazineAnchor();
        return index>=0&&sameGripAnchor(placement,_handPoseAnchors[index].description)?index:-1;
    }
    if(placement.anchorName!="WeaponMagazine"&&placement.anchorName!="WeaponExtra3")return -1;
    for(int i=0;i<_handPoseAnchors.size();++i) {
        const auto& candidate=_handPoseAnchors[i].description;
        if(magazineGrabSupported(candidate)&&candidate.ownerRecord.compare(placement.ownerRecord,Qt::CaseInsensitive)==0&&
            normalizeNifPath(candidate.ownerModel)==normalizeNifPath(placement.ownerModel))return i;
    }
    return -1;
}

bool ReloadMakerController::useMagazineGrabGrip(bool allMagazines) {
    const int anchor=_partGripDraft?magazineGrabAnchor(_partGripDraft->pose.placement):-1;
    if(anchor<0) {
        setStatus("Place the hand on the selected magazine before saving.",true);return false;
    }
    updatePartGripControls();if(!_gripSave->isEnabled()){setStatus("Place the hand within reach and name the grip before assigning it.",true);return false;}
    PartGrip grip=*_partGripDraft;
    if(isGlockConfiguration()&&grip.pose.hand!=PoseHand::Left){setStatus("Glock magazines use the left hand.",true);return false;}
    _interactionHand=grip.pose.hand;
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
    const auto previousAssignments=_magazineGrabGrips,previousDefaults=_magazineGrabDefaults;
    const auto previousWindows=_reloadRecipe?_reloadRecipe->windows:QVector<ReloadWindow>{};
    const auto key=handAssignmentKey(magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord),grip.pose.hand);
    _magazineGrabGrips[key]=grip.pose.id;
    if(allMagazines)_magazineGrabDefaults[handAssignmentKey(grip.weaponRecord,grip.pose.hand)]=grip.pose.id;
    if(sameGripAnchor(_partGripDraft->pose.placement,grip.pose.placement))_partGripDraft=grip;
    else {_partGripDraft->pose.id=grip.pose.id;_partGripDraft->pose.name=grip.pose.name;}
    rememberMagazineGrip();
    if(_reloadRecipe)for(auto& window:_reloadRecipe->windows)for(auto& member:window.members)
        if(sameMechanismPart(_reloadRecipe->tracks[member.track].anchor,grip.pose.placement))member.gripId=grip.pose.id;
    refreshPartGripLibrary();
    resolveMagazineGrabGrip();
    if(!saveSession()) {
        _magazineGrabGrips=previousAssignments;_magazineGrabDefaults=previousDefaults;resolveMagazineGrabGrip();
        if(_reloadRecipe)_reloadRecipe->windows=previousWindows;
        setStatus("Fit snapshot saved, but the project assignment could not be saved. Retry Save for this magazine.",true);return false;
    }
    updatePartGripControls();
    setStatus(allMagazines?"Weapon default saved. Other custom magazine fits are preserved. Project saved.":
        "Magazine fit replaced and project saved. Grab Magazine now uses this fit.");
    return true;
}

void ReloadMakerController::resolveMagazineGrabGrip() {
    _magazineGrabGrip.reset();_magazineGrabBinding=-1;_magazineGrabError.clear();
    const QSignalBlocker block(_simulationMagazineGrip);
    _simulationMagazineGrip->clear();_simulationMagazineGrip->addItem("Magazine grip · native hand",QString());
    if(!_simulationRig||!_reloadRecipe)return;
    const auto anchor=simulationMagazineAnchor();
    if(anchor.ownerRecord.isEmpty()||magazineGrabAnchor(anchor)<0)return;
    const auto weapon=poseRecordKey(currentWeapon()->formId);
    const auto selected=magazineGripFor(weapon,anchor,_partGrips,_magazineGrabGrips,_magazineGrabDefaults,_magazineGrabError,_interactionHand);
    const auto id=selected?selected->pose.id:QString();
    // The normal selector is the active assignment. Historical snapshots are
    // available explicitly in Hands & grips, not mixed with the current fit.
    if(selected){_simulationMagazineGrip->addItem(selected->pose.name+" · "+(selected->pose.hand==PoseHand::Left?"Left":"Right"),id);_simulationMagazineGrip->setCurrentIndex(1);}
    if(!selected)return;
    _magazineGrabBridge={};
    for(int i=0;i<_nodeBindings.size();++i)if(_nodeBindings[i].nodeIndex==_reloadTrackNodes[_simulationRig->magazine]){_magazineGrabBinding=i;break;}
    if(_magazineGrabBinding<0){_magazineGrabError="The magazine grip has no live binding.";return;}
    if(isRifleConfiguration()||!sameGripAnchor(anchor,_reloadRecipe->tracks[_simulationRig->magazine].anchor)) {
        const int reference=currentMagazineAnchor();
        Transformf world,inverse;
        if(reference<0||!sceneNodeWorld(_handPoseAnchors[reference].node,world,_magazineGrabError)||
           !invertUniformTransform(_boundGlobalPose[_magazineGrabBinding],inverse)){_magazineGrabError="The magazine attachment frame is unavailable.";return;}
        _magazineGrabBridge=composed(inverse,world);
    }
    _magazineGrabGrip=*selected;
    for(auto& window:_reloadRecipe->windows)for(auto& member:window.members)
        if(member.track==_simulationRig->magazine&&sameGripAnchor(anchor,_reloadRecipe->tracks[member.track].anchor))member.gripId=id;
}

bool ReloadMakerController::applyMagazineGrabPose() {
    if(!_simulationActive||!_magazineGrabGrip||_magazineFitPreview)return true;
    if(!_simulation.magazine.carried()){_magazineGrabError.clear();return true;}
    const auto target=composed(composed(_boundGlobalPose[_magazineGrabBinding],_magazineGrabBridge),_magazineGrabGrip->pose.placement.handInAnchor);
    if(!solvePartGrip(_skeleton,_localPose,_globalPose,_magazineGrabGrip->pose,target,_localPose,_globalPose,_magazineGrabError,GripReach::FollowTarget))return true;
    // Arm composition preserves the weapon; restore its manually driven part
    // targets after evaluating the updated hand skeleton bindings.
    return evaluateSceneBindings(_skeleton,_nodeBindings,_sourceLocalPose,_stateBaselineLocalPose,
        _globalPose,_boundLocalPose,_boundGlobalPose,_poseError)&&applySimulationPose();
}
}
