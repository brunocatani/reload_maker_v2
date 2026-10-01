#include "ReloadMakerController.h"
#include "CollisionPanel.h"
#include "StudioWindow.h"
#include "StudioPaths.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include "physics-interaction/weapon/WeaponEffectGeometryPolicy.h"
#include <QDockWidget>
#include <QComboBox>
#include <QScrollArea>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QQuaternion>
#include <QSaveFile>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QUuid>
#include <stdexcept>

// The native renderer remains the authority for skinning, LOD and transforms.
QVector<Vector3> Shape::reloadMakerCollisionVertices() const {
    auto result=verts;if(transformRigid||boneWeights0.size()!=size_t(verts.size())||boneTransforms.empty())return result;
    for(qsizetype i=0;i<result.size();++i){Vector3 sum(0,0,0);float total=0;const int count=boneWeights1.size()==size_t(verts.size())?8:4;
        for(int j=0;j<count;++j){const float encoded=j<4?boneWeights0[i][j]:boneWeights1[i][j-4];if(!(encoded>0))break;const int bone=int(encoded);if(bone<0||size_t(bone*3+2)>=boneTransforms.size())continue;const float w=encoded-float(bone);const auto* rows=boneTransforms.data()+bone*3;
            for(int axis=0;axis<3;++axis)sum[axis]+=(verts[i][0]*rows[axis][0]+verts[i][1]*rows[axis][1]+verts[i][2]*rows[axis][2]+rows[axis][3])*w;
            total+=w;}
        if(total>0)result[i]=sum/total;
    }return result;
}
qsizetype Shape::reloadMakerCollisionTriangleCount()const{return std::clamp<qsizetype>(lodTriangleCount,0,triangles.size());}
QJsonObject Shape::reloadMakerRigidMeshJson()const {
    if(!transformRigid||verts.isEmpty()||verts.size()>65535||norms.size()!=verts.size()||tangents.size()!=verts.size()||bitangents.size()!=verts.size()||coords.isEmpty()||coords[0].size()!=verts.size()||lodTriangleCount!=triangles.size())return {};
    QJsonArray positions,normals,tangent,uvs,indices,vertexColors;
    for(qsizetype i=0;i<verts.size();++i) {
        positions.append(QJsonArray{verts[i][0],verts[i][1],verts[i][2]});normals.append(QJsonArray{norms[i][0],norms[i][1],norms[i][2]});
        // Match the native glTF exporter: NIF's bitangent is glTF's tangent.
        const QVector3D n(norms[i][0],norms[i][1],norms[i][2]),t(bitangents[i][0],bitangents[i][1],bitangents[i][2]),b(tangents[i][0],tangents[i][1],tangents[i][2]);
        tangent.append(QJsonArray{t.x(),t.y(),t.z(),QVector3D::dotProduct(QVector3D::crossProduct(n,t),b)>=0?1:-1});
        uvs.append(QJsonArray{coords[0][i][0],coords[0][i][1]});
        if(hasVertexColors){if(colors.size()!=verts.size())return {};vertexColors.append(QJsonArray{colors[i][0],colors[i][1],colors[i][2],colors[i][3]});}
    }
    for(const auto& triangle:triangles)for(int k=0;k<3;++k){if(triangle[k]>=verts.size())return {};indices.append(int(triangle[k]));}
    QJsonObject result{{"positions",positions},{"normals",normals},{"tangents",tangent},{"uvs",uvs},{"indices",indices}};
    if(hasVertexColors)result["colors"]=vertexColors;
    if(coords.size()>1){if(coords[1].size()!=verts.size())return {};QJsonArray second;for(const auto& uv:coords[1])second.append(QJsonArray{uv[0],uv[1]});result["uvs2"]=second;}
    return result;
}

namespace reloadmaker {
namespace {
QMatrix4x4 matrix(const Transform& t){QMatrix4x4 m;const auto o=t*Vector3(0,0,0);const Vector3 axes[]{t*Vector3(1,0,0)-o,t*Vector3(0,1,0)-o,t*Vector3(0,0,1)-o};for(int r=0;r<3;++r){for(int c=0;c<3;++c)m(r,c)=axes[c][r];m(r,3)=o[r];}return m;}
QMatrix4x4 matrix(const Transformf& t){QMatrix4x4 m;m.translate(t.translation.x,t.translation.y,t.translation.z);m.rotate(QQuaternion(t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z));m.scale(t.scale.x,t.scale.y,t.scale.z);return m;}
void require(bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());}
}
void ReloadMakerController::createCollisionsUi(){
    _collisionsDock=new QDockWidget("COLLISIONS",_window);_collisionsDock->setObjectName("CollisionsDock");_collisionsDock->setFeatures(QDockWidget::NoDockWidgetFeatures);_collisionsDock->setMinimumWidth(340);_collisionsDock->setMaximumWidth(470);
    _collisions=new CollisionPanel;auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(_collisions);_collisionsDock->setWidget(scroll);_window->addDockWidget(Qt::RightDockWidgetArea,_collisionsDock);_collisionsDock->hide();
    _collisions->snapshot=[this](int index,bool reference){return collisionSnapshot(index,reference);};
    _collisions->cook=[this]{showCollisionCooker();};
    _collisions->prepare=[this]{const auto output=QFileDialog::getSaveFileName(_window,"Capture visible part in the selected root frame",studioDataPath("exports/objects/PreparedMagazine.nif"),"Prepared world model (*.nif)");if(!output.isEmpty())capturePreparedPart(output);};
    connect(this,&ReloadMakerController::packJobChanged,this,[this]{if(_packJob["operation"]!="prepare")return;
        if(_packJob["phase"]=="complete"){const auto result=_packJob["result"].toObject();_preparedPart=result;setStatus("Prepared visible part saved · "+result["output"].toString());}
        else if(_packJob["phase"]=="failed")setStatus(_packJob["error"].toString(),true);
    });
    _collisions->changed=[this]{++_revision;_window->getGLView()->update();};
    connect(_window->getGLView(),&GLView::paintUpdate,this,[this]{paintCollisions();});
}
void ReloadMakerController::refreshCollisions(){if(!_collisions)return;QStringList names;_collisionTargets.clear();for(int i=0;i<_handPoseAnchor->count();++i)if(i==0||_handPoseAnchors[i].description.anchorKind=="installed-part"||_handPoseAnchors[i].description.anchorName=="WeaponMagazine"){names<<_handPoseAnchor->itemText(i);_collisionTargets.append(i);}_collisions->setTargets(names);}
void ReloadMakerController::invalidateCollisions(){_preparedPart={};if(_collisions)_collisions->invalidate();}
CollisionProfile ReloadMakerController::collisionSnapshot(int target,bool referenceOnly,bool captureRender){
    require(_workspace==CollisionsWorkspace,"Open the Collisions tab first");require(!_loadThread&&!_reloadCapturing&&!_rebuildTimer->isActive(),"Wait for assembly and reload capture");
    const auto* weapon=currentWeapon();auto* model=activeModel();auto* view=_window->getGLView();require(weapon&&model&&view->getScene(),"Load a weapon first");require(target>=0&&target<_collisionTargets.size(),"Select a collision target");target=_collisionTargets[target];
    require(!_playing&&!_reloadPlaying,"Pause playback before creating a collision snapshot");
    const auto anchor=_handPoseAnchors[target];require(!referenceOnly||(target>0&&anchor.description.anchorKind=="installed-part"),"For DIAMOND input, select the installed magazine model, not an animated bone or whole weapon");
    (void)view->studioCaptureFrame();
    CollisionProfile p;p.weapon=poseRecordKey(weapon->formId);p.target=target?anchor.description.ownerRecord+"|"+anchor.description.ownerModel+"|"+anchor.description.anchorPath:"whole-weapon";
    QJsonObject choices;for(const auto& slot:configuration()->entries())choices[QString::number(slot.key)]=poseRecordKey(configuration()->selected(slot.key));
    p.configuration=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(choices).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    p.referencePose={{"animation",_activeAnimationPath},{"seconds",_playbackTime},{"choices",choices}};
    if(target>0){Transformf world;QString error;require(sceneNodeWorld(anchor.node,world,error),error);const auto transform=matrix(world);QJsonArray frame;for(int r=0;r<4;++r)for(int c=0;c<4;++c)frame.append(transform(r,c));p.referencePose["rootToWorld"]=frame;}
    QSet<QString> keys;qsizetype totalTriangles=0;
    for(auto* shape:view->getScene()->shapes){if(!shape||shape->isHidden())continue;const auto node=model->getBlockIndex(shape->id());
        const auto geometryName=model->get<QString>(node,"Name").toUtf8();bool billboard=false;
        int depthLimit=model->getBlockCount();for(auto ancestor=node;ancestor.isValid()&&depthLimit-->0;ancestor=model->getBlockIndex(model->getParent(ancestor))){if(model->blockInherits(ancestor,"NiBillboardNode")){billboard=true;break;}}
        using namespace rock::weapon_effect_geometry_policy;
        if(classify({model->blockInherits(model->getBlockIndex(model->getLink(node,"Shader Property")),"BSEffectShaderProperty"),billboard,std::string_view(geometryName.constData(),size_t(geometryName.size()))})!=ExclusionReason::None)continue;
        bool allowed=false;QModelIndex owner;QStringList path;auto parent=node;
        for(int depth=0;parent.isValid()&&depth<model->getBlockCount();++depth){path.prepend(model->get<QString>(parent,"Name"));if(parent==anchor.node)allowed=true;
            if(parent==_weaponRoot||_partRoots.values().contains(parent)){owner=parent;break;}parent=model->getBlockIndex(model->getParent(parent));}
        if(!owner.isValid()||(target>0&&!allowed))continue;
        CollisionMesh source;source.name=model->get<QString>(node,"Name");source.path=path.join('/');source.model=weapon->baseModelPath;source.record=p.weapon;
        for(const auto& slot:configuration()->entries())if(_partRoots.value(slot.key)==owner){if(const auto* mod=configuration()->mod(configuration()->selected(slot.key))){source.model=mod->modelPath;source.record=poseRecordKey(mod->formId);}break;}
        source.key=source.record+"|"+source.model+"|"+source.path;require(!keys.contains(source.key),"Ambiguous duplicate node path; select a smaller source");keys.insert(source.key);source.frame=matrix(shape->worldTrans());
        source.material=model->get<QString>(model->getBlockIndex(model->getLink(node,"Shader Property")),"Name");
        if(captureRender){source.render=shape->reloadMakerRigidMeshJson();require(!source.render.isEmpty(),"Prepared world capture requires complete rigid geometry with UVs, normals and tangents at its highest detail");const auto shader=model->getBlockIndex(model->getLink(node,"Shader Property"));source.render["material"]=model->get<QString>(shader,"Name");require(!source.render["material"].toString().isEmpty(),"The selected shape needs an explicit source material path");}
        for(const auto& v:shape->reloadMakerCollisionVertices())source.vertices.append({v[0],v[1],v[2]});
        const auto count=shape->reloadMakerCollisionTriangleCount();totalTriangles+=count;require(totalTriangles<=1000000,"Snapshot exceeds one million triangles; choose a smaller part");
        for(qsizetype i=0;i<count;++i){const auto t=shape->triangles[i];source.triangles.append({quint32(t[0]),quint32(t[1]),quint32(t[2])});}if(source.triangles.isEmpty())continue;
        source.geometryHash=collisionGeometryHash(source);p.sources.append(std::move(source));
    }
    require(!p.sources.isEmpty(),"No visible weapon triangles under this target");p.inputTriangles=totalTriangles;
    if(referenceOnly){Transformf world;QString error;require(sceneNodeWorld(anchor.node,world,error),error);CollisionMesh reference;reference.key=p.target;reference.name=anchor.description.anchorName;reference.path=anchor.description.anchorPath;reference.model=anchor.description.ownerModel;reference.record=anchor.description.ownerRecord;reference.frame=matrix(world);
        const auto inverse=reference.frame.inverted();for(const auto& source:p.sources){const auto base=quint32(reference.vertices.size());for(auto point:source.vertices)reference.vertices.append(inverse.map(source.frame.map(point)));for(auto t:source.triangles)reference.triangles.append({t[0]+base,t[1]+base,t[2]+base});}reference.geometryHash=collisionGeometryHash(reference);p.sources={reference};
    }return p;
}
QJsonObject ReloadMakerController::capturePreparedPart(const QString& output) {
    require(!_packProcess,"Wait for the active asset job");
    const auto profile=collisionSnapshot(_collisions->state()["targetIndex"].toInt(-1),false,true);
    const auto rootArray=profile.referencePose["rootToWorld"].toArray();require(rootArray.size()==16,"Select one magazine/part as the reference, not the whole weapon");
    QMatrix4x4 root;for(int i=0;i<16;++i)root(i/4,i%4)=float(rootArray[i].toDouble());bool invertible=false;const auto inverse=root.inverted(&invertible);require(invertible,"Invalid prepared-model root");
    const auto& selected=_collisions->profile();const bool same=selected.valid()&&selected.weapon==profile.weapon&&selected.configuration==profile.configuration&&selected.target==profile.target;
    if(same)require(std::any_of(selected.sources.begin(),selected.sources.end(),[](const auto& source){return source.worldIncluded;}),"Include at least one world-model piece");
    QJsonArray meshes;for(const auto& source:profile.sources){
        if(same){const auto old=std::find_if(selected.sources.begin(),selected.sources.end(),[&](const auto& s){return s.key==source.key;});if(old!=selected.sources.end()){require(old->geometryHash==source.geometryHash,"Source geometry changed; regenerate and review world pieces");if(!old->worldIncluded)continue;}}
        auto mesh=source.render;mesh["name"]=source.name;mesh["source"]=source.key;const auto local=inverse*source.frame;QJsonArray frame;for(int r=0;r<4;++r)for(int c=0;c<4;++c)frame.append(local(r,c));mesh["localToRoot"]=frame;meshes.append(mesh);}
    require(!meshes.isEmpty(),"No selected world geometry remains");
    const QJsonObject snapshot{{"format","reload-maker-visible-part"},{"version",1},{"units","game-units"},{"weapon",profile.weapon},{"target",profile.target},{"configuration",profile.configuration},{"referencePose",profile.referencePose},{"meshes",meshes}};
    const auto path=studioDataPath("local/asset-jobs/"+QUuid::createUuid().toString(QUuid::WithoutBraces)+"/visible.json");QDir().mkpath(QFileInfo(path).absolutePath());QSaveFile file(path);const auto bytes=QJsonDocument(snapshot).toJson(QJsonDocument::Compact);require(bytes.size()<=64*1024*1024,"Choose a smaller part; snapshot exceeds 64 MiB");require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot freeze visible part geometry");
    return packCommand({{"operation","prepare"},{"job",path},{"output",output}});
}
void ReloadMakerController::paintCollisions(){
    if(_workspace!=CollisionsWorkspace||!_collisions||!_collisions->overlay())return;
    auto* view=_window->getGLView();const auto size=QSizeF(view->width(),view->height());const auto dpr=view->devicePixelRatio();QOpenGLPaintDevice device(QSize(qRound(size.width()*dpr),qRound(size.height()*dpr)));device.setDevicePixelRatio(dpr);QPainter painter(&device);painter.setRenderHint(QPainter::Antialiasing);
    const auto camera=view->reloadMakerWorldToClip();
    auto draw=[&](const CollisionProfile& profile,bool baseline){QHash<QString,QMatrix4x4> frames;for(const auto& s:profile.sources)frames.insert(s.key,camera*s.frame);
        for(int i=0;i<profile.hulls.size();++i){const auto& h=profile.hulls[i];if(!h.enabled||(!baseline&&_collisions->isolated()&&i!=_collisions->selected()))continue;const auto transform=frames.value(h.source);
            QColor color=baseline?QColor(93,145,239):i==_collisions->selected()?QColor(255,211,113):profile.origin=="diamond-pre-elric-input"?QColor(244,154,96):QColor(81,210,183);
            auto project=[&](quint32 index,QPointF& out){const auto clip=transform*QVector4D(h.points[int(index)],1);if(clip.w()<=.001||clip.z()<-clip.w())return false;out={(clip.x()/clip.w()+1)*size.width()/2,(1-clip.y()/clip.w())*size.height()/2};return true;};
            if(!baseline){auto fill=color;fill.setAlpha(12);painter.setPen(Qt::NoPen);painter.setBrush(fill);for(auto face:h.faces){QPolygonF polygon;for(auto index:face){QPointF point;if(!project(index,point)){polygon.clear();break;}polygon<<point;}if(polygon.size()==3)painter.drawPolygon(polygon);}}
            painter.setBrush(Qt::NoBrush);color.setAlpha(baseline?140:220);painter.setPen(QPen(color,baseline?.8:1.1));for(auto edge:h.edges){QPointF a,b;if(project(edge[0],a)&&project(edge[1],b))painter.drawLine(a,b);}
        }};
    if(_collisions->compare())draw(_collisions->baseline(),true);
    draw(_collisions->profile(),false);
    painter.setPen(QColor(145,221,208));painter.drawText(QPointF(18,28),"COLLISIONS · convex point envelopes · x-ray · drag background to orbit");
}
}
