#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include "GripViewportMath.h"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QDateTime>
#include <algorithm>
#include <limits>

namespace reloadmaker {
QString ReloadMakerController::operatingGripKey(PoseHand hand) const {
    QString key=poseRecordKey(currentWeapon()->formId)+"|"+operatingGroup();
    const int anchor=operatingAnchor();
    if(isMk18Configuration()&&anchor>=0)key+="|"+normalizeNifPath(_handPoseAnchors[anchor].description.ownerModel);
    return handAssignmentKey(key,hand);
}
QString ReloadMakerController::operatingGroup() const {return isAr15Configuration()?"charging_handle":isBoltActionConfiguration()||isThanatosConfiguration()?"bolt":isM1AConfiguration()?"operating_handle":"slide";}
int ReloadMakerController::operatingAnchor() const {
    if(!_simulationRig||!_reloadRecipe)return -1;
    const auto& target=_reloadRecipe->tracks[_simulationRig->slide].anchor;
    for(int i=0;i<_handPoseAnchors.size();++i)if(sameGripAnchor(target,_handPoseAnchors[i].description))return i;
    return -1;
}
bool ReloadMakerController::setInteractionHand(PoseHand hand) {
    if(isGlockConfiguration()&&hand!=PoseHand::Left){setStatus("The Glock uses a right firing hand and left reload hand.",true);return false;}
    if(_simulation.action!=SimulationAction::Idle||_simulation.magazine.carried()){setStatus("Release the part or finish magazine placement before switching hands.",true);return false;}
    rememberMagazineGrip();_interactionHand=hand;
    resolveMagazineGrabGrip();resolveOperatingGrip();
    if(_workspace==GripsWorkspace&&_editingMagazine){_partGripDraft.reset();return editMagazineGrip();}
    if(_workspace==GripsWorkspace&&_editingOperating){_partGripDraft.reset();return editOperatingGrip();}
    if(_clip.valid())(void)applyPose(_playbackTime);
    updateInteractionGripUi();++_revision;return true;
}
void ReloadMakerController::resolveOperatingGrip() {
    _operatingGrip.reset();_operatingGripBinding=-1;_operatingGripError.clear();
    const int anchor=operatingAnchor();if(anchor<0||!currentWeapon())return;
    const auto key=operatingGripKey(_interactionHand);
    const auto id=_operatingGrips.value(key);if(id.isEmpty())return;
    const PartGrip* found=nullptr;for(const auto& g:_partGrips)if(g.pose.id==id){if(found){_operatingGripError="Duplicate operating grip ID";return;}found=&g;}
    const auto& target=_handPoseAnchors[anchor].description;
    if(!found||found->weaponRecord!=poseRecordKey(currentWeapon()->formId)||found->pose.hand!=_interactionHand||
       found->pose.placement.anchorName!=target.anchorName||normalizeNifPath(found->pose.placement.ownerModel)!=normalizeNifPath(target.ownerModel)||found->pose.placement.anchorPath!=target.anchorPath) {
        _operatingGripError="Assigned operating grip does not match this hand or input part.";return;
    }
    for(int i=0;i<_nodeBindings.size();++i)if(_nodeBindings[i].nodeIndex==_reloadTrackNodes[_simulationRig->slide]){_operatingGripBinding=i;break;}
    if(_operatingGripBinding<0){_operatingGripError="Operating grip binding is missing.";return;}
    _operatingGrip=*found;
    const auto placement=found->pose.placement.handInAnchor;
    _operatingGrip->pose.placement=target;_operatingGrip->pose.placement.handInAnchor=placement;
    // Receiver OMODs may share a model. Export the current exact owner while
    // keeping the group assignment and authored model-space fit independent.
    if(found->pose.placement.ownerRecord!=target.ownerRecord){_operatingGrip->mechanismId.clear();}
}
bool ReloadMakerController::applyOperatingGripPose() {
    if(!_simulationActive||!_operatingGrip||_operatingGripBinding<0||_simulation.action!=SimulationAction::Held)return true;
    const auto target=composed(_boundGlobalPose[_operatingGripBinding],_operatingGrip->pose.placement.handInAnchor);
    if(!solvePartGrip(_skeleton,_localPose,_globalPose,_operatingGrip->pose,target,_localPose,_globalPose,_operatingGripError,GripReach::FollowTarget))return false;
    return evaluateSceneBindings(_skeleton,_nodeBindings,_sourceLocalPose,_stateBaselineLocalPose,_globalPose,_boundLocalPose,_boundGlobalPose,_poseError)&&applySimulationPose();
}
bool ReloadMakerController::editOperatingGrip() {
    const int anchor=operatingAnchor();if(anchor<0){setStatus("Prepare the weapon in Operate first.",true);return false;}
    resolveOperatingGrip();const auto fit=_operatingGrip;
    rememberMagazineGrip();_editingMagazine=false;_editingOperating=true;
    setWorkspace(GripsWorkspace);_handPosePages->setCurrentIndex(1);restoreHandPosePreview();
    if(fit){if(!loadGripRecord(*fit))return false;}
    else {const QSignalBlocker block(_gripReferenceCombo);_gripReferenceCombo->setCurrentIndex(anchor);_gripName->setText(operatingGroup()+" hold");updatePartGripControls();}
    _editingOperating=true;updatePartGripControls();setStatus("Operating-group grip: choose a saved hand pose, adjust Move / Rotate, then Save for operating group.");return true;
}
bool ReloadMakerController::useOperatingGrip() {
    const int anchor=operatingAnchor();
    if(anchor<0||!_partGripDraft||!sameGripAnchor(_partGripDraft->pose.placement,_handPoseAnchors[anchor].description)) {setStatus("The grip must target this group's input part.",true);return false;}
    if(isGlockConfiguration()){setStatus("The Glock retains its dynamic slide grab.",true);return false;}
    _interactionHand=_partGripDraft->pose.hand;
    const bool editing=_editingOperating;_editingOperating=false;
    const bool saved=savePartGrip();_editingOperating=editing;if(!saved)return false;
    const auto key=operatingGripKey(_interactionHand),previous=_operatingGrips.value(key);
    _operatingGrips[key]=_partGripDraft->pose.id;
    if(!saveSession()){if(previous.isEmpty())_operatingGrips.remove(key);else _operatingGrips[key]=previous;return false;}
    resolveOperatingGrip();updateInteractionGripUi();setStatus("Operating-group fit replaced for this hand. Project saved.");return true;
}
bool ReloadMakerController::mirrorMagazineGrip() {
    const int anchor=currentMagazineAnchor();if(anchor<0||isGlockConfiguration()){setStatus("Opposite-hand magazines are available on the rifle testbeds.",true);return false;}
    QString error;const auto weapon=poseRecordKey(currentWeapon()->formId);
    const auto source=magazineGripFor(weapon,_handPoseAnchors[anchor].description,_partGrips,_magazineGrabGrips,_magazineGrabDefaults,error,_interactionHand);
    if(!source){setStatus(error.isEmpty()?"Save a magazine fit for this hand first.":error,true);return false;}
    Transformf world,inverse;if(!sceneNodeWorld(_handPoseAnchors[anchor].node,world,error)||!invertUniformTransform(_globalPose[_weaponBoneIndex],inverse)){setStatus(error,true);return false;}
    auto fit=*source;HandPose mirrored;
    if(!mirrorHandPose(_skeleton,fit.pose,composed(inverse,world),mirrored,error)){setStatus(error,true);return false;}
    fit.pose=mirrored;fit.pose.name=source->pose.name+" · "+(mirrored.hand==PoseHand::Left?"left":"right");
    fit.surfaceContact.reset();fit.mechanismId.clear();_interactionHand=mirrored.hand;
    if(!editMagazineGrip())return false;
    if(!loadGripRecord(fit))return false;
    return useMagazineGrabGrip();
}
void ReloadMakerController::updateInteractionGripUi() {
    const bool free=_simulation.action==SimulationAction::Idle&&!_simulation.magazine.carried();
    for(auto* picker:{_interactionHandPicker,_magazineHandPicker})if(picker){const QSignalBlocker block(picker);picker->setCurrentIndex(int(_interactionHand));picker->setEnabled(!isGlockConfiguration()&&free&&!_loadThread&&!_reloadCapturing);}
    if(_editOperatingGrip){_editOperatingGrip->setVisible(!isGlockConfiguration());_editOperatingGrip->setEnabled(free&&_simulationActive);}
    if(_operatingGripStatus)_operatingGripStatus->setText(!_operatingGripError.isEmpty()?_operatingGripError:_operatingGrip?"Authored grip · "+_operatingGrip->pose.name:"Dynamic grab · this hand has no authored operating fit");
    _operatingHover=false;if(_simulationDrive)_simulationDrive->setMarkers({});refreshGrabLocationsUi();
}
std::optional<QVector3D> ReloadMakerController::hoverOperatingPart(QPointF point) {
    _operatingHover=false;
    if(!_simulationActive||!_simulationRig||!_reloadRecipe||_simulation.action!=SimulationAction::Idle||_simulation.magazine.busy()||!_operatingGripError.isEmpty())return {};
    if(!operatingGrabAvailable())return {};
    int automatic=-1;Vec3f hit;QString surface,error;QVector<int> candidates;
    if(!pickWeaponPart(point,automatic,hit,surface,candidates,error))return {};
    const auto locations=currentGrabLocations();
    if(_grabLocations.contains(grabLocationsKey(locations))) {
        const int a=operatingAnchor();if(a<0||!candidates.contains(a))return {};
        Transformf world;if(!sceneNodeWorld(_handPoseAnchors[a].node,world,error))return {};
        std::optional<QVector3D> best;float distance=std::numeric_limits<float>::infinity();
        for(const auto& p:locations.points)if(p.hands&(_interactionHand==PoseHand::Left?1:2)) {
            const auto v=composed(world,Transformf{p.point}).translation;const QVector3D position(v.x,v.y,v.z);
            const float d=(position-QVector3D(hit.x,hit.y,hit.z)).lengthSquared();if(d<=p.radius*p.radius&&d<distance){best=position;distance=d;}
        }
        _operatingHover=best.has_value();return best;
    }
    const auto& input=_reloadRecipe->tracks[_simulationRig->slide].anchor;
    for(int i:candidates)if(sameGripAnchor(_handPoseAnchors[i].description,input)){
        _operatingHover=true;return QVector3D(hit.x,hit.y,hit.z);
    }
    return {};
}
bool ReloadMakerController::operatingGrabAvailable() const {
    const auto g=currentGrabLocations();
    return currentWeapon()&&_operatingGripError.isEmpty()&&(!_grabLocations.contains(grabLocationsKey(g))||
        std::any_of(g.points.cbegin(),g.points.cend(),[&](const auto& p){return p.hands&(_interactionHand==PoseHand::Left?1:2);}));
}

QJsonObject ReloadMakerController::interactionGripState() const {
    return {{"hand",_interactionHand==PoseHand::Left?"left":"right"},{"support",_interactionHand==PoseHand::Left?"right-firing-grip":"left-support-grip"},
        {"group",operatingGroup()},{"canGrab",operatingGrabAvailable()},{"authored",bool(_operatingGrip)},{"gripId",_operatingGrip?_operatingGrip->pose.id:QString()},
        {"applied",bool(_operatingGrip)&&_simulation.action==SimulationAction::Held},{"error",_operatingGripError},
        {"markerVisible",_operatingHover}};
}
QJsonObject ReloadMakerController::exportInteractionGrips() const {
    if(!_reloadRecipe||!currentWeapon())return {};
    const auto weapon=poseRecordKey(currentWeapon()->formId);QJsonArray operating;
    for(const auto hand:{PoseHand::Left,PoseHand::Right}) {
        const auto id=_operatingGrips.value(operatingGripKey(hand));
        if(id.isEmpty())continue;
        if(!_simulationRig)return {};
        bool found=false;for(const auto& g:_partGrips)if(g.pose.id==id){if(found||g.pose.hand!=hand||g.weaponRecord!=weapon)return {};found=true;QString error;auto fit=g;const auto& anchor=_reloadRecipe->tracks[_simulationRig->slide].anchor;
            if(normalizeNifPath(g.pose.placement.ownerModel)!=normalizeNifPath(anchor.ownerModel)||g.pose.placement.anchorPath!=anchor.anchorPath||g.pose.placement.anchorName!=anchor.anchorName)return {};
            const auto placement=g.pose.placement.handInAnchor;fit.pose.placement=anchor;fit.pose.placement.handInAnchor=placement;fit.mechanismId.clear();
            const auto raw=encodePartGrip(fit,error);if(raw.isEmpty())return {};
            QJsonArray members;for(int t:_simulationRig->slideTracks)members.append(t);if(!_simulationRig->slideTracks.contains(_simulationRig->bolt))members.append(_simulationRig->bolt);
            operating.append(QJsonObject{{"group",operatingGroup()},{"hand",hand==PoseHand::Left?"left":"right"},{"inputTrack",_simulationRig->slide},{"drivenTracks",members},
                {"indicator",exportGrabLocations().isEmpty()?"contacted-part-surface":"authored-grab-locations"},{"grip",QJsonDocument::fromJson(raw).object()},
                {"inputPath",isBoltActionConfiguration()?"linear":"source-motion"}});
        }
        if(!found)return {};
    }
    QJsonObject result{{"version",1},{"firingHand","right"},{"otherHandPolicy","dynamic"},{"operatingGroups",operating}};
    const auto locations=exportGrabLocations();if(!locations.isEmpty()){result["version"]=2;result["grabLocations"]=locations;}return result;
}
}
