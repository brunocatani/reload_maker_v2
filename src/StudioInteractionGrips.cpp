#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include "GripViewportMath.h"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
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
    if((isMk18Configuration()||isSrepConfiguration())&&anchor>=0)key+="|"+normalizeNifPath(_handPoseAnchors[anchor].description.ownerModel);
    return handAssignmentKey(key,hand);
}
void ReloadMakerController::resolveOperatingFingers() {
    _operatingFingers={};_operatingFingerError.clear();
    if(_operatingFingerPicker) {
        const QSignalBlocker block(_operatingFingerPicker);_operatingFingerPicker->clear();
        _operatingFingerPicker->addItem("Choose operating finger pose…");
        QSet<QString> ids;
        const auto add=[&](const HandPose& p){if(ids.contains(p.id))return;ids.insert(p.id);_operatingFingerPicker->addItem(p.name+(p.hand==PoseHand::Left?" · left":" · right"),p.id);};
        for(const auto& p:_savedHandPoses)add(p);
        for(const auto& g:_partGrips)if(_savedOperatingGrips.values().contains(g.pose.id))add(g.pose);
    }
    if(!currentWeapon()||operatingAnchor()<0)return;
    for(const auto hand:{PoseHand::Left,PoseHand::Right}) {
        auto id=_savedOperatingGrips.value(operatingGripKey(hand));
        if(id.isEmpty())id=_savedOperatingGrips.value(operatingGripKey(hand==PoseHand::Left?PoseHand::Right:PoseHand::Left));
        if(id.isEmpty())continue;
        const HandPose* source=nullptr;
        for(const auto& g:_partGrips)if(g.pose.id==id)source=&g.pose;
        for(const auto& p:_savedHandPoses)if(p.id==id)source=&p;
        if(!source){_operatingFingerError="Assigned operating finger pose is missing from the library.";return;}
        HandPose pose;
        if(!handPoseFingersFor(_skeleton,*source,hand,pose,_operatingFingerError))return;
        pose.source["operatingMirrored"]=source->hand!=hand;
        _operatingFingers[int(hand)]=std::move(pose);
    }
}
bool ReloadMakerController::useOperatingFingers(const QString& id) {
    if(!_simulationActive||operatingAnchor()<0||_simulation.action!=SimulationAction::Idle||_simulation.magazine.carried()) {
        setStatus("Release the operating part and magazine before selecting a finger pose.",true);return false;
    }
    const bool found=std::any_of(_savedHandPoses.cbegin(),_savedHandPoses.cend(),[&](const auto& p){return p.id==id;})||
        std::any_of(_partGrips.cbegin(),_partGrips.cend(),[&](const auto& g){return g.pose.id==id;});
    if(!found){setStatus("Select a saved finger pose from the library.",true);return false;}
    const auto key=operatingGripKey(_interactionHand),previous=_savedOperatingGrips.value(key);
    _savedOperatingGrips[key]=id;resolveOperatingFingers();
    if(!_operatingFingerError.isEmpty()||!saveSession()) {
        const auto error=_operatingFingerError.isEmpty()?_lastError:_operatingFingerError;
        if(previous.isEmpty())_savedOperatingGrips.remove(key);else _savedOperatingGrips[key]=previous;
        resolveOperatingFingers();setStatus(error,true);return false;
    }
    updateInteractionGripUi();++_revision;setStatus("Operating fingers saved. Wrist angle stays live; an unassigned opposite hand uses the mirrored finger shape.");return true;
}
QString ReloadMakerController::operatingGroup() const {return isAr15Configuration()?"charging_handle":isBoltActionConfiguration()||isThanatosConfiguration()||isSrepConfiguration()?"bolt":isM1AConfiguration()?"operating_handle":"slide";}
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
    resolveMagazineGrabGrip();resolveOperatingFingers();
    if(_workspace==GripsWorkspace&&_editingMagazine){_partGripDraft.reset();return editMagazineGrip();}
    if(_clip.valid())(void)applyPose(_playbackTime);
    updateInteractionGripUi();++_revision;return true;
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
    const auto& fingers=_operatingFingers[int(_interactionHand)];
    if(_operatingGripStatus)_operatingGripStatus->setText(!_operatingFingerError.isEmpty()?_operatingFingerError:fingers?
        "Fingers only · "+fingers->name+(fingers->source["operatingMirrored"].toBool()?" · mirrored for this hand":"")+"\nWrist position and angle stay live.":"No operating finger pose assigned. Select a saved pose below.");
    if(_operatingFingerPicker) {
        const QSignalBlocker block(_operatingFingerPicker);
        _operatingFingerPicker->setCurrentIndex(fingers?_operatingFingerPicker->findData(fingers->id):0);
        _operatingFingerPicker->setEnabled(free&&_simulationActive);
    }
    _operatingHover=false;if(_simulationDrive)_simulationDrive->setMarkers({});refreshGrabLocationsUi();
}
std::optional<QVector3D> ReloadMakerController::hoverOperatingPart(QPointF point) {
    _operatingHover=false;
    if(!_simulationActive||!_simulationRig||!_reloadRecipe||_simulation.action!=SimulationAction::Idle||_simulation.magazine.busy())return {};
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
    return currentWeapon()&&_operatingFingerError.isEmpty()&&(!_grabLocations.contains(grabLocationsKey(g))||
        std::any_of(g.points.cbegin(),g.points.cend(),[&](const auto& p){return p.hands&(_interactionHand==PoseHand::Left?1:2);}));
}

QJsonObject ReloadMakerController::interactionGripState() const {
    const auto& fingers=_operatingFingers[int(_interactionHand)];
    return {{"hand",_interactionHand==PoseHand::Left?"left":"right"},{"support",_interactionHand==PoseHand::Left?"right-firing-grip":"left-support-grip"},
        {"group",operatingGroup()},{"canGrab",operatingGrabAvailable()},{"posePolicy","authored-fingers-live-wrist"},{"authored",bool(fingers)},{"gripId",fingers?fingers->id:QString()},
        {"applied",bool(fingers)&&_simulation.action==SimulationAction::Held},{"error",_operatingFingerError},
        {"markerVisible",_operatingHover}};
}
QJsonObject ReloadMakerController::exportInteractionGrips() const {
    if(!_reloadRecipe||!currentWeapon()||!_operatingFingerError.isEmpty())return {};
    QJsonArray hands;
    for(const auto& p:_operatingFingers)if(p) {
        QString error;QJsonArray joints;
        for(const auto& f:p->fingers){QJsonObject joint;if(!encodePoseTransform(f,joint,error))return {};joints.append(joint);}
        hands.append(QJsonObject{{"hand",p->hand==PoseHand::Left?"left":"right"},{"sourcePoseId",p->id},{"name",p->name},
            {"fingerLocalTransformMask",0x7fff},{"fingerLocalTransforms",joints}});
    }
    QJsonObject result{{"version",3},{"firingHand","right"},{"operatingPosePolicy","authored-fingers-live-wrist"},
        {"otherHandPolicy","mirror-fingers"},{"operatingFingers",hands}};
    const auto locations=exportGrabLocations();if(!locations.isEmpty())result["grabLocations"]=locations;return result;
}
}
