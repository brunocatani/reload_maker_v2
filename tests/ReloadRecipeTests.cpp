#include "ReloadRecipe.h"
#include "ReloadBundle.h"
#include "ReloadDriveMath.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QQuaternion>
#include <QTextStream>
#include <QUuid>
#include <cmath>
#include <stdexcept>
using namespace reloadmaker;
namespace {
    void check(bool ok,const QString& why){if(!ok)throw std::runtime_error(why.toStdString());}
    float ramp(float time,float a,float b){return std::clamp((time-a)/(b-a),0.f,1.f);}
    Transformf pose(Vec3f t={},float degrees=0){const auto q=QQuaternion::fromAxisAndAngle(0,1,0,degrees);return {t,{q.x(),q.y(),q.z(),q.scalar()},{1.3f,1.3f,1.3f}};}
    ReloadRecipe capture(){
        ReloadRecipe r;r.duration=4;r.frames=481;r.source={{"action","WPNReload"},{"weaponRecord","Test.esp:123456"},{"animation","wpnreload.hkx"},{"animationSha256","source-a"}};
        for(const QString name:{"Bolt","Sight","Magazine","Round","Tilting barrel","Stationary"}){ReloadTrack t;t.anchor={"animated-part",name,"Root/"+name,"Weapons/Test.nif","Test.esp:123456",{}, {}};t.label=name;t.surface=true;r.tracks.append(t);}
        r.tracks[1].parent=0;r.tracks[3].parent=2;
        for(int i=0;i<r.frames;++i){const float time=r.duration*i/(r.frames-1);
            auto bolt=pose({0,-8*(ramp(time,.7f,1.f)-ramp(time,3.f,3.3f)),0},70*(ramp(time,.3f,.6f)-ramp(time,3.5f,3.8f)));
            r.tracks[0].poses.append(bolt);r.tracks[1].poses.append(composed(bolt,Transformf{{2,0,1}}));
            auto mag=pose({0,0,-12*(ramp(time,1.3f,1.6f)-ramp(time,2.1f,2.4f))});
            if(time>=1.7f&&time<1.9f)mag.scale={0,0,0};
            r.tracks[2].poses.append(mag);r.tracks[3].poses.append(composed(mag,Transformf{{0,1,0}}));
            r.tracks[4].poses.append(pose({4,0,0},35*ramp(time,.72f,.98f)));r.tracks[5].poses.append(pose({9,0,0}));
        }
        return r;
    }
    void motion(){
        auto r=capture();QString error;check(identifyReloadRecipe(r,error),error);
        const auto bolt=reloadMotionSpans(r,0);
        check(bolt.size()>=2,"Bolt lift/pull and return were one movement");
        check(float(bolt.back().first)/120>3.4f,"Bolt return missing");
        check(reloadMotionSpans(r,5).isEmpty(),"Stationary part shown as moving");
        const auto magazine=reloadMotionSpans(r,2);
        check(!magazine.isEmpty(),"Magazine movement missing");
        for(const auto& span:magazine)for(int frame=span.first;frame<=span.last;++frame)
            check(reloadPoseVisible(r.tracks[2].poses[frame]),"Movement span crosses the hidden magazine");
        check(reloadMotionSpans(r,-1).isEmpty()&&reloadMotionSpans(r,r.tracks.size()).isEmpty(),"Invalid track accepted");
    }
    void encoding(){
        auto r=capture();QString error;check(identifyReloadRecipe(r,error),error);const auto id=r.id;
        const auto bytes=encodeReloadRecipe(r,error);check(!bytes.isEmpty(),error);
        const auto json=QJsonDocument::fromJson(bytes).object();
        check(json["version"]==2&&!json.contains("windows")&&!json.contains("revision"),"Recipe still carries retired window or revision data");
        ReloadRecipe loaded;check(decodeReloadRecipe(bytes,loaded,error),error);
        check(loaded.id==id&&loaded.tracks.size()==r.tracks.size(),"Round trip changed the recipe");
        check(std::abs(loaded.tracks[0].poses[30].scale.x-1.3f)<.0001f,"Scale was reset to one");
        check(loaded.tracks[2].poses[210].scale.x==0,"Hidden scale lost");
        r.source["animationSha256"]="override-b";check(reloadRecipeIdentity(r)!=id,"Animation override reused stale source identity");
        check(encodeReloadRecipe(r,error).isEmpty(),"Changed source encoded under old identity");
        auto future=json;future["version"]=3;
        check(!decodeReloadRecipe(QJsonDocument(future).toJson(),loaded,error),"Unknown recipe version accepted");
    }
    void curve(){
        check(std::abs(reloadCurveTime({{0,0},{100,0}},{0,1},{98,0},.9f,.025f)-1)<1e-6f,"Bolt pointer stopped before its rear end stop");
        check(reloadCurveTime({{0,0},{100,0}},{0,1},{2,0},.1f,.025f)==0,"Bolt pointer did not finish forward locking");
        check(std::abs(reloadCurveTime({{0,0},{100,0}},{0,1},{50,0},0,.025f)-.5f)<1e-6f,"End-stop snap changed mid-stroke input");

        const QVector<QPointF> points{{0,0},{10,0},{10,10},{20,10}};const QVector<float> times{0,1,2,3};
        check(std::abs(reloadCurveTime(points,times,{10,5},1)-1.5f)<.001f,"Corner path collapsed to a straight axis");
        check(std::abs(reloadCurveTime(points,times,{15,10},2)-2.5f)<.001f,"Curved path time wrong");
        check(std::abs(reloadCurveTime(points,times,{5,0},2.5f)-.5f)<.001f,"Reverse drag failed");
        const QVector<QPointF> crossing{{0,0},{10,0},{0,0}};check(std::abs(reloadCurveTime(crossing,{0,1,2},{5,0},1.6f)-1.5f)<.001f,"Crossing ignored temporal continuity");
    }
    void bundles(){
        auto r=capture();r.tracks[2].anchor.anchorName="WeaponMagazine";
        QString error;check(identifyReloadRecipe(r,error),error);
        PartGrip grip;grip.weaponRecord=r.source["weaponRecord"].toString();grip.sourcePoseId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        grip.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);grip.pose.name="Chosen magazine grip";grip.pose.createdUtc="2026-09-29T22:00:00.000Z";
        grip.pose.hand=PoseHand::Left;grip.pose.placement=r.tracks[2].anchor;grip.pose.placement.handInAnchor=pose({1,2,3},37);
        grip.pose.fingers[0]=pose({.25f,.5f,1.f},61);
        const QHash<QString,QString> bindings{{magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord),grip.pose.id}};
        const auto bytes=encodeReloadBundle(r,{grip},{},bindings,error);check(!bytes.isEmpty(),error);
        const auto bundle=QJsonDocument::fromJson(bytes).object();check(bundle["grips"].toArray().size()==1,"Shared grip exported more than once");
        PartGrip restored;check(decodePartGrip(QJsonDocument(bundle["grips"].toArray()[0].toObject()).toJson(),restored,error),error);
        check(encodePartGrip(restored,error)==encodePartGrip(grip,error),"Export changed fingers or part placement");
        check(bundle["magazineGrabBindings"].toArray().size()==1,"Magazine assignment missing from bundle");
        auto history=grip;history.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);history.pose.placement.ownerModel="ReplacedByOverhaul.nif";
        check(!encodeReloadBundle(r,{grip,history},{},bindings,error).isEmpty(),"Unassigned historical source-model fit blocked the current bundle");
        auto opposite=grip;opposite.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);opposite.pose.hand=grip.pose.hand==PoseHand::Left?PoseHand::Right:PoseHand::Left;
        const auto baseKey=magazineGrabKey(grip.weaponRecord,grip.pose.placement.ownerRecord);
        auto twoHands=bindings;twoHands[handAssignmentKey(baseKey,opposite.pose.hand)]=opposite.pose.id;
        const auto original=magazineGripFor(grip.weaponRecord,grip.pose.placement,{grip,opposite},twoHands,{},error,grip.pose.hand);
        const auto alternate=magazineGripFor(grip.weaponRecord,grip.pose.placement,{grip,opposite},twoHands,{},error,opposite.pose.hand);
        check(original&&original->pose.id==grip.pose.id&&alternate&&alternate->pose.id==opposite.pose.id,"Per-hand assignment overwrote legacy fit");
        check(!magazineGripFor(grip.weaponRecord,grip.pose.placement,{grip},bindings,{},error,opposite.pose.hand),"Other hand inherited a wrong-side legacy fit");
        const auto dual=QJsonDocument::fromJson(encodeReloadBundle(r,{grip,opposite},{},twoHands,error)).object();
        check(dual["version"]==2&&dual["magazineGrabBindings"].toArray().size()==2,"Both magazine hands missing from versioned export");
        check(encodeReloadBundle(r,{}, {},bindings,error).isEmpty(),"Unresolved grip exported");
        auto bad=grip;bad.weaponRecord="Other.esp:123456";check(encodeReloadBundle(r,{bad},{},bindings,error).isEmpty(),"Cross-weapon grip exported");
        bad=grip;bad.pose.placement.anchorPath="Wrong/part";check(encodeReloadBundle(r,{bad},{},bindings,error).isEmpty(),"Wrong-part grip exported");
        bad=grip;bad.mechanismId=QUuid::createUuid().toString(QUuid::WithoutBraces);check(encodeReloadBundle(r,{bad},{},bindings,error).isEmpty(),"Missing part definition exported");
        PartMechanism part;part.id=bad.mechanismId;part.name="Magazine rail";part.weaponRecord=grip.weaponRecord;part.anchor=grip.pose.placement;part.stages={MechanismStage{}};
        auto withPart=QJsonDocument::fromJson(encodeReloadBundle(r,{bad},{part},bindings,error)).object();
        check(withPart["mechanisms"].toArray().size()==1,"Shared part definition was not embedded");
        auto other=bad;other.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);other.pose.placement.ownerRecord="OtherMagazine.esp:123456";
        auto otherBindings=bindings;otherBindings[magazineGrabKey(grip.weaponRecord,other.pose.placement.ownerRecord)]=other.pose.id;
        // Both magazine assignments name one part definition; the grip whose
        // anchor differs from the definition is refused.
        check(encodeReloadBundle(r,{bad,other},{part},otherBindings,error).isEmpty(),"Reused definition hid a mismatched grip anchor");
        auto drum=grip.pose.placement;drum.ownerModel="Weapons/Glock19x/Attachments/Glock19x_Mag_50RND.nif";
        check(magazineGrabSupported(drum),"Drum authoring was excluded");
        drum.ownerRecord="Glock19x.esp:002e6c";
        check(!magazineGripFor(grip.weaponRecord,drum,{grip},bindings,{},error),"Unconfigured drum inherited an unrelated custom fit");
        const QHash<QString,QString> defaults{{grip.weaponRecord.toLower(),grip.pose.id}};
        const auto inherited=magazineGripFor(grip.weaponRecord,drum,{grip},bindings,defaults,error);
        check(inherited.has_value()&&sameGripAnchor(inherited->pose.placement,drum),"Default did not bind the concrete magazine");
        check(QJsonDocument::fromJson(encodeHandPose(inherited->pose,error))["fingerLocalTransforms"]==QJsonDocument::fromJson(encodeHandPose(grip.pose,error))["fingerLocalTransforms"],"Default changed finger transforms");
        check(reloadPoseDistance(inherited->pose.placement.handInAnchor,grip.pose.placement.handInAnchor)<.0001f,"Default lost hand rotation/position");
        auto custom=*inherited;custom.pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);custom.pose.placement.handInAnchor.translation.x+=3;
        auto assigned=bindings;assigned[magazineGrabKey(grip.weaponRecord,drum.ownerRecord)]=custom.pose.id;
        const auto resolved=magazineGripFor(grip.weaponRecord,drum,{grip,custom},assigned,defaults,error);
        check(resolved&&resolved->pose.id==custom.pose.id,"Weapon default overwrote an individual fit");
        auto wrong=drum;wrong.ownerModel="Different.nif";
        check(!magazineGripFor(grip.weaponRecord,wrong,{grip,custom},assigned,defaults,error)&&!error.isEmpty(),"Mismatched custom fit silently fell back to default");
        auto named=grip;named.pose.placement.anchorName="CustomBox";named.pose.placement.anchorPath="Assembly/CustomBox";
        const auto namedFit=magazineGripFor(grip.weaponRecord,named.pose.placement,{named},bindings,{},error);
        check(namedFit&&sameGripAnchor(namedFit->pose.placement,named.pose.placement),"Explicit magazine role still depends on a hardcoded part name");
        auto defaultRecipe=r;defaultRecipe.tracks[2].anchor=drum;
        defaultRecipe.id=reloadRecipeIdentity(defaultRecipe);
        const auto inheritedBundle=encodeReloadBundle(defaultRecipe,{grip},{},bindings,error,defaults);
        check(!inheritedBundle.isEmpty(),error);
        check(QJsonDocument::fromJson(inheritedBundle)["magazineGrabBindings"].toArray().size()==2,"Export omitted concrete default binding");
        auto rifle=r;
        rifle.tracks[0].anchor.anchorName="WeaponBolt";
        rifle.tracks[2].anchor.ownerModel=rifle.tracks[0].anchor.ownerModel;
        rifle.tracks[2].anchor.ownerRecord=rifle.tracks[0].anchor.ownerRecord;
        rifle.id=reloadRecipeIdentity(rifle);
        auto attachment=grip;attachment.pose.placement.anchorKind="installed-part";
        attachment.pose.placement.anchorName="Mag";attachment.pose.placement.anchorPath="Mag";
        const auto rifleBundle=encodeReloadBundle(rifle,{attachment},{},bindings,error,defaults);
        check(!rifleBundle.isEmpty(),error);
        check(QJsonDocument::fromJson(rifleBundle)["magazineGrabBindings"].toArray().size()==1,"AR magazine default was also assigned to its receiver");
    }
    void slowAndStatic(){
        auto r=capture();r.tracks.resize(1);r.tracks[0].parent=-1;
        for(int i=0;i<r.frames;++i)r.tracks[0].poses[i]=pose({0,.5f*ramp(r.duration*i/(r.frames-1),.4f,3.6f),0});
        QString error;check(identifyReloadRecipe(r,error),error);const auto slow=reloadMotionSpans(r,0);
        check(!slow.isEmpty()&&slow.back().last>400,"Slow continuous movement was classified as rest");
        for(auto& p:r.tracks[0].poses)p=pose({},37);
        check(identifyReloadRecipe(r,error),error);check(reloadMotionSpans(r,0).isEmpty(),"Quaternion rounding created fake movement in a static part");
        r.source["action"]="WPNFire";check(!identifyReloadRecipe(r,error),"Non-reload motion source accepted");
    }
    QByteArray read(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),path);return f.readAll();}
    void fixture(const QStringList& args){
        QString error;const auto rig=HkxReader::readSkeleton(read(args[1]),error);check(rig.valid(),error);const auto base=HkxReader::readClip(read(args[2]),error),clip=HkxReader::readClip(read(args[3]),error);check(base.valid()&&clip.valid(),error);
        QVector<Transformf> sampled,baseline,local,global;check(samplePose(rig,base,0,nullptr,sampled,baseline,global,error),error);
        ReloadRecipe r;r.duration=clip.durationSeconds;r.frames=int(std::ceil(r.duration*120))+1;r.source={{"action","WPNReload"}};QVector<int> bones;const int weapon=rig.boneIndex("Weapon");
        for(int i=0;i<rig.bones.size();++i){bool part=false;for(int p=rig.bones[i].parentIndex;p>=0;p=rig.bones[p].parentIndex)if(p==weapon){part=true;break;}if(part){ReloadTrack t;t.anchor={"animated-part",rig.bones[i].name,rig.bones[i].name,"Fixture.nif","Fixture.esp:123456",{}, {}};t.label=rig.bones[i].name;t.surface=true;r.tracks.append(t);bones.append(i);}}
        for(int i=0;i<bones.size();++i)r.tracks[i].parent=bones.indexOf(rig.bones[bones[i]].parentIndex);
        for(int i=0;i<r.frames;++i){check(samplePose(rig,clip,r.duration*i/(r.frames-1),&baseline,sampled,local,global,error),error);Transformf inverse;check(invertUniformTransform(global[weapon],inverse),"Invalid Weapon");for(int j=0;j<bones.size();++j)r.tracks[j].poses.append(composed(inverse,global[bones[j]]));}
        check(identifyReloadRecipe(r,error),error);
        int moving=0;
        for(int i=0;i<r.tracks.size();++i)moving+=reloadMotionSpans(r,i).isEmpty()?0:1;
        check(moving>0,"Fixture has no moving parts");
        QTextStream(stdout)<<"FIXTURE "<<args[3]<<" moving parts="<<moving<<" tracks="<<r.tracks.size()<<'\n';
        if(args.contains("--spans"))for(int i=0;i<r.tracks.size();++i)for(const auto& s:reloadMotionSpans(r,i))
            QTextStream(stdout)<<r.duration*s.first/(r.frames-1)<<".."<<r.duration*s.last/(r.frames-1)<<" "<<r.tracks[i].label<<'\n';
    }
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{motion();encoding();curve();slowAndStatic();bundles();if(app.arguments().size()>=4)fixture(app.arguments());QTextStream(stdout)<<"PASS reload part motion spans, visibility gaps, slow/static motion, reload-only identity, recipe format 2, curved/reverse dragging and self-contained grip exports";return 0;}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
