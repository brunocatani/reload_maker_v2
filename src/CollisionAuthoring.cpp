#include "CollisionAuthoring.h"
#pragma push_macro("slots")
#undef slots
#include "physics-interaction/weapon/WeaponGapDecomposition.h"
#include "physics-interaction/weapon/WeaponGeometry.h"
#pragma pop_macro("slots")
#include "RockCollisionProvenance.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
extern "C" {
#include "libqhull_r/qhull_ra.h"
}

namespace reloadmaker {
namespace {
struct Point {float x{},y{},z{};};
using Face=std::array<quint32,3>;
QJsonArray vector(const QVector3D& p){return {p.x(),p.y(),p.z()};}
bool finite(double n,double bound=1e6){return std::isfinite(n)&&std::abs(n)<=bound;}
bool readPoint(const QJsonValue& value,QVector3D& out){const auto a=value.toArray();if(a.size()!=3)return false;for(int i=0;i<3;++i){if(!a[i].isDouble()||!finite(a[i].toDouble()))return false;out[i]=float(a[i].toDouble());}return true;}
QJsonObject settingsJson(const CollisionSettings& s){return {{"gapGame",s.gap},{"dedupGame",s.dedup},{"supportErrorGame",s.error},{"targetPoints",s.targetPoints},{"radiusHavok",s.radius},{"havokScale",s.scale},{"massKg",s.mass},{"material",double(s.material)},{"layer",int(s.layer)}};}
bool readSettings(const QJsonObject& o,CollisionSettings& s,QString& error){
    for(const auto* k:{"gapGame","dedupGame","supportErrorGame","targetPoints","radiusHavok","havokScale","massKg","material","layer"})if(!o[k].isDouble()){error="Collision settings require numeric values";return false;}
    s.gap=o["gapGame"].toDouble();s.dedup=o["dedupGame"].toDouble();s.error=o["supportErrorGame"].toDouble();s.targetPoints=o["targetPoints"].toInt(-1);s.radius=o["radiusHavok"].toDouble();s.scale=o["havokScale"].toDouble();s.mass=o["massKg"].toDouble();
    const auto material=o["material"].toDouble(),layer=o["layer"].toDouble();if(material<0||material>4294967295.||std::floor(material)!=material||layer<0||layer>63||std::floor(layer)!=layer){error="Invalid collision material/layer";return false;}s.material=quint32(material);s.layer=quint32(layer);return collisionSettingsValid(s,error);
}
std::vector<Point> dedup(const std::vector<Point>& points,double grid){std::set<std::array<qint64,3>> seen;std::vector<Point> result;result.reserve(points.size());for(auto p:points)if(seen.insert({std::llround(p.x/float(grid)),std::llround(p.y/float(grid)),std::llround(p.z/float(grid))}).second)result.push_back(p);return result;}
bool volume(const std::vector<Point>& p){if(p.size()<4)return false;const QVector3D a(p[0].x,p[0].y,p[0].z);QVector3D b,n;for(auto q:p){auto v=QVector3D(q.x,q.y,q.z)-a;if(v.lengthSquared()>b.lengthSquared())b=v;}for(auto q:p){auto c=QVector3D::crossProduct(b,QVector3D(q.x,q.y,q.z)-a);if(c.lengthSquared()>n.lengthSquared())n=c;}if(n.lengthSquared()<1e-14)return false;n.normalize();for(auto q:p)if(std::abs(QVector3D::dotProduct(n,QVector3D(q.x,q.y,q.z)-a))>1e-5)return true;return false;}
void checkCancel(const std::shared_ptr<std::atomic<bool>>& stop){if(stop&&stop->load())throw std::runtime_error("Collision generation cancelled");}
void finish(rock::weapon_geometry_work::Task task,const std::shared_ptr<std::atomic<bool>>& stop){while(task.step())checkCancel(stop);checkCancel(stop);}
}
bool collisionSettingsValid(const CollisionSettings& s,QString& error){
    if(!finite(s.gap)||s.gap<.01||s.gap>100||!finite(s.dedup)||s.dedup<.0001||s.dedup>10||!finite(s.error)||s.error<0||s.error>10||s.targetPoints<4||s.targetPoints>252||!finite(s.radius)||s.radius<0||s.radius>.1||!finite(s.scale)||s.scale<=0||s.scale>1000||!finite(s.mass)||s.mass<=0||s.mass>100||s.layer>63){error="Collision settings exceed supported limits";return false;}return true;
}
// Reentrant Qhull from the existing native dependency draws the envelope.
// Physics/export keeps the original support points, not these preview triangles.
bool collisionHullFaces(CollisionHull& h,QString& error){
    h.faces.clear();h.edges.clear();if(h.points.size()<4||h.points.size()>4096){error="A hull needs 4–4096 finite points";return false;}
    QVector<coordT> coordinates;coordinates.reserve(h.points.size()*3);
    for(auto p:h.points)for(int axis=0;axis<3;++axis){if(!finite(p[axis])){error="Non-finite hull point";return false;}coordinates.append(p[axis]);}
    struct Context {qhT qh{};Context(){qh_zero(&qh,stderr);}~Context(){qh_freeqhull(&qh,!qh_ALL);int count{},bytes{};qh_memfreeshort(&qh,&count,&bytes);}} context;
    qhT* qh=&context.qh;char flags[]="qhull Qt";
    if(qh_new_qhull(qh,3,int(h.points.size()),coordinates.data(),False,flags,nullptr,stderr)){error="Hull has no stable three-dimensional envelope";return false;}
    facetT* facet;vertexT* vertex;vertexT** vertexp;
    FORALLfacets {Face face{};int count=0;FOREACHvertex_(facet->vertices){const auto id=qh_pointid(qh,vertex->point);if(count>=3||id<0||id>=h.points.size()){error="Invalid convex preview facet";h.faces.clear();return false;}face[count++]=quint32(id);}if(count==3)h.faces.append(face);}
    if(h.faces.isEmpty()||h.faces.size()>8192){error="Preview face budget exceeded";h.faces.clear();return false;}
    struct Edge {QVector3D normal;int count{};bool crease{false};};
    std::map<std::pair<quint32,quint32>,Edge> edges;
    for(auto face:h.faces){const auto n=QVector3D::crossProduct(h.points[face[1]]-h.points[face[0]],h.points[face[2]]-h.points[face[0]]).normalized();
        for(int i=0;i<3;++i){auto ids=std::minmax(face[i],face[(i+1)%3]);auto& e=edges[{ids.first,ids.second}];if(e.count&&std::abs(QVector3D::dotProduct(e.normal,n))<.9999f)e.crease=true;e.normal=n;++e.count;}}
    for(const auto& [ids,e]:edges)if(e.count==1||e.crease)h.edges.append({ids.first,ids.second});
    return true;
}
QString collisionGeometryHash(const CollisionMesh& source){QCryptographicHash h(QCryptographicHash::Sha256);for(auto p:source.vertices)for(int k=0;k<3;++k){float v=p[k];h.addData(QByteArrayView(reinterpret_cast<const char*>(&v),sizeof(v)));}for(auto t:source.triangles)h.addData(QByteArrayView(reinterpret_cast<const char*>(t.data()),sizeof(t)));return QString::fromLatin1(h.result().toHex());}
CollisionResult bakeCollision(CollisionProfile input,const std::shared_ptr<std::atomic<bool>>& stop){
    CollisionResult out;try{
        QString error;if(!collisionSettingsValid(input.settings,error))throw std::runtime_error(error.toStdString());
        input.hulls.clear();input.warnings.clear();input.inputTriangles=0;input.origin="rock-geometric-pipeline";input.sourceHash=QString::fromLatin1(kRockCollisionSourceHash);
        if(input.sources.isEmpty()||input.sources.size()>512)throw std::runtime_error("Select 1-512 visible source meshes");
        const auto& s=input.settings;
        for(auto& source:input.sources){checkCancel(stop);if(source.vertices.size()>65536||source.triangles.size()>65536)throw std::runtime_error("Source exceeds ROCK's supported triangle budget");
            source.geometryHash=collisionGeometryHash(source);if(!source.worldIncluded)continue;input.inputTriangles+=source.triangles.size();if(input.inputTriangles>1000000)throw std::runtime_error("Snapshot triangle budget exceeded");
            std::vector<rock::weapon_gap_decomposition::Triangle> mesh;std::vector<Point> all;
            for(auto tri:source.triangles){rock::weapon_gap_decomposition::Triangle t;for(int i=0;i<3;++i){if(tri[i]>=quint32(source.vertices.size()))throw std::runtime_error("Invalid triangle index");auto p=source.vertices[int(tri[i])];if(!finite(p.x())||!finite(p.y())||!finite(p.z()))throw std::runtime_error("Non-finite source geometry");t[i]={p.x(),p.y(),p.z()};all.push_back({p.x(),p.y(),p.z()});}mesh.push_back(t);}
            all=dedup(all,s.dedup);if(!volume(all)){input.warnings<<source.name+": degenerate surface omitted (ROCK also requires a volume)";continue;}
            rock::weapon_gap_decomposition::Result partition;finish(rock::weapon_gap_decomposition::decomposeDeferred(mesh,s.gap,.01,partition),stop);
            std::vector<std::vector<Point>> children;bool accepted=!partition.pieces.empty();
            for(const auto& piece:partition.pieces){std::vector<Point> points;for(auto p:piece)points.push_back({float(p.x),float(p.y),float(p.z)});points=dedup(points,s.dedup);
                rock::weapon_collision_geometry_math::ConvexSupportFitResult<Point> fit;finish(rock::weapon_collision_geometry_math::fitConvexSupportPointCloudDeferred(points,s.targetPoints,252,float(s.error),true,fit),stop);
                if(!fit.accepted||!volume(fit.points)){accepted=false;break;}children.push_back(std::move(fit.points));}
            if(!accepted){children.clear();rock::weapon_collision_geometry_math::ConvexSupportFitResult<Point> fit;finish(rock::weapon_collision_geometry_math::fitConvexSupportPointCloudDeferred(all,s.targetPoints,252,float(s.error),false,fit),stop);
                if(fit.accepted&&!fit.points.empty())children.push_back(std::move(fit.points));
                else {std::vector<std::vector<Point>> split;finish(rock::weapon_collision_geometry_math::splitOversizedClusterDeferred(all,252,split),stop);for(auto& points:split){rock::weapon_collision_geometry_math::ConvexSupportFitResult<Point> child;finish(rock::weapon_collision_geometry_math::fitConvexSupportPointCloudDeferred(points,s.targetPoints,252,float(s.error),false,child),stop);children.push_back(child.accepted&&!child.points.empty()?std::move(child.points):std::move(points));}}
                input.warnings<<source.name+": gap partition rejected; complete source fallback used";}
            if(partition.budgetLimited)input.warnings<<source.name+": ROCK's gap budget was reached; unresolved concavities may remain";
            int child=0;for(const auto& points:children){checkCancel(stop);CollisionHull hull;hull.source=source.key;hull.name=source.name+QString(" / %1").arg(++child);for(auto p:points)hull.points.append({p.x,p.y,p.z});if(!collisionHullFaces(hull,error))throw std::runtime_error((hull.name+": "+error).toStdString());input.hulls.append(std::move(hull));if(input.hulls.size()>1024)throw std::runtime_error("Preview exceeded 1024 hulls; select a smaller part");}
        }
        if(!input.valid())throw std::runtime_error("No volumetric collision shapes could be generated");
        out.profile=std::move(input);
    }catch(const std::exception& e){out.error=QString::fromUtf8(e.what());}return out;
}
QJsonObject collisionCookInput(const CollisionProfile& p,QString& error){
    error.clear();
    const auto frame=p.referencePose["rootToWorld"].toArray();
    if(!p.valid()||frame.size()!=16){error="Regenerate this profile with a selected magazine reference before cooking";return {};}
    QMatrix4x4 root;for(int i=0;i<16;++i){if(!frame[i].isDouble()||!finite(frame[i].toDouble())){error="Invalid magazine root frame";return {};}root(i/4,i%4)=float(frame[i].toDouble());}
    bool invertible=false;const auto inverse=root.inverted(&invertible);if(!invertible){error="Singular magazine root frame";return {};}
    QJsonArray hulls;QHash<QString,QMatrix4x4> frames;for(const auto& source:p.sources)frames.insert(source.key,inverse*source.frame);
    for(const auto& hull:p.hulls)if(hull.enabled){
        if(std::any_of(p.sources.begin(),p.sources.end(),[&](const auto& source){return source.key==hull.source&&!source.worldIncluded;}))continue;
        if(hull.points.size()<4||hull.points.size()>252||!frames.contains(hull.source)){error="Each moving-object hull needs 4–252 points and a valid source";return {};}
        QJsonArray points;for(const auto& point:hull.points)points.append(vector(frames[hull.source].map(point)));
        hulls.append(QJsonObject{{"points",points}});
    }
    if(hulls.isEmpty()){error="Enable at least one magazine hull";return {};}
    QJsonArray pieces;for(const auto& source:p.sources)if(source.worldIncluded)pieces.append(source.key);
    return {{"hulls",hulls},{"havokScale",p.settings.scale},{"precision",.0001},{"radius",p.settings.radius},
        {"layer",int(p.settings.layer)},{"material",double(p.settings.material)},{"mass",p.settings.mass},
        {"capture",QJsonObject{{"weapon",p.weapon},{"target",p.target},{"configuration",p.configuration},{"referencePose",p.referencePose},{"pieces",pieces}}}};
}
QJsonObject collisionJson(const CollisionProfile& p){
    QJsonArray sources,hulls,warnings;for(const auto& s:p.sources){QJsonArray frame;for(int r=0;r<4;++r)for(int c=0;c<4;++c)frame.append(s.frame(r,c));sources.append(QJsonObject{{"key",s.key},{"name",s.name},{"model",s.model},{"record",s.record},{"path",s.path},{"geometrySha256",s.geometryHash},{"material",s.material},{"worldIncluded",s.worldIncluded},{"sourceToWorld",frame}});}
    for(const auto& h:p.hulls){QJsonArray points;for(auto point:h.points)points.append(vector(point));hulls.append(QJsonObject{{"source",h.source},{"name",h.name},{"enabled",h.enabled},{"points",points}});}
    for(auto w:p.warnings)warnings.append(w);
    return {{"format","reload-maker-collision-profile"},{"version",1},{"units","game-units"},{"space","source-local"},{"weapon",p.weapon},{"configuration",p.configuration},{"target",p.target},{"origin",p.origin},{"provenanceSha256",p.sourceHash},{"referencePose",p.referencePose},{"inputTriangles",double(p.inputTriangles)},{"settings",settingsJson(p.settings)},{"sources",sources},{"hulls",hulls},{"warnings",warnings},{"engineCooked",false},{"runtimeConsumerRequired",true}};
}
bool readCollisionJson(const QJsonObject& o,CollisionProfile& output,QString& error){
    CollisionProfile p;auto fail=[&](const QString& e){error=e;return false;};
    if(o["format"]!="reload-maker-collision-profile"||o["version"]!=1||o["units"]!="game-units"||o["space"]!="source-local")return fail("Unknown collision profile format, units or space");
    for(const auto* key:{"weapon","configuration","target","origin"})if(!o[key].isString()||o[key].toString().isEmpty())return fail("Missing collision identity");
    p.weapon=o["weapon"].toString();p.configuration=o["configuration"].toString();p.target=o["target"].toString();p.origin=o["origin"].toString();p.sourceHash=o["provenanceSha256"].toString();p.referencePose=o["referencePose"].toObject();p.inputTriangles=o["inputTriangles"].toInteger();
    if(!readSettings(o["settings"].toObject(),p.settings,error))return false;
    QSet<QString> keys;const auto sources=o["sources"].toArray(),hulls=o["hulls"].toArray();if(sources.isEmpty()||sources.size()>512||hulls.isEmpty()||hulls.size()>1024)return fail("Collision profile exceeds source/hull limits");
    for(auto v:sources){const auto j=v.toObject();CollisionMesh s;s.key=j["key"].toString();if(s.key.isEmpty()||keys.contains(s.key))return fail("Missing or duplicate source binding");keys.insert(s.key);s.name=j["name"].toString();s.path=j["path"].toString();s.model=j["model"].toString();s.record=j["record"].toString();s.geometryHash=j["geometrySha256"].toString();s.material=j["material"].toString();if(j.contains("worldIncluded")&&!j["worldIncluded"].isBool())return fail("Invalid world piece selection");s.worldIncluded=j["worldIncluded"].toBool(true);const auto frame=j["sourceToWorld"].toArray();if(frame.size()!=16)return fail("Invalid collision source frame");for(int i=0;i<16;++i){if(!frame[i].isDouble()||!finite(frame[i].toDouble()))return fail("Invalid collision source frame");s.frame(i/4,i%4)=float(frame[i].toDouble());}if(!s.frame.isAffine()||std::abs(s.frame.determinant())<1e-8)return fail("Singular collision source frame");p.sources.append(s);}
    qsizetype total=0;for(auto v:hulls){const auto j=v.toObject();CollisionHull h;h.source=j["source"].toString();h.name=j["name"].toString();if(!keys.contains(h.source)||!j["enabled"].isBool())return fail("Unknown hull source or enabled state");h.enabled=j["enabled"].toBool();const auto points=j["points"].toArray();total+=points.size();if(points.size()<4||points.size()>4096||total>262144)return fail("Collision point budget exceeded");for(auto q:points){QVector3D point;if(!readPoint(q,point))return fail("Invalid hull point");h.points.append(point);}if(!collisionHullFaces(h,error))return false;p.hulls.append(std::move(h));}
    for(auto w:o["warnings"].toArray())if(w.isString())p.warnings.append(w.toString());
    output=std::move(p);return true;
}
QJsonObject collisionSummary(const CollisionProfile& p){QJsonArray sources;for(int i=0;i<p.sources.size();++i){const auto& s=p.sources[i];sources.append(QJsonObject{{"index",i},{"key",s.key},{"name",s.name},{"material",s.material},{"path",s.path},{"included",s.worldIncluded}});}QJsonArray hulls;qsizetype points=0,enabled=0;for(int i=0;i<p.hulls.size();++i){const auto& h=p.hulls[i];points+=h.points.size();enabled+=h.enabled;hulls.append(QJsonObject{{"index",i},{"name",h.name},{"source",h.source},{"points",int(h.points.size())},{"faces",int(h.faces.size())},{"enabled",h.enabled}});}return {{"sources",sources},{"valid",p.valid()},{"weapon",p.weapon},{"configuration",p.configuration},{"target",p.target},{"origin",p.origin},{"sourceCount",int(p.sources.size())},{"hullCount",int(p.hulls.size())},{"enabledCount",double(enabled)},{"points",double(points)},{"inputTriangles",double(p.inputTriangles)},{"hulls",hulls},{"warnings",QJsonArray::fromStringList(p.warnings)},{"settings",settingsJson(p.settings)},{"engineCooked",false}};}
}
