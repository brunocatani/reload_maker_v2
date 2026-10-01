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
    resolveMagazineGrabGrip();
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
    if(_operatingGripStatus)_operatingGripStatus->setText("Dynamic operating grab · either permitted hand. Magazine poses stay authored.");
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
    return currentWeapon()&&(!_grabLocations.contains(grabLocationsKey(g))||
        std::any_of(g.points.cbegin(),g.points.cend(),[&](const auto& p){return p.hands&(_interactionHand==PoseHand::Left?1:2);}));
}

QJsonObject ReloadMakerController::interactionGripState() const {
    return {{"hand",_interactionHand==PoseHand::Left?"left":"right"},{"support",_interactionHand==PoseHand::Left?"right-firing-grip":"left-support-grip"},
        {"group",operatingGroup()},{"canGrab",operatingGrabAvailable()},{"posePolicy","dynamic"},{"authored",false},{"gripId",QString()},
        {"applied",false},{"error",QString()},
        {"markerVisible",_operatingHover}};
}
QJsonObject ReloadMakerController::exportInteractionGrips() const {
    if(!_reloadRecipe||!currentWeapon())return {};
    QJsonObject result{{"version",1},{"firingHand","right"},{"operatingPosePolicy","dynamic"},
        {"otherHandPolicy","dynamic"},{"operatingGroups",QJsonArray{}}};
    const auto locations=exportGrabLocations();if(!locations.isEmpty()){result["version"]=2;result["grabLocations"]=locations;}return result;
}
}
