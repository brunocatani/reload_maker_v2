#include "SoundProfile.h"
#include "EspReader.h"
#include "LoadOrderLibrary.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    SoundProfile p{"Test.esp:001000","reload.hkx","Test.esp:002000",{}};
    SoundCue open;open.id="pull";open.label="Pull";open.trigger="opening";open.record="Test.esp:003000";open.editorId="SoundPull";open.files={"Sound/FX/Pull.wav"};open.progress=.5f;p.cues.append(open);
    auto close=open;close.id="return";close.trigger="closing";close.progress=.1f;p.cues.append(close);
    SoundCueGate gate;check(gate.cross(p,"opening",0,.49f).isEmpty(),"Early sound");check(gate.cross(p,"opening",.49f,.51f)==QVector<int>{0},"Missing crossing");
    check(gate.cross(p,"opening",.51f,.51f).isEmpty(),"Paused motion repeated");check(gate.cross(p,"opening",.51f,.49f).isEmpty(),"Wrong direction");check(gate.cross(p,"opening",.49f,.51f).isEmpty(),"Threshold jitter repeated");
    (void)gate.cross(p,"opening",.51f,.3f);check(gate.cross(p,"opening",.3f,.6f)==QVector<int>{0},"Reverse did not rearm");
    check(gate.cross(p,"closing",1,0)==QVector<int>{1},"Closing traversal lost sound");check(gate.cross(p,"closing",0,0).isEmpty(),"Closed dwell repeated");
    gate.reset();check(gate.cross(p,"opening",0,1)==QVector<int>{0},"Skipped-frame crossing lost sound");
    QString error;SoundProfile decoded;check(readSoundProfile(soundProfileJson(p),decoded,error),"Roundtrip failed");check(decoded.cues[0].files[0]=="sound/fx/pull.wav","Resource normalization failed");
    auto invalid=soundProfileJson(p);auto cues=invalid["cues"].toArray();auto cue=cues[0].toObject();cue["progress"]=1.1;cues[0]=cue;invalid["cues"]=cues;check(!readSoundProfile(invalid,decoded,error),"Out-of-range trigger accepted");
    cue=soundProfileJson(p)["cues"].toArray()[0].toObject();cue["files"]=QJsonArray{"sound/../../outside.wav"};cues[0]=cue;invalid["cues"]=cues;check(!readSoundProfile(invalid,decoded,error),"Traversal sample path accepted");
    for(int i=1;i<app.arguments().size();++i){const auto plugin=EspReader::read(app.arguments()[i]);check(plugin.parsed&&!plugin.sounds.empty(),"Source sound catalog missing");
        bool dry=false;for(const auto& w:plugin.weapons)if(w.attackFailSound==0x1dacd)dry=true;check(dry,"Packed attack-fail field decoded incorrectly");
        for(const auto& s:plugin.sounds)check(s.attenuationDb>=0&&s.attenuationDb<=100,"Attenuation decoded incorrectly");
    }
    PluginContent base;base.parsed=true;base.fileName="Base.esm";base.sounds.append({0x123,"Original",{"sound/first.wav"},0});
    PluginContent patch;patch.parsed=true;patch.fileName="Patch.esp";patch.masters={"Base.esm"};patch.sounds.append({0x123,"Winning",{"sound/last.wav"},5});
    auto merged=mergeLoadOrder({base,patch},error);check(error.isEmpty()&&merged.sounds.size()==1&&merged.sounds[0].editorId=="Winning"&&recordIdentity(merged,merged.sounds[0].formId)=="Base.esm:000123","Sound override identity lost");
    patch.sounds.clear();patch.deletedRecords={0x123};merged=mergeLoadOrder({base,patch},error);check(error.isEmpty()&&merged.sounds.isEmpty(),"Deleted sound resurrected");
    QTextStream(stdout)<<"PASS source sound records, master/override identity, packed dry-fire field, save validation and direction/pause/jitter/retreat/skip cue behavior\n";return 0;
}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
