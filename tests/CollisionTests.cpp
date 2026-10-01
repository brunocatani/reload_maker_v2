#include "CollisionAuthoring.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <stdexcept>
#include <cmath>
using namespace reloadmaker;
void check(bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());}
void cube(CollisionMesh& m,QVector3D center){const auto base=quint32(m.vertices.size());for(int i=0;i<8;++i)m.vertices.append(center+QVector3D(i&1?1:-1,i&2?1:-1,i&4?1:-1));
    for(auto t:QVector<std::array<quint32,3>>{{0,2,3},{0,3,1},{4,5,7},{4,7,6},{0,1,5},{0,5,4},{2,6,7},{2,7,3},{0,4,6},{0,6,2},{1,3,7},{1,7,5}})m.triangles.append({t[0]+base,t[1]+base,t[2]+base});}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    CollisionProfile input;input.weapon="Test.esp:1";input.configuration="installed";input.target="magazine";CollisionMesh mesh;mesh.key="source";mesh.name="Two islands";cube(mesh,{-4,0,0});cube(mesh,{4,0,0});input.sources={mesh};
    auto result=bakeCollision(input);check(result.error.isEmpty(),result.error);check(result.profile.hulls.size()>=2,"Separate islands were bridged");
    for(const auto& h:result.profile.hulls){check(h.points.size()<=252,"Native point budget exceeded");float min=1e6,max=-1e6;for(auto p:h.points){min=std::min(min,p.x());max=std::max(max,p.x());}check(!(min<0&&max>0),"Preserved gap was filled");}
    auto decoded=CollisionProfile{};QString error;check(readCollisionJson(collisionJson(result.profile),decoded,error),error);check(decoded.hulls.size()==result.profile.hulls.size(),"Roundtrip hull count");check(decoded.sources[0].frame==mesh.frame,"Source frame changed");
    result.profile.hulls[0].enabled=false;check(readCollisionJson(collisionJson(result.profile),decoded,error)&&!decoded.hulls[0].enabled,"Disabled hull lost");
    auto cookProfile=result.profile;QMatrix4x4 root;root.translate(12,-8,4);root.rotate(43,QVector3D(0,0,1));QMatrix4x4 local;local.translate(2,3,4);cookProfile.sources[0].frame=root*local;
    QJsonArray rootFrame;for(int r=0;r<4;++r)for(int c=0;c<4;++c)rootFrame.append(root(r,c));cookProfile.referencePose["rootToWorld"]=rootFrame;
    const auto cooked=collisionCookInput(cookProfile,error);check(error.isEmpty(),error);check(cooked["hulls"].toArray().size()==cookProfile.hulls.size()-1,"Disabled hull reached native cooking");
    const auto actual=cooked["hulls"].toArray()[0].toObject()["points"].toArray()[0].toArray();const auto expected=local.map(cookProfile.hulls[1].points[0]);
    check((QVector3D(actual[0].toDouble(),actual[1].toDouble(),actual[2].toDouble())-expected).length()<.00001f,"Cooked points are not in magazine-root space");
    cookProfile.referencePose={};check(collisionCookInput(cookProfile,error).isEmpty()&&!error.isEmpty(),"Missing root frame silently cooked");
    auto before=collisionJson(decoded);auto invalid=before;auto hulls=invalid["hulls"].toArray();auto h=hulls[0].toObject();h["source"]="missing";hulls[0]=h;invalid["hulls"]=hulls;check(!readCollisionJson(invalid,decoded,error)&&collisionJson(decoded)==before,"Invalid import mutated output");
    invalid=before;auto sources=invalid["sources"].toArray();auto s=sources[0].toObject();QJsonArray zero;for(int i=0;i<16;++i)zero.append(0);s["sourceToWorld"]=zero;sources[0]=s;invalid["sources"]=sources;check(!readCollisionJson(invalid,decoded,error),"Singular frame accepted");
    auto stop=std::make_shared<std::atomic<bool>>(true);check(!bakeCollision(input,stop).error.isEmpty(),"Cancelled worker continued");
    CollisionHull plane;plane.points={{0,0,0},{1,0,0},{1,1,0},{0,1,0}};check(!collisionHullFaces(plane,error),"Flat hull accepted");
    CollisionHull curved;for(int row=1;row<17;++row)for(int col=0;col<32;++col){const double a=3.141592653589793*row/17,b=6.283185307179586*col/32;curved.points.append({float(std::sin(a)*std::cos(b)),float(std::sin(a)*std::sin(b)),float(std::cos(a))});}check(collisionHullFaces(curved,error),error);check(curved.faces.size()>100,"Curved envelope lost detail");
    QJsonArray points;for(int i=0;i<8;++i)points.append(QJsonArray{mesh.vertices[i].x(),mesh.vertices[i].y(),mesh.vertices[i].z()});QJsonObject legacy{{"hulls",QJsonArray{QJsonObject{{"points",points}}}},{"mass",.14},{"havokScale",70},{"radius",.0005}};
    const auto imported=importCollisionHullInput(legacy,input);check(imported.error.isEmpty(),imported.error);check(imported.profile.hulls[0].points[0]==mesh.vertices[0]&&imported.profile.settings.mass==.14,"Legacy units or mass changed");
    legacy["mass"]="heavy";check(!importCollisionHullInput(legacy,input).error.isEmpty(),"Invalid legacy mass accepted");legacy["mass"]=.14;legacy["layer"]=-1;check(!importCollisionHullInput(legacy,input).error.isEmpty(),"Invalid legacy layer accepted");
    int fixtures=0;if(argc>1){QDir dir(QString::fromLocal8Bit(argv[1]));check(dir.exists(),"Fixture folder missing");for(const auto& name:dir.entryList({"*.json"},QDir::Files)){QFile file(dir.filePath(name));check(file.open(QIODevice::ReadOnly),name);const auto doc=QJsonDocument::fromJson(file.readAll());if(!doc.object().contains("hulls"))continue;auto r=importCollisionHullInput(doc.object(),input);check(r.error.isEmpty(),name+": "+r.error);check(readCollisionJson(collisionJson(r.profile),decoded,error),name+": "+error);++fixtures;}}
    QTextStream(stdout)<<"PASS: gap preservation, point budgets, cancellation, convex envelope, atomic validation, source frames, disabled hulls, legacy units; "<<fixtures<<" DIAMOND fixtures\n";return 0;
}catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}}
