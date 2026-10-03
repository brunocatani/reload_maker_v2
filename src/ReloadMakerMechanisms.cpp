#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioWindow.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include <QScopedValueRollback>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QQuaternion>
#include <QSet>
#include <QVector3D>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
    int ReloadMakerController::resolveMechanismAnchor(const HandPosePlacement& anchor,QString& error)const{
        int found=-1;for(int i=1;i<_handPoseAnchors.size();++i)if(sameGripAnchor(anchor,_handPoseAnchors[i].description)){if(found>=0){error="Part frame is ambiguous";return -1;}found=i;}if(found<0)error="Part frame is not installed";return found;
    }
    void ReloadMakerController::refreshMechanisms(){QStringList errors;_mechanisms=_mechanismLibrary.load(errors);for(const auto& e:errors)appendDiagnostic("MECHANISM FILE "+e);refreshMechanismChoices();}
    void ReloadMakerController::refreshMechanismChoices(){
        if(!_mechanismList){return;}
        const auto previous=_mechanismList->currentData();const QSignalBlocker blocker(_mechanismList);_mechanismList->clear();const auto* weapon=currentWeapon();const QString record=weapon?poseRecordKey(weapon->formId):QString();
        for(const auto& p:_mechanisms)if(p.weaponRecord.compare(record,Qt::CaseInsensitive)==0)_mechanismList->addItem(p.name+" · "+p.anchor.anchorName,p.id);
        const int old=_mechanismList->findData(previous);if(old>=0)_mechanismList->setCurrentIndex(old);
        if(_mechanismDraft&&_mechanismDraft->weaponRecord.compare(record,Qt::CaseInsensitive)!=0){stopMechanismPreview();_mechanismDraft.reset();displayMechanism();}
        else if(!_mechanismPreview)displayMechanism();
        refreshWorkshopChoices();
        updateMechanismControls();
    }
    void ReloadMakerController::openMechanismForPart(){selectWorkshopPart(std::max(_gripAnchor,0));}
    void ReloadMakerController::loadMechanism(){
        for(const auto& p:_mechanisms)if(p.id==_mechanismList->currentData().toString()){
            QString error;const int index=resolveMechanismAnchor(p.anchor,error);
            if(index<0){setStatus(error,true);return;}
            stopMechanismPreview();_mechanismDraft.reset();selectWorkshopPart(index);return;
        }
    }
    void ReloadMakerController::displayMechanism(){
        if(_mechanismUpdating){return;}
        _mechanismUpdating=true;const QSignalBlocker a(_mechanismStages),b(_mechanismFollowers);int selected=_mechanismStages->currentRow();_mechanismStages->clear();_mechanismFollowers->clear();
        _mechanismName->setText(_mechanismDraft?_mechanismDraft->name:QString());
        if(_mechanismDraft){
            for(int i=0;i<_mechanismDraft->stages.size();++i){const auto& s=_mechanismDraft->stages[i];_mechanismStages->addItem(QStringLiteral("%1. %2 ↔ %3 · %4 %5 [%6 → %7]").arg(i+1).arg(s.forwardName,s.reverseName,s.rotation?"Rotate":"Slide",QStringList{"X","Y","Z"}[s.axis]).arg(s.from).arg(s.to));}
            if(!_mechanismDraft->stages.isEmpty())_mechanismStages->setCurrentRow(std::clamp(selected,0,int(_mechanismDraft->stages.size())-1));
            QString error;const int master=resolveMechanismAnchor(_mechanismDraft->anchor,error);const int root=master>=0?_sceneNodes.indexOf(_handPoseAnchors[master].node):-1;
            for(int i=1;i<_handPoseAnchors.size();++i){if(i==master)continue;const auto& anchor=_handPoseAnchors[i];auto* item=new QListWidgetItem(_handPoseAnchors[i].label,_mechanismFollowers);item->setData(Qt::UserRole,i);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(Qt::Unchecked);
                MechanismScenePlan plan;QString reason;if(root<0||!compileMechanismScene(_assemblyNodes,_nodeBindings,{root,int(_sceneNodes.indexOf(anchor.node))},plan,reason)){item->setFlags(item->flags()&~Qt::ItemIsEnabled);item->setText(item->text()+" · inherited / overlapping");}
                item->setData(Qt::UserRole+1,item->text());
                for(const auto& f:_mechanismDraft->followers)if(sameGripAnchor(f.anchor,anchor.description))item->setCheckState(Qt::Checked);
            }
            selected=_mechanismStages->currentRow();if(selected>=0){const auto& s=_mechanismDraft->stages[selected];_stageForward->setText(s.forwardName);_stageReverse->setText(s.reverseName);_stageMotion->setCurrentIndex((s.rotation?3:0)+s.axis);_stageRelease->setCurrentIndex(int(s.release));_stageFrom->setValue(s.from);_stageTo->setValue(s.to);_stageRate->setValue(s.returnRate);_stagePivot[0]->setValue(s.pivot.x);_stagePivot[1]->setValue(s.pivot.y);_stagePivot[2]->setValue(s.pivot.z);}
        }
        _mechanismUpdating=false;updateMechanismControls();
    }
    void ReloadMakerController::readMechanismControls(){
        if(_mechanismUpdating||!_mechanismDraft||(_mechanismPreview&&!_mechanismTeaching)){return;}
        auto& p=*_mechanismDraft;p.name=_mechanismName->text().trimmed();const int i=_mechanismStages->currentRow();if(i>=0&&i<p.stages.size()){auto& s=p.stages[i];s.forwardName=_stageForward->text();s.reverseName=_stageReverse->text();s.axis=_stageMotion->currentIndex()%3;s.rotation=_stageMotion->currentIndex()>=3;s.from=float(_stageFrom->value());s.to=float(_stageTo->value());s.returnRate=float(_stageRate->value());s.release=ReleaseRule(_stageRelease->currentIndex());s.pivot={float(_stagePivot[0]->value()),float(_stagePivot[1]->value()),float(_stagePivot[2]->value())};}
        QString error;const int master=resolveMechanismAnchor(p.anchor,error);Transformf world,inverse;if(master<0||!sceneNodeWorld(_handPoseAnchors[master].node,world,error)||!invertUniformTransform(world,inverse)){setStatus(error,true);return;}
        QVector<MechanismFollower> followers;for(int row=0;row<_mechanismFollowers->count();++row){auto* item=_mechanismFollowers->item(row);if(item->checkState()!=Qt::Checked)continue;const auto& anchor=_handPoseAnchors[item->data(Qt::UserRole).toInt()];const auto old=std::find_if(p.followers.cbegin(),p.followers.cend(),[&](const auto& f){return sameGripAnchor(f.anchor,anchor.description);});if(old!=p.followers.cend())followers.append(*old);else {Transformf follower;if(!sceneNodeWorld(anchor.node,follower,error)){setStatus(error,true);return;}followers.append({anchor.description,composed(inverse,follower)});}}
        p.followers=followers;
        if(i>=0&&i<p.stages.size()){
            const auto& s=p.stages[i];
            _mechanismStages->item(i)->setText(QStringLiteral("%1. %2 ↔ %3 · %4 %5 [%6 → %7]").arg(i+1).arg(s.forwardName,s.reverseName,s.rotation?"Rotate":"Slide",QStringList{"X","Y","Z"}[s.axis]).arg(s.from).arg(s.to));
        }
        updateMechanismControls();
        if(_mechanismTeaching){if(i>=0)_teachingRotation=p.stages[i].rotation;(void)applyPose(_playbackTime);updateGripViewport();}
    }
    void ReloadMakerController::saveMechanism(){
        readMechanismControls();if(!_mechanismDraft||!_clip.valid()||_loadThread||_mechanismPreview)return;QString error;
        QVector<int> roots;
        const int master=resolveMechanismAnchor(_mechanismDraft->anchor,error);
        if(master<0){setStatus(error,true);return;}
        roots.append(_sceneNodes.indexOf(_handPoseAnchors[master].node));
        for(const auto& f:_mechanismDraft->followers){const int i=resolveMechanismAnchor(f.anchor,error);if(i<0){setStatus(error,true);return;}roots.append(_sceneNodes.indexOf(_handPoseAnchors[i].node));}
        MechanismScenePlan plan;
        if(!compileMechanismScene(_assemblyNodes,_nodeBindings,roots,plan,error)){setStatus(error,true);return;}
        if(!_mechanismLibrary.save(*_mechanismDraft,error)){setStatus(error,true);return;}
        if(_partGripDraft&&_partGripDraft->weaponRecord.compare(_mechanismDraft->weaponRecord,Qt::CaseInsensitive)==0&&sameMechanismPart(_partGripDraft->pose.placement,_mechanismDraft->anchor))_partGripDraft->mechanismId=_mechanismDraft->id;
        refreshMechanisms();_mechanismList->setCurrentIndex(_mechanismList->findData(_mechanismDraft->id));
        setStatus(QStringLiteral("Saved shared part definition, revision %1. Save the hand grip to persist its definition link.").arg(_mechanismDraft->revision));updatePartGripControls();
    }
    void ReloadMakerController::updateMechanismControls()
    {
        if(!_mechanismStatus||!_workshopHint)return;
        const bool frame=_clip.valid()&&!_loadThread&&!_rebuildTimer->isActive();
        const bool part=frame&&_mechanismDraft.has_value();
        const bool testing=_mechanismPreview&&!_mechanismTeaching;
        const bool step=part&&_mechanismStages->currentRow()>=0;
        QString error;
        const bool valid=_mechanismDraft&&(testing||validateMechanism(*_mechanismDraft,error));
        const bool hand=part&&_partGripDraft&&sameMechanismPart(_partGripDraft->pose.placement,_mechanismDraft->anchor);
        _workshopPick->setEnabled(frame);_workshopPart->setEnabled(frame);_workshopPose->setEnabled(part);
        _workshopContact->setEnabled(hand&&!testing);_workshopHand->setEnabled(hand&&!testing);
        _workshopTeach->setEnabled(part);_mechanismStart->setEnabled(part&&valid&&!_mechanismDraft->stages.isEmpty());
        _mechanismSave->setEnabled(part&&valid&&!testing);
        _mechanismSave->setText(hand?"SAVE PART + HAND":"SAVE PART");
        _workshopAddStage->setEnabled(part&&valid&&!testing);
        _mechanismName->setEnabled(part&&!testing);
        _mechanismStages->setEnabled(part&&!testing);_mechanismFollowers->setEnabled(part&&!testing);
        _workshopStageFields->setVisible(!testing);_workshopTestControls->setVisible(testing);
        _workshopMove->setEnabled(part&&!testing&&!_workshopHandEditing);
        _workshopRotate->setEnabled(part&&!testing&&!_workshopHandEditing);
        _workshopPivot->setVisible(_mechanismTeaching&&_teachingRotation);
        _workshopPivot->setEnabled(part&&!testing);
        {
            const QSignalBlocker a(_workshopTeach),b(_mechanismStart),c(_workshopMove),d(_workshopRotate),e(_workshopHand);
            _workshopTeach->setChecked(_mechanismTeaching);_mechanismStart->setChecked(testing);
            _workshopMove->setChecked(_mechanismTeaching&&!_teachingRotation);
            _workshopRotate->setChecked(_mechanismTeaching&&_teachingRotation);
            _workshopHand->setChecked(_workshopHandEditing);
        }
        for(QWidget* widget:std::initializer_list<QWidget*>{_stageForward,_stageReverse,_stageMotion,_stageRelease,_stageFrom,_stageTo,_stageRate,_stagePivot[0],_stagePivot[1],_stagePivot[2]})widget->setEnabled(step&&!testing);
        _mechanismPoseOnGrab->setEnabled(hand);_mechanismSlider->setEnabled(testing);
        QString hint;
        if(!part)hint="Pick the weapon part in 3D. Then choose a hand pose here, or leave No hand selected.";
        else if(_workshopHandEditing)hint="EDITING HAND · arrows move the hand; rings rotate it. The weapon part stays still. Choose EDIT MOVEMENT to drive the part instead.";
        else if(testing){
            const int i=std::clamp(_mechanismState.activeStage,0,int(_mechanismDraft->stages.size())-1);
            const auto& stage=_mechanismDraft->stages[i];
            const float fraction=std::clamp(_mechanismState.path-i,0.f,1.f);
            const QSignalBlocker blocker(_mechanismSlider);_mechanismSlider->setValue(qRound(fraction*10000));
            hint=QStringLiteral("TESTING · step %1 / %2 · %3\n%4 · grab the part or drag its handle. Release tests its rule. EDIT MOVEMENT returns to endpoint editing.")
                .arg(i+1).arg(_mechanismDraft->stages.size()).arg(stage.forwardName)
                .arg(_mechanismState.returning?"Returning":_mechanismHeld?"Grabbed":"Released");
        }else if(_mechanismTeaching){
            hint=step?QStringLiteral("EDITING PART · step %1\nDrag an arrow or ring to set the endpoint. One axis per step; + NEXT STEP starts from here. TEST GRAB checks limits and release.")
                .arg(_mechanismStages->currentRow()+1)
                :QStringLiteral("EDITING PART · drag any arrow to create the first movement. ROTATE shows the rotation rings. No numeric setup is required.");
        }else hint="Choose EDIT MOVEMENT for part arrows, or ADJUST HAND for hand arrows.";
        if(hand&&(_mechanismTeaching||_workshopHandEditing||_mechanismHeld)&&!_gripPreviewValid)hint+="\nHand cannot reach this target: "+_gripError;
        if(!_workshopPicking)_workshopHint->setText(hint);
        _mechanismStatus->setText(!part?"No part selected.":valid?"Definition can be saved. Movement rules are shared by this part's hand grips.":error);
    }
    Transformf ReloadMakerController::gripReferenceWorld()const{
        if(_partGripFrame.weapon)return _globalPose[_weaponBoneIndex];
        if(_mechanismPreview&&_partGripFrame.node>=0&&_partGripFrame.node<_mechanismWorld.size())return _mechanismWorld[_partGripFrame.node];
        return composed(_boundGlobalPose.at(_partGripFrame.binding),_partGripFrame.bridge);
    }
    void ReloadMakerController::beginMechanismPreview(){startMechanismPreview(false);}
    void ReloadMakerController::beginMechanismTeaching(){startMechanismPreview(true);}
    void ReloadMakerController::startMechanismPreview(bool teaching){
        if(!_mechanismDraft||!_clip.valid()||_loadThread)return;
        QString error;if(!teaching&&(!validateMechanism(*_mechanismDraft,error)||_mechanismDraft->stages.isEmpty())){setStatus("Set a movement endpoint before testing. "+error,true);return;}
        _workshopHandEditing=false;_workshopPicking=false;_gripViewport->setPicking(false);
        stopPlayback();stopMechanismPreview();if(!applyPose(_playbackTime))return;
        QVector<int> roots;for(const auto& anchor:QVector<HandPosePlacement>{_mechanismDraft->anchor}){int i=resolveMechanismAnchor(anchor,error);if(i<0){setStatus(error,true);return;}roots.append(_sceneNodes.indexOf(_handPoseAnchors[i].node));}
        for(const auto& f:_mechanismDraft->followers){int i=resolveMechanismAnchor(f.anchor,error);if(i<0){setStatus(error,true);return;}roots.append(_sceneNodes.indexOf(_handPoseAnchors[i].node));}
        if(!compileMechanismScene(_assemblyNodes,_nodeBindings,roots,_mechanismScene,error)){setStatus(error,true);return;}
        // A selected node may not control skinned geometry bound to external bones.
        // Refuse that preview rather than moving an invisible parent and claiming it works.
        auto* scene=_window->getGLView()->getScene();auto* model=activeModel();for(auto* shape:scene->shapes){if(!shape)continue;int owner=-1;auto parent=model->getBlockIndex(shape->id());for(int depth=0;parent.isValid()&&depth<model->getBlockCount();++depth){int n=_sceneNodes.indexOf(parent);if(n>=0){owner=_mechanismScene.group[n];break;}parent=model->getBlockIndex(model->getParent(parent));}if(owner<0)continue;for(int bone:shape->reloadMakerSkinBones()){int n=_sceneNodes.indexOf(model->getBlockIndex(bone));if(n<0||_mechanismScene.group[n]!=owner){setStatus("This group contains skin driven by an external bone. Choose its actual animated part frame before simulating.",true);return;}}}
        _mechanismState={};_mechanismDirection=1;_mechanismHeld=teaching;_mechanismPreview=true;_mechanismTeaching=teaching;
        if(teaching){
            const int selected=_mechanismStages->currentRow();
            _mechanismState.path=float(selected>=0?selected+1:_mechanismDraft->stages.size());
            if(selected>=0){_teachingRotation=_mechanismDraft->stages[selected].rotation;_teachingAxis=_mechanismDraft->stages[selected].axis;}
        }
        if(!applyPose(_playbackTime)){stopMechanismPreview();return;}updatePartGripControls();
    }
    bool ReloadMakerController::applyMechanismPreview(){
        if(!_mechanismDraft)return false;
        if(!nativeMechanismScene(_mechanismScene,_nodeBindings,_boundLocalPose,_mechanismLocal,_mechanismWorld,_poseError))return false;
        const auto target=composed(composed(_globalPose[_weaponBoneIndex],_mechanismDraft->partInWeaponAtRest),mechanismDelta(*_mechanismDraft,_mechanismState.path));QVector<Transformf> targets{target};for(const auto& follower:_mechanismDraft->followers)targets.append(composed(target,follower.inPartAtRest));
        if(!moveMechanismScene(_mechanismScene,targets,_mechanismLocal,_mechanismWorld,_poseError))return false;
        for(int i=0;i<_nodeBindings.size();++i){_boundLocalPose[i]=_mechanismLocal[_nodeBindings[i].nodeIndex];_boundGlobalPose[i]=_mechanismWorld[_nodeBindings[i].nodeIndex];}return true;
    }
    void ReloadMakerController::stopMechanismPreview(){if(_mechanismTimer)_mechanismTimer->stop();if(_mechanismPreview)_mechanismRestorePending=true;_mechanismPreview=false;_mechanismTeaching=false;_mechanismHeld=false;_mechanismState.returning=false;}
    void ReloadMakerController::releaseMechanismGrab(){if(!_mechanismPreview||!_mechanismDraft||_mechanismTeaching)return;_mechanismHeld=false;releaseMechanism(*_mechanismDraft,_mechanismState);if(_mechanismState.returning){_mechanismClock.restart();_mechanismTimer->start();}(void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();}
    void ReloadMakerController::advanceMechanismPreview(){const QScopedValueRollback<bool> viewOnly(_viewRefresh,true);if(!_mechanismPreview||!_mechanismDraft){_mechanismTimer->stop();return;}const float seconds=float(_mechanismClock.nsecsElapsed())/1e9f;_mechanismClock.restart();advanceMechanism(*_mechanismDraft,_mechanismState,seconds);if(!_mechanismState.returning)_mechanismTimer->stop();if(!applyPose(_playbackTime)){stopMechanismPreview();return;}updateMechanismControls();updateGripViewport();}
    void ReloadMakerController::cancelMechanismGrab(){
        if(!_mechanismPreview)return;
        if(_mechanismTeaching){const int i=_mechanismStages->currentRow();if(_mechanismDraft&&i>=0){_mechanismDraft->stages[i]=_teachingDragStart;displayMechanism();}}
        else{_mechanismState.path=_mechanismGrabPath;_mechanismHeld=false;}
        (void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();
    }
    void ReloadMakerController::beginTeachingDrag(int axis,bool rotation){
        if(!_mechanismPreview||!_mechanismTeaching||!_mechanismDraft)return;
        int i=_mechanismStages->currentRow();
        if(i<0){MechanismStage stage;stage.forwardName=QStringLiteral("Step %1").arg(_mechanismDraft->stages.size()+1);stage.to=0;_mechanismDraft->stages.append(stage);displayMechanism();i=_mechanismDraft->stages.size()-1;_mechanismStages->setCurrentRow(i);}
        _teachingDragStart=_mechanismDraft->stages[i];_teachingAxis=axis;_teachingRotation=rotation;
        _mechanismDraft->stages[i]=mechanismStageFromDrag(_teachingDragStart,axis,rotation,0);
        _mechanismState.path=float(i+1);_mechanismHeld=true;
        const auto base=composed(_globalPose[_weaponBoneIndex],_mechanismDraft->partInWeaponAtRest);_mechanismGrabScale=rotation?1.f:base.scale.x;
        displayMechanism();(void)applyPose(_playbackTime);updateGripViewport();
    }
    void ReloadMakerController::dragMechanismBy(float delta){
        if(!_mechanismPreview||!_mechanismDraft||!_mechanismHeld)return;
        if(_mechanismTeaching){
            const int i=_mechanismStages->currentRow();if(i<0)return;
            auto& stage=_mechanismDraft->stages[i];stage=mechanismStageFromDrag(_teachingDragStart,_teachingAxis,_teachingRotation,delta/_mechanismGrabScale);
            {const QSignalBlocker a(_stageMotion),b(_stageTo);_stageMotion->setCurrentIndex((stage.rotation?3:0)+stage.axis);_stageTo->setValue(stage.to);}
            _mechanismStages->item(i)->setText(QStringLiteral("%1   %2 · %3 %4 → %5").arg(i+1).arg(stage.forwardName,stage.rotation?"Rotate":"Move",QStringList{"X","Y","Z"}[stage.axis]).arg(stage.to,0,'f',2));
        }else{
            const int i=_mechanismState.activeStage;const auto& s=_mechanismDraft->stages[i];const float path=std::clamp(_mechanismGrabPath+delta/(s.to-s.from)/_mechanismGrabScale,float(i),float(i+1));_mechanismDirection=path>=_mechanismState.path?1:-1;_mechanismState.path=path;
        }
        (void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();
    }
    bool ReloadMakerController::grabMechanismAt(QPointF point){
        if(!_mechanismPreview||!_mechanismDraft||_mechanismTeaching){return false;}
        auto* view=_window->getGLView();auto* model=activeModel();QVector<int> allowed;QSet<int> members;
        for(auto* shape:view->getScene()->shapes){
            if(!shape||shape->isHidden())continue;
            auto node=model->getBlockIndex(shape->id());bool weapon=false,member=false;
            for(int d=0;node.isValid()&&d<model->getBlockCount();++d){
                const int i=_sceneNodes.indexOf(node);
                if(i>=0&&_mechanismScene.group[i]>=0)member=true;
                if(node==_weaponRoot)weapon=true;
                for(const auto& root:_partRoots)if(root==node)weapon=true;
                node=model->getBlockIndex(model->getParent(node));
            }
            // Whole-shape picking cannot identify a triangle's skin ownership.
            // Only consume the click if every used skin bone belongs to this mechanism.
            const auto bones=shape->reloadMakerSkinBones();
            if(!bones.isEmpty()){
                member=true;
                for(int bone:bones){const int i=_sceneNodes.indexOf(model->getBlockIndex(bone));if(i<0||_mechanismScene.group[i]<0){member=false;break;}}
            }
            if(weapon){allowed.append(shape->id());}
            if(member){members.insert(shape->id());}
        }
        int block=-1;Vector3 world;if(!view->reloadMakerSurfaceAt(point,allowed,block,world)||!members.contains(block))return false;
        beginMechanismGrab();return true;
    }
    void ReloadMakerController::beginMechanismGrab(){
        if(!_mechanismPreview||!_mechanismDraft)return;
        _mechanismTimer->stop();_mechanismState.returning=false;_mechanismHeld=true;_mechanismGrabPath=_mechanismState.path;
        const float rounded=std::round(_mechanismState.path);if(std::abs(rounded-_mechanismState.path)<.0001f)_mechanismState.activeStage=std::clamp(int(rounded)-(_mechanismDirection<0?1:0),0,int(_mechanismDraft->stages.size())-1);
        const auto& stage=_mechanismDraft->stages[_mechanismState.activeStage];const auto base=composed(_globalPose[_weaponBoneIndex],_mechanismDraft->partInWeaponAtRest);_mechanismGrabScale=stage.rotation?1.f:base.scale.x;
        (void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();
    }
    void ReloadMakerController::analyzeMechanismAnimation(){
        if(!_mechanismDraft||!_clip.valid()||_mechanismPreview||_loadThread)return;
        QString error;
        const int index=resolveMechanismAnchor(_mechanismDraft->anchor,error);
        if(index<0){setStatus(error,true);return;}
        const int master=_sceneNodes.indexOf(_handPoseAnchors[index].node);
        MechanismScenePlan plan;
        if(master<0||!compileMechanismScene(_assemblyNodes,_nodeBindings,{},plan,error)){setStatus(error,true);return;}
        const auto distance=[](Vec3f a,Vec3f b){return QVector3D(a.x-b.x,a.y-b.y,a.z-b.z).length();};
        const auto angle=[](Quaternionf a,Quaternionf b){return 2*std::acos(std::clamp(std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w),0.f,1.f))*180.f/3.14159265f;};
        QVector<Transformf> firstRelative(_handPoseAnchors.size());
        QVector<float> distances(_handPoseAnchors.size(),0),angles(_handPoseAnchors.size(),0);
        QVector<bool> visible(_handPoseAnchors.size(),true);
        float movement=0,rotation=0;Transformf firstMaster;
        QVector<Transformf> local,global,boundLocal,boundGlobal,nativeLocal,world;
        for(int sample=0;sample<=60;++sample){
            if(!sampleActionPose(_clip.durationSeconds*sample/60.f,local,global)) {setStatus(_poseError,true);return;}
            if(!evaluateSceneBindings(_skeleton,_nodeBindings,local,_stateBaselineLocalPose,global,boundLocal,boundGlobal,error)||
                !nativeMechanismScene(plan,_nodeBindings,boundLocal,nativeLocal,world,error)){setStatus(error,true);return;}
            Transformf inverse,weaponInverse;
            if(!invertUniformTransform(world[master],inverse)||!invertUniformTransform(global[_weaponBoneIndex],weaponInverse)){
                setStatus("Master is hidden in this clip; relationship analysis unavailable.",true);return;
            }
            // Whole-weapon movement is not evidence that a bolt/follower articulates.
            const auto relativeMaster=composed(weaponInverse,world[master]);
            if(sample==0)firstMaster=relativeMaster;
            movement=std::max(movement,distance(relativeMaster.translation,firstMaster.translation));
            rotation=std::max(rotation,angle(relativeMaster.rotation,firstMaster.rotation));
            for(int i=1;i<_handPoseAnchors.size();++i){
                const int node=_sceneNodes.indexOf(_handPoseAnchors[i].node);
                if(node<0||std::abs(world[node].scale.x)<.0001f){visible[i]=false;continue;}
                const auto relative=composed(inverse,world[node]);
                if(sample==0)firstRelative[i]=relative;
                distances[i]=std::max(distances[i],distance(relative.translation,firstRelative[i].translation));
                angles[i]=std::max(angles[i],angle(relative.rotation,firstRelative[i].rotation));
            }
        }
        const QSignalBlocker blocker(_mechanismFollowers);
        for(int row=0;row<_mechanismFollowers->count();++row){
            auto* item=_mechanismFollowers->item(row);const int i=item->data(Qt::UserRole).toInt();
            const bool related=visible[i]&&(movement>.1f||rotation>.5f)&&distances[i]<.05f&&angles[i]<.5f;
            item->setText(item->data(Qt::UserRole+1).toString()+(related?" · co-moving in this clip":""));
            item->setToolTip(visible[i]?QStringLiteral("Relative drift: %1 units, %2 degrees. Observation only; not proof of attachment.").arg(distances[i]).arg(angles[i]):"Hidden at one or more sampled frames; no relationship suggested.");
        }
        setStatus("Animation relationships measured at 61 frames in weapon space. Checkboxes unchanged; select followers explicitly.");
    }
}
