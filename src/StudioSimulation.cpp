#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "MagazineViewport.h"
#include "ReloadBundle.h"
#include "glview.h"
#include "gl/glscene.h"
#include "model/nifmodel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QSet>
#include <algorithm>

namespace reloadmaker {
namespace {
QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setObjectName("Hint");
    return label;
}
QString actionName(SimulationAction action) {
    switch(action) {
    case SimulationAction::Fire:return "Firing";
    case SimulationAction::Reload:return "Automatic reload";
    case SimulationAction::Held:return "Slide held";
    case SimulationAction::Returning:return "Slide returning";
    default:return "Ready";
    }
}
}
void ReloadMakerController::createSimulationUi() {
    _simulationDock=new QDockWidget("OPERATE",_window);
    _simulationDock->setObjectName("SimulationDock");
    _simulationDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _simulationDock->setMinimumWidth(300);_simulationDock->setMaximumWidth(400);
    auto* panel=new QWidget(_simulationDock);auto* layout=new QVBoxLayout(panel);
    layout->setContentsMargins(14,14,14,14);layout->setSpacing(10);
    _simulationStatus=new QLabel("Preparing native motion…",panel);_simulationStatus->setWordWrap(true);_simulationStatus->setObjectName("StatusCard");
    _simulationStatus->setTextFormat(Qt::RichText);layout->addWidget(_simulationStatus);
    const auto button=[&](const QString& text,const QString& action) {
        auto* b=new QPushButton(text,panel);b->setMinimumHeight(36);
        connect(b,&QPushButton::clicked,this,[this,action]{runSimulationAction(action);});return b;
    };
    // Primary weapon actions first, then magazine handling: everything used
    // repeatedly stays above the fold.
    auto* actions=new QGridLayout;actions->setSpacing(6);
    _simulationFire=button("FIRE","fire");_simulationFire->setObjectName("PrimaryButton");
    _simulationSlideRelease=button("SLIDE RELEASE","slide_release");
    _simulationReload=button("AUTOMATIC RELOAD","reload");
    _simulationReset=button("RESET","reset");
    actions->addWidget(_simulationFire,0,0);actions->addWidget(_simulationSlideRelease,0,1);
    actions->addWidget(_simulationReload,1,0);actions->addWidget(_simulationReset,1,1);
    layout->addLayout(actions);
    auto* magazineTitle=new QLabel("MAGAZINE",panel);magazineTitle->setObjectName("Section");layout->addWidget(magazineTitle);
    auto* magazine=new QGridLayout;magazine->setSpacing(6);
    _magazineOut=button("MAG OUT","mag_out");_magazineIn=button("MAG IN","mag_in");
    _magazineGrab=button("GRAB","grab_magazine");_simulationDrop=button("DROP","drop_magazine");
    magazine->addWidget(_magazineOut,0,0);magazine->addWidget(_magazineIn,0,1);
    magazine->addWidget(_magazineGrab,1,0);magazine->addWidget(_simulationDrop,1,1);
    layout->addLayout(magazine);
    _simulationMagazineGrip=new QComboBox(panel);layout->addWidget(_simulationMagazineGrip);
    auto* configureMagazine=new QPushButton("CONFIGURE SELECTED MAGAZINE",panel);layout->addWidget(configureMagazine);
    configureMagazine->setToolTip("Edit this magazine's hand fit with Move / Rotate. Save replaces its assignment and saves the project.");
    connect(configureMagazine,&QPushButton::clicked,this,[this]{(void)editMagazineGrip();});
    _simulationMagazineGrip->setToolTip("Saved hand grip for this magazine, used by GRAB. Create one in Hands & grips by selecting a pose and clicking its contact point.");
    connect(_simulationMagazineGrip,qOverload<int>(&QComboBox::activated),this,[this](int index) {
        if(!_simulationRig||!_reloadRecipe)return;
        const auto& anchor=_reloadRecipe->tracks[_simulationRig->magazine].anchor;
        const auto key=magazineGrabKey(poseRecordKey(currentWeapon()->formId),anchor.ownerRecord);
        const auto previous=_magazineGrabGrips.value(key),id=_simulationMagazineGrip->itemData(index).toString();
        if(index>0)return; // This entry displays the resolved fit, including inherited defaults.
        if(_magazineGrabDefaults.contains(poseRecordKey(currentWeapon()->formId).toLower())) {
            setStatus("This weapon has a default hand fit. Configure the selected magazine to change it.");resolveMagazineGrabGrip();return;
        }
        if(id.isEmpty())_magazineGrabGrips.remove(key);else _magazineGrabGrips[key]=id;
        for(auto& window:_reloadRecipe->windows)for(auto& member:window.members)
            if(member.track==_simulationRig->magazine&&(member.gripId.isEmpty()||member.gripId==previous))member.gripId=id;
        _magazineGripDrafts.remove(key);resolveMagazineGrabGrip();(void)applyPose(_simulation.seconds);updateSimulationControls();(void)saveSession();
    });
    auto* optionsTitle=new QLabel("BEHAVIOUR",panel);optionsTitle->setObjectName("Section");layout->addWidget(optionsTitle);
    _simulationSpring=new QCheckBox("Slide returns when released",panel);_simulationSpring->setChecked(true);layout->addWidget(_simulationSpring);
    _simulationNoExtender=new QCheckBox("QuickHands: standard magazine grip",panel);layout->addWidget(_simulationNoExtender);
    _simulationNoExtender->setToolTip("Use the package's optional NoExtender reload with the unextended standard magazine and a QuickHands animation set.");
    connect(_simulationNoExtender,&QCheckBox::toggled,this,[this](bool enabled){_glockNoExtender=enabled;rebuildWeapon();});
    auto* authorGrips=new QPushButton("AUTHOR RELOAD GRIPS",panel);layout->addWidget(authorGrips);
    authorGrips->setToolTip("Pause at this reload moment and open the hand editor with the weapon fixed and the magazine selected.");
    connect(authorGrips,&QPushButton::clicked,this,[this]{(void)authorReloadGrips();});
    _simulationSource=hint({},panel);layout->addWidget(_simulationSource);
    _simulationParts=hint({},panel);layout->addWidget(_simulationParts);
    layout->addStretch(1);
    layout->addWidget(hint("Drag the slide in the viewport to rack it. GRAB: move the mouse, wheel for depth, guide it into the ring and push in. Esc cancels.",panel));
    auto* scroll=new QScrollArea(_simulationDock);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(panel);
    _simulationDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_simulationDock);_simulationDock->hide();
    _simulationTimer=new QTimer(this);_simulationTimer->setInterval(16);_simulationTimer->setTimerType(Qt::PreciseTimer);
    connect(_simulationTimer,&QTimer::timeout,this,&ReloadMakerController::tickSimulation);
}
bool ReloadMakerController::isGlockConfiguration()const {
    const auto* weapon=currentWeapon();
    return weapon&&weapon->editorId.compare("WPNGlock19x",Qt::CaseInsensitive)==0;
}
QString ReloadMakerController::glockNoExtenderPath()const {
    if(!isGlockConfiguration())return {};
    bool standard=false;
    for(const auto* mod:configuredObjectMods())if(normalizeNifPath(mod->modelPath).endsWith("/glock19x/glock19x_mag.nif",Qt::CaseInsensitive))standard=true;
    const auto native=resolveAnimationPath("WPNReload.hkx");
    if(!standard||!native.endsWith("/glock19xanims/quickhands/wpnreload.hkx",Qt::CaseInsensitive))return {};
    return resolveAnimationPath("WPNReloadNoExtender.hkx");
}
void ReloadMakerController::createSimulationViewport() {
    _simulationDrive=new ReloadDriveViewport(_window->getGLView(),this);
    _simulationDrive->setInstructions("SIMULATE · grab the slide to rack it · linked parts follow · background drag orbits");
    _simulationDrive->begin=[this](QPointF point,ReloadDriveCurve& curve){return beginSimulationGrab(point,curve);};
    _simulationDrive->drive=[this](float progress){dragSimulation(progress);};
    _simulationDrive->release=[this](bool cancel){releaseSimulation(cancel);};
    _magazineViewport=new MagazineViewport(_window->getGLView(),this);
    _magazineViewport->rail=[this]{return _magazineRail;};
    _magazineViewport->state=[this]{return _simulation.magazine;};
    _magazineViewport->freeMove=[this](QVector3D p){moveFreeMagazine(p);};
    _magazineViewport->railMove=[this](float d){moveMagazineRail(d);};
    _magazineViewport->cancel=[this]{cancelMagazine();};
}
bool ReloadMakerController::selectSimulationClip(bool fire,bool reload) {
    int index=0;
    if(fire||reload) {
        index=-1;
        // The configured receiver decides automatic vs single; the catalog lists
        // the canonical single clip before its alternates.
        for(int i=0;i<_animationActions.size();++i) {
            const auto& a=_animationActions[i];
            if((reload&&a.baseReload)||(fire&&a.family==ActionFamily::Fire&&a.automatic==_simulationAmmo.automatic)){index=i;break;}
        }
    }
    if(index<0||index>=_animationList->count()){_simulationError=fire?(_simulationAmmo.automatic?"No resolved automatic-fire animation for this receiver.":"No resolved single-fire animation."):"No resolved reload animation.";return false;}
    const QSignalBlocker blocker(_animationList);_animationList->setCurrentRow(index);selectAnimation(_animationList->item(index));
    return _clip.valid();
}
void ReloadMakerController::prepareSimulation() {
    if(_workspace!=5||_loadThread||_reloadCapturing||!_reloadRecipe){updateSimulationControls();return;}
    if(_simulationActive)return;
    SimulationRig rig;_simulationError.clear();
    if(!buildSimulationRig(*_reloadRecipe,rig,_simulationError)){updateSimulationControls();return;}
    _simulationRig=rig;_simulationVisibility.clear();
    const auto& owner=_reloadRecipe->tracks[rig.magazine].anchor;
    const auto* config=configuration();auto* model=activeModel();
    if(config&&model)for(auto it=_partRoots.cbegin();it!=_partRoots.cend();++it) {
        if(poseRecordKey(config->selected(it.key())).compare(owner.ownerRecord,Qt::CaseInsensitive)==0 && it.value().isValid())
            _simulationVisibility.append({it.value(),model->get<int>(it.value(),"Flags")});
    }
    if(_simulationVisibility.isEmpty()) {
        _simulationError="The complete magazine assembly could not be resolved; simulation is unavailable.";
        _simulationRig.reset();updateSimulationControls();return;
    }
    _simulationAmmo=resolveWeaponAmmunition(*currentPlugin(),*currentWeapon(),*config,_referenceNames);
    _glockReloadRackAt=-1;_glockReloadFeedAt=-1;
    if(isGlockConfiguration()) {
        const auto& motion=_reloadRecipe->tracks[rig.slide].poses;
        const auto& rest=motion[rig.closedFrame];const float travel=reloadPoseDistance(rest,motion[rig.openFrame]);
        for(int frame=0;frame<motion.size();++frame)if(reloadPoseDistance(rest,motion[frame])>=travel*.95f) {
            _glockReloadRackAt=_reloadRecipe->duration*frame/(_reloadRecipe->frames-1);break;
        }
        for(const auto& event:_reloadClip.annotations)if(event.text.compare("reloadComplete",Qt::CaseInsensitive)==0&&event.timeSeconds>=_glockReloadRackAt) {
            _glockReloadFeedAt=event.timeSeconds;break;
        }
        if(_glockReloadFeedAt<0){_simulationError="The Glock reload has no completion marker after its slide cycle.";updateSimulationControls();return;}
    }
    _simulation.configureAmmunition(_simulationAmmo.valid()?_simulationAmmo.capacity:0,_simulationAmmo.pistol);
    _simulationActive=true;
    _reloadPreviewTimer->stop();_reloadPlaying=false;_reloadHeld=false;_reloadDrive->setActive(false,false);
    if(!selectSimulationClip(false,false)){stopSimulation();updateSimulationControls();return;}
    prepareMagazineInteraction();
    resolveMagazineGrabGrip();
    (void)applyPose(0);
    _magazineViewport->setActive(true);
    _simulationDrive->clearCurve();_simulationDrive->setActive(true,true);updateSimulationControls();++_revision;
}
void ReloadMakerController::stopSimulation() {
    _magazineFitPreview=false;
    if(_simulationTimer)_simulationTimer->stop();
    _simulationActive=false;_simulation.reset();
    _magazineGrabGrip.reset();_magazineGrabBinding=-1;_magazineGrabError.clear();
    if(_magazineViewport)_magazineViewport->setActive(false);
    if(_simulationDrive){_simulationDrive->setActive(false,false);_simulationDrive->clearCurve();}
    applySimulationVisibility();_simulationVisibility.clear();_magazineVisuals.clear();_magazineRail.reset();
}
void ReloadMakerController::applySimulationVisibility() {
    auto* model=activeModel();auto* scene=_window->getGLView()->getScene();if(!model||!scene)return;
    const QSignalBlocker blocker(model);
    for(const auto& [index,flags]:_simulationVisibility)if(index.isValid()) {
        const int desired=(_simulationActive&&!_simulation.magazine.visible())?flags|1:flags;
        if(model->get<int>(index,"Flags")!=desired) {
            model->set<int>(index,"Flags",desired);
            if(auto* node=scene->getNode(model,index))node->update(model,index);
        }
    }
    const bool authored=_simulation.action==SimulationAction::Reload;
    for(const auto& visual:_magazineVisuals)if(visual.node.isValid()) {
        const bool cartridge=_simulationRig&&!visual.empty&&visual.track!=_simulationRig->magazine;
        const bool noRound=_simulation.magazine.seated()?(!_simulation.chambered&&_simulation.magazineRounds==0):_simulation.magazineRounds==0;
        bool hide=_simulationActive&&!authored&&(cartridge?noRound:(!_magazineRail||!_magazineRail->sharedBody())&&(visual.empty!=(_simulation.magazineRounds==0)));
        if(_simulationActive&&authored&&_reloadRecipe) {
            // Evaluate source visibility afresh at the current time so reverse
            // sampling and action switches never retain an earlier cull event.
            for(int track=visual.track;track>=0;track=_reloadRecipe->tracks[track].parent) {
                bool culled=false;const auto name=_reloadRecipe->tracks[track].anchor.anchorName;
                for(const auto& event:_clip.annotations)if(event.timeSeconds<=_simulation.seconds) {
                    if(event.text.compare("cullbone."+name,Qt::CaseInsensitive)==0)culled=true;
                    if(event.text.compare("uncullbone."+name,Qt::CaseInsensitive)==0)culled=false;
                }
                hide|=culled;
            }
        }
        const int desired=hide?visual.flags|1:visual.flags;
        if(model->get<int>(visual.node,"Flags")!=desired){model->set<int>(visual.node,"Flags",desired);if(auto* node=scene->getNode(model,visual.node))node->update(model,visual.node);}
    }
    _window->getGLView()->update();
}
bool ReloadMakerController::applySimulationPose() {
    if(!_simulationActive||!_simulationRig||!_reloadRecipe)return true;
    const bool holdOpen=_simulation.action==SimulationAction::Reload&&_simulation.reloadStartedOpen&&!_simulation.reloadCycled;
    if(_simulation.action==SimulationAction::Reload&&!holdOpen)return true;
    QVector<Transformf> local,world;
    if(!nativeMechanismScene(_reloadScenePlan,_nodeBindings,_boundLocalPose,local,world,_poseError))return false;
    QHash<int,Transformf> targets;
    if(_simulation.action!=SimulationAction::Fire||_simulation.willLockAfterShot())for(int track:_simulationRig->slideTracks) {
        if(holdOpen&&_simulationRig->magazineTracks.contains(track))continue;
        targets.insert(_reloadTrackNodes[track],simulationTrackPose(*_reloadRecipe,*_simulationRig,track,holdOpen||_simulation.willLockAfterShot()?1.f:_simulation.slide));
    }
    // A newly seated magazine has a top round, not a magically loaded chamber.
    // The source feed curve takes over only during a completed rack/return cycle.
    if(_simulation.action!=SimulationAction::Fire&&_simulation.action!=SimulationAction::Reload&&!_simulation.chambered&&!_simulation.feedCycleActive())
        for(const auto& visual:_magazineVisuals)if(!visual.empty&&visual.track>=0&&visual.track!=_simulationRig->magazine)
            targets.insert(_reloadTrackNodes[visual.track],_reloadRecipe->tracks[visual.track].poses.front());
    QSet<int> fixedMagazine;
    if(_magazineRail&&!_simulation.magazine.seated())for(int track:_simulationRig->magazineTracks) {
        auto pose=_reloadRecipe->tracks[track].poses.front();const auto offset=_simulation.magazine.offset;
        pose.translation.x+=offset.x();pose.translation.y+=offset.y();pose.translation.z+=offset.z();
        targets.insert(_reloadTrackNodes[track],pose);fixedMagazine.insert(_reloadTrackNodes[track]);
    }
    const auto weapon=_globalPose[_weaponBoneIndex];
    for(int node:_reloadScenePlan.order) {
        const int parent=_assemblyNodes[node].parent;
        if(targets.contains(node)) {
            const auto target=fixedMagazine.contains(node)?targets[node]:composed(weapon,targets[node]);Transformf inverse;
            if(parent>=0&&!invertUniformTransform(world[parent],inverse)){_poseError="Simulation parent is not invertible.";return false;}
            local[node]=parent>=0?composed(inverse,target):target;world[node]=target;
        }else world[node]=parent>=0?composed(world[parent],local[node]):local[node];
    }
    for(int i=0;i<_nodeBindings.size();++i){const int node=_nodeBindings[i].nodeIndex;_boundLocalPose[i]=local[node];_boundGlobalPose[i]=world[node];}
    return true;
}
void ReloadMakerController::runSimulationAction(const QString& action) {
    if(!_simulationActive&&_workspace==OperateWorkspace)prepareSimulation();
    if(!_simulationActive){setStatus(_simulationError.isEmpty()?"Wait for the simulation source to be ready.":_simulationError,true);return;}
    _simulationError.clear();bool accepted=false;
    if(action=="reset") {_simulation.reset();_simulationTimer->stop();_simulationDrive->clearCurve();accepted=selectSimulationClip(false,false);}
    else if(action=="drop_magazine")accepted=_simulation.dropMagazine();
    else if(action=="slide_release") {
        accepted=_simulation.slideRelease();
        if(accepted){_simulationClock.restart();_simulationTimer->start();}
    }
    else if(action=="mag_out"||action=="mag_in"||action=="grab_magazine") {
        if(!_magazineRail){setStatus(_magazineError,true);return;}
        if(_simulation.action==SimulationAction::Idle) {
            if(action=="mag_out")accepted=_simulation.magazineOut();
            else if(action=="mag_in")accepted=_simulation.magazineIn(*_magazineRail);
            else {accepted=_simulation.grabMagazine(*_magazineRail);if(accepted){if(_magazineGrabGrip)_showArmsCheck->setChecked(true);_magazineViewport->beginCarry();}}
        }
        if(accepted&&_simulation.magazine.animated()){_simulationClock.restart();_simulationTimer->start();}
    }
    else if(action=="fire"||action=="reload") {
        const auto previous=_simulation;
        accepted=action=="fire"?_simulation.fire():_simulation.reload(_glockReloadRackAt,_glockReloadFeedAt);
        if(accepted&&!selectSimulationClip(action=="fire",action=="reload")){_simulation=previous;accepted=false;}
        if(accepted){_simulationDrive->clearCurve();_simulationClock.restart();_simulationTimer->start();}
    }
    if(!accepted){setStatus(!_simulationAmmo.valid()?_simulationAmmo.error:action=="fire"&&!_simulation.chambered?"Chamber empty. Insert a magazine and cycle or release the slide.":"Finish the current interaction; the slide must be closed and chamber loaded to fire.",true);return;}
    if(!applyPose(_simulation.seconds)){_simulationError=_lastError;stopSimulation();updateSimulationControls();return;}
    applySimulationVisibility();updateSimulationControls();++_revision;
    setStatus(action=="drop_magazine"?"Magazine assembly removed. Rack the slide, or use Automatic reload to restore it.":actionName(_simulation.action));
}
void ReloadMakerController::tickSimulation() {
    if(!_simulationActive||_workspace!=5){_simulationTimer->stop();return;}
    const float delta=float(_simulationClock.nsecsElapsed())/1e9f;_simulationClock.restart();
    const bool finished=_simulation.tick(delta,_clip.durationSeconds);
    if(_magazineRail)_simulation.magazine.tick(*_magazineRail,delta);
    if(finished&&!selectSimulationClip(false,false)){stopSimulation();updateSimulationControls();return;}
    if(!applyPose(_simulation.seconds)){stopSimulation();updateSimulationControls();return;}
    _playbackTime=_simulation.seconds;applySimulationVisibility();_simulationDrive->setTime(_simulation.slide);
    if(_simulation.action==SimulationAction::Idle&&!_simulation.magazine.animated())_simulationTimer->stop();
    updateSimulationControls();
}
bool ReloadMakerController::beginSimulationGrab(QPointF point,ReloadDriveCurve& curve) {
    if(!_simulationActive||!_simulationRig||!_reloadRecipe)return false;
    int automatic=-1;Vec3f hit;QString surface,error;QVector<int> candidates;
    if(!pickWeaponPart(point,automatic,hit,surface,candidates,error))return false;
    const auto& rig=*_simulationRig;const auto& track=_reloadRecipe->tracks[rig.slide];
    const bool slide=std::any_of(candidates.cbegin(),candidates.cend(),[&](int i){return sameMechanismPart(_handPoseAnchors[i].description,track.anchor);});
    if(!slide||!_simulation.grab())return true;
    _simulationTimer->stop();
    Transformf world,inverse;
    if(!sceneNodeWorld(_sceneNodes[_reloadTrackNodes[rig.slide]],world,error)||!invertUniformTransform(world,inverse)){_simulation.release(true,false);setStatus(error,true);return true;}
    const auto contact=composed(inverse,Transformf{hit}).translation;
    curve.current=_simulation.slide;
    const int samples=std::max(16,rig.closedFrame-rig.openFrame);
    for(int i=0;i<=samples;++i) {
        const float progress=float(i)/samples;
        const auto p=composed(simulationTrackPose(*_reloadRecipe,rig,rig.slide,progress),Transformf{contact}).translation;
        curve.points.append({p.x,p.y,p.z});curve.times.append(progress);
    }
    // Gesture travel beyond the catch lets a locked-open slide be pulled and
    // released. Geometry remains clamped to the verified source stroke.
    curve.points.append(curve.points.back()+(curve.points.back()-curve.points.front())*.08f);curve.times.append(1.08f);
    updateSimulationControls();setStatus("Slide grabbed · drag along its travel · Esc cancels");++_revision;return true;
}
void ReloadMakerController::dragSimulation(float progress) {
    if(!_simulationActive)return;
    _simulation.drag(progress);
    if(!applyPose(0)){_simulationError=_lastError;stopSimulation();}
    applySimulationVisibility();updateSimulationControls();
}
void ReloadMakerController::releaseSimulation(bool cancel) {
    if(!_simulationActive)return;
    _simulation.release(cancel,_simulationSpring->isChecked());
    if(_simulation.action==SimulationAction::Returning){_simulationClock.restart();_simulationTimer->start();}
    if(!applyPose(0)){_simulationError=_lastError;stopSimulation();}
    updateSimulationControls();++_revision;
}
void ReloadMakerController::updateSimulationControls() {
    if(!_simulationStatus)return;
    const bool native=_simulation.action==SimulationAction::Fire||_simulation.action==SimulationAction::Reload;
    const bool busy=native||_simulation.magazine.busy();
    _simulationMagazineGrip->setEnabled(_simulationActive&&!busy&&_simulation.action==SimulationAction::Idle&&_simulationRig&&_reloadRecipe&&magazineGrabSupported(_reloadRecipe->tracks[_simulationRig->magazine].anchor));
    const bool glock=isGlockConfiguration();
    _simulationNoExtender->setVisible(glock);_simulationNoExtender->setEnabled(!busy&&!_loadThread&&!_reloadCapturing&&!glockNoExtenderPath().isEmpty());
    {const QSignalBlocker block(_simulationNoExtender);_simulationNoExtender->setChecked(_glockNoExtender);}
    _simulationSource->setVisible(glock);
    if(glock&&_reloadRecipe) {
        const auto path=_reloadRecipe->source["animation"].toString();
        const auto relative=path.section("glock19xanims/",1);
        _simulationSource->setText("Reload source · "+relative+" · "+(_simulationAmmo.automatic?"automatic receiver":"semi-automatic receiver"));
        _simulationSource->setToolTip(path);
    }
    _simulationFire->setEnabled(_simulationActive&&_simulation.canFire());
    _simulationSlideRelease->setEnabled(_simulationActive&&_simulation.ammoKnown()&&!busy&&_simulation.action==SimulationAction::Idle&&_simulation.slide>.001f);
    _simulationReload->setEnabled(_simulationActive&&_simulation.ammoKnown()&&!busy&&_simulation.action!=SimulationAction::Held);
    _simulationDrop->setEnabled(_simulationActive&&!busy&&_simulation.action!=SimulationAction::Held&&_simulation.magazine.visible());
    const bool magazineReady=_simulationActive&&_magazineRail&&_simulation.action==SimulationAction::Idle&&!_simulation.magazine.busy();
    _magazineOut->setEnabled(magazineReady&&_simulation.magazine.seated());
    _magazineIn->setEnabled(magazineReady&&_simulation.ammoKnown()&&!_simulation.magazine.seated());
    _magazineGrab->setEnabled(magazineReady&&_simulation.ammoKnown()&&!_simulation.magazine.seated());
    _simulationReset->setEnabled(_simulationActive);
    if(!_simulationActive) {
        const QString idle=_reloadCapturing?"Learning the native reload…":_clip.valid()&&_workspace==OperateWorkspace&&_reloadRecipe?"Press any action to operate the weapon.":"Preparing native part motion…";
        _simulationStatus->setText(_simulationError.isEmpty()?idle:_simulationError);_simulationParts->clear();return;
    }
    // Compact state card: the numbers the user acts on, then the phase text.
    const QString slide=_simulation.slideLocked?"<b>locked open</b>":QString::number(qRound(_simulation.slide*100))+"% open";
    _simulationStatus->setText(QString("<span style='font-size:15px;font-weight:600'>%1</span><br>"
        "<table cellspacing=0 cellpadding=1><tr><td>Magazine&nbsp;</td><td><b>%2 / %3</b> %4</td></tr>"
        "<tr><td>Chamber&nbsp;</td><td><b>%5</b></td></tr><tr><td>Slide&nbsp;</td><td>%6</td></tr>"
        "<tr><td>Shots&nbsp;</td><td>%7</td></tr></table><span style='color:#91a4ae'>%8</span>")
        .arg(actionName(_simulation.action)).arg(_simulation.magazineRounds).arg(_simulation.capacity).arg(_simulationAmmo.ammoName.toHtmlEscaped())
        .arg(_simulation.chambered?"loaded":"empty").arg(slide).arg(_simulation.shotsFired)
        .arg(_simulation.magazine.seated()?QString("Magazine seated"):_simulation.magazineDescription().toHtmlEscaped()));
    QStringList names;for(int i:_simulationRig->slideTracks)names.append(_reloadRecipe->tracks[i].anchor.anchorName);
    _simulationParts->setText("Moving with the slide: "+names.join(" · "));
    if(!_magazineError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine interaction: "+_magazineError);
    if(!_simulationAmmo.valid())_simulationParts->setText(_simulationParts->text()+"\n\nAmmunition: "+_simulationAmmo.error);
    if(_magazineGrabGrip)_simulationParts->setText(_simulationParts->text()+"\n\nMAGAZINE GRAB · "+_magazineGrabGrip->pose.name);
    if(!_magazineGrabError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine hand: "+_magazineGrabError);
}
QJsonObject ReloadMakerController::simulationState() const {
    QJsonObject result{{"active",_simulationActive},{"action",actionName(_simulation.action)},{"magazinePresent",_simulation.magazinePresent()},{"slide",_simulation.slide},{"seconds",_simulation.seconds},{"error",_simulationError}};
    result["magazineGrip"]=QJsonObject{{"id",_magazineGrabGrip?_magazineGrabGrip->pose.id:QString()},
        {"name",_magazineGrabGrip?_magazineGrabGrip->pose.name:QString()},{"applied",_magazineGrabGrip&&_simulation.magazine.carried()&&_magazineGrabError.isEmpty()},{"error",_magazineGrabError}};
    result["magazine"]=QJsonObject{{"phase",_simulation.magazineDescription()},{"empty",_simulation.magazineRounds==0},
        {"offset",QJsonArray{_simulation.magazine.offset.x(),_simulation.magazine.offset.y(),_simulation.magazine.offset.z()}},{"distance",_simulation.magazine.distance},{"error",_magazineError}};
    if(_magazineRail)result["magazineRail"]=magazineRailJson(*_magazineRail);
    auto ammo=_simulationAmmo.json();ammo["magazineRounds"]=_simulation.magazineRounds;ammo["chambered"]=_simulation.chambered;ammo["slideLocked"]=_simulation.slideLocked;
    ammo["shotsFired"]=_simulation.shotsFired;ammo["roundsEjected"]=_simulation.roundsEjected;ammo["canFire"]=_simulation.canFire();result["ammunition"]=ammo;
    if(isGlockConfiguration())result["reloadProfile"]=QJsonObject{{"animation",_reloadRecipe?_reloadRecipe->source["animation"].toString():QString()},
        {"rackSeconds",_glockReloadRackAt},{"feedSeconds",_glockReloadFeedAt},{"noExtender",_glockNoExtender},{"noExtenderAvailable",!glockNoExtenderPath().isEmpty()},
        {"startedLoaded",_simulation.reloadStartedLoaded},{"startedOpen",_simulation.reloadStartedOpen},{"cycled",_simulation.reloadCycled},{"fed",_simulation.reloadFed},
        {"fireAction",_simulationAmmo.automatic?"Fire — automatic":"Fire — single"}};
    if(_simulationRig&&_reloadRecipe)result["setup"]=simulationRigJson(*_reloadRecipe,*_simulationRig,_simulationSpring->isChecked());
    QJsonArray parts;
    if(_simulationActive&&_reloadRecipe&&_simulationRig)for(int i=0;i<_reloadTrackNodes.size();++i) {
        Transformf world;QString error;
        if(sceneNodeWorld(_sceneNodes[_reloadTrackNodes[i]],world,error)) {
            QJsonObject value;if(encodePoseTransform(world,value,error))parts.append(QJsonObject{{"track",i},{"name",_reloadRecipe->tracks[i].anchor.anchorName},{"world",value},{"magazineOwner",_simulationRig->magazineTracks.contains(i)}});
        }
    }
    result["parts"]=parts;QJsonArray visibility;
    if(auto* model=activeModel())for(const auto& [index,original]:_simulationVisibility)if(index.isValid())
        visibility.append(QJsonObject{{"block",model->getBlockNumber(index)},{"flags",model->get<int>(index,"Flags")},{"originalFlags",original}});
    result["magazineRoots"]=visibility;
    QJsonArray visuals;if(auto* model=activeModel())for(const auto& v:_magazineVisuals)if(v.node.isValid())visuals.append(QJsonObject{{"block",model->getBlockNumber(v.node)},{"emptyVersion",v.empty},{"flags",model->get<int>(v.node,"Flags")},{"originalFlags",v.flags}});
    result["magazineShapes"]=visuals;return result;
}
QJsonObject ReloadMakerController::exportSlideProfile() const {
    if(!isGlockConfiguration()||!_simulationRig||!_reloadRecipe||_weaponBoneIndex<0)return {};
    const auto& rig=*_simulationRig;const auto& recipe=*_reloadRecipe;
    QString error;const auto encoded=encodeReloadRecipe(recipe,error);if(!error.isEmpty())return {};
    const auto original=QJsonDocument::fromJson(encoded).object()["tracks"].toArray();
    QJsonArray tracks;
    for(const int index:rig.slideTracks) {
        if(index<0||index>=_reloadTrackNodes.size())return {};
        const auto binding=std::find_if(_nodeBindings.cbegin(),_nodeBindings.cend(),[&](const auto& b){return b.nodeIndex==_reloadTrackNodes[index];});
        if(binding==_nodeBindings.cend())return {};
        // A model-relative NIF path is not a path in the game's assembled rig.
        // Keep the model identity and export the HKX bone ancestry separately.
        QStringList path;int bone=binding->boneIndex;
        for(int depth=0;bone>=0&&bone<_skeleton.bones.size()&&depth<64;++depth) {
            path.prepend(_skeleton.bones[bone].name);
            if(bone==_weaponBoneIndex)break;
            bone=_skeleton.bones[bone].parentIndex;
        }
        if(bone!=_weaponBoneIndex)return {};
        const auto source=original[index].toObject();const auto poses=source["poses"].toArray();QJsonArray stroke;
        for(int f=rig.openFrame;f<=rig.closedFrame;++f)stroke.append(poses[f]);
        tracks.append(QJsonObject{{"anchor",source["anchor"]},{"runtimePath",path.join('/')},{"poses",stroke}});
    }
    return {{"format","reload-maker-slide-profile"},{"version",1},{"recipeId",recipe.id},{"source",recipe.source},
        {"framePath",_skeleton.bones[_weaponBoneIndex].name},{"space","weapon-local"},
        {"sourceFirstFrame",rig.openFrame},{"sourceLastFrame",rig.closedFrame},{"sourceFrames",recipe.frames},
        {"frames",rig.closedFrame-rig.openFrame+1},{"duration",recipe.duration*(rig.closedFrame-rig.openFrame)/(recipe.frames-1)},
        {"slideTrack",rig.slideTracks.indexOf(rig.slide)},{"openFrame",0},{"closedFrame",rig.closedFrame-rig.openFrame},
        {"springReturn",_simulationSpring->isChecked()},{"tracks",tracks}};
}
QJsonObject ReloadMakerController::exportSimulation() const {
    if(!_simulationRig||!_reloadRecipe)return {};
    QString error;const auto bytes=encodeReloadRecipe(*_reloadRecipe,error);if(!error.isEmpty())return {};
    auto result=simulationRigJson(*_reloadRecipe,*_simulationRig,_simulationSpring->isChecked());
    if(_magazineRail)result["magazineInteraction"]=magazineRailJson(*_magazineRail);
    result["ammunitionConfiguration"]=_simulationAmmo.json();
    if(isGlockConfiguration())result["glockReloadProfile"]=simulationState()["reloadProfile"];
    result["sourceRecipe"]=QJsonDocument::fromJson(bytes).object();return result;
}
}
