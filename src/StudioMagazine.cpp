#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "MagazineViewport.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include <QTimer>
#include <cmath>

namespace reloadmaker {
void ReloadMakerController::prepareMagazineInteraction() {
    _magazineRail.reset();_magazineVisuals.clear();_magazineError.clear();
    if(!_simulationRig||!_reloadRecipe)return;
    auto* model=activeModel();auto* scene=_window->getGLView()->getScene();
    const auto& rig=*_simulationRig;const auto& full=_reloadRecipe->tracks[rig.magazine];
    const auto below=[&](QModelIndex child,const QModelIndex& parent) {
        for(int depth=0;child.isValid()&&depth<model->getBlockCount();++depth){if(child==parent)return true;child=model->getBlockIndex(model->getParent(child));}return false;
    };
    SlotKey slot=0;const auto* config=configuration();
    for(auto it=_partRoots.cbegin();it!=_partRoots.cend();++it)if(poseRecordKey(config->selected(it.key()))==full.anchor.ownerRecord){slot=it.key();break;}
    auto source=sourceNif(_packageResources,full.anchor.ownerModel,_magazineError);if(!source)return;
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
    if(!buildMagazineRail(*_reloadRecipe,rig.magazine,connectionName,connection,body,frame,rail,_magazineError))return;
    bool empty=false,fullBody=false;
    for(const auto& shape:shapes) {
        const bool isEmpty=shape.track==rail.emptyTrack;
        if(isEmpty)empty=true;
        if(shape.track==rig.magazine)fullBody=true;
        _magazineVisuals.append({shape.node,model->get<int>(shape.node,"Flags"),isEmpty,shape.track});
    }
    if(!empty||!fullBody){_magazineVisuals.clear();_magazineError="Separate full and empty magazine geometry could not be resolved.";return;}
    _magazineRail=rail;
}
void ReloadMakerController::updateMagazinePose() {
    if(!_simulationActive)return;
    if(!applyPose(0)){_simulationError=_lastError;stopSimulation();}
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
    _simulation.cancelMagazine();updateMagazinePose();setStatus("Magazine placement cancelled.");
}
}
