#include "PartMechanism.h"
#include "MechanismScene.h"
#include "PartGrip.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <QVector3D>
#include <QQuaternion>
#include <cmath>
#include <stdexcept>
using namespace reloadmaker;
namespace {
    void check(bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());}
    void near(float a,float b){check(std::abs(a-b)<.001f,"Value mismatch");}
    void same(Transformf a,Transformf b){near(a.translation.x,b.translation.x);near(a.translation.y,b.translation.y);near(a.translation.z,b.translation.z);for(auto axis:{Vec3f{1,0,0},Vec3f{0,1,0},Vec3f{0,0,1}}){auto x=rotated(a.rotation,axis),y=rotated(b.rotation,axis);near(x.x,y.x);near(x.y,y.y);near(x.z,y.z);}}
    PartMechanism bolt(){PartMechanism p;p.id=QUuid::createUuid().toString(QUuid::WithoutBraces);p.name="Manual bolt";p.weaponRecord="Rifle.esp:123456";p.anchor={"animated-part","WeaponBolt","Root/WeaponBolt","Weapons/Rifle.nif","Rifle.esp:654321",{}, {}};p.stages={{"Lift","Lower",true,1,0,90,{},ReleaseRule::Hold,90},{"Pull","Push",false,1,0,-10,{},ReleaseRule::Hold,10}};return p;}
    void paths(){auto p=bolt();QString error;check(validateMechanism(p,error),error);same(mechanismDelta(p,0),{});auto up=mechanismDelta(p,1),back=mechanismDelta(p,2);near(up.translation.y,0);near(back.translation.y,-10);same(mechanismDelta(p,1),up);same(mechanismDelta(p,0),{});
        MechanismState state{1.5f,false,0,1};releaseMechanism(p,state);check(!state.returning,"Manual bolt returned on release");advanceMechanism(p,state,2);near(state.path,1.5f);
        p.stages={{"Pull","Forward",false,1,0,-10,{},ReleaseRule::Rest,20}};state={.6f,false,0,0};releaseMechanism(p,state);check(state.returning,"Spring return did not start");advanceMechanism(p,state,.1f);near(state.path,.4f);advanceMechanism(p,state,2);near(state.path,0);check(!state.returning,"Spring return never stopped");
        p=bolt();p.stages[1].release=ReleaseRule::StageStart;state={1.6f,false,0,1};releaseMechanism(p,state);advanceMechanism(p,state,10);near(state.path,1);same(mechanismDelta(p,state.path),up);
        p.stages[1].release=ReleaseRule::Rest;state={1.6f,false,0,1};releaseMechanism(p,state);advanceMechanism(p,state,10);near(state.path,0);
        p.stages[0].to=0;check(!validateMechanism(p,error),"Zero-length stage accepted");
    }
    void persistence(){QTemporaryDir dir;check(dir.isValid(),"No temp directory");MechanismLibrary library(dir.path());auto p=bolt();QString error;check(library.save(p,error),error);check(p.revision==1,"Initial revision missing");auto stale=p;p.name="Changed shared part";check(library.save(p,error),error);check(p.revision==2,"Revision not incremented");check(!library.save(stale,error),"Stale editor overwrote shared definition");QStringList warnings;auto saved=library.load(warnings);check(saved.size()==1&&warnings.isEmpty()&&saved[0].name==p.name,"Shared definition lost after restart");auto duplicate=p;duplicate.id=QUuid::createUuid().toString(QUuid::WithoutBraces);duplicate.revision=0;check(!library.save(duplicate,error),"Duplicate shared part accepted");auto json=QJsonDocument::fromJson(encodeMechanism(p,error)).object();json["version"]=2;check(!decodeMechanism(QJsonDocument(json).toJson(),stale,error),"Unknown version accepted");}
    void scene(){QVector<ScenePoseNode> nodes{{-1,-1,{}},{0,-1,{{1,2,3}}},{1,-1,{{0,4,0}}},{0,-1,{{8,0,0}}},{3,-1,{{0,1,0}}},{0,-1,{{-7,0,0}}}};MechanismScenePlan plan;QString error;check(compileMechanismScene(nodes,{}, {1,3},plan,error),error);QVector<Transformf> local,world;check(nativeMechanismScene(plan,{}, {},local,world,error),error);const auto original=world;check(moveMechanismScene(plan,{{{1,12,3}},{{8,10,0}}},local,world,error),error);near(world[1].translation.y,12);near(world[2].translation.y,16);near(world[4].translation.y,11);same(world[0],original[0]);same(world[5],original[5]);check(!compileMechanismScene(nodes,{}, {1,2},plan,error),"Overlapping groups accepted");}
    void pivotsAndFrames(){
        auto p=bolt();p.stages[0].axis=2;p.stages[0].pivot={1,0,0};p.stages[1].axis=0;
        const auto lifted=mechanismDelta(p,1),pulled=mechanismDelta(p,2);
        near(lifted.translation.x,1);near(lifted.translation.y,-1);
        // Stage two uses the lifted frame's X axis, which is now world +Y.
        near(pulled.translation.x,1);near(pulled.translation.y,-11);
        same(composed(lifted,Transformf{{1,0,0}}),Transformf{{1,0,0},lifted.rotation});
        const auto q=QQuaternion::fromEulerAngles(23,61,-34);
        Transformf frame{{12,-7,31},{q.x(),q.y(),q.z(),q.scalar()},{2,2,2}},inverse;
        check(invertUniformTransform(frame,inverse),"Cannot invert test frame");
        for(float path:{0.f,.25f,1.f,1.8f,2.f,1.f,0.f})
            same(composed(inverse,composed(frame,mechanismDelta(p,path))),mechanismDelta(p,path));
        p.stages[1].release=ReleaseRule::Rest;
        MechanismState state{2,false,0,1};releaseMechanism(p,state);
        advanceMechanism(p,state,1.5f);near(state.path,.5f);
        state.returning=false;const auto grabbed=mechanismDelta(p,state.path);
        advanceMechanism(p,state,10);same(mechanismDelta(p,state.path),grabbed);
        p.stages[0].release=ReleaseRule::Rest;releaseMechanism(p,state);
        advanceMechanism(p,state,1);near(state.path,0);
    }
    void semanticFollowersAndRestore(){
        // Imported child is physically elsewhere but inherits the master animation frame.
        QVector<ScenePoseNode> nodes{{-1,-1,{}},{0,-1,{{2,0,0}}},{0,-1,{{4,0,0}}},
            {2,-1,{{0,1,0},{},{0,0,0}}},{3,-1,{{0,2,0}}},{0,-1,{{-8,0,0}}}};
        QVector<ScenePoseBinding> bindings(2);
        bindings[0].nodeIndex=1;bindings[0].weaponPart=true;
        bindings[1].nodeIndex=2;bindings[1].weaponPart=true;bindings[1].semanticParentBinding=0;
        QVector<Transformf> bound{nodes[1].restLocal,nodes[2].restLocal},local,world;
        MechanismScenePlan plan;QString error;
        check(compileMechanismScene(nodes,bindings,{1},plan,error),error);
        check(plan.group[2]==0&&plan.group[4]==0&&plan.group[5]==-1,"Semantic descendants not inherited");
        check(nativeMechanismScene(plan,bindings,bound,local,world,error),error);const auto original=world;
        check(moveMechanismScene(plan,{{{2,5,0}}},local,world,error),error);
        near(world[2].translation.y,5);near(world[4].translation.y,6);near(world[4].scale.x,0);
        same(world[0],original[0]);same(world[5],original[5]);
        // Stop rebuilds from immutable rest plus native evaluated locals, not moved renderer nodes.
        check(nativeMechanismScene(plan,bindings,bound,local,world,error),error);
        for(int i=0;i<world.size();++i){same(world[i],original[i]);near(world[i].scale.x,original[i].scale.x);}
        check(!compileMechanismScene(nodes,bindings,{1,2},plan,error),"Inherited follower applied twice");
        bindings[0].nodeIndex=100;
        check(!compileMechanismScene(nodes,bindings,{1},plan,error),"Invalid binding accepted");
    }
    void sharedGripLinks(){
        QTemporaryDir directory;check(directory.isValid(),"No temporary library");
        MechanismLibrary library(directory.path());auto p=bolt();QString error;check(library.save(p,error),error);
        auto alias=p;alias.anchor.anchorKind="installed-part";alias.id=QUuid::createUuid().toString(QUuid::WithoutBraces);alias.revision=0;
        check(sameMechanismPart(p.anchor,alias.anchor),"Same-frame labels created separate mechanism identities");
        check(!library.save(alias,error),"Alias created conflicting shared part definition");
        PartGrip left,right;
        left.mechanismId=right.mechanismId=p.id;
        // Grips carry only the ID, so a revised shared rule is visible to both.
        p.stages[0].release=ReleaseRule::Rest;check(library.save(p,error),error);
        QStringList errors;const auto loaded=library.load(errors);check(errors.isEmpty()&&loaded.size()==1,"Missing shared definition");
        check(loaded[0].id==left.mechanismId&&loaded[0].id==right.mechanismId&&loaded[0].stages[0].release==ReleaseRule::Rest,"Per-hand movement rules diverged");
        auto wrong=alias.anchor;wrong.ownerModel="Weapons/Other.nif";
        check(!sameMechanismPart(p.anchor,wrong),"Different model shares a mechanism");
    }
    void endpointAuthoring(){
        auto p=bolt();auto stage=p.stages[1];
        stage.from=0;stage.to=0;
        stage=mechanismStageFromDrag(stage,1,false,-7);near(stage.to,-7);
        // A second drag starts from the existing endpoint, not from zero.
        auto next=mechanismStageFromDrag(stage,1,false,-2);near(next.to,-9);
        // Choosing another axis/tool explicitly starts its travel at the stage start.
        next=mechanismStageFromDrag(next,2,true,65);near(next.to,65);check(next.axis==2&&next.rotation,"Wrong teaching axis/tool");
        p.stages[1]=next;QString error;check(validateMechanism(p,error),error);
        const auto bytes=encodeMechanism(p,error);PartMechanism restored;
        check(decodeMechanism(bytes,restored,error),error);same(mechanismDelta(p,2),mechanismDelta(restored,2));
        next=mechanismStageFromDrag(next,2,true,-65);near(next.to,0);p.stages[1]=next;
        check(!validateMechanism(p,error),"Unfinished zero-travel authoring step was saveable");
        next=mechanismStageFromDrag(next,2,true,20000);near(next.to,10000);
        // Restoring the drag-start value (Esc) does not change earlier steps.
        p=bolt();const auto previous=mechanismDelta(p,1);p.stages[1]=stage;same(mechanismDelta(p,1),previous);
    }
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{paths();persistence();scene();pivotsAndFrames();semanticFollowersAndRestore();sharedGripLinks();endpointAuthoring();QTextStream(stdout)<<"PASS mouse endpoint authoring, ordered/reverse stages, pivot/frame invariance, partial release/regrab, hold/return rules, shared revision/alias safety, follower inheritance and native scene restoration\n";return 0;}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
