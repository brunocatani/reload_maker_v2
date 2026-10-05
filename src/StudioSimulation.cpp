// The Operate panel and viewport drivers. They show the simulation and pass
// user input to it; the simulation itself lives in src/areas/operate/.
#include "ReloadMakerController.h"
#include "design/DesignWidgets.h"
#include "StudioWindow.h"
#include "MagazineViewport.h"
#include "ReloadBundle.h"
#include "ReloadDriveViewport.h"
#include "areas/operate/SimulationNames.h"
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
    const auto button=[&](const QString& text,const QString& action) {
        auto* b=new QPushButton(text,panel);b->setMinimumHeight(36);
        connect(b,&QPushButton::clicked,this,[this,action]{if(action!="fire")runSimulationAction(action);});return b;
    };
    section("Actions");
    auto* actions=new QGridLayout;actions->setSpacing(6);
    _simulationFire=button("Fire","fire");_simulationFire->setObjectName("PrimaryButton");
    _simulationFire->setToolTip("Hold to keep the trigger pulled");
    connect(_simulationFire,&QPushButton::pressed,this,[this]{setSimulationTrigger(true);});
    connect(_simulationFire,&QPushButton::released,this,[this]{setSimulationTrigger(false);});
    // Leaving the application releases a held trigger. An unattended run has no one holding it; only commands do.
    if(!qApp->property("unattended").toBool())connect(qGuiApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState state){if(state!=Qt::ApplicationActive){setSimulationTrigger(false);_audio.stop();}});
    _simulationSlideRelease=button("Slide release","slide_release");
    _simulationReload=button("Automatic reload","reload");
    _simulationReset=button("Reset","reset");
    _magazineOut=button("Magazine out","mag_out");_magazineIn=button("Magazine in","mag_in");
    _magazineGrab=button("Grab magazine","grab_magazine");_simulationDrop=button("Drop magazine","drop_magazine");
    actions->addWidget(_simulationFire,0,0);actions->addWidget(_simulationSlideRelease,0,1);
    actions->addWidget(_magazineOut,1,0);actions->addWidget(_magazineIn,1,1);
    actions->addWidget(_magazineGrab,2,0);actions->addWidget(_simulationDrop,2,1);
    actions->addWidget(_simulationReload,3,0);actions->addWidget(_simulationReset,3,1);
    layout->addLayout(actions);
    layout->addWidget(design::hintLabel("Drag the slide or the magazine in the view to operate them by hand, as in VR.",panel));
    _simulationSpring=new QCheckBox("Slide returns when released",panel);_simulationSpring->setChecked(_slideSpringReturn);layout->addWidget(_simulationSpring);
    connect(_simulationSpring,&QCheckBox::toggled,this,[this](bool enabled){setSlideSpringReturn(enabled);});
    _simulationNoExtender=new QCheckBox("NoExtender reload (unextended magazine)",panel);layout->addWidget(_simulationNoExtender);
    _simulationNoExtender->setToolTip("Use the animation set's optional NoExtender reload. QuickHands sets ship it for the unextended standard magazine.");
    connect(_simulationNoExtender,&QCheckBox::toggled,this,[this](bool enabled){setGlockNoExtender(enabled);rebuildWeapon();});

    section("Magazine");
    layout->addWidget(design::hintLabel("The reload hand, and the hand fit Grab magazine uses. Fits are set up in Reload builder › Part grips.",panel));
    _interactionHandPicker=new QComboBox(panel);
    _interactionHandPicker->addItem("Left hand · right firing grip");_interactionHandPicker->addItem("Right hand · left support grip");
    layout->addWidget(_interactionHandPicker);
    connect(_interactionHandPicker,qOverload<int>(&QComboBox::activated),this,[this](int i){(void)setInteractionHand(PoseHand(i));updateInteractionGripUi();});
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
        _magazineGripDrafts.remove(key);noteDocumentChange("magazine grip");resolveMagazineGrabGrip();(void)applyPose(_simulation.seconds);updateSimulationControls();(void)saveSession();
    });

    section("Details");
    _simulationSource=hint({},panel);layout->addWidget(_simulationSource);
    _simulationParts=hint({},panel);layout->addWidget(_simulationParts);
    layout->addStretch(1);
    _simulationInstructions=hint({},panel);layout->addWidget(_simulationInstructions);
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
}
void ReloadMakerController::updateSimulationControls() {
    if(!_simulationStatus)return;
    _simulationDock->widget()->setEnabled(currentWeapon()!=nullptr);
    updateInteractionGripUi();
    const bool native=_simulation.action==SimulationAction::Fire||_simulation.action==SimulationAction::Reload;
    const bool busy=native||_simulation.magazine.busy();
    _simulationMagazineGrip->setEnabled(_simulationActive&&!busy&&_simulation.action==SimulationAction::Idle&&_simulationRig&&_reloadRecipe&&currentMagazineAnchor()>=0);
    const bool bolt=isBoltActionConfiguration(),ar=isRifleConfiguration()||bolt;
    _simulationInstructions->setText(bolt?"After firing, pull the bolt back to eject, then push it forward to feed and lock. Release holds its position. Magazine: enter the angled line and push straight in.":isM1AConfiguration()||isThanatosConfiguration()?"Drag the operating handle to rack. FIRE shoots once per press. GRAB: enter the angled line, then move straight along it or use the wheel. The magazine follows its reload motion. Esc cancels.":ar?"Drag the charging handle to rack. Hold FIRE for automatic fire. GRAB: move the mouse, wheel for depth, enter the guide and push in. Esc cancels.":"Drag the slide to rack. GRAB: move the mouse, wheel for depth, enter the guide and push in. Esc cancels.");
    _simulationSlideRelease->setVisible(!bolt);_simulationSpring->setVisible(!bolt);
    _simulationSlideRelease->setText(ar?"Bolt release":"Slide release");
    _simulationSpring->setText(isAr15Configuration()?"Charging handle returns when released":ar?"Bolt returns when released":"Slide returns when released");
    _simulationFire->setToolTip(ar&&_simulationAmmo.automatic?"Press for one shot; hold for automatic fire.":"Fire one shot.");
    _simulationDrive->setInstructions(bolt?"Grab the bolt · pull back to lift and open · push forward to close and lock · releasing holds its position":isM1AConfiguration()||isThanatosConfiguration()?"Pull the operating handle to rack · Bolt release closes the action · background drag orbits":ar?"Grab the charging handle to rack · Bolt release closes the bolt · hold Fire for automatic fire":"Grab the slide to rack it · linked parts follow · background drag orbits");
    const bool noExtender=!noExtenderReloadPath().isEmpty(),pistol=simulationMechanism()==WeaponMechanism::PistolSlide;
    _simulationNoExtender->setVisible(noExtender||_glockNoExtender);_simulationNoExtender->setEnabled(!busy&&!_loadThread&&!_reloadCapturing&&noExtender);
    {const QSignalBlocker block(_simulationNoExtender);_simulationNoExtender->setChecked(_glockNoExtender);}
    _simulationSource->setVisible(pistol||ar);
    if((pistol||ar)&&_reloadRecipe) {
        const auto path=_reloadRecipe->source["animation"].toString();
        const auto relative=path.section(isL118Configuration()?"l118/":bolt?"c14timb/":isThanatosConfiguration()?"diam107/":isM1AConfiguration()?"m1aanims/":isMk18Configuration()?"mk18anims/":isSrepConfiguration()?"srep/":ar?"ru556anims/":"glock19xanims/",1);
        _simulationSource->setText("Reload source · "+relative+" · "+(bolt?"manual bolt action":_simulationAmmo.automatic?"automatic receiver":"semi-automatic receiver"));
        _simulationSource->setToolTip(path);
    }
    _simulationFire->setEnabled(_simulationActive&&(_simulation.canFire()||(ar&&_simulation.action==SimulationAction::Fire)||_simulation.canDryFire()));
    _simulationSlideRelease->setEnabled(_simulationActive&&_simulation.ammoKnown()&&!busy&&_simulation.action==SimulationAction::Idle&&_simulation.slide>.001f);
    _simulationReload->setEnabled(_simulationActive&&_simulation.ammoKnown()&&!busy&&_simulation.action!=SimulationAction::Held);
    _simulationDrop->setEnabled(_simulationActive&&!busy&&_simulation.action!=SimulationAction::Held&&_simulation.magazine.visible());
    const bool magazineReady=_simulationActive&&_magazineRail&&_simulation.action==SimulationAction::Idle&&!_simulation.magazine.busy();
    _magazineOut->setEnabled(magazineReady&&_simulation.magazine.seated());
    _magazineIn->setEnabled(magazineReady&&_simulation.ammoKnown()&&!_simulation.magazine.seated());
    _magazineGrab->setEnabled(magazineReady&&_simulation.ammoKnown()&&!_simulation.magazine.seated());
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
    _simulationParts->setText(bolt?QString("%1 · manual bolt cycle\nLift / pull / push / lock follow the native animation\nCases ejected: %2").arg(isL118Configuration()?"L118A2":"C14 Timberwolf").arg(_simulation.casesEjected):isM1AConfiguration()?QString("M1A · reciprocating operating handle and rotating bolt\nSemi-automatic · one shot per press"):
        isThanatosConfiguration()?QString("Thanatos · reciprocating bolt\nSemi-automatic · one shot per press"):ar?QString("AR-15 · independent bolt and charging handle\nHandle %1% · automatic cadence %2 rounds/min").arg(qRound(_simulation.handle*100)).arg(qRound(60/_rifleFireInterval)):"Moving with the slide: "+names.join(" · "));
    if(!_magazineError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine interaction: "+_magazineError);
    if(!_simulationAmmo.valid())_simulationParts->setText(_simulationParts->text()+"\n\nAmmunition: "+_simulationAmmo.error);
    if(_magazineGrabGrip)_simulationParts->setText(_simulationParts->text()+"\n\nMAGAZINE GRAB · "+_magazineGrabGrip->pose.name);
    if(!_magazineGrabError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine hand: "+_magazineGrabError);
}
void ReloadMakerController::updateInteractionGripUi() {
    const bool free=_simulation.action==SimulationAction::Idle&&!_simulation.magazine.carried();
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
