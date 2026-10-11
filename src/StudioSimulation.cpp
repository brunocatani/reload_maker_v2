// The Operate panel and viewport drivers. They show the simulation and pass
// user input to it; the simulation itself lives in src/areas/operate/.
#include "ReloadMakerController.h"
#include "design/DesignWidgets.h"
#include <QButtonGroup>
#include "StudioWindow.h"
#include "MagazineViewport.h"
#include "ReloadBundle.h"
#include "ReloadDriveViewport.h"
#include "areas/operate/SimulationNames.h"
#include "areas/loadout/SceneTree.h"
#include "domain/presets/WeaponPresets.h"
#include "viewport/PartPicker.h"
#include "glview.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <exception>

namespace reloadmaker {
namespace {
QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setObjectName("Hint");
    return label;
}
}
void ReloadMakerController::createSimulationUi() {
    _simulationDock=new QDockWidget("OPERATE",_window);
    _simulationDock->setObjectName("SimulationDock");
    _simulationDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _simulationDock->setMinimumWidth(300);_simulationDock->setMaximumWidth(400);
    // The Operate inspector follows the canvas Test board: the weapon's state,
    // the actions, and how the magazine is handled. It only operates the
    // weapon; setting it up belongs to the Reload builder (owner, 2026-10-04).
    auto* panel=new QWidget(_simulationDock);auto* layout=new QVBoxLayout(panel);
    layout->setContentsMargins(16,10,16,16);layout->setSpacing(8);
    const auto section=[panel,layout](const QString& title){layout->addSpacing(8);layout->addWidget(design::sectionLabel(title,panel));};
    layout->addWidget(design::sectionLabel("Weapon state",panel));
    _simulationStatus=new QLabel("Preparing native motion…",panel);_simulationStatus->setWordWrap(true);_simulationStatus->setObjectName("StatusCard");
    _simulationStatus->setTextFormat(Qt::RichText);layout->addWidget(_simulationStatus);
    // Square icon tiles, three to a row (owner, 2026-10-05).
    const auto button=[&](design::Icon icon,const QString& text,const QString& action) {
        auto* b=new design::IconTile(icon,text,panel);
        connect(b,&QAbstractButton::clicked,this,[this,action]{if(action!="fire")runSimulationAction(action);});return b;
    };
    section("Actions");
    auto* actions=new QGridLayout;actions->setSpacing(8);_simulationActionGrid=actions;
    _simulationFire=button(design::Icon::Fire,"Fire","fire");_simulationFire->setPrimary(true);
    _simulationFire->setToolTip("Hold to keep the trigger pulled");
    connect(_simulationFire,&QPushButton::pressed,this,[this]{setSimulationTrigger(true);});
    connect(_simulationFire,&QPushButton::released,this,[this]{setSimulationTrigger(false);});
    // Leaving the application releases a held trigger. An unattended run has no one holding it; only commands do.
    if(!qApp->property("unattended").toBool())connect(qGuiApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState state){if(state!=Qt::ApplicationActive){setSimulationTrigger(false);_audio.stop();}});
    _simulationSlideRelease=button(design::Icon::ReleaseForward,"Slide release","slide_release");
    _simulationReload=button(design::Icon::Reload,"Automatic reload","reload");
    _simulationReset=button(design::Icon::Rewind,"Reset","reset");
    _magazineOut=button(design::Icon::MagazineOut,"Magazine out","mag_out");_magazineIn=button(design::Icon::MagazineIn,"Magazine in","mag_in");
    _magazineGrab=button(design::Icon::MagazineGrab,"Grab magazine","grab_magazine");_simulationDrop=button(design::Icon::MagazineDrop,"Drop magazine","drop_magazine");
    _simulationReload->setToolTip("Play the native reload animation.");
    _simulationReset->setToolTip("Start again with a full magazine and the weapon ready.");
    _magazineGrab->setToolTip("Carry the magazine with the mouse and insert it by hand.");
    actions->addWidget(_simulationFire,0,0);actions->addWidget(_simulationSlideRelease,0,1);actions->addWidget(_simulationReload,0,2);
    actions->addWidget(_magazineOut,1,0);actions->addWidget(_magazineIn,1,1);actions->addWidget(_magazineGrab,1,2);
    actions->addWidget(_simulationDrop,2,0);actions->addWidget(_simulationReset,2,1);
    for(int column=0;column<3;++column)actions->setColumnStretch(column,1);
    layout->addLayout(actions);
    layout->addWidget(design::hintLabel("Drag the slide or the magazine in the view to operate them by hand, as in VR.",panel));

    section("Options");
    auto* options=new QGridLayout;options->setSpacing(8);_simulationOptionGrid=options;
    const auto option=[&](design::Icon icon,const QString& text,const QString& tip,bool checked) {
        auto* tile=new design::IconTile(icon,text,panel);tile->setCheckable(true);tile->setChecked(checked);tile->setToolTip(tip);return tile;
    };
    _simulationSpring=option(design::Icon::Spring,"Slide returns","The slide returns when released.",_slideSpringReturn);
    connect(_simulationSpring,&QAbstractButton::toggled,this,[this](bool enabled){setSlideSpringReturn(enabled);});
    _simulationGhost=option(design::Icon::Ghost,"Source pose","Where authored motion differs from the source clip, the part is drawn again, tinted, where the clip has it.",_nativeGhost);
    connect(_simulationGhost,&QAbstractButton::toggled,this,[this](bool enabled){setNativeGhost(enabled);});
    _simulationNoExtender=option(design::Icon::MagazineShort,"NoExtender reload","Use the animation set's optional NoExtender reload. QuickHands sets ship it for the unextended standard magazine.",false);
    connect(_simulationNoExtender,&QAbstractButton::toggled,this,[this](bool enabled){setGlockNoExtender(enabled);rebuildWeapon();});
    options->addWidget(_simulationSpring,0,0);options->addWidget(_simulationGhost,0,1);options->addWidget(_simulationNoExtender,0,2);
    for(int column=0;column<3;++column)options->setColumnStretch(column,1);
    layout->addLayout(options);

    section("Magazine");
    layout->addWidget(design::hintLabel("The reload hand, and the hand fit Grab magazine uses. Fits are set up in Reload builder › Part grips.",panel));
    _interactionHandPicker=new design::TileChoice(panel);
    _interactionHandPicker->addChoice(design::Icon::HandLeft,"Left reload hand","Reload with the left hand; the right hand holds the firing grip.");
    _interactionHandPicker->addChoice(design::Icon::HandRight,"Right reload hand","Reload with the right hand; the left hand holds the support grip.");
    layout->addWidget(_interactionHandPicker);
    connect(_interactionHandPicker->group(),&QButtonGroup::idClicked,this,[this](int i){(void)setInteractionHand(PoseHand(i));updateInteractionGripUi();});
    _simulationMagazineGrip=new QComboBox(panel);layout->addWidget(_simulationMagazineGrip);
    _simulationMagazineGrip->setToolTip("Saved hand grip for this magazine, used by Grab magazine. Create one in Reload builder › Part grips by selecting a pose and clicking its contact point.");
    connect(_simulationMagazineGrip,qOverload<int>(&QComboBox::activated),this,[this](int index) {
        if(!_simulationRig||!_reloadRecipe)return;
        const auto anchor=simulationMagazineAnchor();
        const auto key=handAssignmentKey(magazineGrabKey(poseRecordKey(currentWeapon()->formId),anchor.ownerRecord),_interactionHand);
        const auto id=_simulationMagazineGrip->itemData(index).toString();
        if(index>0)return; // This entry displays the resolved fit, including inherited defaults.
        if(_magazineGrabDefaults.contains(poseRecordKey(currentWeapon()->formId).toLower())) {
            setStatus("This weapon has a default hand fit. Change it in Reload builder › Part grips.");resolveMagazineGrabGrip();return;
        }
        if(id.isEmpty())_magazineGrabGrips.remove(key);else _magazineGrabGrips[key]=id;
        _magazineGripDrafts.remove(key);noteDocumentChange("magazine grip");resolveMagazineGrabGrip();(void)applyPose(_simulation.seconds);updateSimulationControls();
    });

    section("Details");
    _simulationSource=hint({},panel);layout->addWidget(_simulationSource);
    _simulationParts=hint({},panel);layout->addWidget(_simulationParts);
    _simulationInstructions=hint({},panel);layout->addWidget(_simulationInstructions);
    layout->addStretch(1);
    auto* scroll=new QScrollArea(_simulationDock);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(panel);
    _simulationDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_simulationDock);_simulationDock->hide();
    _simulationTimer=new QTimer(this);_simulationTimer->setInterval(16);_simulationTimer->setTimerType(Qt::PreciseTimer);
    connect(_simulationTimer,&QTimer::timeout,this,&ReloadMakerController::tickSimulation);
}
void ReloadMakerController::createSimulationViewport() {
    _simulationDrive=new ReloadDriveViewport(_window->getGLView(),this);
    _simulationDrive->hover=[this](QPointF p){return hoverOperatingPart(p);};
    _simulationDrive->hoverLeave=[this]{_operatingHover=false;};
    _simulationDrive->setInstructions("Grab the slide to rack it · linked parts follow · background drag orbits");
    _simulationDrive->begin=[this](QPointF point,ReloadDriveCurve& curve){return beginSimulationGrab(point,curve);};
    _simulationDrive->drive=[this](float progress){dragSimulation(progress);};
    _simulationDrive->release=[this](bool cancel){releaseSimulation(cancel);};
    // Clicks on weapon parts the drive does not move still select them.
    _simulationDrive->surfaceClicked=[this](QPointF point,bool twice){if(twice)_partPicker->doubleClick(point);else _partPicker->click(point);};
    _magazineViewport=new MagazineViewport(_window->getGLView(),this);
    _magazineViewport->rail=[this]{return _magazineRail;};
    _magazineViewport->state=[this]{return _simulation.magazine;};
    _magazineViewport->freeMove=[this](QVector3D p){moveFreeMagazine(p);};
    _magazineViewport->railMove=[this](float d){moveMagazineRail(d);};
    _magazineViewport->cancel=[this]{cancelMagazine();};
    _magazineViewport->acquire=[this](QPointF point){
        if(!_simulationActive||_simulation.action!=SimulationAction::Idle||!_simulation.magazine.seated())return false;
        int picked=-1;Vec3f hit;QString surface,error;QVector<int> candidates;
        if(!pickWeaponPart(point,picked,hit,surface,candidates,error))return false;
        const int magazine=currentMagazineAnchor();
        if(magazine<0||!std::any_of(candidates.cbegin(),candidates.cend(),[&](int anchor){
            return isWithinBlock(*activeModel(),_handPoseAnchors[anchor].node,_handPoseAnchors[magazine].node);
        }))return false;
        if(!_simulation.grabMountedMagazine())return false;
        updateMagazinePose();return true;
    };
}
namespace {
void flowTiles(QGridLayout* grid,std::initializer_list<QWidget*> tiles) {
    for(auto* tile:tiles)grid->removeWidget(tile);
    int slot=0;
    for(auto* tile:tiles)if(!tile->isHidden()){grid->addWidget(tile,slot/3,slot%3);++slot;}
}
}

void ReloadMakerController::updateSimulationControls() {
    if(_workspace==MagazinesWorkspace)updateMagazineAuthoring();
    if(!_simulationStatus)return;
    _simulationDock->widget()->setEnabled(currentWeapon()!=nullptr);
    updateInteractionGripUi();
    const bool native=_simulation.action==SimulationAction::Fire||_simulation.action==SimulationAction::Reload;
    const bool busy=native||_simulation.magazine.busy();
    _simulationMagazineGrip->setEnabled(_simulationActive&&!busy&&_simulation.action==SimulationAction::Idle&&_simulationRig&&_reloadRecipe&&currentMagazineAnchor()>=0);
    const bool bolt=isBoltActionConfiguration(),ar=isRifleConfiguration()||bolt,automatic=ar&&_simulationAmmo.repeatsWhileHeld();
    // The operating part's name, the fire mode and the insertion path word the
    // instructions (rework 14, item 24).
    const QString part=QString(operatingGroup()).replace('_',' ');
    const QString grabText=magazineInsertionPath()==InsertionPath::NativeCurve?"GRAB: enter the angled line, then move straight along it or use the wheel. The magazine follows its reload motion.":"GRAB: move the mouse, wheel for depth, enter the guide and push in.";
    _simulationInstructions->setText(bolt?"After firing, pull the bolt back to eject, then push it forward to feed and lock. Release holds its position. Magazine: enter the angled line and push straight in.":
        "Drag the "+part+" to rack. "+(!ar?QString():automatic?QString("Hold FIRE for automatic fire. "):QString("FIRE shoots once per press. "))+grabText+" Esc cancels.");
    _simulationSlideRelease->setVisible(!bolt&&!_simulation.openBolt);_simulationSpring->setVisible(!bolt);
    _simulationSlideRelease->setText(_simulation.hkSlap?"Slap handle":ar?"Bolt release":"Slide release");
    if(_simulation.hkSlap)_simulationInstructions->setText("Pull straight back fully to latch the handle in its notch. Release your grip, then Slap handle to spring it closed. In VR, strike the latched handle with your free palm. "+grabText);
    _simulationSpring->setText(isAr15Configuration()?"Handle returns":ar?"Bolt returns":"Slide returns");
    _simulationSpring->setToolTip(isAr15Configuration()?"The charging handle returns when released.":ar?"The bolt returns when released.":"The slide returns when released.");
    _simulationFire->setToolTip(_simulationAmmo.repeatsWhileHeld()?"Press for one shot; hold to keep firing.":"Fire one shot.");
    _simulationDrive->setInstructions(bolt?"Grab the bolt · pull back to lift and open · push forward to close and lock · releasing holds its position":
        ar?"Grab the "+part+" to rack · Bolt release closes the bolt · "+(automatic?"hold Fire for automatic fire":"background drag orbits"):"Grab the slide to rack it · linked parts follow · background drag orbits");
    if(_simulation.hkSlap)_simulationDrive->setInstructions("Pull straight back to latch the handle · release your grip · Slap handle springs it closed");
    if(_simulation.openBolt){_simulationInstructions->setText("Pull the charging handle to cock. The bolt stays back with an empty chamber; FIRE feeds and fires from the magazine. Replace the magazine while cocked, or rack after running empty. "+grabText);_simulationDrive->setInstructions("Open bolt · pull handle to cock · trigger feeds and fires · magazine carries every unfired round");}
    const bool cell=_simulation.cellFed();
    _simulationDrop->setVisible(!cell);_magazineOut->setVisible(!cell);
    if(cell){
        _simulationSlideRelease->setVisible(_simulation.slapClose);_simulationSlideRelease->setText("Slap closed");_simulationSpring->hide();
        _simulationInstructions->setText("Pull the cell lever fully to eject. Insert a fresh cell while open, then close the lever. When enabled, Slap closed also closes it. The reload button does not eject cells.");
        _simulationDrive->setInstructions(_simulation.slapClose?"Pull the lever to eject - insert a cell - grab or slap the lever closed":"Pull the lever to eject - insert a cell - close the lever by hand");
    }
    const bool noExtender=!noExtenderReloadPath().isEmpty(),pistol=simulationMechanism()==WeaponMechanism::PistolSlide;
    _simulationNoExtender->setVisible(noExtender||_glockNoExtender);_simulationNoExtender->setEnabled(!busy&&!_loadThread&&!_reloadCapturing&&noExtender);
    {const QSignalBlocker block(_simulationNoExtender);_simulationNoExtender->setChecked(_glockNoExtender);}
    _simulationSource->setVisible(pistol||ar);
    // Tiles a weapon does not use are hidden; the rest fill rows of three
    // from the left, leaving no holes.
    flowTiles(_simulationActionGrid,{_simulationFire,_simulationSlideRelease,_simulationReload,_magazineOut,_magazineIn,_magazineGrab,_simulationDrop,_simulationReset});
    flowTiles(_simulationOptionGrid,{_simulationSpring,_simulationGhost,_simulationNoExtender});
    if((pistol||ar)&&_reloadRecipe) {
        const auto path=_reloadRecipe->source["animation"].toString();
        _simulationSource->setText("Reload source · "+animationSetRelativePath(path)+" · "+(bolt?"manual bolt action":_simulationAmmo.automatic?"automatic receiver":_simulationAmmo.repeatableSingleFire?"repeating single-fire receiver":"semi-automatic receiver"));
        _simulationSource->setToolTip(path);
    }
    _simulationFire->setEnabled(_simulationActive&&(_simulation.canFire()||(ar&&_simulation.action==SimulationAction::Fire)||_simulation.canDryFire()));
    _simulationSlideRelease->setEnabled(_simulationActive&&_simulation.ammoKnown()&&!busy&&_simulation.action==SimulationAction::Idle&&_simulation.slide>.001f);
    _simulationReload->setEnabled(_simulationActive&&_simulation.ammoKnown()&&!busy&&_simulation.action!=SimulationAction::Held);
    _simulationDrop->setEnabled(_simulationActive&&_simulation.magazineRelease==MagazineRelease::Button&&(!busy||_simulation.magazine.mountedHold)&&_simulation.action==SimulationAction::Idle&&_simulation.magazine.visible());
    const bool magazineReady=_simulationActive&&_magazineRail&&_simulation.action==SimulationAction::Idle&&!_simulation.magazine.busy();
    _magazineOut->setEnabled(magazineReady&&_simulation.magazineRelease==MagazineRelease::Button&&_simulation.magazine.seated());
    _magazineIn->setEnabled(magazineReady&&_simulation.ammoKnown()&&!_simulation.magazine.seated());
    _magazineGrab->setEnabled(magazineReady&&_simulation.ammoKnown());
    _simulationReset->setEnabled(_simulationActive);
    if(!_simulationActive) {
        const QString idle=!currentWeapon()?"Choose a plugin, then a weapon above.":_reloadCapturing?"Learning the native reload…":_clip.valid()&&simulationWorkspace(_workspace)&&_reloadRecipe?"Press any action to operate the weapon.":"Preparing native part motion…";
        _simulationStatus->setText(_simulationError.isEmpty()?idle:_simulationError);_simulationParts->clear();return;
    }
    // Compact state card: the numbers the user acts on, then the phase text.
    const QString slide=_simulation.slideLocked?"<b>locked open</b>":QString::number(qRound(_simulation.slide*100))+"% open";
    _simulationStatus->setText(QString("<span style='font-size:15px;font-weight:600'>%1</span><br>"
        "<table cellspacing=0 cellpadding=1><tr><td>Magazine&nbsp;</td><td><b>%2 / %3</b> %4</td></tr>"
        "<tr><td>Chamber&nbsp;</td><td><b>%5</b></td></tr><tr><td>%9&nbsp;</td><td>%6</td></tr>"
        "<tr><td>Shots&nbsp;</td><td>%7</td></tr></table><span style='color:#91a4ae'>%8</span>")
        .arg(simulationActionName(_simulation.action,ar,bolt)).arg(_simulation.magazineRounds).arg(_simulation.capacity).arg(_simulationAmmo.ammoName.toHtmlEscaped())
        .arg(_simulation.spentCase?"spent case":_simulation.chambered?"loaded":"empty").arg(slide).arg(_simulation.shotsFired)
        .arg(_simulation.magazine.seated()?QString("Magazine seated"):_simulation.magazineDescription().toHtmlEscaped()).arg(ar?"Bolt":"Slide"));
    QStringList names;for(int i:_simulationRig->slideTracks)names.append(_reloadRecipe->tracks[i].anchor.anchorName);
    const QString weaponName=currentWeapon()->name.isEmpty()?currentWeapon()->editorId:currentWeapon()->name;
    const auto* preset=checklistPreset();
    const QString fireText=automatic?QString("Automatic · %1 rounds/min").arg(qRound(60/_rifleFireInterval)):QString("Semi-automatic · one shot per press");
    _simulationParts->setText(bolt?QString("%1 · manual bolt cycle\nLift / pull / push / lock follow the native animation\nCases ejected: %2").arg(weaponName).arg(_simulation.casesEjected):
        ar?weaponName+(preset?" · "+preset->label:QString())+"\n"+fireText+(isAr15Configuration()?QString(" · handle %1%").arg(qRound(_simulation.handle*100)):QString()):"Moving with the slide: "+names.join(" · "));
    if(!_magazineError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine interaction: "+_magazineError);
    if(!_simulationAmmo.valid())_simulationParts->setText(_simulationParts->text()+"\n\nAmmunition: "+_simulationAmmo.error);
    if(_magazineGrabGrip)_simulationParts->setText(_simulationParts->text()+"\n\nMAGAZINE GRAB · "+_magazineGrabGrip->pose.name);
    if(!_magazineGrabError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine hand: "+_magazineGrabError);
}
void ReloadMakerController::updateInteractionGripUi() {
    const bool free=_magazineFitPreview||(_simulation.action==SimulationAction::Idle&&!_simulation.magazine.carried());
    for(auto* picker:{_interactionHandPicker,_magazineHandPicker,_operatingHandPicker})if(picker){const QSignalBlocker block(picker);picker->setCurrentIndex(int(_interactionHand));picker->setEnabled(magazineHandAllowed(PoseHand::Left)&&magazineHandAllowed(PoseHand::Right)&&free&&!_loadThread&&!_reloadCapturing);}
    const auto& fingers=_operatingFingers[int(_interactionHand)];
    if(_operatingGripStatus)_operatingGripStatus->setText(!_operatingFingerError.isEmpty()?_operatingFingerError:fingers?
        "Fingers only · "+fingers->name+(fingers->source["operatingMirrored"].toBool()?" · mirrored for this hand":"")+"\nWrist position and angle stay live.":"No operating finger pose assigned. Select a saved pose below.");
    if(_operatingFingerPicker) {
        const QSignalBlocker block(_operatingFingerPicker);
        _operatingFingerPicker->setCurrentIndex(fingers?_operatingFingerPicker->findData(fingers->id):0);
        _operatingFingerPicker->setEnabled(free&&operatingAnchor()>=0);
    }
    _operatingHover=false;if(_simulationDrive)_simulationDrive->setMarkers({});refreshGrabLocationsUi();
}
// Saved hand poses, then grips assigned as operating fingers, once each.
void ReloadMakerController::showOperatingFingerChoices() {
    if(_operatingFingerPicker) {
        const QSignalBlocker block(_operatingFingerPicker);_operatingFingerPicker->clear();
        _operatingFingerPicker->addItem("Choose operating finger pose…");
        QSet<QString> ids;
        const auto add=[&](const HandPose& p){if(ids.contains(p.id))return;ids.insert(p.id);_operatingFingerPicker->addItem(p.name+(p.hand==PoseHand::Left?" · left":" · right"),p.id);};
        for(const auto& p:_savedHandPoses)add(p);
        for(const auto& g:_partGrips)if(_savedOperatingGrips.values().contains(g.pose.id))add(g.pose);
    }
}
// The magazine grip Grab Magazine uses: the native hand, or the active fit.
void ReloadMakerController::showMagazineGrabGrip(const PartGrip* selected) {
    const QSignalBlocker block(_simulationMagazineGrip);
    _simulationMagazineGrip->clear();_simulationMagazineGrip->addItem("Magazine grip · native hand",QString());
    if(!selected)return;
    _simulationMagazineGrip->addItem(selected->pose.name+" · "+(selected->pose.hand==PoseHand::Left?"Left":"Right"),selected->pose.id);
    _simulationMagazineGrip->setCurrentIndex(1);
}
}
