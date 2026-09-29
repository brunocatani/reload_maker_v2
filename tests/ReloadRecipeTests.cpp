#include "ReloadRecipe.h"
#include "ReloadDriveMath.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QQuaternion>
#include <QTemporaryDir>
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
    void windows(){
        auto r=capture();QString error;check(analyzeReloadWindows(r,error),error);check(r.windows.size()>=6,"Lost reload phases");
        bool lateReturn=false,rigid=false,timed=false,mag=false;
        for(const auto& w:r.windows){bool bolt=false,magazine=false;
            for(const auto& m:w.members){
                check(m.track!=5,"Stationary part grouped");
                if(m.track==0){bolt=true;}
                if(m.track==2){magazine=true;mag=true;}
                if(m.track==1&&m.relation==ReloadRelation::Rigid)rigid=true;
                if(m.track==4&&m.relation==ReloadRelation::Timed)timed=true;
                if(m.track==1||m.track==3)check(!m.grabbable,"Inherited follower became an independent grab by default");
                if(m.track==0&&float(w.first)/120>3.4f)lateReturn=true;
                for(int frame=w.first;frame<=w.last;++frame)check(reloadPoseVisible(r.tracks[m.track].poses[frame]),"Hidden teleport included in a drive window");
            }
            check(!(bolt&&magazine),"Separated bolt and magazine phases were grouped");
        }
        check(lateReturn&&rigid&&timed&&mag,"Missing return/rigid/co-timed/magazine evidence");
        int split=-1;for(int i=0;i<r.windows.size();++i)if(r.windows[i].last-r.windows[i].first>10){split=i;break;}
        check(split>=0,"No splittable window");const auto original=r.windows[split];r.windows[split].label="Lift bolt";r.windows[split].returnOnRelease=true;
        check(splitReloadWindow(r,split,(original.first+original.last)/2,error),error);
        check(r.windows[split].returnOnRelease&&r.windows[split+1].returnOnRelease,"Split lost release policy");
        check(mergeReloadWindow(r,split,error),error);check(r.windows[split].first==original.first&&r.windows[split].last==original.last,"Merge lost original interval");
        check(!splitReloadWindow(r,split,original.first,error),"Zero-length split accepted");
    }
    void persistence(){
        auto r=capture();QString error;check(analyzeReloadWindows(r,error),error);const auto id=r.id;QTemporaryDir dir;ReloadRecipeLibrary library(dir.path());
        check(library.save(r,error),error);auto stale=r;r.windows[0].label="Lift the handle";r.tracks[0].label="Bolt handle";
        r.windows[0].members[0].grabbable=false;r.windows[0].members[0].gripId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        check(library.save(r,error),error);check(!library.save(stale,error),"Stale editor overwrote reload setup");
        ReloadRecipe loaded;check(library.load(id,loaded,error),error);check(loaded.windows[0].label==r.windows[0].label&&loaded.revision==2,"Lost edit after restart");
        check(!loaded.windows[0].members[0].grabbable&&loaded.windows[0].members[0].gripId==r.windows[0].members[0].gripId,"Lost grab/hand assignment after restart");
        check(std::abs(loaded.tracks[0].poses[30].scale.x-1.3f)<.0001f,"Scale was reset to one");check(loaded.tracks[2].poses[210].scale.x==0,"Hidden scale lost");
        check(loaded.id==id&&loaded.tracks[0].label=="Bolt handle","Labels changed source identity or were lost");r.source["animationSha256"]="override-b";check(reloadRecipeIdentity(r)!=id,"Animation override reused stale source identity");
        check(encodeReloadRecipe(r,error).isEmpty(),"Changed source saved under old identity");
        auto json=QJsonDocument::fromJson(encodeReloadRecipe(loaded,error)).object();json["version"]=2;
        check(!decodeReloadRecipe(QJsonDocument(json).toJson(),r,error),"Unknown recipe version accepted");
    }
    void curve(){
        const QVector<QPointF> points{{0,0},{10,0},{10,10},{20,10}};const QVector<float> times{0,1,2,3};
        check(std::abs(reloadCurveTime(points,times,{10,5},1)-1.5f)<.001f,"Corner path collapsed to a straight axis");
        check(std::abs(reloadCurveTime(points,times,{15,10},2)-2.5f)<.001f,"Curved path time wrong");
        check(std::abs(reloadCurveTime(points,times,{5,0},2.5f)-.5f)<.001f,"Reverse drag failed");
        const QVector<QPointF> crossing{{0,0},{10,0},{0,0}};check(std::abs(reloadCurveTime(crossing,{0,1,2},{5,0},1.6f)-1.5f)<.001f,"Crossing ignored temporal continuity");
    }
    void slowAndStatic(){
        auto r=capture();r.tracks.resize(1);r.tracks[0].parent=-1;
        for(int i=0;i<r.frames;++i)r.tracks[0].poses[i]=pose({0,.5f*ramp(r.duration*i/(r.frames-1),.4f,3.6f),0});
        QString error;check(analyzeReloadWindows(r,error),error);check(!r.windows.isEmpty()&&r.windows.back().last>400,"Slow continuous movement was classified as rest");
        for(auto& p:r.tracks[0].poses)p=pose({},37);
        check(analyzeReloadWindows(r,error),error);check(r.windows.isEmpty(),"Quaternion rounding created fake movement in a static part");
        r.source["action"]="WPNFire";check(!analyzeReloadWindows(r,error),"Non-reload motion source accepted");
    }
    void mergeSafety(){
        auto r=capture();QString error;check(analyzeReloadWindows(r,error),error);
        r.windows[0].returnOnRelease=true;const int count=r.windows.size();check(!mergeReloadWindow(r,0,error)&&r.windows.size()==count,"Merge silently discarded a release rule");
        r.windows[0].returnOnRelease=false;
        for(int i=0;i+1<r.windows.size();++i){
            bool firstMag=false,nextMag=false;for(const auto& m:r.windows[i].members)firstMag|=m.track==2;for(const auto& m:r.windows[i+1].members)nextMag|=m.track==2;
            if(firstMag&&nextMag&&r.windows[i].last<210&&r.windows[i+1].first>210){check(!mergeReloadWindow(r,i,error),"Merge interpolated across a hidden magazine");return;}
        }
        check(false,"No visibility-gap test windows found");
    }
    QByteArray read(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),path);return f.readAll();}
    void fixture(const QStringList& args){
        QString error;const auto rig=HkxReader::readSkeleton(read(args[1]),error);check(rig.valid(),error);const auto base=HkxReader::readClip(read(args[2]),error),clip=HkxReader::readClip(read(args[3]),error);check(base.valid()&&clip.valid(),error);
        QVector<Transformf> sampled,baseline,local,global;check(samplePose(rig,base,0,nullptr,sampled,baseline,global,error),error);
        ReloadRecipe r;r.duration=clip.durationSeconds;r.frames=int(std::ceil(r.duration*120))+1;r.source={{"action","WPNReload"}};QVector<int> bones;const int weapon=rig.boneIndex("Weapon");
        for(int i=0;i<rig.bones.size();++i){bool part=false;for(int p=rig.bones[i].parentIndex;p>=0;p=rig.bones[p].parentIndex)if(p==weapon){part=true;break;}if(part){ReloadTrack t;t.anchor={"animated-part",rig.bones[i].name,rig.bones[i].name,"Fixture.nif","Fixture.esp:123456",{}, {}};t.label=rig.bones[i].name;t.surface=true;r.tracks.append(t);bones.append(i);}}
        for(int i=0;i<bones.size();++i)r.tracks[i].parent=bones.indexOf(rig.bones[bones[i]].parentIndex);
        for(int i=0;i<r.frames;++i){check(samplePose(rig,clip,r.duration*i/(r.frames-1),&baseline,sampled,local,global,error),error);Transformf inverse;check(invertUniformTransform(global[weapon],inverse),"Invalid Weapon");for(int j=0;j<bones.size();++j)r.tracks[j].poses.append(composed(inverse,global[bones[j]]));}
        check(analyzeReloadWindows(r,error),error);check(!r.windows.isEmpty(),"Fixture has no motion windows");
        QTextStream(stdout)<<"FIXTURE "<<args[3]<<" windows="<<r.windows.size()<<" tracks="<<r.tracks.size()<<'\n';
        if(args.contains("--windows"))for(const auto& w:r.windows){QStringList members;for(const auto& m:w.members)members.append(r.tracks[m.track].label);QTextStream(stdout)<<r.duration*w.first/(r.frames-1)<<".."<<r.duration*w.last/(r.frames-1)<<" "<<members.join(", ")<<'\n';}
    }
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{windows();persistence();curve();slowAndStatic();mergeSafety();if(app.arguments().size()>=4)fixture(app.arguments());QTextStream(stdout)<<"PASS complete reload phases, co-timed/rigid groups, visibility gaps, slow/static motion, reload-only identity, saved labels/grabs, safe split/merge and curved/reverse dragging\n";return 0;}catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}}
