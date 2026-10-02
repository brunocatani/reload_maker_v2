#include "ReloadMakerController.h"
#include "StudioWindow.h"
#include "CollisionPanel.h"
#include "glview.h"
#include "gl/glshape.h"
#include "model/nifmodel.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QQuaternion>
#include <QScopeGuard>
#include <stdexcept>

namespace reloadmaker {
namespace {
void requireAsset(bool ok,const QString& error){if(!ok)throw std::runtime_error(error.toStdString());}
QMatrix4x4 assetMatrix(const Transformf& t){QMatrix4x4 m;m.translate(t.translation.x,t.translation.y,t.translation.z);m.rotate(QQuaternion(t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z));m.scale(t.scale.x,t.scale.y,t.scale.z);return m;}
QJsonArray assetMatrixJson(const QMatrix4x4& m){QJsonArray out;for(int r=0;r<4;++r)for(int c=0;c<4;++c)out.append(m(r,c));return out;}
}
QJsonObject ReloadMakerController::captureMagazineWorld(bool empty) {
    requireAsset(_workspace==OperateWorkspace&&_simulationActive&&_simulationRig&&_reloadRecipe&&_magazineRail&&currentWeapon(),"Prepare the selected weapon in Operate first");
    requireAsset(_simulation.action==SimulationAction::Idle&&!_packProcess,"Finish the current interaction or asset job first");
    requireAsset(_simulationError.isEmpty()&&_magazineError.isEmpty(),"Resolve the current magazine simulation error first");
    const auto anchor=simulationMagazineAnchor();const auto original=_simulation;
    CollisionProfile captured;auto presentation=original;
    {
        // Use the very same payload state as Operate, including M14's pre-feed
        // follower positions and separately authored empty magazine body.
        const auto restore=qScopeGuard([&]{_simulation=original;(void)applyPose(0);applySimulationVisibility();});
        _simulation.magazine.phase=MagazinePhase::Rail;_simulation.magazine.distance=_magazineRail->travel;
        _simulation.magazine.offset=magazineInputOffset(*_magazineRail,_magazineRail->travel);
        _simulation.magazineRounds=empty?0:_simulationAmmo.capacity;_simulation.chambered=false;
        presentation=_simulation;
        requireAsset(applyPose(0),_lastError);applySimulationVisibility();
        auto* view=_window->getGLView();auto* model=activeModel();
        requireAsset(!view->studioCaptureFrame().isNull(),"Wait for the native scene before capturing the magazine");
        const int carrierTrack=empty?_magazineRail->emptyTrack:_magazineRail->fullTrack;
        Transformf carrier;QString error;
        requireAsset(sceneNodeWorld(_sceneNodes[_reloadTrackNodes[carrierTrack]],carrier,error),error);
        const auto root=assetMatrix(carrier);
        captured.weapon=poseRecordKey(currentWeapon()->formId);
        captured.target=anchor.ownerRecord+"|magazine-carrier|"+(empty?"empty":"full");
        const auto choices=configuration()->savedChoices(*currentPlugin());
        captured.configuration=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(choices).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        captured.referencePose={{"space","magazine-carrier"},{"magazineRecord",anchor.ownerRecord},{"model",anchor.ownerModel},
            {"presentation",empty?"empty":"full"},{"sourceRecipeId",_reloadRecipe->id},{"rootToWorld",assetMatrixJson(root)},{"sources",sourceEvidence()}};
        if(_magazineSetup)captured.referencePose["presentationSetup"]=magazineSetupJson(*_magazineSetup);
        QSet<int> included;qsizetype total=0;
        for(auto* shape:view->getScene()->shapes){
            if(!shape||shape->isHidden())continue;
            const auto node=model->getBlockIndex(shape->id());
            const auto visual=std::find_if(_magazineVisuals.cbegin(),_magazineVisuals.cend(),[&](const auto& v){return v.node==node;});
            if(visual==_magazineVisuals.cend()||visual->nativeOnly||(empty&&visual->cartridge))continue;
            if(!_magazineRail->sharedBody()&&visual->empty!=empty)continue;
            requireAsset(!included.contains(shape->id()),"Duplicate magazine surface");included.insert(shape->id());
            CollisionMesh source;source.name=model->get<QString>(node,"Name");source.model=anchor.ownerModel;source.record=anchor.ownerRecord;
            QStringList path;auto parent=node;for(int depth=0;parent.isValid()&&depth<model->getBlockCount();++depth){path.prepend(model->get<QString>(parent,"Name"));if(parent==_sceneNodes[_reloadTrackNodes[carrierTrack]])break;parent=model->getBlockIndex(model->getParent(parent));}
            source.path=path.join('/');source.key=source.record+"|"+source.model+"|"+source.path;
            requireAsset(std::none_of(captured.sources.cbegin(),captured.sources.cend(),[&](const auto& s){return s.key==source.key;}),"Magazine pieces need unique source paths");
            Transformf world;requireAsset(sceneNodeWorld(node,world,error),error);source.frame=assetMatrix(world);
            source.render=shape->reloadMakerRigidMeshJson();requireAsset(!source.render.isEmpty(),"Magazine export needs complete rigid source geometry");
            source.material=model->get<QString>(model->getBlockIndex(model->getLink(node,"Shader Property")),"Name");
            requireAsset(!source.material.isEmpty(),"A magazine piece is missing its material path");source.render["material"]=source.material;
            for(const auto& v:shape->verts)source.vertices.append({v[0],v[1],v[2]});
            for(const auto& t:shape->triangles)source.triangles.append({quint32(t[0]),quint32(t[1]),quint32(t[2])});
            total+=source.triangles.size();requireAsset(total<=1000000,"Magazine exceeds one million triangles");
            source.geometryHash=collisionGeometryHash(source);captured.sources.append(std::move(source));
        }
        requireAsset(!captured.sources.isEmpty(),"No visible magazine body found for this presentation");captured.inputTriangles=total;
    }
    _magazineWorldSnapshot=std::move(captured);_simulation=presentation;_magazineWorldPreview=true;setWorkspace(CollisionsWorkspace);refreshCollisions();
    const auto result=_collisions->command({{"operation","defaults"},{"target",_collisionTargets.size()-1}});
    setStatus(QString("%1 magazine captured in its carrier frame. Review world pieces and collision, then capture and cook the model.").arg(empty?"Empty":"Full"));return result;
}
}
