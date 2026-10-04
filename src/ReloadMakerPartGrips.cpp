#include "ReloadMakerController.h"
#include "design/DesignWidgets.h"
#include "ReloadBundle.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QQuaternion>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace reloadmaker {
    void ReloadMakerController::createPartGripUi()
    {
        using namespace design;
        // The Part grips inspector in authoring order: what is held, which
        // hand, its fingers, where the palm sits, then save. Exact numbers
        // and other parts stay folded.
        auto* page=new QWidget(_handPosePages);
        auto* layout=new QVBoxLayout(page);
        layout->setContentsMargins(16,10,16,16); layout->setSpacing(8);
        const auto section=[page,layout](const QString& title,const QString& hint){
            layout->addSpacing(8);
            layout->addWidget(sectionLabel(title,page));
            if(!hint.isEmpty())layout->addWidget(hintLabel(hint,page));
        };
        layout->addWidget(hintLabel(QStringLiteral("Select a magazine or part in the loadout, place its hand, then save. The part stays fixed while you move and rotate the hand."),page));
        _magazineFitStatus=new QLabel(page);_magazineFitStatus->setWordWrap(true);_magazineFitStatus->setObjectName("StatusCard");layout->addWidget(_magazineFitStatus);
        _editMagazineGrip=new QPushButton("Configure selected magazine",page);layout->addWidget(_editMagazineGrip);
        connect(_editMagazineGrip,&QPushButton::clicked,this,[this]{(void)editMagazineGrip();});

        section(QStringLiteral("Hand"),QStringLiteral("The hand that holds it during the reload."));
        _magazineHandPicker=new QComboBox(page);_magazineHandPicker->addItem("Left reload hand");_magazineHandPicker->addItem("Right reload hand");
        auto* mirrorMagazine=new QPushButton("Mirror to other hand",page);
        mirrorMagazine->setToolTip(QStringLiteral("Create the opposite hand's fit for this magazine by mirroring this one."));
        auto* handRow=new QHBoxLayout();handRow->setSpacing(6);handRow->addWidget(_magazineHandPicker,1);handRow->addWidget(mirrorMagazine);layout->addLayout(handRow);
        connect(_magazineHandPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)setInteractionHand(PoseHand(i));updateInteractionGripUi();});
        connect(mirrorMagazine,&QPushButton::clicked,this,[this]{(void)mirrorMagazineGrip();});

        section(QStringLiteral("Fingers"),QStringLiteral("A saved pose from the Poses library."));
        _gripPoseCombo=new QComboBox(page);
        _gripReferenceCombo=new QComboBox(page);
        for(auto* combo:{_gripPoseCombo,_gripReferenceCombo}) {
            combo->setMinimumContentsLength(12); combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        }
        _gripReferenceCombo->setToolTip(QStringLiteral("The part's coordinate frame, not an automatic contact point. Position and angles define where the wrist sits relative to it."));
        layout->addWidget(_gripPoseCombo);
        _gripCapturedStart=new QPushButton(QStringLiteral("Use captured offset"),page);
        _gripCurrentStart=new QPushButton(QStringLiteral("Apply pose and hand rotation"),page);
        _gripCapturedStart->setToolTip(QStringLiteral("Apply the saved position AND angle relative to the selected reference part. Different part origins may require adjustment; this is not automatic grip fitting."));
        _gripCurrentStart->setToolTip(QStringLiteral("Use the named pose's exact fingers and saved wrist rotation. Keep the current hand position; use Move to fit it to this magazine."));
        layout->addWidget(_gripCurrentStart);
        auto* capture=new QPushButton(QStringLiteral("Capture poses on the reload clip"),page);
        capture->setToolTip(QStringLiteral("Open the Poses library on the reload clip with the weapon fixed and the magazine as the reference part."));
        layout->addWidget(capture);
        connect(capture,&QPushButton::clicked,this,[this]{if(ensureOperatingRig())(void)authorReloadGrips();});

        section(QStringLiteral("Placement"),QStringLiteral("Pick where the palm touches, then drag the arrows and rings on the model."));
        _gripPick=new QPushButton(QStringLiteral("Pick palm point on the model"),page);
        _gripPick->setToolTip(QStringLiteral("Click a weapon surface. A uniquely identified part becomes the reference automatically; multi-bone surfaces require an explicit reference choice. Use viewport arrows and rings to fine-tune the generic palm placement."));
        layout->addWidget(_gripPick);
        connect(_gripPick,&QPushButton::clicked,this,&ReloadMakerController::beginGripSurfacePick);
        auto* offsets=new QWidget(page);
        auto* offsetLayout=new QVBoxLayout(offsets);offsetLayout->setContentsMargins(0,0,0,0);offsetLayout->setSpacing(6);
        offsetLayout->addWidget(hintLabel(QStringLiteral("In the reference part's space: game units and degrees."),offsets));
        auto* grid=new QGridLayout();grid->setHorizontalSpacing(6);grid->setVerticalSpacing(4);
        grid->addWidget(new QLabel(QStringLiteral("Position"),offsets),1,0);
        grid->addWidget(new QLabel(QStringLiteral("Rotation"),offsets),3,0);
        for(int axis=0;axis<3;++axis) {
            auto* positionAxis=hintLabel(QStringList{"X","Y","Z"}[axis],offsets);
            auto* angleAxis=hintLabel(QStringList{"Pitch · X","Yaw · Y","Roll · Z"}[axis],offsets);
            grid->addWidget(positionAxis,0,axis+1); grid->addWidget(angleAxis,2,axis+1);
            _gripPosition[axis]=new QDoubleSpinBox(offsets);
            _gripAngles[axis]=new QDoubleSpinBox(offsets);
            _gripPosition[axis]->setRange(-10000,10000); _gripPosition[axis]->setDecimals(4); _gripPosition[axis]->setSingleStep(.1);
            _gripAngles[axis]->setRange(-360,360); _gripAngles[axis]->setDecimals(3); _gripAngles[axis]->setSingleStep(1);
            for(auto* spin:{_gripPosition[axis],_gripAngles[axis]}) {
                spin->setKeyboardTracking(false); spin->setMinimumWidth(70);
            }
            grid->addWidget(_gripPosition[axis],1,axis+1); grid->addWidget(_gripAngles[axis],3,axis+1);
            connect(_gripPosition[axis],qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,axis](double value){setGripPosition(axis,float(value));});
            connect(_gripAngles[axis],qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](){
                setGripRotation(QVector3D(float(_gripAngles[0]->value()),float(_gripAngles[1]->value()),float(_gripAngles[2]->value())));
            });
        }
        offsetLayout->addLayout(grid);
        layout->addWidget(disclosure(QStringLiteral("Exact placement"),offsets,page));
        layout->addWidget(offsets);

        section(QStringLiteral("Save"),{});
        _gripName=new QLineEdit(page); _gripName->setMaxLength(kPoseNameLength);
        _gripName->setPlaceholderText(QStringLiteral("Fit name, shown when you grab this magazine")); layout->addWidget(_gripName);
        _useMagazineGrip=new QPushButton(QStringLiteral("Save for this magazine"),page);
        _useMagazineGrip->setObjectName("PrimaryButton");
        _useMagazineGrip->setToolTip(QStringLiteral("Replace the selected magazine’s active fit and save the project. Older snapshots remain in history, not as additional assignments."));
        layout->addWidget(_useMagazineGrip);
        _saveAllMagazineGrips=new QPushButton("Save for all magazines",page);
        _saveAllMagazineGrips->setToolTip("Set this weapon's default. Other custom magazine fits stay unchanged. Unconfigured magazines inherit this placement.");
        _useDefaultMagazineGrip=new QPushButton("Use weapon default",page);
        auto* scope=new QHBoxLayout();scope->setSpacing(6);scope->addWidget(_saveAllMagazineGrips);scope->addWidget(_useDefaultMagazineGrip);layout->addLayout(scope);
        connect(_saveAllMagazineGrips,&QPushButton::clicked,this,[this]{(void)useMagazineGrabGrip(true);});
        connect(_useDefaultMagazineGrip,&QPushButton::clicked,this,[this]{(void)useDefaultMagazineGrip();});
        layout->addWidget(hintLabel("All magazines sets this weapon’s default. Individual fits take priority.",page));
        auto* actions=new QHBoxLayout();actions->setSpacing(6);
        _gripSave=new QPushButton(QStringLiteral("Save part grip"),page); _gripSave->setObjectName("PrimaryButton");
        _gripEnd=new QPushButton(QStringLiteral("End preview"),page);
        actions->addWidget(_gripSave); actions->addWidget(_gripEnd); layout->addLayout(actions);
        connect(_useMagazineGrip,&QPushButton::clicked,this,[this]{(void)useMagazineGrabGrip();});
        _gripStatus=new QLabel(page); _gripStatus->setObjectName("Callout"); _gripStatus->setTextFormat(Qt::PlainText); _gripStatus->setWordWrap(true); layout->addWidget(_gripStatus);
        auto* back=new QPushButton("Back to Operate",page);layout->addWidget(back);
        connect(back,&QPushButton::clicked,this,[this]{rememberMagazineGrip();(void)saveSession();setWorkspace(OperateWorkspace);});

        layout->addSpacing(4);
        auto* advanced=new QWidget(page);auto* advancedLayout=new QVBoxLayout(advanced);advancedLayout->setContentsMargins(0,0,0,0);advancedLayout->setSpacing(8);
        layout->addWidget(disclosure(QStringLiteral("Other parts and saved fit history"),advanced,page));
        auto* otherPart=new QPushButton("Configure another part",advanced);advancedLayout->addWidget(otherPart);
        connect(otherPart,&QPushButton::clicked,this,[this]{
            rememberMagazineGrip();_editingMagazine=false;
            if(_magazineFitPreview)stopSimulation();
            restoreHandPosePreview();updatePartGripControls();
        });
        auto* mechanism=new QPushButton(QStringLiteral("Define or edit this part's mechanism"),advanced);
        connect(mechanism,&QPushButton::clicked,this,&ReloadMakerController::openMechanismForPart);
        advancedLayout->addWidget(sectionLabel(QStringLiteral("Reference part"),advanced));advancedLayout->addWidget(_gripReferenceCombo);
        advancedLayout->addWidget(_gripCapturedStart);advancedLayout->addWidget(mechanism);
        _gripHistory=new QCheckBox("Show older snapshots and unassigned copies",advanced);advancedLayout->addWidget(_gripHistory);
        connect(_gripHistory,&QCheckBox::toggled,this,&ReloadMakerController::setShowGripHistory);
        layout->addWidget(advanced);
        _gripList=new QListWidget(page); _gripList->setMinimumHeight(180);advancedLayout->addWidget(_gripList);
        auto* footer=new QHBoxLayout();footer->setSpacing(6);
        _gripLoad=new QPushButton(QStringLiteral("Load saved grip"),page);
        auto* refresh=new QPushButton(QStringLiteral("Refresh"),page);
        auto* folder=new QPushButton(QStringLiteral("Open folder"),page);
        footer->addWidget(_gripLoad); footer->addWidget(refresh); footer->addWidget(folder); advancedLayout->addLayout(footer);
        layout->addStretch(1);
        _handPosePages->addTab(page,QStringLiteral("Part grips"));
        connect(_gripCapturedStart,&QPushButton::clicked,this,[this](){startPartGrip(true);});
        connect(_gripCurrentStart,&QPushButton::clicked,this,&ReloadMakerController::applyGripPoseToDraft);
        connect(_gripSave,&QPushButton::clicked,this,&ReloadMakerController::savePartGrip);
        connect(_gripLoad,&QPushButton::clicked,this,&ReloadMakerController::loadPartGrip);
        connect(_gripEnd,&QPushButton::clicked,this,&ReloadMakerController::restoreHandPosePreview);
        connect(refresh,&QPushButton::clicked,this,&ReloadMakerController::refreshPartGripLibrary);
        connect(_gripName,&QLineEdit::textChanged,this,&ReloadMakerController::setGripDraftName);
        connect(_gripList,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item){
            selectPartGrip(item?item->data(Qt::UserRole).toString():QString());
        });
        connect(_gripList,&QListWidget::itemActivated,this,[this](){loadPartGrip();});
        connect(_gripPoseCombo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){
            setGripPose(_gripPoseCombo->itemData(index).toString());
        });
        connect(_gripReferenceCombo,qOverload<int>(&QComboBox::currentIndexChanged),this,&ReloadMakerController::setGripAnchor);
        connect(folder,&QPushButton::clicked,this,[this](){
            const auto path=PartGripLibrary::defaultDirectory();
            if(!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) setStatus(QStringLiteral("Cannot open the part-grip folder"),true);
        });
    }
    void ReloadMakerController::showPartGripChoices()
    {
        const QSignalBlocker a(_gripPoseCombo), b(_gripReferenceCombo);
        _gripPoseCombo->clear(); _gripReferenceCombo->clear();
        for(const auto& pose:_savedHandPoses) _gripPoseCombo->addItem(QStringLiteral("%1 · %2 hand").arg(pose.name,pose.hand==PoseHand::Left?"Left":"Right"),pose.id);
        for(int i=0;i<_handPoseAnchors.size();++i) {
            _gripReferenceCombo->addItem(_handPoseAnchors[i].label,i);
            _gripReferenceCombo->setItemData(i,_handPoseAnchors[i].tooltip,Qt::ToolTipRole);
        }
        _gripPoseCombo->setCurrentIndex(_gripPoseCombo->findData(_gripPoseId));
        _gripReferenceCombo->setCurrentIndex(_gripAnchor);
    }
    void ReloadMakerController::showGripPose()
    {
        const QSignalBlocker blocker(_gripPoseCombo);
        _gripPoseCombo->setCurrentIndex(_gripPoseCombo->findData(_gripPoseId));
    }
    void ReloadMakerController::showGripAnchor()
    {
        const QSignalBlocker blocker(_gripReferenceCombo);
        _gripReferenceCombo->setCurrentIndex(_gripAnchor);
    }
    void ReloadMakerController::showGripDraftName()
    {
        if(_gripName->text()==_gripDraftName) return;
        const QSignalBlocker blocker(_gripName);
        _gripName->setText(_gripDraftName);
    }
    void ReloadMakerController::showSelectedPartGrip()
    {
        const QSignalBlocker blocker(_gripList);
        for(int row=0;row<_gripList->count();++row) if(_gripList->item(row)->data(Qt::UserRole).toString()==_selectedGripId) { _gripList->setCurrentRow(row); return; }
        _gripList->setCurrentRow(-1);
    }
    void ReloadMakerController::showGripHistory()
    {
        const QSignalBlocker blocker(_gripHistory);
        _gripHistory->setChecked(_showGripHistory);
    }
    void ReloadMakerController::showGripStatus(const QString& text)
    {
        _gripStatus->setText(text);
    }
    // This weapon's saved grips; older snapshots and unassigned magazine
    // copies only when history is shown. The selection stays on the selected
    // grip while it is listed, else moves to the first listed grip.
    void ReloadMakerController::refreshPartGripList()
    {
        const QSignalBlocker blocker(_gripList); _gripList->clear();
        QString first; int listed=0; bool selectedListed=false;
        for(const auto& grip:_partGrips) {
            if(!partGripShown(grip)) continue;
            auto* item=new QListWidgetItem(QStringLiteral("%1\n%2 hand · %3").arg(grip.pose.name,grip.pose.hand==PoseHand::Left?"Left":"Right",grip.pose.placement.anchorName),_gripList);
            item->setData(Qt::UserRole,grip.pose.id);
            item->setToolTip(QStringLiteral("%1\n%2\n%3").arg(grip.pose.placement.ownerModel,grip.pose.placement.ownerRecord,grip.pose.createdUtc));
            if(listed++==0) first=grip.pose.id;
            if(grip.pose.id==_selectedGripId) { _gripList->setCurrentItem(item); selectedListed=true; }
        }
        // Filtering is not a user selection; the document follows the list.
        if(!selectedListed) { _selectedGripId=first; if(listed) _gripList->setCurrentRow(0); }
        updatePartGripControls();
    }
    void ReloadMakerController::updatePartGripControls()
    {
        if(!_gripPoseCombo) return;
        const bool frame=partGripFrameReady();
        const bool start=canStartPartGrip();
        _gripCapturedStart->setEnabled(start); _gripCurrentStart->setEnabled(start);
        _gripPick->setEnabled(canPickGripSurface());
        const bool editing=partGripEditable();
        _gripReferenceCombo->setEnabled(!_editingMagazine);
        for(auto* spin:_gripPosition) spin->setEnabled(editing);
        for(auto* spin:_gripAngles) spin->setEnabled(editing);
        _gripSave->setEnabled(canSavePartGrip());
        const bool magazine=editing&&magazineGrabAnchor(_partGripDraft->pose.placement)>=0;
        if(_useMagazineGrip)_useMagazineGrip->setEnabled(_gripSave->isEnabled()&&magazine);
        if(_saveAllMagazineGrips)_saveAllMagazineGrips->setEnabled(_useMagazineGrip->isEnabled());
        _gripSave->setVisible(!magazine&&!_editingMagazine);
        _gripEnd->setVisible(!_editingMagazine);
        updateMagazineGripUi();updateInteractionGripUi();
        _gripEnd->setEnabled(editing);
        _gripLoad->setEnabled(canLoadPartGrip());
        if(editing) {
            _gripStatus->setText(_gripPreviewValid
                ? QStringLiteral("%1 hand · elbow solved. Move and Rotate adjust the hand only. Save for this magazine updates its active fit and the project.").arg(_partGripDraft->pose.hand==PoseHand::Left?"Left":"Right")
                : QStringLiteral("Not applied: %1\nThe native animation is shown. Adjust the target; saving is disabled.").arg(_gripError));
        } else {
            _gripStatus->setText(frame?QStringLiteral("Choose a finger pose and reference part, then choose a starting placement. Saved grips include fingers, position and angle.")
                :QStringLiteral("Load a weapon and animation to configure its part grips."));
        }
        updateGripViewport();
        updateMechanismControls();
    }
    void ReloadMakerController::showPartGripValues()
    {
        const auto& t=_partGripDraft->pose.placement.handInAnchor;
        const auto e=QQuaternion(t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z).toEulerAngles();
        const float positions[]{t.translation.x,t.translation.y,t.translation.z};
        const float angles[]{e.x(),e.y(),e.z()};
        for(int i=0;i<3;++i) {
            const QSignalBlocker a(_gripPosition[i]),b(_gripAngles[i]);
            _gripPosition[i]->setValue(positions[i]); _gripAngles[i]->setValue(angles[i]);
        }
        // Merely displaying rounded Euler values never overwrites the saved matrix.
        updatePartGripControls();
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
}
