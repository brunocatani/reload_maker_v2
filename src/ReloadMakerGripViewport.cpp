#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioWindow.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QSignalBlocker>

namespace reloadmaker {
    void ReloadMakerController::createGripViewport()
    {
        _gripViewport=new GripViewport(_window->getGLView(),this);
        _gripViewport->beginPick=[this](){if(_handPosePages->currentIndex()==2)beginWorkshopPick(_workshopHandEditing);else beginGripSurfacePick();};
        _gripViewport->surfacePicked=[this](QPointF point){if(_handPosePages->currentIndex()==2)pickWorkshopPart(point);else pickGripSurface(point);};
        _gripViewport->edited=[this](const Transformf& world){editGripWorld(world);};
        _gripViewport->save=[this](){if(_handPosePages->currentIndex()==2)saveWorkshop();else (void)savePartGrip();};
        _gripViewport->end=[this](){_gripViewport->setPicking(false);if(_handPosePages->currentIndex()==2)editWorkshopHand(false);else restoreHandPosePreview();};
        _gripViewport->teachingGrab=[this](int axis,bool rotation){beginTeachingDrag(axis,rotation);};
        _gripViewport->teachingTool=[this](bool rotation){_teachingRotation=rotation;_gripViewport->setRotationTool(rotation);updateMechanismControls();updateGripViewport();};
        _gripViewport->test=[this](){beginMechanismPreview();};
        _gripViewport->pickingCancelled=[this](){
            _workshopPicking=false;
            if(_workshopPivotPicking&&_mechanismTeaching){_workshopPivotPicking=false;_mechanismState.path=float(_mechanismStages->currentRow()+1);(void)applyPose(_playbackTime);}
            updateMechanismControls();updateGripViewport();
        };
        _gripViewport->simulationHit=[this](QPointF p){return grabMechanismAt(p);};
        _gripViewport->simulationGrab=[this](){beginMechanismGrab();};
        _gripViewport->simulationDrag=[this](float value){dragMechanismBy(value);};
        _gripViewport->simulationRelease=[this](){releaseMechanismGrab();};
        _gripViewport->simulationCancel=[this](){cancelMechanismGrab();};
        _gripViewport->simulationReset=[this](){if(!_mechanismPreview)return;_mechanismTimer->stop();_mechanismState={};_mechanismHeld=false;_mechanismDirection=1;(void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();};
        _gripViewport->simulationStop=[this](){beginMechanismTeaching();};
        connect(_handPosePages,&QTabWidget::currentChanged,this,[this](int page){
            if(page!=2){_workshopPicking=false;_workshopHandEditing=false;_gripViewport->setPicking(false);if(_mechanismPreview){stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);}}
            updatePartGripControls();
            // Finger, grip and mechanism previews depend on the visible tab.
            if(_clip.valid()&&!_reloadCapturing)(void)applyPose(_playbackTime);
            refreshFingerEditor();updateGripViewport();
        });
        connect(_handPoseDock,&QDockWidget::visibilityChanged,this,[this](){updateGripViewport();});
        updateGripViewport();
    }
    void ReloadMakerController::updateGripViewport()
    {
        if(!_gripViewport) return;
        const bool workshop=_handPosePages->currentIndex()==2;
        const bool active=!_reloadWorkspaceActive && (_handPosePages->currentIndex()==1 || workshop) && _handPoseDock->isVisible() && _handPoseDock->widget()->isEnabled() && !_loadThread &&
            !_rebuildTimer->isActive() && _clip.valid() && _renderedWeapon==_weaponCombo->currentIndex();
        _gripViewport->configure(active,workshop?_workshopPick->isEnabled():_gripPick->isEnabled(),workshop?_mechanismSave->isEnabled():_gripSave->isEnabled());
        _gripViewport->setWorkshop(workshop,_mechanismDraft?_mechanismDraft->name:QString(),_workshopHandEditing);
        if(_mechanismPreview&&_mechanismDraft&&!_workshopPicking){
            const int i=_mechanismTeaching?std::max(0,_mechanismStages->currentRow()):std::clamp(_mechanismState.activeStage,0,int(_mechanismDraft->stages.size())-1);
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
        if(_slotOverlay) _slotOverlay->setActive(_pages->currentIndex()==kLoadoutPage && _showPartLabels && !active);
        if(!_partGripDraft || (workshop&&!_mechanismPreview&&!_workshopHandEditing)) { _gripViewport->clearGrip(); return; }
        const auto reference=gripReferenceWorld();
        const auto hand=composed(reference,_partGripDraft->pose.placement.handInAnchor);
        const auto& contact=_partGripDraft->surfaceContact;
        const auto palm=contact?contact->palmInHand:genericPalmPoint(_partGripDraft->pose);
        std::optional<Vec3f> surface;
        if(contact) surface=composed(reference,Transformf{contact->pointInAnchor}).translation;
        _gripViewport->setGrip(reference,hand,palm,surface);
    }
    void ReloadMakerController::beginGripSurfacePick()
    {
        if(_mechanismPreview){stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);}
        updatePartGripControls(); if(!_gripPick->isEnabled()) return;
        if(!_partGripDraft) {
            Quaternionf rotation{};
            for(const auto& pose:_savedHandPoses) if(pose.id==_gripPoseCombo->currentData().toString()) rotation=pose.placement.handInAnchor.rotation;
            startPartGrip(false);
            if(!_partGripDraft) return;
            _partGripDraft->pose.placement.handInAnchor.rotation=rotation;
            showPartGripValues(); (void)applyPose(_playbackTime);
        }
        if(!_partGripDraft) return;
        _handPosePages->setCurrentIndex(1); updateGripViewport(); _gripViewport->setPicking(true);
        _gripStatus->setText(QStringLiteral("Click a weapon surface to place the palm and identify its part. Multi-bone surfaces need an explicit Reference part choice. Esc cancels picking."));
    }
    void ReloadMakerController::pickGripSurface(QPointF point)
    {
        if(!_partGripDraft || !_gripViewport->picking() || _loadThread || _rebuildTimer->isActive()) return;
        int automatic=-1;Vec3f world;QString surface,error;QVector<int> candidates;
        if(!pickWeaponPart(point,automatic,world,surface,candidates,error)){_gripStatus->setText(error);return;}
        QString assignmentError;
        if(automatic>=0 && !assignGripPart(automatic,assignmentError)) {setStatus(assignmentError,true);return;}
        const auto reference=_partGripFrame.weapon?_globalPose[_weaponBoneIndex]:composed(_boundGlobalPose.at(_partGripFrame.binding),_partGripFrame.bridge);
        Transformf inverse;
        if(!invertUniformTransform(reference,inverse)) {setStatus("Reference part cannot be inverted",true);return;}
        const Vec3f pointWorld=world;
        const auto palm=genericPalmPoint(_partGripDraft->pose);
        const auto hand=placePalmAt(composed(reference,_partGripDraft->pose.placement.handInAnchor),palm,pointWorld);
        _partGripDraft->pose.placement.handInAnchor=composed(inverse,hand);
        _partGripDraft->surfaceContact=PartGrip::SurfaceContact{composed(inverse,Transformf{pointWorld}).translation,palm,surface};
        _gripViewport->setPicking(false);
        const QString referenceName=_partGripDraft->pose.placement.anchorName;
        showPartGripValues(); if(!applyPose(_playbackTime)) return; updateHandPoseControls();
        setStatus(automatic>=0?QStringLiteral("Point attached to %1. Configure its shared mechanism to set stages and followers.").arg(referenceName):
            QStringLiteral("This surface has no single resolved moving-part frame. Point captured against %1; explicitly choose its Reference part before defining movement.").arg(referenceName));
    }
    bool ReloadMakerController::pickWeaponPart(QPointF point,int& automatic,Vec3f& world,QString& surface,QVector<int>& candidates,QString& error)
    {
        automatic=-1;candidates.clear();error.clear();
        auto* model=activeModel();auto* scene=_window->getGLView()->getScene();
        if(!model||!scene||!currentWeapon()){error="Load a weapon first.";return false;}
        QVector<int> allowed;
        for(auto* shape:scene->shapes){
            if(!shape||shape->isHidden())continue;
            auto node=model->getBlockIndex(shape->id());bool weapon=false;
            for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth){
                if(node==_weaponRoot)weapon=true;
                for(const auto& root:_partRoots)if(root==node)weapon=true;
                if(weapon)break;
                node=model->getBlockIndex(model->getParent(node));
            }
            if(weapon)allowed.append(shape->id());
        }
        int block=-1;Vector3 hit;
        if(!_window->getGLView()->reloadMakerSurfaceAt(point,allowed,block,hit)){
            error="No visible weapon surface at that point.";return false;
        }
        world={hit[0],hit[1],hit[2]};
        surface=model->get<QString>(model->getBlockIndex(block),"Name");
        const auto* shape=dynamic_cast<Shape*>(scene->getNode(model,model->getBlockIndex(block)));
        const auto skin=shape?shape->reloadMakerSkinBones():QVector<int>{};
        for(int bone:skin){
            for(int i=1;i<_handPoseAnchors.size();++i)
                if(model->getBlockNumber(_handPoseAnchors[i].node)==bone&&_handPoseAnchors[i].description.anchorKind=="animated-part")candidates.append(i);
        }
        if(skin.size()==1&&candidates.size()==1)automatic=candidates[0];
        if(candidates.isEmpty()){
            auto ancestor=model->getBlockIndex(block);
            for(int depth=0;ancestor.isValid()&&depth<model->getBlockCount()&&candidates.isEmpty();++depth){
                int selected=-1;
                for(int i=1;i<_handPoseAnchors.size();++i)if(_handPoseAnchors[i].node==ancestor){
                    selected=i;if(_handPoseAnchors[i].description.anchorKind=="animated-part")break;
                }
                if(selected>=0)candidates.append(selected);
                ancestor=model->getBlockIndex(model->getParent(ancestor));
            }
            if(skin.isEmpty()&&candidates.size()==1)automatic=candidates[0];
        }
        return true;
    }
    bool ReloadMakerController::assignGripPart(int index,QString& error)
    {
        if(!_partGripDraft || index<0 || index>=_handPoseAnchors.size()){error="No valid part selection";return false;}
        QString mechanismId;
        for(const auto& definition:_mechanisms) {
            if(definition.weaponRecord.compare(_partGripDraft->weaponRecord,Qt::CaseInsensitive)!=0 ||
                !sameMechanismPart(definition.anchor,_handPoseAnchors[index].description))continue;
            if(!mechanismId.isEmpty()){error="This part has duplicate shared definitions; resolve the duplicate files first";return false;}
            mechanismId=definition.id;
        }
        if(_mechanismPreview){stopMechanismPreview();if(!applyPose(_playbackTime))return false;}
        const auto old=gripReferenceWorld();const auto hand=composed(old,_partGripDraft->pose.placement.handInAnchor);
        std::optional<Vec3f> contact;if(_partGripDraft->surfaceContact)contact=composed(old,Transformf{_partGripDraft->surfaceContact->pointInAnchor}).translation;
        const auto previousFrame=_partGripFrame;
        if(!preparePartGripFrame(index,error))return false;
        const auto frame=gripReferenceWorld();Transformf inverse;if(!invertUniformTransform(frame,inverse)){_partGripFrame=previousFrame;error="Invalid part frame";return false;}
        _partGripDraft->pose.placement=_handPoseAnchors[index].description;
        _partGripDraft->pose.placement.handInAnchor=composed(inverse,hand);
        if(contact)_partGripDraft->surfaceContact->pointInAnchor=composed(inverse,Transformf{*contact}).translation;
        _partGripDraft->mechanismId=mechanismId;
        const QSignalBlocker blocker(_gripReferenceCombo);_gripReferenceCombo->setCurrentIndex(index);
        return true;
    }
    void ReloadMakerController::editGripWorld(const Transformf& world)
    {
        if(!_partGripDraft || _loadThread || _rebuildTimer->isActive() || !_clip.valid()) return;
        const auto reference=_partGripFrame.weapon?_globalPose[_weaponBoneIndex]:composed(_boundGlobalPose.at(_partGripFrame.binding),_partGripFrame.bridge);
        Transformf inverse; if(!invertUniformTransform(reference,inverse)) return;
        _partGripDraft->pose.placement.handInAnchor=composed(inverse,world);
        if(_handPosePages->currentIndex()==2)_workshopGripDirty=true;
        showPartGripValues(); (void)applyPose(_playbackTime); updateHandPoseControls();
    }
}
