#include "PartGrip.h"
#include "ScenePoseBinding.h"
#include "MechanismScene.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QQuaternion>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <QVector3D>
#include <cmath>
#include <stdexcept>
using namespace reloadmaker;
namespace {
    void require(bool ok,const QString& error) { if(!ok) throw std::runtime_error(error.toStdString()); }
    float distance(Vec3f a,Vec3f b) { return QVector3D(a.x-b.x,a.y-b.y,a.z-b.z).length(); }
    Transformf tf(Vec3f p={},float angle=0) {
        auto q=QQuaternion::fromEulerAngles(angle,angle*.7f,-angle*.3f);
        return {p,{q.x(),q.y(),q.z(),q.scalar()},{1,1,1}};
    }
    void expect(const Transformf& a,const Transformf& b) {
        require(distance(a.translation,b.translation)<.002f,"Translation mismatch");
        require(distance(a.scale,b.scale)<.002f,"Scale mismatch");
        for(auto axis:{Vec3f{1,0,0},Vec3f{0,1,0},Vec3f{0,0,1}})
            require(distance(rotated(a.rotation,axis),rotated(b.rotation,axis))<.002f,"Rotation mismatch");
    }
    HandPose pose(PoseHand side) {
        HandPose p; p.id=QUuid::createUuid().toString(QUuid::WithoutBraces); p.name="Bolt grip"; p.hand=side;
        p.createdUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        p.placement={"animated-part","WeaponBolt","Root/WeaponBolt","Weapons/Test/receiver.nif","Test.esp:012345",tf({1,2,3},35),tf({4,5,6},17)};
        for(int i=0;i<15;++i) p.fingers[i]=tf({float(i)*.02f,.3f,0},float(i)*5);
        return p;
    }
    HkxSkeleton rig() {
        HkxSkeleton s; s.name="Root"; s.bones.append({"Root",-1,false,tf()});
        for(auto side:{PoseHand::Left,PoseHand::Right}) {
            const QString prefix=side==PoseHand::Left?"LArm_":"RArm_";
            const int upper=s.bones.size(); s.bones.append({prefix+"UpperArm",0,false,tf({side==PoseHand::Left?-8.f:8.f,0,0})});
            s.bones.append({prefix+"ForeArm1",qint16(upper),false,tf({0,0,10},25)});
            s.bones.append({prefix+"ForeArm2",qint16(upper+1),false,tf({0,0,3})});
            s.bones.append({prefix+"ForeArm3",qint16(upper+2),false,tf({0,0,3})});
            const int hand=s.bones.size(); s.bones.append({prefix+"Hand",qint16(upper+3),false,tf({0,0,4})});
            for(int i=0;i<15;++i) s.bones.append({handPoseBoneName(side,i),qint16(i%3==0?hand:s.bones.size()-1),false,tf({0,0,.2f})});
        }
        const int weapon=s.bones.size(); s.bones.append({"Weapon",qint16(s.boneIndex("RArm_Hand")),false,tf({2,-4,5},43)});
        s.bones.append({"WeaponBolt",qint16(weapon),false,tf({2,0,0})});
        const int hidden=s.bones.size(); s.bones.append({"WeaponExtra1",qint16(weapon),false,{{},{},{0,0,0}}});
        s.bones.append({"WeaponExtra2",qint16(hidden),false,tf({1,0,0})});
        return s;
    }
    void solveCase(const HkxSkeleton& s,const QVector<Transformf>& local,PoseHand side) {
        QString error; QVector<Transformf> global;
        require(buildPoseGlobals(s,local,global,error),error);
        const QString prefix=side==PoseHand::Left?"LArm_":"RArm_";
        const int hand=s.boneIndex(prefix+"Hand"), upper=s.boneIndex(prefix+"UpperArm"), fore=s.boneIndex(prefix+"ForeArm1");
        auto p=pose(side);
        require(captureHandPose(s,local,global,global[s.boneIndex("Weapon")],p,error),error);
        for(int variant:{0,1,2}) {
            const bool move=variant!=0;
            auto target=global[hand];
            if(move) {
                const float weight=variant==2?-.9f:.9f;
                target.translation={target.translation.x*weight+global[upper].translation.x*(1-weight),
                    target.translation.y*weight+global[upper].translation.y*(1-weight),target.translation.z*weight+global[upper].translation.z*(1-weight)};
                target.rotation=multiplied(tf({},33).rotation,target.rotation);
            }
            QVector<Transformf> outLocal,outGlobal,rebuilt;
            require(solvePartGrip(s,local,global,p,target,outLocal,outGlobal,error),error);
            expect(outGlobal[hand],target);
            require(distance(outGlobal[upper].translation,global[upper].translation)<.001f,"Shoulder moved");
            require(std::abs(distance(global[fore].translation,global[upper].translation)-distance(outGlobal[fore].translation,outGlobal[upper].translation))<.002f,"Upper arm stretched");
            require(std::abs(distance(global[hand].translation,global[fore].translation)-distance(outGlobal[hand].translation,outGlobal[fore].translation))<.002f,"Forearm stretched");
            require(buildPoseGlobals(s,outLocal,rebuilt,error),error);
            for(int i=0;i<s.bones.size();++i) {
                expect(rebuilt[i],outGlobal[i]);
                if(!s.bones[i].name.startsWith(prefix)) expect(global[i],outGlobal[i]);
                if(!move) expect(global[i],outGlobal[i]);
            }
            QVector<ScenePoseNode> nodes; QVector<int> canonical;
            for(int i=0;i<s.bones.size();++i) {
                const bool visible=std::abs(global[i].scale.x)>.0001f;
                nodes.append({s.bones[i].parentIndex,visible?i:-1,local[i]}); canonical.append(visible?i:-1);
            }
            QVector<ScenePoseBinding> bindings;
            require(compileSceneBindings(s,s.boneIndex("Weapon"),nodes,canonical,bindings,error),error);
            QVector<Transformf> nativeBound,previewBound,boundLocal;
            require(evaluateSceneBindings(s,bindings,local,local,global,boundLocal,nativeBound,error),error);
            require(evaluateSceneBindings(s,bindings,local,local,outGlobal,boundLocal,previewBound,error),error);
            for(int i=0;i<bindings.size();++i) {
                const auto bone=bindings[i].boneIndex;
                if(!s.bones[bone].name.startsWith(prefix)) expect(nativeBound[i],previewBound[i]);
                if(bone==hand) expect(previewBound[i],target);
            }
            // Equivariance: changing the display/world frame must not change the grip.
            const auto frame=tf({23,-14,9},67); auto movedLocal=local,movedGlobal=global;
            require(reframePose(s,{},frame,movedLocal,movedGlobal,error),error);
            QVector<Transformf> againLocal,againGlobal;
            require(solvePartGrip(s,movedLocal,movedGlobal,p,composed(frame,target),againLocal,againGlobal,error),error);
            for(int i=0;i<outGlobal.size();++i) expect(againGlobal[i],composed(frame,outGlobal[i]));
            auto unreachable=target; unreachable.translation.x+=10000;
            const auto before=outGlobal;
            require(!solvePartGrip(s,local,global,p,unreachable,outLocal,outGlobal,error),"Unreachable target accepted");
            for(int i=0;i<outGlobal.size();++i) expect(outGlobal[i],before[i]);
        }
    }
    void movingPartGrip(const HkxSkeleton& s,const QVector<Transformf>& local,PoseHand side){
        QString error;QVector<Transformf> global;
        require(buildPoseGlobals(s,local,global,error),error);
        const int hand=s.boneIndex(side==PoseHand::Left?"LArm_Hand":"RArm_Hand");
        const int upper=s.boneIndex(side==PoseHand::Left?"LArm_UpperArm":"RArm_UpperArm");
        const int weapon=s.boneIndex("Weapon"),bolt=s.boneIndex("WeaponBolt");
        auto grip=pose(side);
        require(captureHandPose(s,local,global,global[bolt],grip,error),error);
        QVector<ScenePoseNode> nodes;QVector<int> canonical;
        for(int i=0;i<s.bones.size();++i){
            const int bone=std::abs(global[i].scale.x)>.0001f?i:-1;
            nodes.append({s.bones[i].parentIndex,bone,local[i]});canonical.append(bone);
        }
        QVector<ScenePoseBinding> bindings;MechanismScenePlan plan;
        require(compileSceneBindings(s,weapon,nodes,canonical,bindings,error),error);
        require(compileMechanismScene(nodes,bindings,{bolt},plan,error),error);
        QVector<Transformf> boundLocal,boundGlobal,sceneLocal,world;
        auto targetHand=global[hand];
        targetHand.translation={global[upper].translation.x+(targetHand.translation.x-global[upper].translation.x)*.8f,
            global[upper].translation.y+(targetHand.translation.y-global[upper].translation.y)*.8f,
            global[upper].translation.z+(targetHand.translation.z-global[upper].translation.z)*.8f};
        Transformf inverse;
        require(invertUniformTransform(grip.placement.handInAnchor,inverse),"Invalid grip frame");
        const auto targetPart=composed(targetHand,inverse);
        require(evaluateSceneBindings(s,bindings,local,local,global,boundLocal,boundGlobal,error),error);
        require(nativeMechanismScene(plan,bindings,boundLocal,sceneLocal,world,error),error);
        require(moveMechanismScene(plan,{targetPart},sceneLocal,world,error),error);
        QVector<Transformf> posedLocal,posedGlobal;
        require(solvePartGrip(s,local,global,grip,composed(world[bolt],grip.placement.handInAnchor),posedLocal,posedGlobal,error),error);
        // Controller re-evaluates native part bindings after solving the arm, then
        // reapplies only the explicit mechanism targets. Neither can drive Weapon.
        require(evaluateSceneBindings(s,bindings,local,local,posedGlobal,boundLocal,boundGlobal,error),error);
        require(nativeMechanismScene(plan,bindings,boundLocal,sceneLocal,world,error),error);
        require(moveMechanismScene(plan,{targetPart},sceneLocal,world,error),error);
        expect(world[hand],targetHand);expect(world[bolt],targetPart);expect(world[weapon],global[weapon]);
        expect(world[s.boneIndex(side==PoseHand::Left?"RArm_Hand":"LArm_Hand")],global[s.boneIndex(side==PoseHand::Left?"RArm_Hand":"LArm_Hand")]);
        // Released pose and STOP both reconstruct native scene state from immutable data.
        require(evaluateSceneBindings(s,bindings,local,local,global,boundLocal,boundGlobal,error),error);
        require(nativeMechanismScene(plan,bindings,boundLocal,sceneLocal,world,error),error);
        for(int i=0;i<world.size();++i)expect(world[i],global[i]);
    }
    void freeMagazineReach(const HkxSkeleton& s,const QVector<Transformf>& local,PoseHand side) {
        QString error;QVector<Transformf> global,outLocal,outGlobal,rebuilt;require(buildPoseGlobals(s,local,global,error),error);
        const QString prefix=side==PoseHand::Left?"LArm_":"RArm_";
        const int hand=s.boneIndex(prefix+"Hand"),fore=s.boneIndex(prefix+"ForeArm1"),upper=s.boneIndex(prefix+"UpperArm");
        auto target=global[hand];target.translation.x+=100;target.translation.z-=60;
        const auto p=pose(side);
        require(!solvePartGrip(s,local,global,p,target,outLocal,outGlobal,error),"Strict authoring reach was weakened");
        require(solvePartGrip(s,local,global,p,target,outLocal,outGlobal,error,GripReach::FollowTarget),error);
        expect(outGlobal[hand],target);
        require(std::abs(distance(global[fore].translation,global[upper].translation)-distance(outGlobal[fore].translation,outGlobal[upper].translation))<.002f,"Free grab stretched upper arm");
        require(std::abs(distance(global[hand].translation,global[fore].translation)-distance(outGlobal[hand].translation,outGlobal[fore].translation))<.002f,"Free grab stretched forearm");
        require(buildPoseGlobals(s,outLocal,rebuilt,error),error);
        for(int i=0;i<s.bones.size();++i){expect(rebuilt[i],outGlobal[i]);if(!s.bones[i].name.startsWith(prefix))expect(global[i],outGlobal[i]);}
    }
    QByteArray read(const QString& path) { QFile f(path); require(f.open(QIODevice::ReadOnly),path); return f.readAll(); }
    void storage() {
        QTemporaryDir dir; require(dir.isValid(),"temp dir unavailable");
        PartGripLibrary library(dir.path()); PartGrip g{pose(PoseHand::Right),"Rifle.esp:123456",QUuid::createUuid().toString(QUuid::WithoutBraces)};
        QString error; require(library.save(g,error),error); require(!library.save(g,error),"Overwrite accepted");
        QStringList diagnostics; auto loaded=PartGripLibrary(dir.path()).load(diagnostics);
        require(loaded.size()==1 && diagnostics.isEmpty(),"Restart lost grip");
        expect(g.pose.placement.handInAnchor,loaded[0].pose.placement.handInAnchor);
        require(resolveGripAnchor(g,"rifle.ESP:123456",{g.pose.placement},error)==0,error);
        require(resolveGripAnchor(g,"Other.esp:123456",{g.pose.placement},error)<0,"Wrong weapon matched");
        auto wrong=g.pose.placement; wrong.ownerRecord="Wrong.esp:123456";
        require(resolveGripAnchor(g,g.weaponRecord,{wrong},error)<0,"Wrong part owner matched");
        require(resolveGripAnchor(g,g.weaponRecord,{g.pose.placement,g.pose.placement},error)<0,"Ambiguous part matched");
        auto json=QJsonDocument::fromJson(encodePartGrip(g,error)).object(); json["version"]=2;
        PartGrip unchanged=g; require(!decodePartGrip(QJsonDocument(json).toJson(),unchanged,error),"Unknown version accepted");
        require(unchanged.pose.id==g.pose.id,"Failure mutated grip");
    }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto s=rig(); QVector<Transformf> local; for(auto& bone:s.bones) local.append(bone.referenceLocal);
        for(auto side:{PoseHand::Left,PoseHand::Right}) solveCase(s,local,side);
        for(auto side:{PoseHand::Left,PoseHand::Right}) movingPartGrip(s,local,side);
        for(auto side:{PoseHand::Left,PoseHand::Right}) freeMagazineReach(s,local,side);
        for(auto side:{PoseHand::Left,PoseHand::Right}) local[s.boneIndex(side==PoseHand::Left?"LArm_ForeArm1":"RArm_ForeArm1")].rotation={};
        for(auto side:{PoseHand::Left,PoseHand::Right}) solveCase(s,local,side);
        storage();
        if(app.arguments().size()>1) {
            const auto args=app.arguments(); require(args.size()>=4,"usage: PartGripTests skeleton idle action..."); QString error;
            const auto rig=HkxReader::readSkeleton(read(args[1]),error); require(rig.valid(),error);
            const auto idle=HkxReader::readClip(read(args[2]),error); require(idle.valid(),error);
            QVector<Transformf> tracks,base,global; require(samplePose(rig,idle,0,nullptr,tracks,base,global,error),error);
            for(int arg=3;arg<args.size();++arg) {
                const auto clip=HkxReader::readClip(read(args[arg]),error); require(clip.valid(),error);
                for(float fraction:{0.f,.25f,.5f,.75f,1.f}) {
                    QVector<Transformf> local; require(samplePose(rig,clip,clip.durationSeconds*fraction,&base,tracks,local,global,error),error);
                    for(auto side:{PoseHand::Left,PoseHand::Right}) solveCase(rig,local,side);
                }
            }
        }
        QTextStream(stdout)<<"PASS part grip persistence, owner resolution, arm lengths, exact wrist rotation, frame invariance, moved-part hand attachment, unchanged weapon/other hand and failure safety\n"; return 0;
    } catch(const std::exception& e) { QTextStream(stderr)<<"FAIL "<<e.what()<<'\n'; return 1; }
}
