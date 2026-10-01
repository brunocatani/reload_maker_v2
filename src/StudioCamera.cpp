#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

QVector<QVector3D> GLView::studioGeometryBounds(const QVector<int>& blocks) {
    if(!scene||!isValid()||blocks.isEmpty())return {};
    makeCurrent();paintGL();
    // A model replacement can defer compilation for two paints. Never walk
    // the previous scene's raw shape list while its nodes are being retired.
    for(int pass=0;pass<2&&doCompile;++pass)paintGL();
    const bool ready=!doCompile&&!isDisabled;doneCurrent();if(!ready)return {};
    const QSet<int> allowed(blocks.cbegin(),blocks.cend());
    QVector3D low(std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()),high=-low;
    bool any=false;
    const auto include=[&](const Vector3& p) {
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2]))return;
        for(int axis=0;axis<3;++axis){low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);}any=true;
    };
    for(const auto* shape:scene->shapes) {
        if(!shape||!allowed.contains(shape->id())||shape->isHidden())continue;
        // A laser beam/spot is an effect, not the physical attachment housing.
        // Its authored length must not determine the weapon's camera scale.
        const auto index=model->getBlockIndex(shape->id());
        if(model->blockInherits(model->getBlockIndex(model->getLink(index,"Shader Property")),"BSEffectShaderProperty"))continue;
        if(shape->reloadMakerSkinBones().isEmpty())for(const auto& p:shape->verts)include(shape->worldTrans()*p);
        else {const auto sphere=shape->bounds();if(sphere.radius>0){include(sphere.center-Vector3(sphere.radius,sphere.radius,sphere.radius));include(sphere.center+Vector3(sphere.radius,sphere.radius,sphere.radius));}}
    }
    if(!any||(high-low).lengthSquared()<1e-8f)return {};
    // Native pan/right-drag sensitivity uses this same physical reference.
    axis=(high-low).length()*.5f;
    QVector<QVector3D> points;for(float x:{low.x(),high.x()})for(float y:{low.y(),high.y()})for(float z:{low.z(),high.z()})points.append({x,y,z});
    return points;
}

bool GLView::studioFitBounds(const QVector<QVector3D>& points) {
    if(points.isEmpty()||!scene||!isValid()||width()<1||height()<1)return false;
    // Finish renderer updates before reading camera orientation. Fitting is
    // synchronous: a later paint must not reset a subsequent zoom request.
    makeCurrent();paintGL();doneCurrent();
    QVector3D center;for(const auto& p:points)center+=p;center/=float(points.size());
    const float tanY=std::tan(float(cfg.fov)*float(M_PI)/360.f)*.78f;
    const float tanX=tanY*float(width())/float(height());
    float distance=.1f,halfExtent=.1f;
    for(const auto& p:points) {
        const auto d=p-center;const auto q=scene->view.rotation*Vector3(d.x(),d.y(),d.z());
        distance=std::max(distance,q[2]+std::max(std::abs(q[0])/tanX,std::abs(q[1])/tanY));
        halfExtent=std::max(halfExtent,std::max(std::abs(q[0])*float(height())/float(width()),std::abs(q[1])));
    }
    if(!std::isfinite(distance)||distance<=0)return false;
    Pos=Vector3(-center.x(),-center.y(),-center.z());Zoom=1.f;
    if(view==ViewWalk)view=ViewUser;
    Dist=perspectiveMode?distance*.5f:halfExtent/.78f;
    doCenter=false;updateViewpoint();update();
    makeCurrent();paintGL();doneCurrent();return true;
}

QJsonObject GLView::studioCameraState()const {
    return {{"position",QJsonArray{Pos[0],Pos[1],Pos[2]}},{"rotation",QJsonArray{Rot[0],Rot[1],Rot[2]}},
        {"distance",Dist},{"zoom",Zoom},{"perspective",perspectiveMode},{"view",int(view)}};
}
bool GLView::studioRestoreCamera(const QJsonObject& value) {
    const auto p=value["position"].toArray(),r=value["rotation"].toArray();
    const double d=value["distance"].toDouble(-1),z=value["zoom"].toDouble(-1);const int v=value["view"].toInt(-1);
    if(p.size()!=3||r.size()!=3||!std::isfinite(d)||!std::isfinite(z)||d<=0||d>1e8||z<.05||z>20||v<int(ViewDefault)||v>int(ViewUser))return false;
    for(int i=0;i<3;++i)if(!p[i].isDouble()||!r[i].isDouble()||!std::isfinite(p[i].toDouble())||!std::isfinite(r[i].toDouble())||std::abs(p[i].toDouble())>1e8)return false;
    Pos=Vector3(p[0].toDouble(),p[1].toDouble(),p[2].toDouble());Rot=Vector3(r[0].toDouble(),r[1].toDouble(),r[2].toDouble());
    Dist=float(d);Zoom=float(z);view=ViewState(v);perspectiveMode=value["perspective"].toBool(true);doCenter=false;updateViewpoint();update();
    if(isValid()){makeCurrent();paintGL();doneCurrent();}return true;
}

namespace reloadmaker {
void ReloadMakerController::cacheWeaponFitBounds() {
    _weaponFitCorners.clear();auto* model=activeModel();if(!model)return;
    QSet<int> blocks;
    for(const auto& surfaces:_partSurfaces)for(const auto& surface:surfaces)if(surface.isValid()&&surface.model()==model)blocks.insert(model->getBlockNumber(surface));
    // The model is the live ownership authority during assembly. The renderer
    // may still contain pointers to shapes from the previously loaded weapon.
    for(int block=0;block<model->getBlockCount();++block) {
        auto node=model->getBlockIndex(block);
        if(!model->blockInherits(node,"BSTriShape")&&!model->blockInherits(node,"NiTriBasedGeom"))continue;
        for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth) {
            if(node==_weaponRoot){blocks.insert(block);break;}
            node=model->getBlockIndex(model->getParent(node));
        }
    }
    // An attachment's supplied connection identifies its installed branch.
    // Independently animated sibling copies are reload props; Ready can park
    // them hundreds of units away even while their NIF visibility flag is set.
    for(auto it=blocks.begin();it!=blocks.end();) {
        bool auxiliary=false;auto node=model->getBlockIndex(*it);
        for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth) {
            const int block=model->getBlockNumber(node);
            const auto alignment=_partAlignmentAnchors.constFind(block);
            if(alignment!=_partAlignmentAnchors.cend()&&alignment.value()!=block){auxiliary=true;break;}
            node=model->getBlockIndex(model->getParent(node));
        }
        if(auxiliary)it=blocks.erase(it);else ++it;
    }
    _weaponFitCorners=_window->getGLView()->studioGeometryBounds(blocks.values());
}
bool ReloadMakerController::fitWeapon() {
    if(_weaponFitCorners.isEmpty()){setStatus("Load an assembled weapon before using Fit.",true);return false;}
    QVector<QVector3D> world;world.reserve(_weaponFitCorners.size());
    const Transformf weapon=_weaponBoneIndex>=0&&_weaponBoneIndex<_globalPose.size()?_globalPose[_weaponBoneIndex]:Transformf{};
    for(const auto& p:_weaponFitCorners){const auto q=composed(weapon,Transformf{{p.x(),p.y(),p.z()}}).translation;world.append({q.x,q.y,q.z});}
    return _window->getGLView()->studioFitBounds(world);
}
}
