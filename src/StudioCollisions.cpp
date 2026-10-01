#include "ReloadMakerController.h"
#include "CollisionPanel.h"
#include "StudioWindow.h"
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
    _collisions->changed=[this]{++_revision;_window->getGLView()->update();};
    connect(_window->getGLView(),&GLView::paintUpdate,this,[this]{paintCollisions();});
}
void ReloadMakerController::refreshCollisions(){if(!_collisions)return;QStringList names;_collisionTargets.clear();for(int i=0;i<_handPoseAnchor->count();++i)if(i==0||_handPoseAnchors[i].description.anchorKind=="installed-part"||_handPoseAnchors[i].description.anchorName=="WeaponMagazine"){names<<_handPoseAnchor->itemText(i);_collisionTargets.append(i);}_collisions->setTargets(names);}
void ReloadMakerController::invalidateCollisions(){if(_collisions)_collisions->invalidate();}
CollisionProfile ReloadMakerController::collisionSnapshot(int target,bool referenceOnly){
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
