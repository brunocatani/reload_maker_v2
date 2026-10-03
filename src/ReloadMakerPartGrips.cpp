#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
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
        auto* page=new QWidget(_handPosePages);
        auto* layout=new QVBoxLayout(page);
        layout->setContentsMargins(14,14,14,14); layout->setSpacing(10);
        auto* intro=new QLabel(QStringLiteral("MAGAZINE & PART GRIPS\nSelect a magazine in Loadout, configure its hand, then save. The magazine stays fixed while you move and rotate the hand."),page);
        intro->setWordWrap(true); layout->addWidget(intro);
        _magazineFitStatus=new QLabel(page);_magazineFitStatus->setWordWrap(true);_magazineFitStatus->setObjectName("StatusCard");layout->addWidget(_magazineFitStatus);
        _magazineHandPicker=new QComboBox(page);_magazineHandPicker->addItem("Left reload hand");_magazineHandPicker->addItem("Right reload hand");layout->addWidget(_magazineHandPicker);
        connect(_magazineHandPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)setInteractionHand(PoseHand(i));updateInteractionGripUi();});
        auto* mirrorMagazine=new QPushButton("CREATE OPPOSITE-HAND MAGAZINE FIT",page);layout->addWidget(mirrorMagazine);
        connect(mirrorMagazine,&QPushButton::clicked,this,[this]{(void)mirrorMagazineGrip();});
        _editMagazineGrip=new QPushButton("CONFIGURE SELECTED MAGAZINE",page);layout->addWidget(_editMagazineGrip);
        connect(_editMagazineGrip,&QPushButton::clicked,this,[this]{(void)editMagazineGrip();});
        auto* form=new QFormLayout();
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        _gripPoseCombo=new QComboBox(page);
        _gripReferenceCombo=new QComboBox(page);
        for(auto* combo:{_gripPoseCombo,_gripReferenceCombo}) {
            combo->setMinimumContentsLength(12); combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        }
        _gripReferenceCombo->setToolTip(QStringLiteral("The part's coordinate frame, not an automatic contact point. Position and angles below define where the wrist sits relative to it."));
        form->addRow(QStringLiteral("Saved pose"),_gripPoseCombo);
        layout->addLayout(form);
        auto* starts=new QVBoxLayout();
        _gripCapturedStart=new QPushButton(QStringLiteral("USE CAPTURED OFFSET"),page);
        _gripCurrentStart=new QPushButton(QStringLiteral("USE CURRENT WRIST"),page);
        _gripCapturedStart->setToolTip(QStringLiteral("Apply the saved position AND angle relative to the selected reference part. Different part origins may require adjustment; this is not automatic grip fitting."));
        _gripCurrentStart->setToolTip(QStringLiteral("Keep the animated wrist's current position and angle, apply the selected fingers, then adjust the grip below."));
        _gripCurrentStart->setText("APPLY POSE + HAND ROTATION");
        _gripCurrentStart->setToolTip("Use the named pose's exact fingers and saved wrist rotation. Keep the current hand position; use Move to fit it to this magazine.");
        starts->addWidget(_gripCurrentStart); layout->addLayout(starts);
        _gripPick=new QPushButton(QStringLiteral("PICK PALM POINT IN VIEWPORT"),page);
        _gripPick->setObjectName("PrimaryButton");
        _gripPick->setToolTip(QStringLiteral("Click a weapon surface. A uniquely identified part becomes the reference automatically; multi-bone surfaces require an explicit reference choice. Use viewport arrows and rings to fine-tune the generic palm placement."));
        layout->addWidget(_gripPick);
        connect(_gripPick,&QPushButton::clicked,this,&ReloadMakerController::beginGripSurfacePick);
        auto* mechanism=new QPushButton(QStringLiteral("DEFINE / EDIT THIS PART'S MECHANISM"),page);
        connect(mechanism,&QPushButton::clicked,this,&ReloadMakerController::openMechanismForPart);
        auto* offsets=new QGroupBox(QStringLiteral("HAND PLACEMENT · REFERENCE-PART SPACE"),page);
        auto* grid=new QGridLayout(offsets);
        grid->addWidget(new QLabel(QStringLiteral("Game units"),offsets),0,0);
        grid->addWidget(new QLabel(QStringLiteral("Position"),offsets),1,0);
        grid->addWidget(new QLabel(QStringLiteral("Rotation °"),offsets),3,0);
        for(int axis=0;axis<3;++axis) {
            grid->addWidget(new QLabel(QStringList{"X","Y","Z"}[axis],offsets),0,axis+1);
            grid->addWidget(new QLabel(QStringList{"Pitch / X","Yaw / Y","Roll / Z"}[axis],offsets),2,axis+1);
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
        layout->addWidget(offsets);
        _gripName=new QLineEdit(page); _gripName->setMaxLength(kPoseNameLength);
        _gripName->setPlaceholderText(QStringLiteral("Fit name · shown when you grab this magazine")); layout->addWidget(_gripName);
        auto* actions=new QHBoxLayout();
        _gripSave=new QPushButton(QStringLiteral("SAVE PART GRIP"),page); _gripSave->setObjectName("PrimaryButton");
        _gripEnd=new QPushButton(QStringLiteral("END PREVIEW"),page);
        actions->addWidget(_gripSave); actions->addWidget(_gripEnd); layout->addLayout(actions);
        _useMagazineGrip=new QPushButton(QStringLiteral("SAVE FOR THIS MAGAZINE"),page);
        _useMagazineGrip->setObjectName("PrimaryButton");
        _useMagazineGrip->setToolTip(QStringLiteral("Replace the selected magazine’s active fit and save the project. Older snapshots remain in history, not as additional assignments."));
        layout->addWidget(_useMagazineGrip);
        _saveAllMagazineGrips=new QPushButton("SAVE FOR ALL MAGAZINES",page);layout->addWidget(_saveAllMagazineGrips);
        _saveAllMagazineGrips->setToolTip("Set this weapon's default. Other custom magazine fits stay unchanged. Unconfigured magazines inherit this placement.");
        connect(_saveAllMagazineGrips,&QPushButton::clicked,this,[this]{(void)useMagazineGrabGrip(true);});
        _useDefaultMagazineGrip=new QPushButton("USE WEAPON DEFAULT",page);layout->addWidget(_useDefaultMagazineGrip);
        connect(_useDefaultMagazineGrip,&QPushButton::clicked,this,[this]{(void)useDefaultMagazineGrip();});
        auto* scopeHint=new QLabel("All magazines = this weapon’s default. Individual fits take priority.",page);scopeHint->setWordWrap(true);scopeHint->setObjectName("Hint");layout->addWidget(scopeHint);
        auto* back=new QPushButton("BACK TO OPERATE",page);layout->addWidget(back);
        connect(back,&QPushButton::clicked,this,[this]{rememberMagazineGrip();(void)saveSession();setWorkspace(OperateWorkspace);});
        connect(_useMagazineGrip,&QPushButton::clicked,this,[this]{(void)useMagazineGrabGrip();});
        _gripStatus=new QLabel(page); _gripStatus->setTextFormat(Qt::PlainText); _gripStatus->setWordWrap(true); layout->addWidget(_gripStatus);
        auto* advancedToggle=new QPushButton("OTHER PARTS / SAVED FIT HISTORY",page);advancedToggle->setCheckable(true);layout->addWidget(advancedToggle);
        auto* advanced=new QWidget(page);auto* advancedLayout=new QVBoxLayout(advanced);advancedLayout->setContentsMargins(0,0,0,0);
        auto* otherPart=new QPushButton("CONFIGURE ANOTHER PART",advanced);advancedLayout->addWidget(otherPart);
        connect(otherPart,&QPushButton::clicked,this,[this]{
            rememberMagazineGrip();_editingMagazine=false;
            if(_magazineFitPreview)stopSimulation();
            restoreHandPosePreview();updatePartGripControls();
        });
        advancedLayout->addWidget(new QLabel("Reference part",advanced));advancedLayout->addWidget(_gripReferenceCombo);
        advancedLayout->addWidget(_gripCapturedStart);advancedLayout->addWidget(mechanism);
        _gripHistory=new QCheckBox("Show older snapshots and unassigned copies",advanced);advancedLayout->addWidget(_gripHistory);
        connect(_gripHistory,&QCheckBox::toggled,this,&ReloadMakerController::setShowGripHistory);
        layout->addWidget(advanced);advanced->hide();connect(advancedToggle,&QPushButton::toggled,advanced,&QWidget::setVisible);
        _gripList=new QListWidget(page); _gripList->setAlternatingRowColors(true); _gripList->setMinimumHeight(180);advancedLayout->addWidget(_gripList);
        auto* footer=new QHBoxLayout();
        _gripLoad=new QPushButton(QStringLiteral("LOAD SAVED GRIP"),page);
        auto* refresh=new QPushButton(QStringLiteral("REFRESH"),page);
        auto* folder=new QPushButton(QStringLiteral("GRIP FILES"),page);
        footer->addWidget(_gripLoad); footer->addWidget(refresh); footer->addWidget(folder); advancedLayout->addLayout(footer);
        layout->addStretch(1);
        _handPosePages->addTab(page,QStringLiteral("PART GRIPS"));
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
                ? QStringLiteral("%1 hand · elbow solved. Move / Rotate adjusts the hand only. Save for this magazine updates its active fit and project.").arg(_partGripDraft->pose.hand==PoseHand::Left?"Left":"Right")
                : QStringLiteral("NOT APPLIED: %1\nNative animation is shown. Adjust the target; saving is disabled.").arg(_gripError));
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
