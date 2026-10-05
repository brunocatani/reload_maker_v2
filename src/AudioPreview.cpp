#include "AudioPreview.h"
#define NOMINMAX
#include <Windows.h>
#include <xaudio2.h>
#include <QtEndian>
#include <QHash>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace reloadmaker {
struct AudioPreview::Impl {
    struct Sample {WAVEFORMATEX format{};QByteArray pcm;};
    struct Voice {IXAudio2SourceVoice* native{};std::shared_ptr<Sample> sample;quint64 order{};};
    IXAudio2* engine{};IXAudio2MasteringVoice* master{};
    std::array<Voice,32> voices{};Voice looping{};QHash<QString,std::shared_ptr<Sample>> samples;
    bool com{};quint64 played{};QString error;
    bool start(){
        if(engine)return true;
        const auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);com=SUCCEEDED(hr);
        HRESULT result=XAudio2Create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR);
        if(SUCCEEDED(result))result=engine->CreateMasteringVoice(&master);
        if(FAILED(result)){if(engine){engine->Release();engine=nullptr;}if(com){CoUninitialize();com=false;}error=QString("Audio output could not open (0x%1).").arg(quint32(result),8,16,QChar('0'));return false;}
        error.clear();return true;
    }
    void reap(){for(auto& v:voices)if(v.native){XAUDIO2_VOICE_STATE state{};v.native->GetState(&state);if(!state.BuffersQueued){v.native->DestroyVoice();v={};}}}
};
AudioPreview::AudioPreview():impl_(std::make_unique<Impl>()){}
AudioPreview::~AudioPreview(){clear();if(impl_->master)impl_->master->DestroyVoice();if(impl_->engine)impl_->engine->Release();if(impl_->com)CoUninitialize();}
bool AudioPreview::contains(const QString& key) const{return impl_->samples.contains(key);}
bool AudioPreview::cache(const QString& key,const QByteArray& wav,QString& error){
    error.clear();const auto fail=[&](const char* message){error=QString::fromLatin1(message);return false;};
    if(wav.size()<12||wav.size()>16*1024*1024||wav.left(4)!="RIFF"||wav.mid(8,4)!="WAVE")return fail("Preview requires a PCM WAV sample (up to 16 MiB).");
    auto sample=std::make_shared<Impl::Sample>();bool format=false,data=false;
    for(qsizetype at=12;at<=wav.size()-8;){
        const auto count=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(wav.constData()+at+4));const auto start=at+8;
        if(count>quint32(wav.size()-start))return fail("Truncated WAV chunk.");
        if(wav.mid(at,4)=="fmt "){
            if(count<16)return fail("Truncated WAV format.");auto& f=sample->format;const auto* bytes=reinterpret_cast<const uchar*>(wav.constData()+start);
            f.wFormatTag=qFromLittleEndian<quint16>(bytes);f.nChannels=qFromLittleEndian<quint16>(bytes+2);f.nSamplesPerSec=qFromLittleEndian<quint32>(bytes+4);
            f.nAvgBytesPerSec=qFromLittleEndian<quint32>(bytes+8);f.nBlockAlign=qFromLittleEndian<quint16>(bytes+12);f.wBitsPerSample=qFromLittleEndian<quint16>(bytes+14);format=true;
        }else if(wav.mid(at,4)=="data"){sample->pcm=wav.mid(start,count);data=true;}
        at=start+count+(count&1);
    }
    const auto& f=sample->format;
    if(!format||!data||sample->pcm.isEmpty()||f.wFormatTag!=WAVE_FORMAT_PCM||f.nChannels<1||f.nChannels>2||
       (f.wBitsPerSample!=8&&f.wBitsPerSample!=16)||f.nSamplesPerSec<8000||f.nSamplesPerSec>192000||
       f.nBlockAlign!=f.nChannels*f.wBitsPerSample/8||f.nAvgBytesPerSec!=f.nSamplesPerSec*f.nBlockAlign||sample->pcm.size()%f.nBlockAlign)return fail("Unsupported or invalid PCM sample format.");
    if(impl_->samples.size()>=256&&!contains(key))return fail("Sound preview cache is full.");
    impl_->samples.insert(key,std::move(sample));return true;
}
bool AudioPreview::play(const QString& key,float gain,QString& error){
    error.clear();if(!std::isfinite(gain)||gain<0||gain>2){error="Invalid preview volume.";return false;}
    const auto found=impl_->samples.constFind(key);if(found==impl_->samples.cend()){error="Sound sample is not prepared.";return false;}
    if(!impl_->start()){error=impl_->error;return false;}impl_->reap();
    // Simulated time can run faster than playback (a long step fires many
    // shots at once). As in the game's audio engine, a new sound then takes
    // over the voice of the oldest one-shot instead of failing.
    auto slot=std::find_if(impl_->voices.begin(),impl_->voices.end(),[](const Impl::Voice& v){return !v.native;});
    if(slot==impl_->voices.end()){
        slot=std::min_element(impl_->voices.begin(),impl_->voices.end(),[](const Impl::Voice& a,const Impl::Voice& b){return a.order<b.order;});
        slot->native->DestroyVoice();*slot={};
    }
    auto& v=*slot;v.order=impl_->played+1;
    v.sample=*found;auto hr=impl_->engine->CreateSourceVoice(&v.native,&v.sample->format);
    XAUDIO2_BUFFER buffer{};buffer.Flags=XAUDIO2_END_OF_STREAM;buffer.AudioBytes=UINT32(v.sample->pcm.size());buffer.pAudioData=reinterpret_cast<const BYTE*>(v.sample->pcm.constData());
    if(SUCCEEDED(hr))hr=v.native->SetVolume(gain);if(SUCCEEDED(hr))hr=v.native->SubmitSourceBuffer(&buffer);if(SUCCEEDED(hr))hr=v.native->Start();
    if(FAILED(hr)){if(v.native)v.native->DestroyVoice();v={};error=QString("Sound playback failed (0x%1).").arg(quint32(hr),8,16,QChar('0'));return false;}
    ++impl_->played;return true;
}
bool AudioPreview::loop(const QString& key,float gain,QString& error){
    error.clear();if(!std::isfinite(gain)||gain<0||gain>2){error="Invalid preview volume.";return false;}
    const auto found=impl_->samples.constFind(key);if(found==impl_->samples.cend()){error="Sound sample is not prepared.";return false;}
    if(!impl_->start()){error=impl_->error;return false;}stopLoop();
    auto& v=impl_->looping;v.sample=*found;auto hr=impl_->engine->CreateSourceVoice(&v.native,&v.sample->format);
    XAUDIO2_BUFFER buffer{};buffer.Flags=XAUDIO2_END_OF_STREAM;buffer.AudioBytes=UINT32(v.sample->pcm.size());buffer.pAudioData=reinterpret_cast<const BYTE*>(v.sample->pcm.constData());
    buffer.LoopCount=XAUDIO2_LOOP_INFINITE;
    if(SUCCEEDED(hr))hr=v.native->SetVolume(gain);if(SUCCEEDED(hr))hr=v.native->SubmitSourceBuffer(&buffer);if(SUCCEEDED(hr))hr=v.native->Start();
    if(FAILED(hr)){if(v.native)v.native->DestroyVoice();v={};error=QString("Sound playback failed (0x%1).").arg(quint32(hr),8,16,QChar('0'));return false;}
    ++impl_->played;return true;
}
bool AudioPreview::looping() const{return impl_->looping.native!=nullptr;}
void AudioPreview::stopLoop(){auto& v=impl_->looping;if(v.native){v.native->DestroyVoice();v={};}}
void AudioPreview::stop(){for(auto& v:impl_->voices)if(v.native){v.native->DestroyVoice();v={};}stopLoop();}
void AudioPreview::clear(){stop();impl_->samples.clear();}
QJsonObject AudioPreview::state() const {int active=0;for(const auto& v:impl_->voices)if(v.native){XAUDIO2_VOICE_STATE s{};v.native->GetState(&s);if(s.BuffersQueued)++active;}
    return {{"ready",bool(impl_->engine)},{"played",double(impl_->played)},{"activeVoices",active},{"cachedSamples",impl_->samples.size()},{"error",impl_->error}};}
}
