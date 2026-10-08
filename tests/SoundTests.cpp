#include "domain/sounds/PreviewWave.h"
#include "SoundProfile.h"
#include "EspReader.h"
#include "LoadOrderLibrary.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
// A RIFF/WAVE file with one fmt chunk and one data chunk.
QByteArray wave(const QByteArray& format,const QByteArray& data){
    QByteArray body="WAVE";auto chunk=[&](const char* id,const QByteArray& bytes){body+=id;quint32 n=bytes.size();body.append(reinterpret_cast<const char*>(&n),4);body+=bytes;};
    chunk("fmt ",format);chunk("data",data);quint32 n=body.size();return QByteArray("RIFF")+QByteArray(reinterpret_cast<const char*>(&n),4)+body;
}
QByteArray fmt(quint16 tag,quint16 channels,quint32 rate,quint16 bits,quint16 subtype=0){
    QByteArray f;auto put=[&](auto v){f.append(reinterpret_cast<const char*>(&v),sizeof v);};
    const quint16 align=channels*bits/8;put(tag);put(channels);put(rate);put(quint32(rate*align));put(align);put(bits);
    if(tag==0xFFFE){put(quint16(22));put(bits);put(quint32(3));put(subtype);f+=QByteArray::fromHex("000000001000800000aa00389b71");}
    return f;
}
qint16 at(const PreviewWave& w,int i){return reinterpret_cast<const qint16*>(w.pcm16.constData())[i];}
void previewWaves(){
    QString error;PreviewWave out;
    // 32-bit integer PCM in an extensible container (the G3's foley).
    QByteArray pcm32;for(qint32 v:{qint32(0x40000000),qint32(-0x40000000),qint32(0),qint32(0x7fffffff)})pcm32.append(reinterpret_cast<const char*>(&v),4);
    check(decodePreviewWave(wave(fmt(0xFFFE,2,48000,32,1),pcm32),out,error),"Extensible 32-bit PCM refused");
    check(out.channels==2&&out.sampleRate==48000&&out.pcm16.size()==8&&at(out,0)==0x4000&&at(out,1)==-0x4000&&at(out,2)==0&&at(out,3)==0x7fff,"32-bit PCM converted wrongly");
    QByteArray pcm16;for(qint16 v:{qint16(1234),qint16(-5)})pcm16.append(reinterpret_cast<const char*>(&v),2);
    check(decodePreviewWave(wave(fmt(1,1,22050,16),pcm16),out,error)&&out.pcm16==pcm16,"Plain 16-bit PCM changed");
    check(decodePreviewWave(wave(fmt(1,1,44100,24),QByteArray::fromHex("0000400000c0")),out,error)&&at(out,0)==0x4000&&at(out,1)==-0x4000,"24-bit PCM converted wrongly");
    QByteArray floats;for(float v:{.5f,-2.f})floats.append(reinterpret_cast<const char*>(&v),4);
    check(decodePreviewWave(wave(fmt(0xFFFE,1,48000,32,3),floats),out,error)&&at(out,0)==16384&&at(out,1)==-32767,"Float samples converted wrongly");
    check(!decodePreviewWave(wave(fmt(0xFFFE,2,48000,32,2),pcm32),out,error),"Compressed extensible subformat accepted");
    check(!decodePreviewWave(wave(fmt(1,2,48000,16),QByteArray(3,0)),out,error),"Partial frame accepted");
    check(!decodePreviewWave(QByteArray("RIFF"),out,error),"Truncated file accepted");
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    previewWaves();
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
    // KSSM choice: the most specific mapping inside the shot's keyword context.
    const auto mapping=[](const char* id,const char* descriptor,QSet<QString> keywords){return SoundMapping{id,id,descriptor,keywords};};
    const QVector<SoundMapping> mappings{mapping("npc","test.esp:000101",{"test.esp:000001"}),mapping("player","test.esp:000102",{"test.esp:000001",kPlayerSoundKeyword}),
        mapping("silencedplayer","test.esp:000103",{"test.esp:000001",kPlayerSoundKeyword,"fallout4.esm:054a67"}),mapping("other","test.esp:000104",{"test.esp:000002"})};
    const auto* chosen=chooseSoundMapping(mappings,{"test.esp:000001",kPlayerSoundKeyword},error);check(chosen&&chosen->record=="player","Player mapping not chosen");
    chosen=chooseSoundMapping(mappings,{"test.esp:000001",kPlayerSoundKeyword,"fallout4.esm:054a67"},error);check(chosen&&chosen->record=="silencedplayer","Most specific mapping not chosen");
    chosen=chooseSoundMapping(mappings,{"test.esp:000001"},error);check(chosen&&chosen->record=="npc","Subset mapping not chosen");
    check(!chooseSoundMapping(mappings,{"test.esp:000009",kPlayerSoundKeyword},error)&&!error.isEmpty(),"Unrelated weapon got a fire sound");
    auto tied=mappings;tied.append(mapping("player2","test.esp:000105",{"test.esp:000001",kPlayerSoundKeyword}));
    check(!chooseSoundMapping(tied,{"test.esp:000001",kPlayerSoundKeyword},error)&&error.contains("equally"),"Ambiguous mappings accepted");
    tied.last().loadOrder=1;chosen=chooseSoundMapping(tied,{"test.esp:000001",kPlayerSoundKeyword},error);check(chosen&&chosen->record=="player2","Later mapping did not win the tie");
    tied.last().loadOrder=-1;chosen=chooseSoundMapping(tied,{"test.esp:000001",kPlayerSoundKeyword},error);check(chosen&&chosen->record=="player","Earlier mapping won the tie");
    // Fire plans: the L115A3 compound keeps only its outdoor child; automatic
    // descriptors loop by rate; unknown conditions and cycles fail closed.
    const auto descriptor=[](const char* id,SoundDescriptorType type,QStringList files,QStringList children={},QVector<SoundCondition> conditions={}){
        SoundDescriptor d;d.record=id;d.editorId=id;d.type=type;d.files=files;d.children=children;d.conditions=conditions;return d;};
    QHash<QString,SoundDescriptor> sounds;
    const SoundCondition outdoors{0x00,300,0},indoors{0x00,300,1};
    for(const auto& d:{descriptor("comp",SoundDescriptorType::Compound,{},{"outdoor","indoor","layer"}),descriptor("outdoor",SoundDescriptorType::Standard,{"out.wav"},{},{outdoors}),
            descriptor("indoor",SoundDescriptorType::Standard,{"in.wav"},{},{indoors}),descriptor("layer",SoundDescriptorType::Standard,{"layer.wav"}),
            descriptor("odd",SoundDescriptorType::Standard,{"odd.wav"},{},{SoundCondition{0x00,72,1}}),descriptor("loop",SoundDescriptorType::Compound,{},{"loop"}),
            descriptor("silent",SoundDescriptorType::Compound,{},{"indoor"})})sounds.insert(d.record,d);
    auto automatic=descriptor("auto",SoundDescriptorType::AutoWeapon,{});automatic.ratesOfFire={{420,"a420.wav"},{780,"a780.wav"},{900,"a900.wav"}};sounds.insert("auto",automatic);
    FireSoundPlan plan;check(planFireSound(sounds,"comp",plan,error)&&plan.layers.size()==2&&plan.layers[0].record=="outdoor"&&plan.layers[1].record=="layer"&&!plan.loop,"Compound conditions not applied");
    check(planFireSound(sounds,"auto",plan,error)&&plan.layers.isEmpty()&&plan.loop&&nearestRateOfFire(*plan.loop,700)->file=="a780.wav","Automatic loop not planned");
    check(!planFireSound(sounds,"odd",plan,error)&&error.contains("function 72"),"Unknown condition guessed");
    check(!planFireSound(sounds,"loop",plan,error)&&error.contains("deeply"),"Compound cycle accepted");
    check(!planFireSound(sounds,"silent",plan,error)&&!planFireSound(sounds,"missing",plan,error),"Empty or missing fire sound accepted");
    PluginContent mapped;mapped.parsed=true;mapped.fileName="Patch.esp";mapped.masters={"Base.esm"};mapped.soundKeywords={0x01000001};
    mapped.soundMappings.append({0x01000010,"Map",0x00000123,{0x01000001,0x00051747}});mapped.keywordNames.insert(0x01000001,"s_40_Test");
    merged=mergeLoadOrder({base,mapped},error);check(error.isEmpty()&&merged.soundMappings.size()==1&&recordIdentity(merged,merged.soundMappings[0].descriptor)=="Base.esm:000123"&&
        recordIdentity(merged,merged.soundMappings[0].keywords[1])=="Base.esm:051747"&&merged.soundKeywords.size()==1,"Sound mapping identities lost in the merge");
    QTextStream(stdout)<<"PASS sound keyword mapping choice and merge, source sound records, master/override identity, packed dry-fire field, save validation and direction/pause/jitter/retreat/skip cue behavior\n";return 0;
}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
