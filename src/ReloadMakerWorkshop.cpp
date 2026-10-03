#include "ReloadMakerController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

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
        connect(_workshopPose,qOverload<int>(&QComboBox::activated),this,[this](int i){chooseWorkshopPose(_workshopPose->itemData(i).toString());});
        connect(_workshopHand,&QPushButton::toggled,this,&ReloadMakerController::editWorkshopHand);
        connect(_workshopTeach,&QPushButton::clicked,this,&ReloadMakerController::beginMechanismTeaching);
        connect(_mechanismStart,&QPushButton::clicked,this,&ReloadMakerController::beginMechanismPreview);
        connect(_workshopMove,&QPushButton::clicked,this,[this](){chooseTeachingTool(false);});
        connect(_workshopRotate,&QPushButton::clicked,this,[this](){chooseTeachingTool(true);});
        connect(_workshopPivot,&QPushButton::clicked,this,&ReloadMakerController::beginWorkshopPivotPick);
        connect(_mechanismSave,&QPushButton::clicked,this,&ReloadMakerController::saveWorkshop);
        connect(stop,&QPushButton::clicked,this,&ReloadMakerController::endMechanismTest);
        connect(release,&QPushButton::clicked,this,&ReloadMakerController::releaseMechanismGrab);
        connect(_mechanismPoseOnGrab,&QCheckBox::toggled,this,&ReloadMakerController::setMechanismHandShownOnGrab);
        connect(_mechanismStages,&QListWidget::currentRowChanged,this,[this](int row){
            if(_mechanismUpdating)return;
            selectMechanismStage(row);
        });
        connect(_mechanismName,&QLineEdit::textEdited,this,&ReloadMakerController::readMechanismForm);
        for(auto* edit:{_stageForward,_stageReverse})connect(edit,&QLineEdit::textEdited,this,&ReloadMakerController::readMechanismForm);
        for(auto* spin:{_stageFrom,_stageTo,_stageRate,_stagePivot[0],_stagePivot[1],_stagePivot[2]})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,&ReloadMakerController::readMechanismForm);
        for(auto* combo:{_stageMotion,_stageRelease})connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,&ReloadMakerController::readMechanismForm);
        connect(_mechanismFollowers,&QListWidget::itemChanged,this,[this](){
            if(_mechanismUpdating)return;
            applyFollowerChange(mechanismForm());
        });
        connect(_mechanismList,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){selectMechanism(_mechanismList->itemData(i).toString());});
        connect(_workshopAddStage,&QPushButton::clicked,this,&ReloadMakerController::addMechanismStage);
        connect(remove,&QPushButton::clicked,this,&ReloadMakerController::removeMechanismStage);
        connect(up,&QPushButton::clicked,this,[this](){moveMechanismStage(-1);});connect(down,&QPushButton::clicked,this,[this](){moveMechanismStage(1);});
        connect(rest,&QPushButton::clicked,this,&ReloadMakerController::captureMechanismRest);
        connect(analyze,&QPushButton::clicked,this,&ReloadMakerController::suggestMechanismFollowers);
        connect(load,&QPushButton::clicked,this,&ReloadMakerController::loadMechanism);
        connect(refresh,&QPushButton::clicked,this,&ReloadMakerController::refreshHandPoseLibrary);
        connect(folder,&QPushButton::clicked,this,[](){const auto path=MechanismLibrary::defaultDirectory();if(QDir().mkpath(path))QDesktopServices::openUrl(QUrl::fromLocalFile(path));});
        connect(_mechanismSlider,&QSlider::sliderPressed,this,&ReloadMakerController::holdMechanismSlider);
        connect(_mechanismSlider,&QSlider::valueChanged,this,&ReloadMakerController::slideMechanism);
        connect(_mechanismSlider,&QSlider::sliderReleased,this,&ReloadMakerController::releaseMechanismGrab);
        connect(previous,&QPushButton::clicked,this,[this](){changeMechanismStep(-1);});connect(next,&QPushButton::clicked,this,[this](){changeMechanismStep(1);});
        refreshWorkshopChoices();updateMechanismControls();
    }
    // The part list (each mechanical part once) and the hand choices. The
    // hand choice stays while it is listed, else no hand.
    void ReloadMakerController::refreshWorkshopChoices()
    {
        if(!_workshopPart||_workshopRefreshing)return;
        _workshopRefreshing=true;
        const QSignalBlocker a(_workshopPart),b(_workshopPose);
        _workshopPart->clear();_workshopPart->addItem("Choose a part — or click PICK PART IN 3D",-1);
        QVector<int> listed;
        for(int i=1;i<_handPoseAnchors.size();++i){
            if(std::any_of(listed.cbegin(),listed.cend(),[&](int old){return sameMechanismPart(_handPoseAnchors[old].description,_handPoseAnchors[i].description);}))continue;
            listed.append(i);_workshopPart->addItem(_handPoseAnchors[i].label,i);
            _workshopPart->setItemData(_workshopPart->count()-1,_handPoseAnchors[i].tooltip,Qt::ToolTipRole);
            if(_mechanismDraft&&sameMechanismPart(_mechanismDraft->anchor,_handPoseAnchors[i].description))_workshopPart->setCurrentIndex(_workshopPart->count()-1);
        }
        // Refreshing is not a user choice; the document follows the list.
        if(!workshopPoseListed(_workshopPoseChoice))_workshopPoseChoice.clear();
        _workshopPose->clear();
        for(const auto& choice:workshopPoseChoices())_workshopPose->addItem(choice.label,choice.key);
        _workshopPose->setCurrentIndex(std::max(0,_workshopPose->findData(_workshopPoseChoice)));
        _workshopRefreshing=false;
    }
    void ReloadMakerController::showWorkshopPoseChoice()
    {
        const QSignalBlocker blocker(_workshopPose);
        _workshopPose->setCurrentIndex(std::max(0,_workshopPose->findData(_workshopPoseChoice)));
    }
    void ReloadMakerController::showWorkshopHint(const QString& text)
    {
        _workshopHint->setText(text);
    }
    void ReloadMakerController::showMechanismStage()
    {
        const QSignalBlocker blocker(_mechanismStages);
        _mechanismStages->setCurrentRow(_mechanismStage);
    }
    void ReloadMakerController::showMechanismChoices()
    {
        const QSignalBlocker blocker(_mechanismList);
        _mechanismList->clear();
        for(const auto* p:weaponMechanisms())_mechanismList->addItem(p->name+" · "+p->anchor.anchorName,p->id);
        _mechanismList->setCurrentIndex(_mechanismList->findData(_selectedMechanismId));
    }
    void ReloadMakerController::showSelectedMechanism()
    {
        const QSignalBlocker blocker(_mechanismList);
        _mechanismList->setCurrentIndex(_mechanismList->findData(_selectedMechanismId));
    }
    void ReloadMakerController::showMechanismHandShownOnGrab()
    {
        const QSignalBlocker blocker(_mechanismPoseOnGrab);
        _mechanismPoseOnGrab->setChecked(_mechanismHandShownOnGrab);
    }
}
