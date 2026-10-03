#include "ReloadMakerController.h"
#include "GripViewport.h"
#include "StudioWindow.h"
#include "glview.h"
#include <QCheckBox>
#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
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
    void ReloadMakerController::createMechanismUi()
    {
        auto* scroll=new QScrollArea(_handPosePages);
        scroll->setWidgetResizable(true);
        auto* page=new QWidget(scroll);
        auto* layout=new QVBoxLayout(page);
        layout->setContentsMargins(12,12,12,12);
        layout->setSpacing(9);
        const auto heading=[page,layout](const QString& title){
            auto* label=new QLabel(title,page);
            label->setStyleSheet("color:#e6bd7d;font-weight:700;padding-top:5px;");
            layout->addWidget(label);
        };
        heading("01   CHOOSE THE PART");
        _workshopPick=new QPushButton("PICK PART IN 3D",page);
        _workshopPick->setObjectName("PrimaryButton");
        layout->addWidget(_workshopPick);
        _workshopPart=new QComboBox(page);
        _workshopPart->setMinimumContentsLength(18);
        _workshopPart->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        layout->addWidget(_workshopPart);

        heading("02   HAND POSE  /  OPTIONAL");
        _workshopPose=new QComboBox(page);
        _workshopPose->setMinimumContentsLength(18);
        _workshopPose->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        layout->addWidget(_workshopPose);
        auto* handRow=new QHBoxLayout();
        _workshopContact=new QPushButton("PLACE PALM",page);
        _workshopHand=new QPushButton("ADJUST HAND",page);
        _workshopHand->setCheckable(true);
        handRow->addWidget(_workshopContact);handRow->addWidget(_workshopHand);
        layout->addLayout(handRow);

        heading("03   MOVEMENT STEPS");
        _mechanismStages=new QListWidget(page);
        _mechanismStages->setMaximumHeight(105);
        _mechanismStages->setMinimumHeight(65);
        layout->addWidget(_mechanismStages);
        auto* sequence=new QHBoxLayout();
        _workshopAddStage=new QPushButton("+ NEXT STEP",page);
        auto* remove=new QPushButton("REMOVE",page);
        auto* up=new QPushButton("↑",page);auto* down=new QPushButton("↓",page);
        up->setToolTip("Move selected step earlier");down->setToolTip("Move selected step later");
        up->setMaximumWidth(36);down->setMaximumWidth(36);
        for(auto* button:{_workshopAddStage,remove,up,down})sequence->addWidget(button);
        layout->addLayout(sequence);
        auto* mode=new QHBoxLayout();
        _workshopTeach=new QPushButton("EDIT MOVEMENT",page);_workshopTeach->setCheckable(true);
        _mechanismStart=new QPushButton("TEST GRAB",page);_mechanismStart->setCheckable(true);
        mode->addWidget(_workshopTeach);mode->addWidget(_mechanismStart);layout->addLayout(mode);
        _workshopStageFields=new QWidget(page);
        auto* stageLayout=new QVBoxLayout(_workshopStageFields);stageLayout->setContentsMargins(0,0,0,0);
        auto* tools=new QHBoxLayout();
        _workshopMove=new QPushButton("MOVE  ↔",page);_workshopRotate=new QPushButton("ROTATE  ↻",page);
        _workshopMove->setCheckable(true);_workshopRotate->setCheckable(true);
        tools->addWidget(_workshopMove);tools->addWidget(_workshopRotate);stageLayout->addLayout(tools);
        _workshopPivot=new QPushButton("PICK ROTATION PIVOT IN 3D",page);stageLayout->addWidget(_workshopPivot);
        _stageForward=new QLineEdit(page);_stageForward->setPlaceholderText("Step name · e.g. Lift / Pull");
        stageLayout->addWidget(_stageForward);
        _stageRelease=new QComboBox(page);
        _stageRelease->addItems({"On release: stay where released","On release: return to this step's start","On release: return all the way to rest"});
        stageLayout->addWidget(_stageRelease);
        layout->addWidget(_workshopStageFields);

        _workshopTestControls=new QWidget(page);
        auto* sim=new QVBoxLayout(_workshopTestControls);sim->setContentsMargins(0,0,0,0);
        _mechanismPoseOnGrab=new QCheckBox("Show chosen hand while grabbing",page);
        _mechanismPoseOnGrab->setChecked(true);sim->addWidget(_mechanismPoseOnGrab);
        _mechanismSlider=new QSlider(Qt::Horizontal,page);_mechanismSlider->setRange(0,10000);sim->addWidget(_mechanismSlider);
        auto* phase=new QHBoxLayout();
        auto* previous=new QPushButton("← STEP",page);auto* next=new QPushButton("STEP →",page);auto* release=new QPushButton("RELEASE",page);
        phase->addWidget(previous);phase->addWidget(next);phase->addWidget(release);sim->addLayout(phase);
        auto* stop=new QPushButton("STOP TEST / RESTORE ANIMATION",page);sim->addWidget(stop);
        layout->addWidget(_workshopTestControls);
        _workshopHint=new QLabel(page);_workshopHint->setWordWrap(true);_workshopHint->setTextFormat(Qt::PlainText);
        _workshopHint->setStyleSheet("background:#25221d;color:#f1cf99;padding:10px;border-left:3px solid #b98748;");
        layout->addWidget(_workshopHint);
        _mechanismSave=new QPushButton("SAVE PART + HAND",page);_mechanismSave->setObjectName("PrimaryButton");layout->addWidget(_mechanismSave);

        auto* advancedToggle=new QPushButton("PART DETAILS / EXACT VALUES / LINKED PARTS",page);
        advancedToggle->setCheckable(true);layout->addWidget(advancedToggle);
        auto* advanced=new QWidget(page);auto* details=new QVBoxLayout(advanced);details->setContentsMargins(0,0,0,0);
        _mechanismName=new QLineEdit(page);_mechanismName->setMaxLength(128);_mechanismName->setPlaceholderText("Saved part name");details->addWidget(_mechanismName);
        auto* rest=new QPushButton("USE CURRENT ANIMATION FRAME AS REST",page);details->addWidget(rest);
        auto* form=new QFormLayout();_stageReverse=new QLineEdit(page);_stageMotion=new QComboBox(page);
        _stageMotion->addItems({"Slide X","Slide Y","Slide Z","Rotate X","Rotate Y","Rotate Z"});
        _stageFrom=new QDoubleSpinBox(page);_stageTo=new QDoubleSpinBox(page);_stageRate=new QDoubleSpinBox(page);
        for(auto* spin:{_stageFrom,_stageTo}){spin->setRange(-10000,10000);spin->setDecimals(3);spin->setKeyboardTracking(false);}
        _stageRate->setRange(.001,100000);_stageRate->setDecimals(3);_stageRate->setKeyboardTracking(false);
        form->addRow("Reverse name",_stageReverse);form->addRow("Step axis",_stageMotion);
        form->addRow("Start value",_stageFrom);form->addRow("End value",_stageTo);form->addRow("Return units/s or °/s",_stageRate);
        auto* pivot=new QHBoxLayout();for(auto& spin:_stagePivot){spin=new QDoubleSpinBox(page);spin->setRange(-10000,10000);spin->setDecimals(3);spin->setKeyboardTracking(false);pivot->addWidget(spin);}
        form->addRow("Rotation pivot XYZ",pivot);details->addLayout(form);
        auto* explanation=new QLabel("Each step uses the part's axes after the previous step. Rotation pivot is local to the step start, not the hand's grab point.",page);explanation->setWordWrap(true);details->addWidget(explanation);
        details->addWidget(new QLabel("ADDITIONAL PARTS THAT MOVE WITH THIS PART",page));
        _mechanismFollowers=new QListWidget(page);_mechanismFollowers->setMaximumHeight(140);details->addWidget(_mechanismFollowers);
        auto* analyze=new QPushButton("SUGGEST LINKED PARTS FROM ANIMATION",page);details->addWidget(analyze);
        _mechanismList=new QComboBox(page);details->addWidget(_mechanismList);
        auto* files=new QHBoxLayout();auto* load=new QPushButton("LOAD DEFINITION",page);auto* folder=new QPushButton("FILES",page);auto* refresh=new QPushButton("REFRESH",page);
        files->addWidget(load);files->addWidget(folder);files->addWidget(refresh);details->addLayout(files);
        _mechanismStatus=new QLabel(page);_mechanismStatus->setWordWrap(true);_mechanismStatus->setTextFormat(Qt::PlainText);details->addWidget(_mechanismStatus);
        layout->addWidget(advanced);advanced->hide();layout->addStretch();scroll->setWidget(page);
        _handPosePages->addTab(scroll,"PART WORKSHOP");
        connect(advancedToggle,&QPushButton::toggled,advanced,&QWidget::setVisible);

        _mechanismTimer=new QTimer(this);_mechanismTimer->setTimerType(Qt::PreciseTimer);_mechanismTimer->setInterval(16);
        connect(_mechanismTimer,&QTimer::timeout,this,&ReloadMakerController::advanceMechanismPreview);
        connect(_workshopPick,&QPushButton::clicked,this,[this](){beginWorkshopPick();});
        connect(_workshopContact,&QPushButton::clicked,this,[this](){beginWorkshopPick(true);});
        connect(_workshopPart,qOverload<int>(&QComboBox::activated),this,[this](int i){selectWorkshopPart(_workshopPart->itemData(i).toInt());});
        connect(_workshopPose,qOverload<int>(&QComboBox::activated),this,[this](){applyWorkshopPose();});
        connect(_workshopHand,&QPushButton::toggled,this,&ReloadMakerController::editWorkshopHand);
        connect(_workshopTeach,&QPushButton::clicked,this,&ReloadMakerController::beginMechanismTeaching);
        connect(_mechanismStart,&QPushButton::clicked,this,&ReloadMakerController::beginMechanismPreview);
        const auto tool=[this](bool rotation){
            if(!_mechanismTeaching)beginMechanismTeaching();
            _teachingRotation=rotation;_gripViewport->setRotationTool(rotation);updateMechanismControls();updateGripViewport();
        };
        connect(_workshopMove,&QPushButton::clicked,this,[tool](){tool(false);});
        connect(_workshopRotate,&QPushButton::clicked,this,[tool](){tool(true);});
        connect(_workshopPivot,&QPushButton::clicked,this,&ReloadMakerController::beginWorkshopPivotPick);
        connect(_mechanismSave,&QPushButton::clicked,this,&ReloadMakerController::saveWorkshop);
        connect(stop,&QPushButton::clicked,this,[this](){stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);updatePartGripControls();});
        connect(release,&QPushButton::clicked,this,&ReloadMakerController::releaseMechanismGrab);
        connect(_mechanismPoseOnGrab,&QCheckBox::toggled,this,[this](){if(_mechanismPreview){(void)applyPose(_playbackTime);updateMechanismControls();}});
        connect(_mechanismStages,&QListWidget::currentRowChanged,this,[this](){
            if(_mechanismUpdating)return;
            displayMechanism();if(_mechanismTeaching){
                const int i=_mechanismStages->currentRow();if(i>=0)_teachingRotation=_mechanismDraft->stages[i].rotation;
                _mechanismState.path=float(i+1);(void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();
            }
        });
        connect(_mechanismName,&QLineEdit::textEdited,this,&ReloadMakerController::readMechanismControls);
        for(auto* edit:{_stageForward,_stageReverse})connect(edit,&QLineEdit::editingFinished,this,&ReloadMakerController::readMechanismControls);
        for(auto* spin:{_stageFrom,_stageTo,_stageRate,_stagePivot[0],_stagePivot[1],_stagePivot[2]})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,&ReloadMakerController::readMechanismControls);
        for(auto* combo:{_stageMotion,_stageRelease})connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,&ReloadMakerController::readMechanismControls);
        connect(_mechanismFollowers,&QListWidget::itemChanged,this,[this](){
            if(_mechanismUpdating)return;
            const bool resume=_mechanismTeaching;stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);
            readMechanismControls();if(resume)beginMechanismTeaching();
        });
        connect(_workshopAddStage,&QPushButton::clicked,this,[this](){
            if(!_mechanismDraft||_mechanismDraft->stages.size()>=32)return;
            QString error;if(!validateMechanism(*_mechanismDraft,error)){setStatus("Finish this step's travel before adding another.",true);return;}
            MechanismStage stage;stage.forwardName=QStringLiteral("Step %1").arg(_mechanismDraft->stages.size()+1);stage.to=0;
            _mechanismDraft->stages.append(stage);displayMechanism();_mechanismStages->setCurrentRow(_mechanismDraft->stages.size()-1);beginMechanismTeaching();
        });
        connect(remove,&QPushButton::clicked,this,[this](){
            const int i=_mechanismStages->currentRow();if(!_mechanismDraft||i<0)return;
            stopMechanismPreview();_mechanismDraft->stages.removeAt(i);displayMechanism();beginMechanismTeaching();
        });
        const auto reorder=[this](int delta){
            const int i=_mechanismStages->currentRow();if(!_mechanismDraft||i<0||i+delta<0||i+delta>=_mechanismDraft->stages.size())return;
            stopMechanismPreview();_mechanismDraft->stages.swapItemsAt(i,i+delta);displayMechanism();_mechanismStages->setCurrentRow(i+delta);beginMechanismTeaching();
        };
        connect(up,&QPushButton::clicked,this,[reorder](){reorder(-1);});connect(down,&QPushButton::clicked,this,[reorder](){reorder(1);});
        connect(rest,&QPushButton::clicked,this,&ReloadMakerController::captureMechanismRest);
        connect(analyze,&QPushButton::clicked,this,[this](){stopMechanismPreview();if(_clip.valid())(void)applyPose(_playbackTime);analyzeMechanismAnimation();});
        connect(load,&QPushButton::clicked,this,&ReloadMakerController::loadMechanism);
        connect(refresh,&QPushButton::clicked,this,&ReloadMakerController::refreshHandPoseLibrary);
        connect(folder,&QPushButton::clicked,this,[](){const auto path=MechanismLibrary::defaultDirectory();if(QDir().mkpath(path))QDesktopServices::openUrl(QUrl::fromLocalFile(path));});
        connect(_mechanismSlider,&QSlider::sliderPressed,this,[this](){if(!_mechanismPreview||_mechanismTeaching)return;_mechanismHeld=true;_mechanismState.returning=false;_mechanismTimer->stop();});
        connect(_mechanismSlider,&QSlider::valueChanged,this,[this](int value){
            if(!_mechanismPreview||_mechanismTeaching||!_mechanismDraft||_mechanismUpdating)return;
            const float path=_mechanismState.activeStage+float(value)/10000;_mechanismDirection=path>=_mechanismState.path?1:-1;
            _mechanismState.path=path;_mechanismState.returning=false;_mechanismHeld=true;(void)applyPose(_playbackTime);updateMechanismControls();updateGripViewport();
        });
        connect(_mechanismSlider,&QSlider::sliderReleased,this,&ReloadMakerController::releaseMechanismGrab);
        const auto changePhase=[this](int direction){
            if(!_mechanismPreview||_mechanismTeaching||!_mechanismDraft||_mechanismState.returning)return;
            const int next=_mechanismState.activeStage+direction;const float boundary=float(_mechanismState.activeStage+(direction>0));
            if(next<0||next>=_mechanismDraft->stages.size()||std::abs(_mechanismState.path-boundary)>.0001f){setStatus("Reach the step endpoint before changing direction.",true);return;}
            _mechanismState.activeStage=next;_mechanismDirection=direction;updateMechanismControls();updateGripViewport();
        };
        connect(previous,&QPushButton::clicked,this,[changePhase](){changePhase(-1);});connect(next,&QPushButton::clicked,this,[changePhase](){changePhase(1);});
        refreshWorkshopChoices();updateMechanismControls();
    }
    void ReloadMakerController::refreshWorkshopChoices()
    {
        if(!_workshopPart||_workshopRefreshing)return;
        _workshopRefreshing=true;
        const QSignalBlocker a(_workshopPart),b(_workshopPose);
        const QString previous=_workshopPose->currentData().toString();
        _workshopPart->clear();_workshopPart->addItem("Choose a part — or click PICK PART IN 3D",-1);
        QVector<int> listed;
        for(int i=1;i<_handPoseAnchors.size();++i){
            if(std::any_of(listed.cbegin(),listed.cend(),[&](int old){return sameMechanismPart(_handPoseAnchors[old].description,_handPoseAnchors[i].description);}))continue;
            listed.append(i);_workshopPart->addItem(_handPoseAnchors[i].label,i);
            _workshopPart->setItemData(_workshopPart->count()-1,_handPoseAnchors[i].tooltip,Qt::ToolTipRole);
            if(_mechanismDraft&&sameMechanismPart(_mechanismDraft->anchor,_handPoseAnchors[i].description))_workshopPart->setCurrentIndex(_workshopPart->count()-1);
        }
        _workshopPose->clear();_workshopPose->addItem("No hand — edit the part only",QString());
        if(_mechanismDraft){
            for(const auto& grip:_partGrips)if(grip.weaponRecord.compare(_mechanismDraft->weaponRecord,Qt::CaseInsensitive)==0&&sameMechanismPart(grip.pose.placement,_mechanismDraft->anchor))
                _workshopPose->addItem(QStringLiteral("Saved grip · %1 · %2").arg(grip.pose.hand==PoseHand::Left?"Left":"Right",grip.pose.name),"grip:"+grip.pose.id);
            for(const auto& pose:_savedHandPoses)_workshopPose->addItem(QStringLiteral("Finger pose · %1 · %2").arg(pose.hand==PoseHand::Left?"Left":"Right",pose.name),"pose:"+pose.id);
        }
        const int found=_workshopPose->findData(previous);if(found>=0)_workshopPose->setCurrentIndex(found);
        _workshopRefreshing=false;
    }
    void ReloadMakerController::selectWorkshopPart(int index,std::optional<Vec3f> point,const QString& surface)
    {
        if(!_clip.valid()||_loadThread||_rebuildTimer->isActive()||index<=0||index>=_handPoseAnchors.size())return;
        const auto anchor=_handPoseAnchors[index];const QString weapon=poseRecordKey(currentWeapon()->formId);
        const bool same=_mechanismDraft&&_mechanismDraft->weaponRecord.compare(weapon,Qt::CaseInsensitive)==0&&sameMechanismPart(_mechanismDraft->anchor,anchor.description);
        stopMechanismPreview();_workshopHandEditing=false;_workshopPicking=false;
        _gripViewport->setPicking(false);
        if(!same){_workshopContactPoint.reset();_workshopSurface.clear();_workshopGripDirty=false;}
        if(_partGripDraft&&!sameMechanismPart(_partGripDraft->pose.placement,anchor.description))_partGripDraft.reset();
        if(!applyPose(_playbackTime))return;
        QString error;Transformf world,inverse;
        if(!sceneNodeWorld(anchor.node,world,error)||!invertUniformTransform(world,inverse)){setStatus("Cannot select part: "+error,true);return;}
        if(point){_workshopContactPoint=composed(inverse,Transformf{*point}).translation;_workshopSurface=surface;}
        if(!same){
            const PartMechanism* existing=nullptr;
            for(const auto& saved:_mechanisms)if(saved.weaponRecord.compare(weapon,Qt::CaseInsensitive)==0&&sameMechanismPart(saved.anchor,anchor.description)){
                if(existing){setStatus("Duplicate definitions describe this part; resolve the duplicate files first.",true);return;}existing=&saved;
            }
            if(existing)_mechanismDraft=*existing;
            else {
                if(!invertUniformTransform(_globalPose[_weaponBoneIndex],inverse)){setStatus("Cannot resolve Weapon frame",true);return;}
                PartMechanism part;part.id=QUuid::createUuid().toString(QUuid::WithoutBraces);part.name=anchor.description.anchorName;
                part.weaponRecord=weapon;part.anchor=anchor.description;part.partInWeaponAtRest=composed(inverse,world);_mechanismDraft=part;
            }
            const QSignalBlocker blocker(_workshopPose);_workshopPose->setCurrentIndex(0);
        }
        _gripAnchor=index;showGripAnchor();
        displayMechanism();refreshWorkshopChoices();
        if(_partGripDraft){
            QString key="grip:"+_partGripDraft->pose.id;int choice=_workshopPose->findData(key);
            if(choice<0)choice=_workshopPose->findData("pose:"+_partGripDraft->sourcePoseId);
            if(choice>=0)_workshopPose->setCurrentIndex(choice);
        }else if(!same&&_workshopPose->count()>1&&_workshopPose->itemData(1).toString().startsWith("grip:"))_workshopPose->setCurrentIndex(1);
        setHandEditorPage(HandEditorPage::Mechanism);
        applyWorkshopPose();
    }
    void ReloadMakerController::applyWorkshopPose()
    {
        if(!_mechanismDraft||!_clip.valid()||_loadThread)return;
        const QString choice=_workshopPose->currentData().toString();
        stopMechanismPreview();_workshopHandEditing=false;
        if(!applyPose(_playbackTime))return;
        QString error;const int anchor=resolveMechanismAnchor(_mechanismDraft->anchor,error);
        if(anchor<0||!preparePartGripFrame(anchor,error)){setStatus(error,true);return;}
        if(choice.isEmpty()){
            _partGripDraft.reset();_workshopGripDirty=false;
        }else if(choice.startsWith("grip:")){
            const QString id=choice.mid(5);
            const auto saved=std::find_if(_partGrips.cbegin(),_partGrips.cend(),[&](const auto& grip){return grip.pose.id==id;});
            if(saved==_partGrips.cend()){setStatus("Saved grip is no longer available.",true);return;}
            _partGripDraft=*saved;_workshopGripDirty=saved->mechanismId!=_mechanismDraft->id;
        }else{
            const QString id=choice.mid(5);
            const auto saved=std::find_if(_savedHandPoses.cbegin(),_savedHandPoses.cend(),[&](const auto& pose){return pose.id==id;});
            if(saved==_savedHandPoses.cend()){setStatus("Finger pose is no longer available.",true);return;}
            const bool keep=_partGripDraft&&_partGripDraft->pose.hand==saved->hand&&sameMechanismPart(_partGripDraft->pose.placement,_mechanismDraft->anchor);
            PartGrip draft{*saved,_mechanismDraft->weaponRecord,saved->id};
            draft.pose.placement=_mechanismDraft->anchor;
            if(keep){draft.pose.placement=_partGripDraft->pose.placement;draft.surfaceContact=_partGripDraft->surfaceContact;}
            else {
                const auto reference=gripReferenceWorld();Transformf inverse;
                if(!invertUniformTransform(reference,inverse)){setStatus("Invalid part frame",true);return;}
                draft.pose.placement.handInAnchor.rotation=saved->placement.handInAnchor.rotation;
                const auto palm=genericPalmPoint(draft.pose);const Vec3f contact=_workshopContactPoint.value_or(Vec3f{});
                const auto placed=placePalmAt(composed(reference,draft.pose.placement.handInAnchor),palm,composed(reference,Transformf{contact}).translation);
                draft.pose.placement.handInAnchor=composed(inverse,placed);
                if(_workshopContactPoint)draft.surfaceContact=PartGrip::SurfaceContact{contact,palm,_workshopSurface};
            }
            _partGripDraft=draft;_workshopGripDirty=true;
        }
        if(_partGripDraft){
            _partGripDraft->mechanismId=_mechanismDraft->id;
            followGripInForm(_partGripDraft->sourcePoseId,anchor);
            setGripDraftName(_partGripDraft->pose.name+" · "+_mechanismDraft->name);
            setArmsShown(true);showPartGripValues();
        }
        if(!applyPose(_playbackTime))return;
        beginMechanismTeaching();
    }
    void ReloadMakerController::beginWorkshopPick(bool contactOnly)
    {
        if(!_clip.valid()||_loadThread||(contactOnly&&!_partGripDraft))return;
        stopMechanismPreview();_workshopHandEditing=false;
        stopPlayback();
        if(!applyPose(_playbackTime))return;
        _workshopPicking=true;_workshopContactOnly=contactOnly;_workshopPivotPicking=false;
        setHandEditorPage(HandEditorPage::Mechanism);updateGripViewport();_gripViewport->setPicking(true);
        _workshopHint->setText(contactOnly?"Click the contact point on the selected part. Then ADJUST HAND shows its move/rotate handles.":"Click the part you want to drive. No hand pose is required. If several bones drive the surface, choose the intended part from the menu.");
    }
    void ReloadMakerController::pickWorkshopPart(QPointF point)
    {
        if(!_workshopPicking)return;
        int automatic=-1;Vec3f world;QString surface,error;QVector<int> candidates;
        if(!pickWeaponPart(point,automatic,world,surface,candidates,error)){_workshopHint->setText(error);return;}
        if(_workshopPivotPicking&&_mechanismDraft){
            const int i=_mechanismStages->currentRow();if(i<0)return;
            const auto start=composed(composed(_globalPose[_weaponBoneIndex],_mechanismDraft->partInWeaponAtRest),mechanismDelta(*_mechanismDraft,float(i)));
            Transformf inverse;if(!invertUniformTransform(start,inverse))return;
            _mechanismDraft->stages[i].pivot=composed(inverse,Transformf{world}).translation;
            _workshopPicking=false;_workshopPivotPicking=false;_gripViewport->setPicking(false);_mechanismState.path=float(i+1);
            displayMechanism();(void)applyPose(_playbackTime);updateGripViewport();return;
        }
        if(_workshopContactOnly&&_mechanismDraft&&_partGripDraft){
            QString error;const int anchor=resolveMechanismAnchor(_mechanismDraft->anchor,error);
            if(anchor<0||!std::any_of(candidates.cbegin(),candidates.cend(),[&](int i){return sameMechanismPart(_handPoseAnchors[i].description,_mechanismDraft->anchor);})){_workshopHint->setText("That surface is not owned by the selected part. Pick the selected part, or change the part first.");return;}
            const auto reference=gripReferenceWorld();Transformf inverse;
            if(!invertUniformTransform(reference,inverse))return;
            _workshopContactPoint=composed(inverse,Transformf{world}).translation;_workshopSurface=surface;
            const auto palm=genericPalmPoint(_partGripDraft->pose);
            const auto hand=placePalmAt(composed(reference,_partGripDraft->pose.placement.handInAnchor),palm,world);
            _partGripDraft->pose.placement.handInAnchor=composed(inverse,hand);
            _partGripDraft->surfaceContact=PartGrip::SurfaceContact{*_workshopContactPoint,palm,surface};
            _workshopPicking=false;_workshopGripDirty=true;_gripViewport->setPicking(false);
            editWorkshopHand(true);return;
        }
        if(automatic>=0){selectWorkshopPart(automatic,world,surface);return;}
        if(candidates.isEmpty()){_workshopHint->setText("No unique moving frame was found. Choose the part in the part dropdown.");return;}
        _workshopPicking=false;_gripViewport->setPicking(false);
        auto* menu=new QMenu(_window);menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addSection("Which part should this point drive?");
        for(int index:candidates){auto* action=menu->addAction(_handPoseAnchors[index].label);connect(action,&QAction::triggered,this,[this,index,world,surface](){selectWorkshopPart(index,world,surface);});}
        menu->popup(_window->getGLView()->mapToGlobal(point.toPoint()));
    }
    void ReloadMakerController::editWorkshopHand(bool enabled)
    {
        if(enabled&&!_partGripDraft){updateMechanismControls();return;}
        stopMechanismPreview();_workshopPicking=false;_gripViewport->setPicking(false);_workshopHandEditing=enabled;
        if(_clip.valid())(void)applyPose(_playbackTime);
        if(!enabled&&_mechanismDraft)beginMechanismTeaching();
        updatePartGripControls();
    }
    void ReloadMakerController::beginWorkshopPivotPick()
    {
        if(!_mechanismDraft||!_clip.valid()||_loadThread)return;
        if(!_mechanismTeaching)beginMechanismTeaching();
        if(!_mechanismTeaching)return;
        int i=_mechanismStages->currentRow();
        if(i<0){MechanismStage stage;stage.rotation=true;stage.to=0;stage.forwardName="Rotate";_mechanismDraft->stages.append(stage);displayMechanism();i=_mechanismDraft->stages.size()-1;_mechanismStages->setCurrentRow(i);}
        _mechanismState.path=float(i);_workshopPicking=true;_workshopPivotPicking=true;_workshopContactOnly=false;
        (void)applyPose(_playbackTime);updateGripViewport();_gripViewport->setPicking(true);
        _workshopHint->setText("Click the rotation center on the weapon. The part is shown at this step's start; its ring will turn around the point you choose. Esc cancels.");
    }
    void ReloadMakerController::captureMechanismRest()
    {
        if(!_mechanismDraft||!_clip.valid()||_loadThread)return;
        stopMechanismPreview();if(!applyPose(_playbackTime))return;
        QString error;const int i=resolveMechanismAnchor(_mechanismDraft->anchor,error);Transformf master,inverse;
        if(i<0||!sceneNodeWorld(_handPoseAnchors[i].node,master,error)||!invertUniformTransform(_globalPose[_weaponBoneIndex],inverse)){setStatus("Cannot capture rest: "+error,true);return;}
        auto captured=*_mechanismDraft;captured.partInWeaponAtRest=composed(inverse,master);
        if(!invertUniformTransform(master,inverse)){setStatus("Rest part is hidden / non-invertible",true);return;}
        for(auto& f:captured.followers){const int j=resolveMechanismAnchor(f.anchor,error);Transformf w;if(j<0||!sceneNodeWorld(_handPoseAnchors[j].node,w,error)){setStatus(error,true);return;}f.inPartAtRest=composed(inverse,w);}
        _mechanismDraft=std::move(captured);beginMechanismTeaching();setStatus("Part rest captured from this animation frame.");
    }
    void ReloadMakerController::saveWorkshop()
    {
        if(!_mechanismDraft)return;
        const bool resume=_mechanismTeaching;
        stopMechanismPreview();if(_clip.valid()&&!applyPose(_playbackTime))return;
        const int revision=_mechanismDraft->revision;saveMechanism();
        if(_mechanismDraft->revision==revision){if(resume)beginMechanismTeaching();return;}
        if(_partGripDraft&&_workshopGripDirty){
            _partGripDraft->mechanismId=_mechanismDraft->id;
            if(!savePartGrip()){setStatus("Part saved, but the hand grip was not saved. Check its reach/placement before saving again.",true);return;}
            _workshopGripDirty=false;
        }
        refreshWorkshopChoices();
        if(_partGripDraft){const int choice=_workshopPose->findData("grip:"+_partGripDraft->pose.id);if(choice>=0)_workshopPose->setCurrentIndex(choice);}
        setStatus(_partGripDraft?"Saved this part's movement and its selected hand grip. Source finger poses are unchanged.":"Saved this part's movement. No hand pose is attached.");
        if(resume)beginMechanismTeaching();
    }
}
