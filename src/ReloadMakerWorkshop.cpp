#include "ReloadMakerController.h"
#include "design/DesignWidgets.h"
#include "design/Icons.h"
#include "design/Theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
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
    // The Mechanisms inspector, in the order a part is authored: the part,
    // how it moves, the parts that move with it, an optional hand, then save.
    // Exact numbers and saved files stay folded away.
    void ReloadMakerController::createMechanismUi()
    {
        using namespace design;
        auto* scroll=new QScrollArea(_handPosePages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* page=new QWidget(scroll);
        auto* layout=new QVBoxLayout(page);
        layout->setContentsMargins(16,10,16,16);
        layout->setSpacing(8);
        const auto section=[page,layout](const QString& title,const QString& hint){
            layout->addSpacing(8);
            layout->addWidget(sectionLabel(title,page));
            if(!hint.isEmpty())layout->addWidget(hintLabel(hint,page));
        };
        const auto row=[layout](std::initializer_list<QWidget*> widgets){
            auto* line=new QHBoxLayout();line->setSpacing(6);
            for(auto* widget:widgets)line->addWidget(widget,widget?1:0);
            layout->addLayout(line);
            return line;
        };

        section("Part","Choose the moving part, or pick it on the model.");
        _workshopPart=new QComboBox(page);
        _workshopPart->setMinimumContentsLength(14);
        _workshopPart->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        _workshopPick=new QPushButton("Pick in 3D",page);
        _workshopPick->setToolTip("Click the part on the model");
        auto* partRow=row({_workshopPart});partRow->addWidget(_workshopPick);

        section("Movement","Each step moves the part along one axis or turns it around one pivot.");
        _mechanismStages=new QListWidget(page);
        _mechanismStages->setMaximumHeight(120);
        _mechanismStages->setMinimumHeight(70);
        layout->addWidget(_mechanismStages);
        _workshopAddStage=new QPushButton("Add step",page);
        auto* remove=new QPushButton("Remove",page);
        auto* up=new QPushButton(page);auto* down=new QPushButton(page);
        up->setObjectName("IconButton");down->setObjectName("IconButton");
        up->setIcon(icon(Icon::ChevronUp,color::textSecondary));down->setIcon(icon(Icon::ChevronDown,color::textSecondary));
        up->setToolTip("Move the selected step earlier");down->setToolTip("Move the selected step later");
        up->setAccessibleName("Step earlier");down->setAccessibleName("Step later");
        auto* sequence=row({});
        sequence->addWidget(_workshopAddStage);sequence->addWidget(remove);sequence->addStretch(1);
        sequence->addWidget(up);sequence->addWidget(down);
        _workshopTeach=new QPushButton("Edit movement",page);_workshopTeach->setCheckable(true);
        _workshopTeach->setToolTip("Drag the part's arrows and rings to set where each step ends");
        _mechanismStart=new QPushButton("Test grab",page);_mechanismStart->setCheckable(true);
        _mechanismStart->setToolTip("Grab and pull the part to check its limits and release rules");
        row({_workshopTeach,_mechanismStart});
        _workshopStageFields=new QWidget(page);
        auto* stageLayout=new QVBoxLayout(_workshopStageFields);stageLayout->setContentsMargins(0,0,0,0);stageLayout->setSpacing(8);
        auto* tools=new QHBoxLayout();tools->setSpacing(6);
        _workshopMove=new QPushButton("Move",page);_workshopRotate=new QPushButton("Rotate",page);
        _workshopMove->setCheckable(true);_workshopRotate->setCheckable(true);
        tools->addWidget(_workshopMove);tools->addWidget(_workshopRotate);stageLayout->addLayout(tools);
        _workshopPivot=new QPushButton("Pick rotation pivot in 3D",page);stageLayout->addWidget(_workshopPivot);
        _stageForward=new QLineEdit(page);_stageForward->setPlaceholderText("Step name, e.g. Pull slide");
        stageLayout->addWidget(_stageForward);
        _stageRelease=new QComboBox(page);
        _stageRelease->addItems({"On release: stay where released","On release: return to this step's start","On release: return all the way to rest"});
        stageLayout->addWidget(_stageRelease);
        layout->addWidget(_workshopStageFields);

        _workshopTestControls=new QWidget(page);
        auto* sim=new QVBoxLayout(_workshopTestControls);sim->setContentsMargins(0,0,0,0);sim->setSpacing(8);
        _mechanismPoseOnGrab=new QCheckBox("Show the chosen hand while grabbing",page);
        _mechanismPoseOnGrab->setChecked(true);sim->addWidget(_mechanismPoseOnGrab);
        _mechanismSlider=new QSlider(Qt::Horizontal,page);_mechanismSlider->setRange(0,10000);sim->addWidget(_mechanismSlider);
        auto* phase=new QHBoxLayout();phase->setSpacing(6);
        auto* previous=new QPushButton("Previous step",page);auto* next=new QPushButton("Next step",page);auto* release=new QPushButton("Release",page);
        phase->addWidget(previous);phase->addWidget(next);phase->addWidget(release);sim->addLayout(phase);
        auto* stop=new QPushButton("Stop test",page);stop->setToolTip("Stop testing and restore the animation");sim->addWidget(stop);
        layout->addWidget(_workshopTestControls);
        _workshopHint=new QLabel(page);_workshopHint->setObjectName("Callout");
        _workshopHint->setWordWrap(true);_workshopHint->setTextFormat(Qt::PlainText);
        layout->addWidget(_workshopHint);

        // The suggestion sits on the section heading so it is seen with the list.
        layout->addSpacing(8);
        auto* linkedHeader=new QHBoxLayout();linkedHeader->setSpacing(6);
        linkedHeader->addWidget(sectionLabel("Linked parts",page),1,Qt::AlignBottom);
        _mechanismSuggest=new QPushButton("Suggest from animation",page);
        _mechanismSuggest->setObjectName("ViewChip");
        _mechanismSuggest->setToolTip("Samples the current clip and marks the parts that moved with this part. A hint only; the ticks stay yours.");
        linkedHeader->addWidget(_mechanismSuggest);
        layout->addLayout(linkedHeader);
        layout->addWidget(hintLabel("Parts that move with this one, like sights on a slide. Tick each linked part.",page));
        _mechanismFollowers=new QListWidget(page);
        _mechanismFollowers->setMinimumHeight(90);_mechanismFollowers->setMaximumHeight(180);
        layout->addWidget(_mechanismFollowers);

        section("Hand · optional","Show a hand on the part while you edit and test it.");
        _workshopPose=new QComboBox(page);
        _workshopPose->setMinimumContentsLength(14);
        _workshopPose->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        layout->addWidget(_workshopPose);
        _workshopContact=new QPushButton("Place palm",page);
        _workshopHand=new QPushButton("Adjust hand",page);
        _workshopHand->setCheckable(true);
        row({_workshopContact,_workshopHand});

        layout->addSpacing(8);
        _mechanismSave=new QPushButton("Save part and hand",page);_mechanismSave->setObjectName("PrimaryButton");layout->addWidget(_mechanismSave);
        _mechanismStatus=hintLabel({},page);_mechanismStatus->setTextFormat(Qt::PlainText);layout->addWidget(_mechanismStatus);

        layout->addSpacing(4);
        auto* advancedToggle=new QPushButton("Exact values and saved files",page);
        advancedToggle->setObjectName("Disclosure");advancedToggle->setCheckable(true);
        advancedToggle->setIcon(icon(Icon::ChevronRight,color::textMuted));layout->addWidget(advancedToggle);
        auto* advanced=new QWidget(page);auto* details=new QVBoxLayout(advanced);details->setContentsMargins(0,0,0,0);details->setSpacing(8);
        _mechanismName=new QLineEdit(page);_mechanismName->setMaxLength(128);_mechanismName->setPlaceholderText("Saved part name");details->addWidget(_mechanismName);
        auto* rest=new QPushButton("Use current animation frame as rest",page);details->addWidget(rest);
        auto* form=new QFormLayout();form->setSpacing(8);_stageReverse=new QLineEdit(page);_stageMotion=new QComboBox(page);
        _stageMotion->addItems({"Slide X","Slide Y","Slide Z","Rotate X","Rotate Y","Rotate Z"});
        _stageFrom=new QDoubleSpinBox(page);_stageTo=new QDoubleSpinBox(page);_stageRate=new QDoubleSpinBox(page);
        for(auto* spin:{_stageFrom,_stageTo}){spin->setRange(-10000,10000);spin->setDecimals(3);spin->setKeyboardTracking(false);}
        _stageRate->setRange(.001,100000);_stageRate->setDecimals(3);_stageRate->setKeyboardTracking(false);
        form->addRow("Reverse name",_stageReverse);form->addRow("Step axis",_stageMotion);
        form->addRow("Start value",_stageFrom);form->addRow("End value",_stageTo);form->addRow("Return units/s or °/s",_stageRate);
        auto* pivot=new QHBoxLayout();for(auto& spin:_stagePivot){spin=new QDoubleSpinBox(page);spin->setRange(-10000,10000);spin->setDecimals(3);spin->setKeyboardTracking(false);pivot->addWidget(spin);}
        form->addRow("Rotation pivot XYZ",pivot);details->addLayout(form);
        details->addWidget(hintLabel("Each step uses the part's axes after the previous step. The rotation pivot is local to the step start, not the hand's grab point.",page));
        details->addWidget(sectionLabel("Saved definitions",page));
        _mechanismList=new QComboBox(page);details->addWidget(_mechanismList);
        auto* files=new QHBoxLayout();files->setSpacing(6);auto* load=new QPushButton("Load definition",page);auto* folder=new QPushButton("Open folder",page);auto* refresh=new QPushButton("Refresh",page);
        files->addWidget(load);files->addWidget(folder);files->addWidget(refresh);details->addLayout(files);
        layout->addWidget(advanced);advanced->hide();layout->addStretch();scroll->setWidget(page);
        _handPosePages->addTab(scroll,"Mechanisms");
        connect(advancedToggle,&QPushButton::toggled,advanced,[advanced,advancedToggle](bool open){
            advanced->setVisible(open);
            advancedToggle->setIcon(icon(open?Icon::ChevronDown:Icon::ChevronRight,color::textMuted));
        });

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
        connect(_mechanismSuggest,&QPushButton::clicked,this,&ReloadMakerController::suggestMechanismFollowers);
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
        _workshopPart->clear();_workshopPart->addItem("Choose a part…",-1);
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
