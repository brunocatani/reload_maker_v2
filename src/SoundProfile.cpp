#include "SoundProfile.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace reloadmaker {
namespace {
// FO4 CTDA: the operator is the top three type bits; the low bits are the OR,
// alias, global, pack-data and swap flags (xEdit wbCTDA).
bool previewCondition(const SoundCondition& condition,bool& holds){
    constexpr std::uint16_t isInInterior=300;
    if((condition.type&0x1f)!=0||condition.function!=isInInterior)return false;
    const float value=0; // the preview stands outdoors
    switch(condition.type>>5){
    case 0:holds=value==condition.value;return true;
    case 1:holds=value!=condition.value;return true;
    case 2:holds=value>condition.value;return true;
    case 3:holds=value>=condition.value;return true;
    case 4:holds=value<condition.value;return true;
    case 5:holds=value<=condition.value;return true;
    default:return false;
    }
}
bool expand(const QHash<QString,SoundDescriptor>& descriptors,const QString& record,int depth,FireSoundPlan& plan,QString& error){
    const auto found=descriptors.constFind(record.toLower());
    if(found==descriptors.cend()){error="Missing sound descriptor "+record;return false;}
    const auto& descriptor=found.value();
    for(const auto& condition:descriptor.conditions){
        bool holds=false;
        if(!previewCondition(condition,holds)){error=QString("%1 has a condition the preview cannot evaluate (function %2)").arg(descriptor.editorId).arg(condition.function);return false;}
        if(!holds)return true; // not played outdoors
    }
    if(descriptor.type==SoundDescriptorType::AutoWeapon){
        if(descriptor.ratesOfFire.isEmpty()){error=descriptor.editorId+" has no rate-of-fire samples";return false;}
        if(plan.loop&&plan.loop->record!=descriptor.record){error=descriptor.editorId+" layers two automatic loops";return false;}
        plan.loop=descriptor;return true;
    }
    if(descriptor.type==SoundDescriptorType::Compound){
        // Bounded: compounds nest a level or two in practice; a cycle fails.
        if(depth>=4){error=descriptor.editorId+" nests compound sounds too deeply";return false;}
        for(const auto& child:descriptor.children)if(!expand(descriptors,child,depth+1,plan,error))return false;
        return true;
    }
    if(descriptor.files.isEmpty()){error=descriptor.editorId+" has no samples";return false;}
    plan.layers.append(descriptor);return true;
}
}
bool planFireSound(const QHash<QString,SoundDescriptor>& descriptors,const QString& record,FireSoundPlan& plan,QString& error){
    plan={};error.clear();
    if(!expand(descriptors,record,0,plan,error)){plan={};return false;}
    if(plan.layers.isEmpty()&&!plan.loop){error="Nothing in "+record+" plays outdoors";return false;}
    return true;
}
const RateOfFireSample* nearestRateOfFire(const SoundDescriptor& descriptor,float rpm){
    const RateOfFireSample* best=nullptr;
    for(const auto& sample:descriptor.ratesOfFire)if(!best||std::abs(sample.rpm-rpm)<std::abs(best->rpm-rpm))best=&sample;
    return best;
}
const SoundMapping* chooseSoundMapping(const QVector<SoundMapping>& mappings,const QSet<QString>& context,QString& error){
    const SoundMapping* best=nullptr;bool tied=false;error.clear();
    for(const auto& mapping:mappings){
        if(mapping.keywords.isEmpty()||mapping.descriptor.isEmpty()||!context.contains(mapping.keywords))continue;
        if(!best||mapping.keywords.size()>best->keywords.size()||(mapping.keywords.size()==best->keywords.size()&&mapping.loadOrder>best->loadOrder)){best=&mapping;tied=false;}
        else if(mapping.keywords.size()==best->keywords.size()&&mapping.loadOrder==best->loadOrder&&mapping.descriptor!=best->descriptor)tied=true;
    }
    if(!best){error="No sound keyword mapping matches this weapon's sound keywords.";return nullptr;}
    if(tied){
        QStringList names;for(const auto& mapping:mappings)if(mapping.keywords.size()==best->keywords.size()&&mapping.loadOrder==best->loadOrder&&context.contains(mapping.keywords))names<<mapping.editorId;
        error="Sound keyword mappings match this weapon equally: "+names.join(", ");return nullptr;
    }
    return best;
}
QStringList soundTriggers(){return {"opening","closing","extraction","insertion","magazine_release","magazine_seated","magazine_grab","dry_fire","bolt_release"};}
QString soundTriggerLabel(const QString& trigger){
    static const QStringList labels{"Pulling back / unlocking","Moving forward / locking","Magazine moving out","Magazine moving in","Magazine release","Magazine seated","Magazine grabbed","Empty trigger press","Bolt catch release"};
    const int i=soundTriggers().indexOf(trigger);return i<0?trigger:labels[i];
}
QJsonObject soundProfileJson(const SoundProfile& p){
    QJsonArray cues;
    for(const auto& c:p.cues)cues.append(QJsonObject{{"id",c.id},{"label",c.label},{"trigger",c.trigger},{"progress",c.progress},
        {"record",c.record},{"editorId",c.editorId},{"files",QJsonArray::fromStringList(c.files)},{"gain",c.gain},{"attenuationDb",c.attenuationDb},
        {"enabled",c.enabled},{"sourceAnimation",c.sourceAnimation},{"sourceSeconds",c.sourceSeconds}});
    return {{"format","reload-maker-sound-profile"},{"version",1},{"weapon",p.weapon},{"animation",p.animation},{"magazine",p.magazine},
        {"collisionPolicy","native-material-impacts"},{"cues",cues}};
}
bool readSoundProfile(const QJsonObject& j,SoundProfile& output,QString& error){
    error.clear();SoundProfile p;
    const auto fail=[&](const QString& message){error=message;return false;};
    static const QRegularExpression form("^[^:/\\\\]+\\.(?:esm|esp|esl):[0-9a-f]{6}$",QRegularExpression::CaseInsensitiveOption);
    if(j["format"]!="reload-maker-sound-profile"||j["version"]!=1||j["collisionPolicy"]!="native-material-impacts")return fail("Unsupported sound profile.");
    p.weapon=j["weapon"].toString();p.animation=j["animation"].toString();p.magazine=j["magazine"].toString();
    if(!form.match(p.weapon).hasMatch()||(!p.magazine.isEmpty()&&!form.match(p.magazine).hasMatch())||p.animation.size()>1024||!j["cues"].isArray()||j["cues"].toArray().size()>64)return fail("Invalid sound profile identity or cue count.");
    QSet<QString> ids;
    for(const auto& value:j["cues"].toArray()){
        if(!value.isObject())return fail("Invalid sound cue.");const auto v=value.toObject();SoundCue c;
        c.id=v["id"].toString();c.label=v["label"].toString();c.trigger=v["trigger"].toString();c.record=v["record"].toString();c.editorId=v["editorId"].toString();
        c.sourceAnimation=v["sourceAnimation"].toString();
        const auto number=[&](const char* key,double lo,double hi,float& result){const auto n=v[key];if(!n.isDouble()||!std::isfinite(n.toDouble())||n.toDouble()<lo||n.toDouble()>hi)return false;result=float(n.toDouble());return true;};
        if(c.id.isEmpty()||c.id.size()>128||ids.contains(c.id)||c.label.size()>256||c.editorId.size()>256||!soundTriggers().contains(c.trigger)||
            (!c.record.isEmpty()&&!form.match(c.record).hasMatch())||!v["enabled"].isBool()||!number("progress",0,1,c.progress)||!number("gain",0,2,c.gain)||
            !number("attenuationDb",0,100,c.attenuationDb)||!number("sourceSeconds",-1,3600,c.sourceSeconds)||c.sourceAnimation.size()>1024||!v["files"].isArray()||v["files"].toArray().size()>32)return fail("Invalid sound cue values.");
        for(const auto& file:v["files"].toArray()){
            QString path=file.toString();path.replace('\\','/');if(path.startsWith("data/",Qt::CaseInsensitive))path.remove(0,5);
            if(!file.isString()||!path.startsWith("sound/",Qt::CaseInsensitive)||path.contains(':')||path.split('/').contains("..")||path.size()>1024)return fail("Invalid sound resource path.");
            c.files.append(path.toLower());
        }
        c.enabled=v["enabled"].toBool();if(c.enabled&&(c.record.isEmpty()||c.editorId.isEmpty()))return fail("An enabled cue needs a sound descriptor.");
        ids.insert(c.id);p.cues.append(c);
    }
    output=std::move(p);return true;
}
QVector<int> SoundCueGate::cross(const SoundProfile& p,const QString& trigger,float before,float after){
    QVector<int> result;if(!std::isfinite(before)||!std::isfinite(after))return result;
    const bool descending=trigger=="closing";
    for(int i=0;i<p.cues.size()&&i<64;++i){const auto& c=p.cues[i];if(c.trigger!=trigger||!c.enabled)continue;
        const float a=descending?1-before:before,b=descending?1-after:after,t=descending?1-c.progress:c.progress;
        if(b<t-.04f||(t<.04f&&b<=.0001f&&a>b))fired_[i]=false;
        if(!fired_[i]&&b>a+.00001f&&a<=t&&b>=t){fired_[i]=true;result.append(i);}
    }
    return result;
}
QVector<int> SoundCueGate::event(const SoundProfile& p,const QString& trigger) const {
    QVector<int> result;for(int i=0;i<p.cues.size();++i)if(p.cues[i].enabled&&p.cues[i].trigger==trigger)result.append(i);return result;
}
}
