#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
namespace {
QString audioPath(QString path){path.replace('\\','/');if(path.startsWith("data/",Qt::CaseInsensitive))path.remove(0,5);return ResourceArchive::normalizeResourcePath(path);}
bool continuous(const QString& trigger){return trigger=="opening"||trigger=="closing"||trigger=="extraction"||trigger=="insertion";}
}
QString ReloadMakerController::soundProfileKey() const{return (_soundProfile.weapon+"|"+_soundProfile.animation).toLower();}
void ReloadMakerController::createSoundsUi(){
    _soundsDock=new QDockWidget("SOUNDS",_window);_soundsDock->setObjectName("SoundsDock");_soundsDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    _soundsDock->setMinimumWidth(330);_soundsDock->setMaximumWidth(460);
    auto* panel=new QWidget;auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(14,14,14,14);
    auto* intro=new QLabel("Physical reload sounds\nChoose an action, audition its original sound, and set where it plays along the movement.");intro->setWordWrap(true);layout->addWidget(intro);
    _soundCueList=new QListWidget;_soundCueList->setMinimumHeight(245);layout->addWidget(_soundCueList);
    _soundEnabled=new QCheckBox("Play this cue");layout->addWidget(_soundEnabled);
    _soundChoice=new QComboBox;_soundChoice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);_soundChoice->setMinimumContentsLength(18);
    _soundTrigger=new QComboBox;for(const auto& trigger:soundTriggers())_soundTrigger->addItem(soundTriggerLabel(trigger),trigger);
    _soundProgress=new QDoubleSpinBox;_soundProgress->setRange(0,100);_soundProgress->setDecimals(1);_soundProgress->setSuffix(" %");
    _soundGain=new QDoubleSpinBox;_soundGain->setRange(0,200);_soundGain->setDecimals(0);_soundGain->setSuffix(" %");_soundGain->setSingleStep(5);
    auto* form=new QFormLayout;form->addRow("Original sound",_soundChoice);form->addRow("When",_soundTrigger);form->addRow("Movement",_soundProgress);form->addRow("Volume",_soundGain);layout->addLayout(form);
    auto* buttons=new QHBoxLayout;auto* play=new QPushButton("PLAY SOUND");auto* stop=new QPushButton("STOP");buttons->addWidget(play);buttons->addWidget(stop);layout->addLayout(buttons);
    _soundInfo=new QLabel;_soundInfo->setWordWrap(true);_soundInfo->setObjectName("Hint");layout->addWidget(_soundInfo);
    _soundStatus=new QLabel;_soundStatus->setWordWrap(true);_soundStatus->setObjectName("Hint");layout->addWidget(_soundStatus);
    auto* save=new QPushButton("SAVE SOUNDS");save->setObjectName("PrimaryButton");layout->addWidget(save);
    auto* test=new QPushButton("TEST IN OPERATE");layout->addWidget(test);
    _soundMute=new QCheckBox("Mute preview");layout->addWidget(_soundMute);
    auto* collision=new QLabel("Ground and object impacts use the game's collision materials. Releasing a magazine does not play a ground-impact sound.\n\nOpening: 0% closed → 100% rear. Closing runs back to 0%. Magazine insertion: 0% entry → 100% seated.");collision->setWordWrap(true);collision->setObjectName("Hint");layout->addWidget(collision);layout->addStretch();
    auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(panel);_soundsDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_soundsDock);_soundsDock->hide();
    connect(_soundCueList,&QListWidget::currentRowChanged,this,[this]{refreshSoundsUi();});
    connect(_soundChoice,qOverload<int>(&QComboBox::activated),this,[this]{editSoundCue();});
    connect(_soundTrigger,qOverload<int>(&QComboBox::activated),this,[this]{editSoundCue();});
    connect(_soundEnabled,&QCheckBox::toggled,this,[this]{editSoundCue();});
    for(auto* spin:{_soundProgress,_soundGain})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{editSoundCue();});
    connect(play,&QPushButton::clicked,this,[this]{(void)playSoundCue(_soundCueList->currentRow());});
    connect(stop,&QPushButton::clicked,this,[this]{_audio.stop();});
    connect(save,&QPushButton::clicked,this,[this]{(void)saveSounds();});
    connect(test,&QPushButton::clicked,this,[this]{setWorkspace(OperateWorkspace);});
    connect(_soundMute,&QCheckBox::toggled,this,[this](bool value){_soundsMuted=value;if(value)_audio.stop();++_revision;});
}
void ReloadMakerController::prepareSounds(){
    if(!currentWeapon()||!currentPlugin()||!_reloadRecipe||!_simulationRig)return;
    if(!_soundProfile.weapon.isEmpty())_savedSounds[soundProfileKey()]=_soundProfile;
    _audio.clear();_soundCatalog=_referenceSounds;_soundError.clear();
    for(const auto& plugin:_plugins)for(const auto& s:plugin.sounds){SoundCue c;c.record=recordIdentity(plugin,s.formId);c.editorId=s.editorId;c.files=s.files;c.attenuationDb=s.attenuationDb;_soundCatalog[c.record.toLower()]=c;}
    const QString weapon=poseRecordKey(currentWeapon()->formId),animation=_reloadRecipe->source["animation"].toString();
    const QString magazine=simulationMagazineAnchor().ownerRecord,key=(weapon+"|"+animation).toLower();
    _soundProfile={weapon,animation,magazine,{}};
    HkxClip cycle;QString cyclePath;
    if(isBoltActionConfiguration()){QString error;cyclePath=_boltCycleSource["animation"].toString();cycle=HkxReader::readClip(readAnimationResource(cyclePath,error),error);}
    const auto lookup=[&](const QString& edid){SoundCue found;if(edid.isEmpty())return found;for(const auto& c:_soundCatalog)if(c.editorId.compare(edid,Qt::CaseInsensitive)==0){if(!found.record.isEmpty()){_soundError="Ambiguous sound name: "+edid;return SoundCue{};}found=c;}return found;};
    const auto mkCue=[&](const QString& original,const QString& alternate){
        if(isMk18Configuration())for(const auto& a:_reloadClip.annotations)if(a.text.compare("SoundPlay."+alternate,Qt::CaseInsensitive)==0)return alternate;
        return original;
    };
    const auto add=[&](const char* id,const char* label,const char* trigger,float progress,const QString& edid,bool bolt=false){
        SoundCue c=lookup(edid);c.id=id;c.label=label;c.trigger=trigger;c.progress=progress;c.enabled=!c.record.isEmpty()&&!c.files.isEmpty();
        const auto& clip=bolt?cycle:_reloadClip;c.sourceAnimation=bolt?cyclePath:animation;
        for(const auto& a:clip.annotations)if(a.text.compare("SoundPlay."+edid,Qt::CaseInsensitive)==0){c.sourceSeconds=a.timeSeconds;break;}
        if(bolt&&c.sourceSeconds>=0&&!_boltCycle.opening.isEmpty()){
            const bool closing=c.trigger=="closing";const auto& curve=closing?_boltCycle.closing:_boltCycle.opening;
            const float f=std::clamp(c.sourceSeconds/_boltCycle.duration*(_boltCycle.poses.front().size()-1)-(closing?_boltCycle.rear:_boltCycle.first),0.f,float(curve.size()-1));
            const int a=int(f),b=std::min(a+1,int(curve.size()-1));const float value=curve[a]+(curve[b]-curve[a])*(f-a);c.progress=closing?1-value:value;
        }
        for(auto& path:c.files)path=audioPath(path);_soundProfile.cues.append(c);
    };
    const bool glock=isGlockConfiguration(),ru=isAr15Configuration(),m1a=isM1AConfiguration(),bolt=isBoltActionConfiguration();
    const QString prefix=glock?"WPNGlock19x":isMk18Configuration()?"WPNMK18":ru?"WPNRU556":m1a?"WPNM1A":isThanatosConfiguration()?"WPNM107":isL118Configuration()?"L115A3_":bolt?"C14_":"";
    add("unlock","Bolt unlock","opening",.01f,bolt?prefix+"BoltUnlatch":"",bolt);
    add("pull","Slide / handle pull","opening",.035f,isMk18Configuration()?"WPNRifleIstvanReloadBoltOpen":prefix+(glock?"SlideBack":ru?"Boltopen":"BoltBack"),bolt);
    add("rear","Rear stop","opening",1,"",bolt);
    add("forward","Forward return","closing",.975f,isMk18Configuration()?"WPNRifleIstvanReloadBoltClose":prefix+(glock?"SlideForward":ru?"Boltclose":isThanatosConfiguration()?"Boltrelease":"BoltForward"),bolt);
    add("lock","Bolt lock","closing",0,bolt?prefix+"BoltLatch":"",bolt);
    add("mag-release","Magazine release","magazine_release",0,bolt||isThanatosConfiguration()?prefix+"MagRelease":"");
    add("mag-out","Magazine withdrawal","extraction",.035f,mkCue(prefix+"MagOut","WPNMK18MagOut141"));
    add("mag-grab","Magazine grabbed","magazine_grab",0,isMk18Configuration()?"WPNMK18ClothGrabMag":m1a?prefix+"Grab":"");
    add("mag-entry","Magazine entering","insertion",.02f,glock?prefix+"MagIns1":isL118Configuration()?prefix+"ReloadMagIn":bolt||isMk18Configuration()?prefix+"MagInserting":isThanatosConfiguration()?prefix+"Magin":"");
    add("mag-insert","Magazine insertion","insertion",.65f,glock?prefix+"MagIns2":"");
    add("mag-seat","Magazine seated","magazine_seated",1,mkCue(prefix+(isThanatosConfiguration()?"Magslam":"MagIn"),"WPNMK18MagIn141"));
    const auto dryKey=recordIdentity(*currentPlugin(),currentWeapon()->attackFailSound).toLower();const auto dry=_soundCatalog.value(dryKey);
    add("dry","Dry fire","dry_fire",0,dry.editorId);
    if(_savedSounds.contains(key)){_soundProfile=_savedSounds.value(key);_soundProfile.magazine=magazine;}
    if(ru&&std::none_of(_soundProfile.cues.cbegin(),_soundProfile.cues.cend(),[](const auto& c){return c.id=="bolt-release";}))
        add("bolt-release","Bolt catch release","bolt_release",0,isMk18Configuration()?mkCue("WPNMK18BoltCatchPress","WPNMK18BoltCatchSlap"):"WPNRU556BoltRelease");
    // Resolve current winning descriptors on each assembly; saved identities do
    // not pin obsolete file paths from a previous override or package version.
    for(auto& c:_soundProfile.cues)if(!c.record.isEmpty()){
        const auto it=_soundCatalog.constFind(c.record.toLower());
        if(it==_soundCatalog.cend()){if(c.enabled)_soundError="Missing sound descriptor: "+c.record;continue;}
        c.files=it->files;for(auto& p:c.files)p=audioPath(p);c.editorId=it->editorId;c.attenuationDb=it->attenuationDb;
        if(c.enabled)(void)cacheSound(c);
    }
    cacheSourceSounds(_reloadClip);
    _soundsDirty=false;resetSoundTracking();refreshSoundsUi();
}
bool ReloadMakerController::cacheSound(const SoundCue& cue){
    for(const auto& file:cue.files){const auto path=audioPath(file);if(_audio.contains(path))continue;
        QString error;const auto& archive=_packageResources.contains(path)?_packageResources:_gameSounds;
        const auto bytes=archive.read(path,error);if(bytes.isEmpty()||!_audio.cache(path,bytes,error)){_soundError=cue.editorId+": "+error;return false;}}
    if(cue.files.isEmpty()){_soundError="This descriptor has no directly previewable samples: "+cue.editorId;return false;}return true;
}
bool ReloadMakerController::playSound(const SoundCue& cue){
    _lastSound=cue.editorId;++_soundEventCount;
    if(_soundsMuted)return true;
    if(cue.files.isEmpty()){_soundError="No sample for "+cue.editorId;return false;}
    const auto file=audioPath(cue.files[QRandomGenerator::global()->bounded(int(cue.files.size()))]);
    QString error;const float gain=cue.gain*std::pow(10.f,-cue.attenuationDb/20.f);
    if(!_audio.play(file,gain,error)){_soundError=cue.editorId+": "+error;return false;}
    return true;
}
bool ReloadMakerController::playSoundCue(int index){
    if(index<0||index>=_soundProfile.cues.size())return false;
    const auto& cue=_soundProfile.cues[index];const bool ok=cacheSound(cue)&&playSound(cue);if(!ok)setStatus(_soundError,true);refreshSoundsUi();return ok;
}
void ReloadMakerController::emitSoundEvent(const QString& event){for(const int i:_soundGate.event(_soundProfile,event))(void)playSound(_soundProfile.cues[i]);}
void ReloadMakerController::resetSoundTracking(bool stop){if(stop)_audio.stop();_soundGate.reset();_soundPrevious=_simulation;_soundTracking=true;}
void ReloadMakerController::playSourceSounds(const HkxClip& clip,float from,float to){
    if(to<from)return;
    for(const auto& a:clip.annotations)if(a.timeSeconds>from&&a.timeSeconds<=to&&a.text.startsWith("SoundPlay.",Qt::CaseInsensitive))
        for(const auto& cue:_soundCatalog)if(cue.editorId.compare(a.text.mid(10),Qt::CaseInsensitive)==0){(void)playSound(cue);break;}
}
void ReloadMakerController::cacheSourceSounds(const HkxClip& clip){
    for(const auto& a:clip.annotations)if(a.text.startsWith("SoundPlay.",Qt::CaseInsensitive))
        for(const auto& cue:_soundCatalog)if(cue.editorId.compare(a.text.mid(10),Qt::CaseInsensitive)==0){(void)cacheSound(cue);break;}
}
void ReloadMakerController::updateSimulationSounds(){
    if(!_simulationActive)return;if(!_soundTracking){resetSoundTracking(false);return;}
    const auto& old=_soundPrevious;
    const auto cross=[&](const QString& event,float a,float b){for(const int i:_soundGate.cross(_soundProfile,event,a,b))(void)playSound(_soundProfile.cues[i]);};
    if(old.action!=SimulationAction::Fire&&old.action!=SimulationAction::Reload&&_simulation.action!=SimulationAction::Fire&&_simulation.action!=SimulationAction::Reload){
        if(_simulation.mechanism==WeaponMechanism::Ar15&&old.slideLocked&&!_simulation.slideLocked&&_simulation.inputProgress()<=.001f)emitSoundEvent("bolt_release");
        cross("opening",old.inputProgress(),_simulation.inputProgress());cross("closing",old.inputProgress(),_simulation.inputProgress());
        if(old.magazine.seated()&&!_simulation.magazine.seated()){emitSoundEvent("magazine_release");if(_simulation.magazine.phase==MagazinePhase::Absent)cross("extraction",0,1);}
        if(_magazineRail){
            const float a=std::clamp(old.magazine.distance/_magazineRail->travel,0.f,1.f),b=std::clamp(_simulation.magazine.distance/_magazineRail->travel,0.f,1.f);
            if(_simulation.magazine.phase==MagazinePhase::Outgoing||old.magazine.phase==MagazinePhase::Outgoing)cross("extraction",a,b);
            if(_simulation.magazine.phase==MagazinePhase::Rail||_simulation.magazine.phase==MagazinePhase::Incoming||_simulation.magazine.seated())cross("insertion",1-a,1-b);
        }
        if(!old.magazine.seated()&&_simulation.magazine.seated())emitSoundEvent("magazine_seated");
        if(!old.magazine.carried()&&_simulation.magazine.carried()){emitSoundEvent("magazine_grab");_soundGate.reset();}
    }
    _soundPrevious=_simulation;
}
void ReloadMakerController::refreshSoundsUi(){
    if(!_soundCueList||_editingSoundUi)return;_editingSoundUi=true;
    {const QSignalBlocker block(_soundMute);_soundMute->setChecked(_soundsMuted);}
    const int selected=std::clamp(_soundCueList->currentRow(),0,std::max(0,int(_soundProfile.cues.size())-1));
    {const QSignalBlocker block(_soundCueList);_soundCueList->clear();for(const auto& c:_soundProfile.cues)_soundCueList->addItem((c.enabled?"●  ":"○  ")+c.label);_soundCueList->setCurrentRow(selected);}
    const bool has=selected<_soundProfile.cues.size();
    for(QWidget* w:std::initializer_list<QWidget*>{_soundChoice,_soundTrigger,_soundProgress,_soundGain,_soundEnabled})w->setEnabled(has);
    if(has){const auto& c=_soundProfile.cues[selected];const QSignalBlocker a(_soundChoice),b(_soundEnabled),d(_soundTrigger),e(_soundProgress),f(_soundGain);
        _soundChoice->clear();_soundChoice->addItem("No sound",QString());
        const auto origin=_soundProfile.weapon.section(':',0,0);QVector<SoundCue> choices;
        for(const auto& s:_soundCatalog)if(!s.files.isEmpty()&&(s.record.section(':',0,0).compare(origin,Qt::CaseInsensitive)==0||s.record.compare(c.record,Qt::CaseInsensitive)==0))choices.append(s);
        std::sort(choices.begin(),choices.end(),[](const auto& a,const auto& b){return a.editorId<b.editorId;});
        for(const auto& s:choices)_soundChoice->addItem(s.editorId,s.record.toLower());
        _soundChoice->setCurrentIndex(std::max(0,_soundChoice->findData(c.record.toLower())));_soundEnabled->setChecked(c.enabled);
        _soundTrigger->setCurrentIndex(_soundTrigger->findData(c.trigger));_soundProgress->setValue(c.progress*100);_soundProgress->setEnabled(continuous(c.trigger));_soundGain->setValue(c.gain*100);
        _soundInfo->setText(c.record.isEmpty()?"Choose an original sound to enable this action.":QString("%1 sample(s)%2").arg(c.files.size()).arg(c.sourceSeconds>=0?QString(" · source cue at %1 s").arg(c.sourceSeconds,0,'f',3):QString()));
        _soundInfo->setToolTip(c.record+"\n"+c.files.join('\n'));
    }else _soundInfo->setText("Prepare a weapon in Operate to import its sounds.");
    _soundStatus->setText(!_soundError.isEmpty()?_soundError:_soundsDirty?"Unsaved sound changes":QString("%1 cues · last cue: %2").arg(_soundProfile.cues.size()).arg(_lastSound.isEmpty()?"none":_lastSound));
    _editingSoundUi=false;
}
void ReloadMakerController::editSoundCue(){
    if(_editingSoundUi)return;const int index=_soundCueList->currentRow();if(index<0||index>=_soundProfile.cues.size())return;
    auto& cue=_soundProfile.cues[index];const auto record=_soundChoice->currentData().toString();
    if(cue.record.compare(record,Qt::CaseInsensitive)!=0){const auto source=_soundCatalog.value(record);cue.record=source.record;cue.editorId=source.editorId;cue.files=source.files;cue.attenuationDb=source.attenuationDb;cue.sourceSeconds=-1;cue.sourceAnimation.clear();}
    cue.trigger=_soundTrigger->currentData().toString();cue.progress=float(_soundProgress->value()/100);cue.gain=float(_soundGain->value()/100);cue.enabled=_soundEnabled->isChecked()&&!cue.record.isEmpty();
    _soundError.clear();if(cue.enabled)(void)cacheSound(cue);_savedSounds[soundProfileKey()]=_soundProfile;_soundsDirty=true;resetSoundTracking();++_revision;refreshSoundsUi();
}
bool ReloadMakerController::saveSounds(){
    if(_soundProfile.weapon.isEmpty())return false;SoundProfile checked;QString error;
    if(!readSoundProfile(soundProfileJson(_soundProfile),checked,error)){setStatus(error,true);return false;}
    _savedSounds[soundProfileKey()]=_soundProfile;if(!saveSession())return false;_soundsDirty=false;refreshSoundsUi();setStatus("Sound cues saved for this weapon and reload set.");return true;
}
QJsonObject ReloadMakerController::soundState() const{return {{"profile",soundProfileJson(_soundProfile)},{"audio",_audio.state()},{"muted",_soundsMuted},{"dirty",_soundsDirty},{"error",_soundError},{"lastCue",_lastSound},{"eventsTriggered",double(_soundEventCount)}};}
QJsonObject ReloadMakerController::exportSounds() const {if(_soundProfile.weapon.isEmpty())return {};SoundProfile checked;QString error;const auto json=soundProfileJson(_soundProfile);return readSoundProfile(json,checked,error)?json:QJsonObject{};}
}
