#include "ReloadMakerController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace reloadmaker {
    void ReloadMakerController::displayMechanism(){
        if(_mechanismUpdating){return;}
        _mechanismUpdating=true;const QSignalBlocker a(_mechanismStages),b(_mechanismFollowers);normalizeMechanismStage();_mechanismStages->clear();_mechanismFollowers->clear();
        _mechanismName->setText(_mechanismDraft?_mechanismDraft->name:QString());
        if(_mechanismDraft){
            for(int i=0;i<_mechanismDraft->stages.size();++i){const auto& s=_mechanismDraft->stages[i];_mechanismStages->addItem(QStringLiteral("%1. %2 ↔ %3 · %4 %5 [%6 → %7]").arg(i+1).arg(s.forwardName,s.reverseName,s.rotation?"Rotate":"Slide",QStringList{"X","Y","Z"}[s.axis]).arg(s.from).arg(s.to));}
            if(!_mechanismDraft->stages.isEmpty())_mechanismStages->setCurrentRow(_mechanismStage);
            QString error;const int master=resolveMechanismAnchor(_mechanismDraft->anchor,error);const int root=master>=0?_sceneNodes.indexOf(_handPoseAnchors[master].node):-1;
            for(int i=1;i<_handPoseAnchors.size();++i){if(i==master)continue;const auto& anchor=_handPoseAnchors[i];auto* item=new QListWidgetItem(_handPoseAnchors[i].label,_mechanismFollowers);item->setData(Qt::UserRole,i);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(Qt::Unchecked);
                MechanismScenePlan plan;QString reason;if(root<0||!compileMechanismScene(_assemblyNodes,_nodeBindings,{root,int(_sceneNodes.indexOf(anchor.node))},plan,reason)){item->setFlags(item->flags()&~Qt::ItemIsEnabled);item->setText(item->text()+" · inherited / overlapping");}
                item->setData(Qt::UserRole+1,item->text());
                for(const auto& f:_mechanismDraft->followers)if(sameGripAnchor(f.anchor,anchor.description))item->setCheckState(Qt::Checked);
            }
            const int selected=_mechanismStage;if(selected>=0){const auto& s=_mechanismDraft->stages[selected];_stageForward->setText(s.forwardName);_stageReverse->setText(s.reverseName);_stageMotion->setCurrentIndex((s.rotation?3:0)+s.axis);_stageRelease->setCurrentIndex(int(s.release));_stageFrom->setValue(s.from);_stageTo->setValue(s.to);_stageRate->setValue(s.returnRate);_stagePivot[0]->setValue(s.pivot.x);_stagePivot[1]->setValue(s.pivot.y);_stagePivot[2]->setValue(s.pivot.z);}
        }
        _mechanismUpdating=false;updateMechanismControls();
    }
    void ReloadMakerController::updateMechanismControls()
    {
        if(!_mechanismStatus||!_workshopHint)return;
        const bool frame=_clip.valid()&&!_loadThread&&!_rebuildTimer->isActive();
        const bool part=frame&&_mechanismDraft.has_value();
        const bool testing=_mechanismPreview&&!_mechanismTeaching;
        const bool step=part&&_mechanismStage>=0;
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
                .arg(_mechanismStage+1)
                :QStringLiteral("EDITING PART · drag any arrow to create the first movement. ROTATE shows the rotation rings. No numeric setup is required.");
        }else hint="Choose EDIT MOVEMENT for part arrows, or ADJUST HAND for hand arrows.";
        if(hand&&(_mechanismTeaching||_workshopHandEditing||_mechanismHeld)&&!_gripPreviewValid)hint+="\nHand cannot reach this target: "+_gripError;
        if(!_workshopPicking)_workshopHint->setText(hint);
        _mechanismStatus->setText(!part?"No part selected.":valid?"Definition can be saved. Movement rules are shared by this part's hand grips.":error);
    }
    // The definition form as the user left it: name, the selected step's
    // fields and the checked follower parts.
    ReloadMakerController::MechanismForm ReloadMakerController::mechanismForm() const
    {
        MechanismForm form;
        form.name=_mechanismName->text().trimmed();
        form.forwardName=_stageForward->text();form.reverseName=_stageReverse->text();
        form.motion=_stageMotion->currentIndex();form.release=_stageRelease->currentIndex();
        form.from=float(_stageFrom->value());form.to=float(_stageTo->value());form.returnRate=float(_stageRate->value());
        form.pivot={float(_stagePivot[0]->value()),float(_stagePivot[1]->value()),float(_stagePivot[2]->value())};
        for(int row=0;row<_mechanismFollowers->count();++row){auto* item=_mechanismFollowers->item(row);if(item->checkState()==Qt::Checked)form.followers.append(item->data(Qt::UserRole).toInt());}
        return form;
    }
    void ReloadMakerController::readMechanismForm(){
        if(_mechanismUpdating||!_mechanismDraft)return;
        applyMechanismForm(mechanismForm());
    }
    void ReloadMakerController::showMechanismStageSummary(int i){
        const auto& s=_mechanismDraft->stages[i];
        _mechanismStages->item(i)->setText(QStringLiteral("%1. %2 ↔ %3 · %4 %5 [%6 → %7]").arg(i+1).arg(s.forwardName,s.reverseName,s.rotation?"Rotate":"Slide",QStringList{"X","Y","Z"}[s.axis]).arg(s.from).arg(s.to));
    }
    // A step being taught: its axis, endpoint and list entry.
    void ReloadMakerController::showTaughtStage(int i){
        const auto& stage=_mechanismDraft->stages[i];
        {const QSignalBlocker a(_stageMotion),b(_stageTo);_stageMotion->setCurrentIndex((stage.rotation?3:0)+stage.axis);_stageTo->setValue(stage.to);}
        _mechanismStages->item(i)->setText(QStringLiteral("%1   %2 · %3 %4 → %5").arg(i+1).arg(stage.forwardName,stage.rotation?"Rotate":"Move",QStringList{"X","Y","Z"}[stage.axis]).arg(stage.to,0,'f',2));
    }
    void ReloadMakerController::showFollowerObservations(const QVector<FollowerObservation>& observations){
        const QSignalBlocker blocker(_mechanismFollowers);
        for(int row=0;row<_mechanismFollowers->count();++row){
            auto* item=_mechanismFollowers->item(row);const auto& o=observations[item->data(Qt::UserRole).toInt()];
            item->setText(item->data(Qt::UserRole+1).toString()+(o.related?" · co-moving in this clip":""));
            item->setToolTip(o.visible?QStringLiteral("Relative drift: %1 units, %2 degrees. Observation only; not proof of attachment.").arg(o.distance).arg(o.angle):"Hidden at one or more sampled frames; no relationship suggested.");
        }
    }
}
