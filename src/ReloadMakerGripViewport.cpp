#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "viewport/PartPicker.h"
#include "viewport/PinOverlay.h"
#include "StudioWindow.h"
#include "glview.h"
#include <QDockWidget>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

namespace reloadmaker {
    void ReloadMakerController::createGripViewport()
    {
        _gripViewport=new GripViewport(_window->getGLView(),this);
        _gripViewport->beginPick=[this](){if(_handEditorPage==HandEditorPage::GrabLocations)beginGrabLocationPick();else if(_handEditorPage==HandEditorPage::Mechanism)beginWorkshopPick(_workshopHandEditing);else beginGripSurfacePick();};
        _gripViewport->surfacePicked=[this](QPointF point){if(_handEditorPage==HandEditorPage::GrabLocations)pickGrabLocation(point);else if(_handEditorPage==HandEditorPage::Mechanism)pickWorkshopPart(point);else pickGripSurface(point);};
        _gripViewport->edited=[this](const Transformf& world){if(_handEditorPage==HandEditorPage::GrabLocations)editGrabLocationWorld(world);else editGripWorld(world);};
        _gripViewport->save=[this](){if(_handEditorPage==HandEditorPage::GrabLocations)saveGrabLocations();else if(_handEditorPage==HandEditorPage::Mechanism)saveWorkshop();else (void)savePartGrip();};
        _gripViewport->end=[this](){_gripViewport->setPicking(false);if(_handEditorPage==HandEditorPage::GrabLocations){_pickingGrabLocation=false;setWorkspace(OperateWorkspace);}else if(_handEditorPage==HandEditorPage::Mechanism)editWorkshopHand(false);else restoreHandPosePreview();};
        _gripViewport->teachingGrab=[this](int axis,bool rotation){beginTeachingDrag(axis,rotation);};
        _gripViewport->teachingTool=[this](bool rotation){_teachingRotation=rotation;_gripViewport->setRotationTool(rotation);updateMechanismControls();updateGripViewport();};
        _gripViewport->test=[this](){beginMechanismPreview();};
        _gripViewport->pickingCancelled=[this](){
            _pickingGrabLocation=false;
            _workshopPicking=false;
            if(_workshopPivotPicking&&_mechanismTeaching){_workshopPivotPicking=false;_mechanismState.path=float(_mechanismStage+1);(void)applyPose(_playbackTime);}
            updateMechanismControls();updateGripViewport();
        };
        _gripViewport->simulationHit=[this](QPointF p){return grabMechanismAt(p);};
        _gripViewport->simulationGrab=[this](){beginMechanismGrab();};
        _gripViewport->simulationDrag=[this](float value){dragMechanismBy(value);};
        _gripViewport->simulationRelease=[this](){releaseMechanismGrab();};
        _gripViewport->simulationCancel=[this](){cancelMechanismGrab();};
        _gripViewport->simulationReset=[this](){if(!_mechanismPreview)return;_mechanismTimer->stop();_mechanismState={};_mechanismHeld=false;_mechanismDirection=1;(void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();};
        _gripViewport->simulationStop=[this](){beginMechanismTeaching();};
        showHandEditorPage();
        connect(_handPosePages,&QTabWidget::currentChanged,this,[this](int page){setHandEditorPage(static_cast<HandEditorPage>(page));});
        connect(_handPoseDock,&QDockWidget::visibilityChanged,this,[this](){updateGripViewport();});
        updateGripViewport();
    }
    void ReloadMakerController::updateGripViewport()
    {
        updateScopeViewport();
        if(!_gripViewport) return;
        const bool workshop=_handEditorPage==HandEditorPage::Mechanism;
        const bool points=_handEditorPage==HandEditorPage::GrabLocations;_gripViewport->setPointAuthoring(points);
        const bool active=!_reloadWorkspaceActive && (_handEditorPage==HandEditorPage::PartGrip || workshop || points) && _handPoseDock->isVisible() && _handPoseDock->widget()->isEnabled() && !_loadThread &&
            !_rebuildTimer->isActive() && _clip.valid() && _renderedWeapon==_selectedWeapon;
        _gripViewport->configure(active,points?operatingAnchor()>=0:workshop?workshopFrameReady():canPickGripSurface(),points?operatingAnchor()>=0:workshop?canSaveMechanism():canSavePartGrip());
        _gripViewport->setWorkshop(workshop,_mechanismDraft?_mechanismDraft->name:QString(),_workshopHandEditing);
        if(_mechanismPreview&&_mechanismDraft&&!_workshopPicking){
            const int i=_mechanismTeaching?std::max(0,_mechanismStage):std::clamp(_mechanismState.activeStage,0,int(_mechanismDraft->stages.size())-1);
            const auto stage=i<_mechanismDraft->stages.size()?_mechanismDraft->stages[i]:MechanismStage{};
            const auto start=composed(composed(_globalPose[_weaponBoneIndex],_mechanismDraft->partInWeaponAtRest),mechanismDelta(*_mechanismDraft,float(i)));
            const auto current=composed(composed(_globalPose[_weaponBoneIndex],_mechanismDraft->partInWeaponAtRest),mechanismDelta(*_mechanismDraft,_mechanismState.path));
            const bool rotation=_mechanismTeaching?_teachingRotation:stage.rotation;
            const auto pivot=rotation?composed(start,Transformf{stage.pivot}).translation:
                composed(current,Transformf{_workshopContactPoint.value_or(Vec3f{})}).translation;
            const auto axis=rotated(start.rotation,{stage.axis==0?1.f:0.f,stage.axis==1?1.f:0.f,stage.axis==2?1.f:0.f});
            _gripViewport->setSimulation(true,{pivot.x,pivot.y,pivot.z},QVector3D(axis.x,axis.y,axis.z).normalized(),rotation,stage.axis);
            _gripViewport->setTeaching(_mechanismTeaching,start);
            if(_mechanismTeaching)_gripViewport->setRotationTool(_teachingRotation);
        }else {_gripViewport->setSimulation(false);_gripViewport->setTeaching(false);}
        if(_slotOverlay) _slotOverlay->setActive(loadoutPageActive() && _showPartLabels && !active);
        // Clicking the model selects parts on the loadout page, unless the grip tools own the viewport.
        // The loadout picker and Magazines' pieces share the part outlines:
        // the one leaving clears them before the other draws.
        if(_workspace!=MagazinesWorkspace) updateMagazineAuthoring();
        if(_partPicker) _partPicker->setActive(loadoutPageActive() && !active);
        if(_workspace==MagazinesWorkspace) updateMagazineAuthoring();
        if(_pinOverlay) _pinOverlay->setActive(loadoutPageActive() && !active);
        if(points){
            const auto g=currentGrabLocations();const int i=_grabLocationList?_selectedGrabLocation:-1,a=operatingAnchor();
            Transformf reference;QString error;
            if(i<0||i>=g.points.size()||a<0||!sceneNodeWorld(_handPoseAnchors[a].node,reference,error)){_gripViewport->clearGrip();return;}
            const auto target=composed(reference,Transformf{g.points[i].point});_gripViewport->setGrip(reference,target,{},target.translation);return;
        }
        if(!_partGripDraft || (workshop&&!_mechanismPreview&&!_workshopHandEditing)) { _gripViewport->clearGrip(); return; }
        const auto reference=gripReferenceWorld();
        const auto hand=composed(reference,_partGripDraft->pose.placement.handInAnchor);
        const auto& contact=_partGripDraft->surfaceContact;
        const auto palm=contact?contact->palmInHand:genericPalmPoint(_partGripDraft->pose);
        std::optional<Vec3f> surface;
        if(contact) surface=composed(reference,Transformf{contact->pointInAnchor}).translation;
        _gripViewport->setGrip(reference,hand,palm,surface);
    }
}
