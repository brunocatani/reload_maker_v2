#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "MagazineViewport.h"
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
#include <QScrollArea>
#include <QSet>
#include <algorithm>

namespace reloadmaker {
namespace {
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
    _simulationDock=new QDockWidget("SIMULATE / WEAPON",_window);
    _simulationDock->setObjectName("SimulationDock");
    _simulationDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _simulationDock->setMinimumWidth(290);_simulationDock->setMaximumWidth(380);
    auto* panel=new QWidget(_simulationDock);auto* layout=new QVBoxLayout(panel);
    layout->setContentsMargins(16,18,16,18);layout->setSpacing(12);
    auto* title=new QLabel("OPERATE THE WEAPON",panel);
    title->setStyleSheet("font-size:18px;font-weight:700;color:#90d6c7;");layout->addWidget(title);
    auto* intro=new QLabel("Grab the slide in the viewport and pull it back. Its linked parts follow their own motion.",panel);
    intro->setWordWrap(true);layout->addWidget(intro);
    _simulationReloadSet=new QComboBox(panel);_simulationReloadSet->setModel(_animationSetCombo->model());
    _simulationReloadSet->setMinimumContentsLength(18);_simulationReloadSet->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _simulationReloadSet->setToolTip("Reload animation set. Automatic follows the equipped magazine and sight keywords.");
    layout->addWidget(_simulationReloadSet);
    connect(_simulationReloadSet,qOverload<int>(&QComboBox::activated),_animationSetCombo,&QComboBox::setCurrentIndex);
    _simulationNoExtender=new QCheckBox("QuickHands: standard magazine grip",panel);layout->addWidget(_simulationNoExtender);
    _simulationNoExtender->setToolTip("Use the package's optional NoExtender reload with the unextended standard magazine and a QuickHands animation set.");
    connect(_simulationNoExtender,&QCheckBox::toggled,this,[this](bool enabled){_glockNoExtender=enabled;rebuildWeapon();});
    _simulationSource=new QLabel(panel);_simulationSource->setWordWrap(true);_simulationSource->setObjectName("Hint");layout->addWidget(_simulationSource);
    _simulationStatus=new QLabel("Preparing native motion…",panel);_simulationStatus->setWordWrap(true);
    _simulationStatus->setStyleSheet("padding:12px;background:#243b3a;color:#d3f7ec;");layout->addWidget(_simulationStatus);
    const auto button=[&](const QString& text,const QString& action) {
        auto* b=new QPushButton(text,panel);b->setMinimumHeight(40);layout->addWidget(b);
        connect(b,&QPushButton::clicked,this,[this,action]{runSimulationAction(action);});return b;
    };
    _simulationFire=button("FIRE","fire");_simulationFire->setObjectName("PrimaryButton");
    _simulationSlideRelease=button("SLIDE RELEASE","slide_release");
    _simulationReload=button("AUTOMATIC RELOAD","reload");
    _simulationDrop=button("DROP MAGAZINE","drop_magazine");
    auto* magazineRow=new QHBoxLayout;
    _magazineOut=new QPushButton("MAG OUT",panel);_magazineIn=new QPushButton("MAG IN",panel);
    magazineRow->addWidget(_magazineOut);magazineRow->addWidget(_magazineIn);layout->addLayout(magazineRow);
    connect(_magazineOut,&QPushButton::clicked,this,[this]{runSimulationAction("mag_out");});
    connect(_magazineIn,&QPushButton::clicked,this,[this]{runSimulationAction("mag_in");});
    _magazineGrab=button("GRAB MAGAZINE","grab_magazine");
    _simulationReset=button("RESET WEAPON","reset");
    _simulationSpring=new QCheckBox("Return slide when released",panel);_simulationSpring->setChecked(true);layout->addWidget(_simulationSpring);
    auto* arms=new QCheckBox("Show arms / hands",panel);arms->setChecked(_showArmsCheck->isChecked());layout->addWidget(arms);
    connect(arms,&QCheckBox::toggled,_showArmsCheck,&QCheckBox::setChecked);
    connect(_showArmsCheck,&QCheckBox::toggled,arms,&QCheckBox::setChecked);
    _simulationParts=new QLabel(panel);_simulationParts->setWordWrap(true);_simulationParts->setStyleSheet("color:#99adb5;padding-top:12px;");layout->addWidget(_simulationParts);
    layout->addStretch();
    auto* exportButton=new QPushButton("EXPORT SIMULATION SETUP",panel);layout->addWidget(exportButton);
    connect(exportButton,&QPushButton::clicked,this,[this] {
        try{const auto result=executeCommand({{"method","export"},{"params",QJsonObject{{"kind","simulation"}}}});setStatus("Exported · "+result["path"].toString());}
        catch(const std::exception& e){setStatus(QString::fromUtf8(e.what()),true);}
    });
    auto* help=new QLabel("Empty pistol: insert a fresh magazine, then use Slide Release or pull the slide slightly farther back and let go.\n\nGrab Magazine: move mouse, wheel for depth. Guide it to the entrance ring, then push in. Esc cancels.\nSpare magazines are supplied full.",panel);
    help->setWordWrap(true);help->setObjectName("Hint");layout->addWidget(help);
    auto* scroll=new QScrollArea(_simulationDock);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(panel);
    _simulationDock->setWidget(scroll);_window->addDockWidget(Qt::LeftDockWidgetArea,_simulationDock);_simulationDock->hide();
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
        const QString fireLabel=isGlockConfiguration()&&_simulationAmmo.automatic?"Fire — automatic":"Fire — single";
        for(int i=0;i<_animationActions.size();++i)
            if((reload&&_animationActions[i].baseReload)||(fire&&_animationActions[i].label==fireLabel)){index=i;break;}
    }
    if(index<0||index>=_animationList->count()){_simulationError=fire?"No resolved single-fire animation.":"No resolved reload animation.";return false;}
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
    (void)applyPose(0);
    _magazineViewport->setActive(true);
    _simulationDrive->clearCurve();_simulationDrive->setActive(true,true);updateSimulationControls();++_revision;
}
void ReloadMakerController::stopSimulation() {
    if(_simulationTimer)_simulationTimer->stop();
    _simulationActive=false;_simulation.reset();
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
            else {accepted=_simulation.grabMagazine(*_magazineRail);if(accepted)_magazineViewport->beginCarry();}
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
    const bool glock=isGlockConfiguration();
    {const QSignalBlocker block(_simulationReloadSet);_simulationReloadSet->setCurrentIndex(_animationSetCombo->currentIndex());}
    _simulationReloadSet->setEnabled(!busy&&!_loadThread&&!_reloadCapturing&&_simulation.action==SimulationAction::Idle);
    _simulationNoExtender->setVisible(glock);_simulationNoExtender->setEnabled(!busy&&!_loadThread&&!_reloadCapturing&&!glockNoExtenderPath().isEmpty());
    {const QSignalBlocker block(_simulationNoExtender);_simulationNoExtender->setChecked(_glockNoExtender);}
    _simulationSource->setVisible(glock);
    if(glock&&_reloadRecipe) {
        const auto path=_reloadRecipe->source["animation"].toString();
        const auto relative=path.section("glock19xanims/",1);
        _simulationSource->setText("SOURCE · "+relative+"\n"+(_simulationAmmo.automatic?"Automatic receiver · one round per Fire press":"Semi-automatic receiver")+"\nNative reload cycles the slide. Manual magazine exchange retains the chamber.");
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
    if(!_simulationActive){_simulationStatus->setText(_simulationError.isEmpty()?"Preparing native part motion…":_simulationError);_simulationParts->clear();return;}
    _simulationStatus->setText(QString("%1\n%2\n%3\nMagazine: %4 / %5\nChamber: %6 · shots: %7\nSlide: %8").arg(actionName(_simulation.action),_simulation.magazineDescription(),_simulationAmmo.ammoName)
        .arg(_simulation.magazineRounds).arg(_simulation.capacity).arg(_simulation.chambered?"loaded":"empty").arg(_simulation.shotsFired)
        .arg(_simulation.slideLocked?"LOCKED OPEN":QString::number(qRound(_simulation.slide*100))+"% open"));
    QStringList names;for(int i:_simulationRig->slideTracks)names.append(_reloadRecipe->tracks[i].anchor.anchorName);
    _simulationParts->setText("MOVING TOGETHER\n"+names.join(" · ")+"\n\nEach part retains its own translation and rotation.");
    if(!_magazineError.isEmpty())_simulationParts->setText(_simulationParts->text()+"\n\nMagazine interaction: "+_magazineError);
    if(!_simulationAmmo.valid())_simulationParts->setText(_simulationParts->text()+"\n\nAmmunition: "+_simulationAmmo.error);
}
QJsonObject ReloadMakerController::simulationState() const {
    QJsonObject result{{"active",_simulationActive},{"action",actionName(_simulation.action)},{"magazinePresent",_simulation.magazinePresent()},{"slide",_simulation.slide},{"seconds",_simulation.seconds},{"error",_simulationError}};
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
