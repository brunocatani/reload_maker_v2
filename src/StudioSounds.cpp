#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

namespace reloadmaker {
namespace {
bool continuous(const QString& trigger){return trigger=="opening"||trigger=="closing"||trigger=="extraction"||trigger=="insertion";}
}
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
    auto* buttons=new QHBoxLayout;auto* play=new QPushButton("Play sound");auto* stop=new QPushButton("Stop");buttons->addWidget(play);buttons->addWidget(stop);layout->addLayout(buttons);
    _soundInfo=new QLabel;_soundInfo->setWordWrap(true);_soundInfo->setObjectName("Hint");layout->addWidget(_soundInfo);
    _soundStatus=new QLabel;_soundStatus->setWordWrap(true);_soundStatus->setObjectName("Hint");layout->addWidget(_soundStatus);
    auto* save=new QPushButton("Save sounds");save->setObjectName("PrimaryButton");layout->addWidget(save);
    auto* test=new QPushButton("Test in Operate");layout->addWidget(test);
    _soundMute=new QCheckBox("Mute preview");layout->addWidget(_soundMute);
    auto* collision=new QLabel("Ground and object impacts use the game's collision materials. Releasing a magazine does not play a ground-impact sound.\n\nOpening: 0% closed → 100% rear. Closing runs back to 0%. Magazine insertion: 0% entry → 100% seated.");collision->setWordWrap(true);collision->setObjectName("Hint");layout->addWidget(collision);layout->addStretch();
    auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(panel);_soundsDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_soundsDock);_soundsDock->hide();
    connect(_soundCueList,&QListWidget::currentRowChanged,this,[this](int row){selectSoundCue(row);});
    connect(_soundChoice,qOverload<int>(&QComboBox::activated),this,[this]{editSoundCue();});
    connect(_soundTrigger,qOverload<int>(&QComboBox::activated),this,[this]{editSoundCue();});
    connect(_soundEnabled,&QCheckBox::toggled,this,[this]{editSoundCue();});
    for(auto* spin:{_soundProgress,_soundGain})connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{editSoundCue();});
    connect(play,&QPushButton::clicked,this,[this]{(void)playSoundCue(_selectedSoundCue);});
    connect(stop,&QPushButton::clicked,this,[this]{_audio.stop();});
    connect(save,&QPushButton::clicked,this,[this]{(void)saveSounds();});
    connect(test,&QPushButton::clicked,this,[this]{setWorkspace(OperateWorkspace);});
    connect(_soundMute,&QCheckBox::toggled,this,[this](bool value){setSoundsMuted(value);});
}
void ReloadMakerController::refreshSoundsUi(){
    if(!_soundCueList||_editingSoundUi)return;_editingSoundUi=true;
    showSoundsMuted();
    normalizeSoundCueSelection();const int selected=_selectedSoundCue;
    {const QSignalBlocker block(_soundCueList);_soundCueList->clear();for(const auto& c:_soundProfile.cues)_soundCueList->addItem((c.enabled?"●  ":"○  ")+c.label);_soundCueList->setCurrentRow(selected);}
    const bool has=selected>=0;
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
void ReloadMakerController::editSoundCue(){if(_editingSoundUi)return;applySoundCueForm(soundCueForm());}
ReloadMakerController::SoundCueForm ReloadMakerController::soundCueForm() const{
    return {_soundChoice->currentData().toString(),_soundTrigger->currentData().toString(),float(_soundProgress->value()/100),float(_soundGain->value()/100),_soundEnabled->isChecked()};
}
void ReloadMakerController::showSoundsMuted(){
    if(!_soundMute)return;
    const QSignalBlocker block(_soundMute);_soundMute->setChecked(_soundsMuted);
}
}
