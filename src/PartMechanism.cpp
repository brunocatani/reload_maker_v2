#include "PartMechanism.h"
#include "PartGrip.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QQuaternion>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace reloadmaker {
    namespace {
        constexpr qint64 maximumBytes=256*1024;
        bool uuid(const QString& id){return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces)==id;}
        bool finite(Vec3f p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<=10000&&std::abs(p.y)<=10000&&std::abs(p.z)<=10000;}
        QJsonObject anchorJson(const HandPosePlacement& a){return {{"kind",a.anchorKind},{"name",a.anchorName},{"path",a.anchorPath},{"model",a.ownerModel},{"record",a.ownerRecord}};}
        HandPosePlacement readAnchor(const QJsonObject& o){return {o["kind"].toString(),o["name"].toString(),o["path"].toString(),o["model"].toString(),o["record"].toString(),{}, {}};}
        bool validAnchor(const HandPosePlacement& a){return (a.anchorKind=="installed-part"||a.anchorKind=="animated-part") && !a.anchorName.isEmpty()&&!a.ownerRecord.isEmpty()&&!a.ownerModel.isEmpty();}
        QString releaseName(ReleaseRule rule){return rule==ReleaseRule::Hold?"hold":rule==ReleaseRule::StageStart?"stage-start":"rest";}
        bool readNumber(const QJsonValue& value,float& out){if(!value.isDouble()||!std::isfinite(value.toDouble())||std::abs(value.toDouble())>100000)return false;out=float(value.toDouble());return true;}
    }
    bool sameMechanismPart(const HandPosePlacement& a,const HandPosePlacement& b){
        if(!validAnchor(a)||!validAnchor(b))return false;
        auto other=b;other.anchorKind=a.anchorKind;
        return sameGripAnchor(a,other);
    }
    bool compatibleMechanismPart(const HandPosePlacement& a,const HandPosePlacement& b){
        if(!validAnchor(a)||!validAnchor(b))return false;
        auto other=b;other.ownerRecord=a.ownerRecord;other.anchorKind=a.anchorKind;
        return sameGripAnchor(a,other);
    }
    bool validateMechanism(const PartMechanism& part,QString& error){
        error.clear();
        if(!uuid(part.id)||part.name.trimmed().isEmpty()||part.name.size()>128||part.weaponRecord.isEmpty()||part.revision<0||!validAnchor(part.anchor)||part.stages.size()>32||part.followers.size()>64){error="Invalid part identity, anchor or definition size";return false;}
        QJsonObject t;
        if(!encodePoseTransform(part.partInWeaponAtRest,t,error))return false;
        for(int i=0;i<part.followers.size();++i){
            const auto& follower=part.followers[i];
            if(!validAnchor(follower.anchor)||sameMechanismPart(follower.anchor,part.anchor)){error="Invalid follower or self-following part";return false;}
            for(int j=0;j<i;++j)if(sameMechanismPart(follower.anchor,part.followers[j].anchor)){error="Duplicate follower";return false;}
            if(!encodePoseTransform(follower.inPartAtRest,t,error))return false;
        }
        for(const auto& stage:part.stages){
            if(stage.forwardName.trimmed().isEmpty()||stage.reverseName.trimmed().isEmpty()||stage.forwardName.size()>128||stage.reverseName.size()>128||stage.axis<0||stage.axis>2||!std::isfinite(stage.from)||!std::isfinite(stage.to)||std::abs(stage.from)>10000||std::abs(stage.to)>10000||std::abs(stage.to-stage.from)<.0001f||!finite(stage.pivot)||!std::isfinite(stage.returnRate)||stage.returnRate<=0||stage.returnRate>100000||int(stage.release)<0||int(stage.release)>2){error="Invalid stage: use a local axis, distinct finite limits and a positive return rate";return false;}
        }
        return true;
    }
    QByteArray encodeMechanism(const PartMechanism& part,QString& error){
        if(!validateMechanism(part,error))return {};
        QJsonObject rest;(void)encodePoseTransform(part.partInWeaponAtRest,rest,error);
        QJsonArray followers,stages;
        for(const auto& follower:part.followers){QJsonObject t;(void)encodePoseTransform(follower.inPartAtRest,t,error);followers.append(QJsonObject{{"anchor",anchorJson(follower.anchor)},{"inPartAtRest",t}});}
        for(const auto& s:part.stages)stages.append(QJsonObject{{"forward",s.forwardName},{"reverse",s.reverseName},{"motion",s.rotation?"rotation":"translation"},{"axis",s.axis},{"from",s.from},{"to",s.to},{"pivot",QJsonArray{s.pivot.x,s.pivot.y,s.pivot.z}},{"release",releaseName(s.release)},{"returnRate",s.returnRate}});
        const auto bytes=QJsonDocument(QJsonObject{{"format","reload-maker-part-mechanism"},{"version",1},{"convention","staged-part-local-v1"},{"id",part.id},{"name",part.name},{"weaponRecord",part.weaponRecord},{"revision",part.revision},{"anchor",anchorJson(part.anchor)},{"partInWeaponAtRest",rest},{"followers",followers},{"stages",stages}}).toJson(QJsonDocument::Indented);
        if(bytes.size()>maximumBytes){error="Mechanism file too large";return {};}
        return bytes;
    }
    bool decodeMechanism(const QByteArray& bytes,PartMechanism& part,QString& error){
        error.clear();if(bytes.isEmpty()||bytes.size()>maximumBytes){error="Mechanism file is empty or too large";return false;}
        QJsonParseError parse;const auto d=QJsonDocument::fromJson(bytes,&parse);
        if(parse.error!=QJsonParseError::NoError||!d.isObject()||d["format"]!="reload-maker-part-mechanism"||d["version"].toDouble()!=1||d["convention"]!="staged-part-local-v1"||!d["followers"].isArray()||!d["stages"].isArray()){error="Invalid or unsupported mechanism file";return false;}
        PartMechanism p;p.id=d["id"].toString();p.name=d["name"].toString();p.weaponRecord=d["weaponRecord"].toString();p.revision=d["revision"].toInt(-1);p.anchor=readAnchor(d["anchor"].toObject());
        if(!decodePoseTransform(d["partInWeaponAtRest"],p.partInWeaponAtRest,error))return false;
        for(const auto& value:d["followers"].toArray()){const auto o=value.toObject();MechanismFollower f;f.anchor=readAnchor(o["anchor"].toObject());if(!decodePoseTransform(o["inPartAtRest"],f.inPartAtRest,error))return false;p.followers.append(f);}
        for(const auto& value:d["stages"].toArray()){
            const auto o=value.toObject();MechanismStage s;s.forwardName=o["forward"].toString();s.reverseName=o["reverse"].toString();s.axis=o["axis"].toInt(-1);s.rotation=o["motion"]=="rotation";
            const auto pivot=o["pivot"].toArray();const auto release=o["release"].toString();
            if((o["motion"]!="rotation"&&o["motion"]!="translation")||!readNumber(o["from"],s.from)||!readNumber(o["to"],s.to)||!readNumber(o["returnRate"],s.returnRate)||pivot.size()!=3||!readNumber(pivot[0],s.pivot.x)||!readNumber(pivot[1],s.pivot.y)||!readNumber(pivot[2],s.pivot.z)||(release!="hold"&&release!="stage-start"&&release!="rest")){error="Invalid stage data";return false;}
            s.release=release=="hold"?ReleaseRule::Hold:release=="stage-start"?ReleaseRule::StageStart:ReleaseRule::Rest;p.stages.append(s);
        }
        if(!validateMechanism(p,error)){return false;}
        part=std::move(p);return true;
    }
    Transformf mechanismDelta(const PartMechanism& part,float path){
        Transformf result;if(!std::isfinite(path))return result;
        for(int i=0;i<part.stages.size()&&path>i;++i){
            const auto& s=part.stages[i];const float value=(s.to-s.from)*std::clamp(path-i,0.f,1.f);const Vec3f axis{s.axis==0?1.f:0.f,s.axis==1?1.f:0.f,s.axis==2?1.f:0.f};Transformf delta;
            if(!s.rotation)delta.translation={axis.x*value,axis.y*value,axis.z*value};
            else {const auto q=QQuaternion::fromAxisAndAngle(QVector3D(axis.x,axis.y,axis.z),value);delta.rotation={q.x(),q.y(),q.z(),q.scalar()};const auto p=rotated(delta.rotation,s.pivot);delta.translation={s.pivot.x-p.x,s.pivot.y-p.y,s.pivot.z-p.z};}
            result=composed(result,delta);
        }
        return result;
    }
    MechanismStage mechanismStageFromDrag(const MechanismStage& initial,int axis,bool rotation,float delta){
        auto stage=initial;
        if(axis<0||axis>2||!std::isfinite(delta))return stage;
        const float base=initial.axis==axis&&initial.rotation==rotation?initial.to:initial.from;
        stage.axis=axis;stage.rotation=rotation;stage.to=std::clamp(base+delta,-10000.f,10000.f);
        return stage;
    }
    void releaseMechanism(const PartMechanism& part,MechanismState& state){
        state.returning=false;if(part.stages.isEmpty()||!std::isfinite(state.path))return;
        const int i=std::clamp(state.activeStage,0,int(part.stages.size())-1);const auto rule=part.stages[i].release;
        if(rule==ReleaseRule::Hold){return;}
        state.returnTarget=rule==ReleaseRule::Rest?0.f:float(i);state.returning=state.path>state.returnTarget;
    }
    void advanceMechanism(const PartMechanism& part,MechanismState& state,float seconds){
        if(!state.returning||!std::isfinite(seconds)||seconds<=0||part.stages.isEmpty())return;
        for(int guard=0;guard<=part.stages.size()&&seconds>0&&state.path>state.returnTarget;++guard){
            const int i=std::clamp(int(std::ceil(state.path))-1,0,int(part.stages.size())-1);const auto& stage=part.stages[i];const float destination=std::max(state.returnTarget,float(i));
            const float rate=stage.returnRate/std::abs(stage.to-stage.from);const float duration=(state.path-destination)/rate;
            if(seconds>=duration){state.path=destination;seconds-=duration;}else{state.path-=seconds*rate;seconds=0;}
            state.activeStage=i;
        }
        if(state.path<=state.returnTarget){state.path=state.returnTarget;state.returning=false;}
    }
    MechanismLibrary::MechanismLibrary(QString directory):_directory(std::move(directory)){}
    QString MechanismLibrary::defaultDirectory(){return QDir::cleanPath(QDir(HandPoseLibrary::defaultDirectory()).filePath("../part-mechanisms"));}
    QVector<PartMechanism> MechanismLibrary::load(QStringList& errors)const{
        errors.clear();QVector<PartMechanism> result;
        for(const auto& info:QDir(_directory).entryInfoList({"*.json"},QDir::Files|QDir::NoSymLinks,QDir::Name)){
            QFile file(info.absoluteFilePath());PartMechanism part;QString error;
            if(info.size()>maximumBytes)error="File too large";else if(!file.open(QIODevice::ReadOnly))error=file.errorString();else if(decodeMechanism(file.read(maximumBytes+1),part,error)&&info.completeBaseName()!=part.id)error="Filename differs from mechanism ID";
            if(error.isEmpty())result.append(part);else errors.append(info.fileName()+": "+error);
        }
        return result;
    }
    bool MechanismLibrary::save(PartMechanism& part,QString& error)const{
        if(!validateMechanism(part,error)){return false;}
        QDir dir(_directory);if(!dir.mkpath(".")){error="Cannot create mechanism library";return false;}
        QLockFile lock(dir.filePath("library.lock"));if(!lock.tryLock(0)){error="Another editor is saving mechanisms";return false;}
        QStringList errors;const auto existing=load(errors);if(!errors.isEmpty()){error="Resolve invalid mechanism files before saving: "+errors.join("; ");return false;}
        bool found=false;
        for(const auto& saved:existing){
            const bool identity=saved.weaponRecord.compare(part.weaponRecord,Qt::CaseInsensitive)==0&&sameMechanismPart(saved.anchor,part.anchor);
            if(saved.id==part.id){found=true;if(!identity||saved.revision!=part.revision){error="Mechanism changed elsewhere or its part identity changed; reload it";return false;}}
            else if(identity){error="This part already has a shared definition; load that definition instead";return false;}
        }
        if(!found&&part.revision!=0){error="Existing mechanism disappeared; reload before saving";return false;}
        if(part.revision==std::numeric_limits<int>::max()){error="Mechanism revision limit reached";return false;}
        auto next=part;++next.revision;const auto bytes=encodeMechanism(next,error);if(bytes.isEmpty())return false;
        QSaveFile file(dir.filePath(part.id+".json"));file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){error=file.errorString();return false;}
        part=std::move(next);return true;
    }
}
