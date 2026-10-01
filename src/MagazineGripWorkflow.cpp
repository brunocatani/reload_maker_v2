#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include "MagazineViewport.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <cmath>

namespace reloadmaker {
namespace {
bool samePlacement(const Transformf& a,const Transformf& b) {
    const auto near=[](float x,float y){return std::abs(x-y)<.00002f;};
    if(!near(a.translation.x,b.translation.x)||!near(a.translation.y,b.translation.y)||!near(a.translation.z,b.translation.z)||
       !near(a.scale.x,b.scale.x)||!near(a.scale.y,b.scale.y)||!near(a.scale.z,b.scale.z))return false;
    const auto& x=a.rotation;const auto& y=b.rotation;
    const float sign=x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w<0?-1.f:1.f;
    return near(x.x,sign*y.x)&&near(x.y,sign*y.y)&&near(x.z,sign*y.z)&&near(x.w,sign*y.w);
}
bool sameFit(const PartGrip& a,const PartGrip& b,const QString& name) {
    if(name!=b.pose.name||a.pose.hand!=b.pose.hand||!sameGripAnchor(a.pose.placement,b.pose.placement)||
       !samePlacement(a.pose.placement.handInAnchor,b.pose.placement.handInAnchor))return false;
    for(int i=0;i<15;++i)if(!samePlacement(a.pose.fingers[i],b.pose.fingers[i]))return false;
    return true;
}
}
int ReloadMakerController::currentMagazineAnchor() const {
    int result=-1;
    for(int i=0;i<_handPoseAnchors.size();++i)if(magazineGrabSupported(_handPoseAnchors[i].description)) {
        const auto& anchor=_handPoseAnchors[i].description;
        if(isRifleConfiguration()&&!isMk18Configuration()&&!isSrepEmbeddedMagazine()?(anchor.anchorKind!="installed-part"):(anchor.anchorName.compare("WeaponMagazine",Qt::CaseInsensitive)!=0))continue;
        if(result>=0)return -1;
        result=i;
    }
    return result;
}

void ReloadMakerController::rememberMagazineGrip() {
    if(!_partGripDraft||magazineGrabAnchor(_partGripDraft->pose.placement)<0)return;
    auto copy=*_partGripDraft;copy.pose.name=_gripName->text().trimmed();
    const auto key=handAssignmentKey(magazineGrabKey(copy.weaponRecord,copy.pose.placement.ownerRecord),copy.pose.hand);
    if(magazineEditorState()["unsaved"].toBool())_magazineGripDrafts[key]=copy;
    else _magazineGripDrafts.remove(key);
}

bool ReloadMakerController::editMagazineGrip(const QString& savedId) {
    const int anchor=currentMagazineAnchor();
    if(anchor<0||_loadThread||_reloadCapturing||_rebuildTimer->isActive()) {
        setStatus("Wait for the selected magazine to finish loading.",true);return false;
    }
    rememberMagazineGrip();
    const auto part=_handPoseAnchors[anchor].description;
    const auto weapon=poseRecordKey(currentWeapon()->formId),key=handAssignmentKey(magazineGrabKey(weapon,part.ownerRecord),_interactionHand);
    QString error;std::optional<PartGrip> fit;
    if(!savedId.isEmpty()) {
        for(const auto& grip:_partGrips)if(grip.pose.id==savedId)fit=grip;
        if(!fit||fit->weaponRecord.compare(weapon,Qt::CaseInsensitive)!=0||magazineGrabAnchor(fit->pose.placement)!=anchor) {
            setStatus("Choose a saved fit for this magazine. Use Saved pose to reuse fingers from another part.",true);return false;
        }
    } else if(_magazineGripDrafts.contains(key))fit=_magazineGripDrafts.value(key);
    else fit=magazineGripFor(weapon,part,_partGrips,_magazineGrabGrips,_magazineGrabDefaults,error,_interactionHand);
    if(!error.isEmpty()){setStatus(error,true);return false;}
    // Reuse the existing detached-magazine presentation, frozen for hand editing.
    // The magazine's pose remains the reference; gizmos never drive its geometry.
    if(_workspace==OperateWorkspace&&_simulationActive&&_magazineRail) {
        _simulationTimer->stop();_simulation.reset();
        if(!selectSimulationClip(false,false)){setStatus(_simulationError,true);return false;}
        _simulation.magazine.drop();_simulation.magazine.grab(*_magazineRail);
        _magazineFitPreview=true;
    }
    _editingMagazine=true;setWorkspace(GripsWorkspace);_handPosePages->setCurrentIndex(1);
    _playing=false;_playbackTimer->stop();_playButton->setText("PLAY");
    if(fit) {
        if(!loadGripRecord(*fit))return false;
    } else {
        restoreHandPosePreview();
        const QSignalBlocker block(_gripReferenceCombo);_gripReferenceCombo->setCurrentIndex(anchor);
        if(const auto* pose=selectedHandPose()) {
            const QSignalBlocker poseBlock(_gripPoseCombo);_gripPoseCombo->setCurrentIndex(_gripPoseCombo->findData(pose->id));
        }
        _gripName->setText("Magazine hold");
        (void)applyPose(_playbackTime);updatePartGripControls();
    }
    if(_magazineFitPreview){_magazineViewport->setActive(false);_simulationDrive->setActive(false,false);}
    setStatus(fit?"Edit this magazine’s hand with Move / Rotate. Save replaces its active fit.":
        "No fit assigned. Choose a named Saved pose, then Apply pose + hand rotation.");
    return true;
}

QJsonObject ReloadMakerController::magazineEditorState() const {
    const int anchor=currentMagazineAnchor();if(anchor<0||!currentWeapon())return {};
    const auto& part=_handPoseAnchors[anchor].description;
    const auto weapon=poseRecordKey(currentWeapon()->formId),key=handAssignmentKey(magazineGrabKey(weapon,part.ownerRecord),_interactionHand);
    QString error;const auto fit=magazineGripFor(weapon,part,_partGrips,_magazineGrabGrips,_magazineGrabDefaults,error,_interactionHand);
    QString name=part.ownerRecord;
    if(const auto* config=configuration())for(const auto& slot:config->entries())if(const auto* mod=config->mod(config->selected(slot.key)))
        if(poseRecordKey(mod->formId).compare(part.ownerRecord,Qt::CaseInsensitive)==0)name=mod->name;
    const bool editing=_partGripDraft&&magazineGrabAnchor(_partGripDraft->pose.placement)==anchor;
    const bool dirty=editing&&(!fit||!sameFit(*_partGripDraft,*fit,_gripName->text().trimmed()));
    const bool custom=_magazineGrabGrips.contains(key)||(fit&&_magazineGrabGrips.value(magazineGrabKey(weapon,part.ownerRecord))==fit->pose.id);
    return {{"magazine",name},{"ownerRecord",part.ownerRecord},{"anchor",anchor},
        {"hand",_interactionHand==PoseHand::Left?"left":"right"},{"assignment",custom?"custom":fit?"default":"none"},
        {"gripId",fit?fit->pose.id:QString()},{"name",fit?fit->pose.name:QString()},
        {"editing",editing},{"unsaved",dirty},{"canSave",editing&&_gripPreviewValid&&!_gripName->text().trimmed().isEmpty()},
        {"preview",_magazineFitPreview},{"error",error.isEmpty()&&editing&&!_gripPreviewValid?_gripError:error}};
}

void ReloadMakerController::updateMagazineGripUi() {
    if(!_magazineFitStatus)return;
    const auto state=magazineEditorState();
    _editMagazineGrip->setEnabled(!state.isEmpty()&&!_loadThread&&!_reloadCapturing);
    const auto weapon=currentWeapon()?poseRecordKey(currentWeapon()->formId).toLower():QString();
    _useDefaultMagazineGrip->setEnabled(!state.isEmpty()&&(_magazineGrabDefaults.contains(handAssignmentKey(weapon,_interactionHand))||_magazineGrabDefaults.contains(weapon)));
    if(state.isEmpty()){_magazineFitStatus->setText("Select a magazine in Loadout to configure its hand.");return;}
    const auto assignment=state["assignment"].toString();
    const auto source=assignment=="custom"?"Custom fit":assignment=="default"?"Weapon default":"No saved fit";
    QString text=state["magazine"].toString()+"\n"+source;
    if(!state["name"].toString().isEmpty())text+=" · "+state["name"].toString();
    if(state["unsaved"].toBool())text+="\nUnsaved adjustments — Save for this magazine";
    else if(state["editing"].toBool())text+="\nSaved · adjust with Move / Rotate";
    if(!state["error"].toString().isEmpty())text+="\n"+state["error"].toString();
    _magazineFitStatus->setText(text);
}

bool ReloadMakerController::useDefaultMagazineGrip() {
    const int anchor=currentMagazineAnchor();if(anchor<0||!currentWeapon())return false;
    const auto weapon=poseRecordKey(currentWeapon()->formId);
    QString error;if(!magazineGripFor(weapon,_handPoseAnchors[anchor].description,_partGrips,{},_magazineGrabDefaults,error,_interactionHand))return false;
    const auto key=handAssignmentKey(magazineGrabKey(weapon,_handPoseAnchors[anchor].description.ownerRecord),_interactionHand);
    const auto assignments=_magazineGrabGrips;const auto drafts=_magazineGripDrafts;
    const auto base=magazineGrabKey(weapon,_handPoseAnchors[anchor].description.ownerRecord);
    for(const auto& grip:_partGrips)if(grip.pose.id==_magazineGrabGrips.value(base)&&grip.pose.hand==_interactionHand)_magazineGrabGrips.remove(base);
    _magazineGrabGrips.remove(key);_magazineGripDrafts.remove(key);
    _partGripDraft.reset();
    if(!editMagazineGrip()||!saveSession()) {_magazineGrabGrips=assignments;_magazineGripDrafts=drafts;return false;}
    resolveMagazineGrabGrip();refreshPartGripList();updatePartGripControls();
    setStatus("This magazine now uses the weapon default. Project saved.");return true;
}
}
