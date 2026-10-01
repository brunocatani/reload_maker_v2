#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "MagazineViewport.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include <QTimer>
#include <cmath>

namespace reloadmaker {
HandPosePlacement ReloadMakerController::simulationMagazineAnchor() const {
    if(isRifleConfiguration()) {
        const int index=currentMagazineAnchor();
        return index>=0?_handPoseAnchors[index].description:HandPosePlacement{};
    }
    return _simulationRig&&_reloadRecipe?_reloadRecipe->tracks[_simulationRig->magazine].anchor:HandPosePlacement{};
}
void ReloadMakerController::prepareMagazineInteraction() {
    _magazineRail.reset();_magazineVisuals.clear();_magazineError.clear();
    if(!_simulationRig||!_reloadRecipe)return;
    auto* model=activeModel();auto* scene=_window->getGLView()->getScene();
    const auto& rig=*_simulationRig;
    const auto below=[&](QModelIndex child,const QModelIndex& parent) {
        for(int depth=0;child.isValid()&&depth<model->getBlockCount();++depth){if(child==parent)return true;child=model->getBlockIndex(model->getParent(child));}return false;
    };
    const SlotKey slot=_simulationMagazineSlot;
    const auto anchor=simulationMagazineAnchor();
    auto source=sourceNif(_packageResources,anchor.ownerModel,_magazineError);if(!source)return;
    ConnectTarget target;QString connectionName;
    for(const auto& name:childConnectPoints(*source)){target=findConnectTarget(name,nullptr,slot);if(target.valid()){connectionName="P-"+name.mid(2);break;}}
    Transformf parent;
    if(!target.valid()||!sceneNodeWorld(target.parent,parent,_magazineError)){_magazineError="Could not resolve the magazine's seated connection.";return;}
    const Transformf connection=composed(parent,Transformf{target.translation,target.rotation,{target.scale,target.scale,target.scale}});
    QVector<QVector3D> body,frame;
    struct ShapeOwner {QPersistentModelIndex node;int track;};QVector<ShapeOwner> shapes;
    for(auto* shape:scene->shapes) {
        if(!shape)continue;
        const auto index=model->getBlockIndex(shape->id());
        bool magazine=false;for(const auto& [root,flags]:_simulationVisibility)if(below(index,root)){magazine=true;break;}
        int owner=-1;
        if(magazine)for(int track:rig.magazineTracks) {
            if(below(index,_sceneNodes[_reloadTrackNodes[track]])) {
                if(owner<0||below(_sceneNodes[_reloadTrackNodes[track]],_sceneNodes[_reloadTrackNodes[owner]]))owner=track;
            }
        }
        if(magazine&&owner>=0)shapes.append({index,owner});
        bool weapon=below(index,_weaponRoot);
        if(!weapon)for(const auto& root:_partRoots)if(below(index,root)){weapon=true;break;}
        if(!weapon||(!magazine&&shape->isHidden())||(magazine&&owner!=rig.magazine))continue;
        if(!shape->reloadMakerSkinBones().isEmpty()){_magazineError="Magazine rail geometry needs a rigid source surface.";return;}
        Transformf world;if(!sceneNodeWorld(index,world,_magazineError))return;
        auto& output=magazine?body:frame;
        for(const auto& vertex:shape->verts){const auto point=composed(world,Transformf{{vertex[0],vertex[1],vertex[2]}}).translation;output.append({point.x,point.y,point.z});}
    }
    MagazineRail rail;
    if(!buildMagazineRail(*_reloadRecipe,rig.magazine,connectionName,connection,body,frame,rail,_magazineError,isM1AConfiguration()||isBoltActionConfiguration()||isThanatosConfiguration()))return;
    if(isM1AConfiguration()||isBoltActionConfiguration()||isThanatosConfiguration()) {
        float entry=-1,empty=-1;
        for(const auto& event:_reloadClip.annotations) {
            if(event.text.endsWith(isL118Configuration()?"L115A3_ReloadMagIn":isBoltActionConfiguration()?"C14_MagInserting":isThanatosConfiguration()?"WPNM107Magin":"WPNM1AMagIn",Qt::CaseInsensitive))entry=event.timeSeconds;
            if(event.text.endsWith(isL118Configuration()?"L115A3_MagOut":isBoltActionConfiguration()?"C14_MagOut":isThanatosConfiguration()?"WPNM107Magout":"WPNM1AMagOut",Qt::CaseInsensitive))empty=event.timeSeconds;
        }
        if(empty<0){_magazineError="Native empty-magazine frame is unavailable.";return;}
        _m1aEmptyMagazineFrame=std::clamp(qRound(empty/_reloadRecipe->duration*(_reloadRecipe->frames-1)),0,_reloadRecipe->frames-1);
        if(!buildMagazineAnimation(*_reloadRecipe,entry,body,rail,_magazineError))return;
    }
    bool empty=false,fullBody=false;
    for(const auto& shape:shapes) {
        const bool isEmpty=!rail.sharedBody()&&shape.track==rail.emptyTrack;
        if(isEmpty)empty=true;
        if(shape.track==rig.magazine)fullBody=true;
        const auto name=model->get<QString>(shape.node,"Name");
        bool cartridge=isRifleConfiguration()?name.startsWith(isM1AConfiguration()?"Bullet:":"MagBullet:",Qt::CaseInsensitive):!isEmpty&&shape.track!=rig.magazine;
        if(isBoltActionConfiguration()) {
            // The source culls this static round stack and Child1's top round
            // on an empty reload. Magazine body/follower helpers are separate.
            auto node=shape.node;
            for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth) {
                if(model->get<QString>(node,"Name").compare("MagHelperNode",Qt::CaseInsensitive)==0){cartridge=true;break;}
                node=model->getBlockIndex(model->getParent(node));
            }
        }
        _magazineVisuals.append({shape.node,model->get<int>(shape.node,"Flags"),isEmpty,shape.track,cartridge});
    }
    if((!empty&&!rail.sharedBody())||!fullBody){_magazineVisuals.clear();_magazineError="Magazine body geometry could not be resolved.";return;}
    _magazineRail=rail;
}
void ReloadMakerController::updateMagazinePose() {
    if(!_simulationActive)return;
    if(!applyPose(0)){_simulationError=_lastError;stopSimulation();}
    updateSimulationSounds();
    applySimulationVisibility();updateSimulationControls();++_revision;
}
void ReloadMakerController::moveFreeMagazine(QVector3D offset) {
    if(!_simulationActive||!_magazineRail)return;
    _simulation.magazine.moveFree(*_magazineRail,offset);updateMagazinePose();
}
void ReloadMakerController::moveMagazineRail(float distance) {
    if(!_simulationActive||!_magazineRail)return;
    const bool before=_simulation.magazine.seated();
    _simulation.magazine.moveRail(*_magazineRail,distance);updateMagazinePose();
    if(!before&&_simulation.magazine.seated())setStatus("Full magazine seated.");
}
void ReloadMakerController::cancelMagazine() {
    _simulation.cancelMagazine();resetSoundTracking();updateMagazinePose();setStatus("Magazine placement cancelled.");
}
}
