#include "ReloadMakerController.h"
#include "ReloadBundle.h"
#include "StudioWindow.h"
#include "model/nifmodel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
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
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

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
            connect(_gripPosition[axis],qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,axis](double value){
                if(!_partGripDraft) return;
                auto& p=_partGripDraft->pose.placement.handInAnchor.translation;
                if(axis==0) p.x=float(value); else if(axis==1) p.y=float(value); else p.z=float(value);
                (void)applyPose(_playbackTime); updatePartGripControls();
            });
            connect(_gripAngles[axis],qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](){
                if(!_partGripDraft) return;
                const auto q=QQuaternion::fromEulerAngles(float(_gripAngles[0]->value()),float(_gripAngles[1]->value()),float(_gripAngles[2]->value()));
                _partGripDraft->pose.placement.handInAnchor.rotation={q.x(),q.y(),q.z(),q.scalar()};
                (void)applyPose(_playbackTime); updatePartGripControls();
            });
        }
        layout->addWidget(offsets);
        _gripName=new QLineEdit(page); _gripName->setMaxLength(128);
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
        connect(_gripHistory,&QCheckBox::toggled,this,&ReloadMakerController::refreshPartGripList);
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
        connect(_gripCurrentStart,&QPushButton::clicked,this,[this](){
            const auto id=_gripPoseCombo->currentData().toString();
            const auto selected=std::find_if(_savedHandPoses.cbegin(),_savedHandPoses.cend(),[&](const auto& p){return p.id==id;});
            if(selected==_savedHandPoses.cend())return;
            const auto pose=*selected;
            if(!_partGripDraft)startPartGrip(true);
            if(!_partGripDraft)return;
            _partGripDraft->pose.hand=pose.hand;_partGripDraft->pose.fingers=pose.fingers;_partGripDraft->sourcePoseId=pose.id;
            _partGripDraft->pose.placement.handInAnchor.rotation=pose.placement.handInAnchor.rotation;
            showPartGripValues();(void)applyPose(_playbackTime);rememberMagazineGrip();updatePartGripControls();
        });
        connect(_gripSave,&QPushButton::clicked,this,&ReloadMakerController::savePartGrip);
        connect(_gripLoad,&QPushButton::clicked,this,&ReloadMakerController::loadPartGrip);
        connect(_gripEnd,&QPushButton::clicked,this,&ReloadMakerController::restoreHandPosePreview);
        connect(refresh,&QPushButton::clicked,this,&ReloadMakerController::refreshPartGripLibrary);
        connect(_gripName,&QLineEdit::textChanged,this,&ReloadMakerController::updatePartGripControls);
        connect(_gripList,&QListWidget::currentItemChanged,this,&ReloadMakerController::updatePartGripControls);
        connect(_gripList,&QListWidget::itemActivated,this,[this](){loadPartGrip();});
        connect(_gripPoseCombo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](){
            updatePartGripControls();
        });
        connect(_gripReferenceCombo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){
            if(_partGripDraft && index>=0){
                QString error;
                if(!assignGripPart(index,error)) {
                    const QSignalBlocker blocker(_gripReferenceCombo);
                    for(int i=0;i<_handPoseAnchors.size();++i)
                        if(sameGripAnchor(_handPoseAnchors[i].description,_partGripDraft->pose.placement))
                            _gripReferenceCombo->setCurrentIndex(i);
                    setStatus(error,true);
                } else showPartGripValues();
            }
            updatePartGripControls();
        });
        connect(folder,&QPushButton::clicked,this,[this](){
            const auto path=PartGripLibrary::defaultDirectory();
            if(!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) setStatus(QStringLiteral("Cannot open the part-grip folder"),true);
        });
    }
    void ReloadMakerController::openPartGripEditor()
    {
        const auto* pose=selectedHandPose();
        if(pose) _gripPoseCombo->setCurrentIndex(_gripPoseCombo->findData(pose->id));
        _gripReferenceCombo->setCurrentIndex(_handPoseAnchor->currentIndex());
        _handPosePages->setCurrentIndex(1);
    }
    void ReloadMakerController::refreshPartGripLibrary()
    {
        QStringList diagnostics; _partGrips=_partGripLibrary.load(diagnostics);
        for(const auto& error:diagnostics) appendDiagnostic("PART GRIP FILE "+error);
        if(!diagnostics.isEmpty()) setStatus(QStringLiteral("Some part-grip files could not be loaded; see Diagnostics. Files were left untouched."),true);
        refreshPartGripChoices(); refreshPartGripList();
    }
    void ReloadMakerController::refreshPartGripChoices()
    {
        const auto poseId=_gripPoseCombo->currentData(); const int anchor=_gripReferenceCombo->currentData().toInt();
        const QSignalBlocker a(_gripPoseCombo), b(_gripReferenceCombo);
        _gripPoseCombo->clear(); _gripReferenceCombo->clear();
        for(const auto& pose:_savedHandPoses) _gripPoseCombo->addItem(QStringLiteral("%1 · %2 hand").arg(pose.name,pose.hand==PoseHand::Left?"Left":"Right"),pose.id);
        for(int i=0;i<_handPoseAnchors.size();++i) {
            _gripReferenceCombo->addItem(_handPoseAnchor->itemText(i),i);
            _gripReferenceCombo->setItemData(i,_handPoseAnchor->itemData(i,Qt::ToolTipRole),Qt::ToolTipRole);
        }
        const int found=_gripPoseCombo->findData(poseId); if(found>=0) _gripPoseCombo->setCurrentIndex(found);
        if(anchor>=0 && anchor<_gripReferenceCombo->count()) _gripReferenceCombo->setCurrentIndex(anchor);
        updatePartGripControls();
    }
    const PartGrip* ReloadMakerController::selectedPartGrip() const
    {
        const auto* item=_gripList->currentItem(); if(!item) return nullptr;
        for(const auto& grip:_partGrips) if(grip.pose.id==item->data(Qt::UserRole).toString()) return &grip;
        return nullptr;
    }
    void ReloadMakerController::refreshPartGripList()
    {
        const auto* current=_gripList->currentItem(); const auto id=current?current->data(Qt::UserRole).toString():QString();
        const QSignalBlocker blocker(_gripList); _gripList->clear();
        const auto* weapon=currentWeapon(); const auto record=weapon?poseRecordKey(weapon->formId):QString();
        for(const auto& grip:_partGrips) {
            if(grip.weaponRecord.compare(record,Qt::CaseInsensitive)!=0) continue;
            const bool assigned=_magazineGrabGrips.values().contains(grip.pose.id)||_magazineGrabDefaults.values().contains(grip.pose.id);
            if(_gripHistory&&!_gripHistory->isChecked()&&magazineGrabSupported(grip.pose.placement)&&!assigned)continue;
            auto* item=new QListWidgetItem(QStringLiteral("%1\n%2 hand · %3").arg(grip.pose.name,grip.pose.hand==PoseHand::Left?"Left":"Right",grip.pose.placement.anchorName),_gripList);
            item->setData(Qt::UserRole,grip.pose.id);
            item->setToolTip(QStringLiteral("%1\n%2\n%3").arg(grip.pose.placement.ownerModel,grip.pose.placement.ownerRecord,grip.pose.createdUtc));
            if(grip.pose.id==id) _gripList->setCurrentItem(item);
        }
        if(!_gripList->currentItem() && _gripList->count()) _gripList->setCurrentRow(0);
        updatePartGripControls();
    }
    void ReloadMakerController::updatePartGripControls()
    {
        if(!_gripPoseCombo) return;
        const bool frame=!_loadThread && !_rebuildTimer->isActive() && _clip.valid() && !_geometrySignature.isEmpty() &&
            _renderedWeapon>=0 && _renderedWeapon==_selectedWeapon;
        const bool start=frame && _gripPoseCombo->currentIndex()>=0 && _gripReferenceCombo->currentIndex()>=0;
        _gripCapturedStart->setEnabled(start); _gripCurrentStart->setEnabled(start);
        _gripPick->setEnabled(frame && (_partGripDraft.has_value() || start));
        const bool editing=frame && _partGripDraft.has_value() && !_mechanismPreview;
        _gripReferenceCombo->setEnabled(!_editingMagazine);
        for(auto* spin:_gripPosition) spin->setEnabled(editing);
        for(auto* spin:_gripAngles) spin->setEnabled(editing);
        _gripSave->setEnabled(editing && _gripPreviewValid && !_gripName->text().trimmed().isEmpty());
        const bool magazine=editing&&magazineGrabAnchor(_partGripDraft->pose.placement)>=0;
        if(_useMagazineGrip)_useMagazineGrip->setEnabled(_gripSave->isEnabled()&&magazine);
        if(_saveAllMagazineGrips)_saveAllMagazineGrips->setEnabled(_useMagazineGrip->isEnabled());
        _gripSave->setVisible(!magazine&&!_editingMagazine);
        _gripEnd->setVisible(!_editingMagazine);
        updateMagazineGripUi();updateInteractionGripUi();
        _gripEnd->setEnabled(editing);
        _gripLoad->setEnabled(frame && selectedPartGrip());
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
    bool ReloadMakerController::preparePartGripFrame(int anchorIndex,QString& error)
    {
        if(anchorIndex<0 || anchorIndex>=_handPoseAnchors.size()) { error="Choose a reference part"; return false; }
        const auto& anchor=_handPoseAnchors[anchorIndex];
        if(anchor.description.anchorKind=="weapon") { _partGripFrame={true,-1,{}}; return true; }
        Transformf world;
        if(!sceneNodeWorld(anchor.node,world,error)) return false;
        const auto* model=activeModel(); QModelIndex node=anchor.node;
        for(int depth=0;node.isValid() && depth<model->getBlockCount();++depth) {
            for(int i=0;i<_nodeBindings.size();++i) {
                if(_sceneNodes[_nodeBindings[i].nodeIndex]!=node) continue;
                Transformf inverse;
                if(!invertUniformTransform(_boundGlobalPose[i],inverse)) { error="Reference part has a zero-scaled animated parent"; return false; }
                _partGripFrame={false,i,composed(inverse,world),int(_sceneNodes.indexOf(anchor.node))}; return true;
            }
            node=model->getBlockIndex(model->getParent(node));
        }
        error="Reference part has no animated assembly frame"; return false;
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
    void ReloadMakerController::startPartGrip(bool capturedOffset)
    {
        updatePartGripControls(); if(!_gripCapturedStart->isEnabled()) return;
        const HandPose* selected=nullptr;
        for(const auto& pose:_savedHandPoses) if(pose.id==_gripPoseCombo->currentData().toString()) selected=&pose;
        if(!selected) return;
        if(_editingMagazine&&selected->hand!=_interactionHand){setStatus("Choose a saved pose for the selected reload hand.",true);return;}
        const bool sameSource=_partGripDraft&&_partGripDraft->sourcePoseId==selected->id;
        const int anchor=_gripReferenceCombo->currentData().toInt();
        _editingMagazine=anchor>=0&&anchor<_handPoseAnchors.size()&&magazineGrabAnchor(_handPoseAnchors[anchor].description)>=0;
        restoreHandPosePreview();
        _playing=false; _playbackTimer->stop(); _playButton->setText("PLAY");
        if(!applyPose(_playbackTime)) return;
        QString error;
        if(!preparePartGripFrame(anchor,error)) { setStatus(error,true); return; }
        PartGrip draft{*selected,poseRecordKey(currentWeapon()->formId),selected->id};
        draft.pose.placement=_handPoseAnchors[anchor].description;
        draft.pose.placement.handInAnchor=selected->placement.handInAnchor;
        if(!capturedOffset) {
            const auto reference=_partGripFrame.weapon?_globalPose[_weaponBoneIndex]:composed(_boundGlobalPose[_partGripFrame.binding],_partGripFrame.bridge);
            if(!captureHandPose(_skeleton,_localPose,_globalPose,reference,draft.pose,error)) { setStatus(error,true); return; }
            draft.pose.fingers=selected->fingers;
        }
        _partGripDraft=std::move(draft);
        if(!assignGripPart(anchor,error)) {setStatus(error,true);_partGripDraft.reset();return;}
        if(!sameSource||_gripName->text().trimmed().isEmpty()) _gripName->setText(selected->name+" · "+_handPoseAnchors[anchor].description.anchorName);
        _showArmsCheck->setChecked(true);
        showPartGripValues(); (void)applyPose(_playbackTime); updateHandPoseControls();
    }
    bool ReloadMakerController::applyPartGripPreview()
    {
        const auto reference=gripReferenceWorld();
        const auto target=composed(reference,_partGripDraft->pose.placement.handInAnchor);
        _gripPreviewValid=solvePartGrip(_skeleton,_localPose,_globalPose,_partGripDraft->pose,target,_localPose,_globalPose,_gripError,magazineGrabAnchor(_partGripDraft->pose.placement)>=0?GripReach::FollowTarget:GripReach::FixedShoulder);
        if(!_gripPreviewValid) { updatePartGripControls(); return true; }
        return evaluateSceneBindings(_skeleton,_nodeBindings,_sourceLocalPose,_stateBaselineLocalPose,
                                     _globalPose,_boundLocalPose,_boundGlobalPose,_poseError)&&applySimulationPose();
    }
    bool ReloadMakerController::savePartGrip()
    {
        if(_partGripDraft&&magazineGrabAnchor(_partGripDraft->pose.placement)>=0)return useMagazineGrabGrip();
        updatePartGripControls(); if(!_gripSave->isEnabled()) return false;
        PartGrip grip=*_partGripDraft;
        grip.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        grip.pose.name=_gripName->text().trimmed(); grip.pose.createdUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        grip.pose.source=currentPoseSource();
        Transformf inverse; QString error;
        const int hand=_skeleton.boneIndex(grip.pose.hand==PoseHand::Left?"LArm_Hand":"RArm_Hand");
        if(!invertUniformTransform(_globalPose[_weaponBoneIndex],inverse) || hand<0) { setStatus("Cannot capture grip placement",true); return false; }
        grip.pose.placement.handInWeapon=composed(inverse,_globalPose[hand]);
        if(!_partGripLibrary.save(grip,error)) { setStatus("Part grip was not saved: "+error,true); return false; }
        _partGripDraft=grip;
        refreshPartGripLibrary();
        for(int row=0;row<_gripList->count();++row) if(_gripList->item(row)->data(Qt::UserRole).toString()==grip.pose.id) _gripList->setCurrentRow(row);
        appendDiagnostic(QStringLiteral("SAVED PART GRIP %1 · %2 · reference=%3").arg(grip.pose.name,grip.pose.id,grip.pose.placement.anchorName));
        setStatus(QStringLiteral("Saved part grip “%1”: fingers, position and angle. Source pose unchanged.").arg(grip.pose.name));
        return true;
    }
    void ReloadMakerController::loadPartGrip()
    {
        updatePartGripControls(); const auto* selected=selectedPartGrip(); if(!_gripLoad->isEnabled() || !selected) return;
        (void)loadGripRecord(*selected);
    }
    bool ReloadMakerController::loadGripRecord(const PartGrip& selected)
    {
        const PartGrip copy=selected;
        _editingMagazine=magazineGrabAnchor(copy.pose.placement)>=0;
        QVector<HandPosePlacement> anchors; for(const auto& anchor:_handPoseAnchors) anchors.append(anchor.description);
        QString error; const int anchor=resolveGripAnchor(copy,poseRecordKey(currentWeapon()->formId),anchors,error);
        if(anchor<0) { setStatus(error,true); return false; }
        if(!copy.mechanismId.isEmpty()) {
            const auto definition=std::find_if(_mechanisms.cbegin(),_mechanisms.cend(),[&](const auto& p){return p.id==copy.mechanismId;});
            if(definition==_mechanisms.cend() || definition->weaponRecord.compare(copy.weaponRecord,Qt::CaseInsensitive)!=0 || !sameMechanismPart(definition->anchor,copy.pose.placement)) {
                setStatus(QStringLiteral("The grip's shared part definition is missing or does not match this part."),true);return false;
            }
        }
        restoreHandPosePreview(); _playing=false; _playbackTimer->stop(); _playButton->setText("PLAY");
        if(!applyPose(_playbackTime)) return false;
        if(!preparePartGripFrame(anchor,error)) { setStatus(error,true); return false; }
        {
            const QSignalBlocker a(_gripPoseCombo),b(_gripReferenceCombo);
            _gripReferenceCombo->setCurrentIndex(anchor);
            _gripPoseCombo->setCurrentIndex(_gripPoseCombo->findData(copy.sourcePoseId));
        }
        _partGripDraft=copy; _gripName->setText(copy.pose.name);
        if(copy.mechanismId.isEmpty() && !assignGripPart(anchor,error)) {
            _partGripDraft.reset();setStatus(error,true);return false;
        }
        // Loading an already-qualified frame is not an edit. Keep exact stored
        // matrices instead of a world -> local floating-point round trip.
        _partGripDraft->pose.placement=copy.pose.placement;
        _partGripDraft->surfaceContact=copy.surfaceContact;
        _showArmsCheck->setChecked(true); showPartGripValues();
        (void)applyPose(_playbackTime); updateHandPoseControls();return _gripPreviewValid;
    }
}
