#include "ReloadMakerController.h"
#include "ReloadTemplates.h"
#include "ReloadBundle.h"
#include "model/nifmodel.h"
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker {
namespace {
void requireCapture(bool ok,const QString& reason){if(!ok)throw std::runtime_error(reason.toStdString());}
QJsonArray runtimePose(const Transformf& p){return {p.translation.x,p.translation.y,p.translation.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w,p.scale.x,p.scale.y,p.scale.z};}
QJsonArray runtimeCurve(const QVector<Transformf>& poses,int first,int last,bool reverse=false){
    requireCapture(first>=0&&last>=first&&last<poses.size(),"Invalid captured motion window");QJsonArray result;
    if(reverse){for(int i=last;i>=first;--i)result.append(runtimePose(poses[i]));}
    else {for(int i=first;i<=last;++i)result.append(runtimePose(poses[i]));}
    return result;
}
}
QString ReloadMakerController::runtimeTrackPath(int track) const {
    requireCapture(track>=0&&track<_reloadTrackNodes.size()&&_weaponBoneIndex>=0,"Missing native part binding");
    const auto binding=std::find_if(_nodeBindings.cbegin(),_nodeBindings.cend(),[&](const auto& b){return b.nodeIndex==_reloadTrackNodes[track];});
    requireCapture(binding!=_nodeBindings.cend(),"Part has no native animation binding");
    QStringList path;int bone=binding->boneIndex;
    for(int depth=0;bone>=0&&bone<_skeleton.bones.size()&&depth<64;++depth) {
        path.prepend(_skeleton.bones[bone].name);if(bone==_weaponBoneIndex)break;bone=_skeleton.bones[bone].parentIndex;
    }
    requireCapture(bone==_weaponBoneIndex&&path.size()>1,"Part is outside the native Weapon hierarchy");
    return path.join('/');
}
QJsonObject ReloadMakerController::exportRuntimeConfiguration() const {
    requireCapture(_simulationActive&&_simulationRig&&_reloadRecipe&&_magazineRail&&currentWeapon()&&configuration(),"Prepare this configuration in Operate first");
    requireCapture(_simulationError.isEmpty()&&_magazineError.isEmpty()&&_magazineGrabError.isEmpty(),"Resolve the current simulation/magazine errors before capture");
    requireCapture(_simulation.action==SimulationAction::Idle&&_simulation.magazine.seated(),"Reset Operate to a seated magazine before capturing runtime data");
    const auto& recipe=*_reloadRecipe;const auto& rig=*_simulationRig;const auto& rail=*_magazineRail;
    requireCapture(rig.mechanism==WeaponMechanism::BoltAction||_slideSpringReturn,"This template requires spring return; enable it before export");
    const auto anchor=simulationMagazineAnchor();const auto weapon=poseRecordKey(currentWeapon()->formId);
    const auto* receiver=resolvedReceiverObjectMod();
    requireCapture(receiver||recipe.tracks[rig.slide].anchor.ownerRecord.compare(weapon,Qt::CaseInsensitive)==0,"This template needs a concrete receiver selection");
    const auto receiverKey=receiver?poseRecordKey(receiver->formId):weapon;const auto magazineKey=anchor.ownerRecord;
    requireCapture(!magazineKey.isEmpty(),"The selected magazine has no source record");
    QJsonObject selection{{"receiver",receiverKey},{"magazine",magazineKey}};
    QJsonObject profile;QVector<int> indices=rig.slideTracks;
    const auto fullTrack=[&](int index){for(int parent=index,depth=0;parent>=0&&depth<recipe.tracks.size();parent=recipe.tracks[parent].parent,++depth)if(parent==rail.fullTrack)return true;return false;};
    const auto payloadTrack=[&](int index){return sampledMagazinePayload()&&index!=rail.fullTrack&&recipe.tracks[index].surface&&fullTrack(index);};
    const auto type=rig.mechanism;
    if(type==WeaponMechanism::PistolSlide){profile=exportSlideProfile();requireCapture(!profile.isEmpty(),"The pistol has no complete native slide binding");}
    else {
        if(!indices.contains(rig.bolt))indices.append(rig.bolt);
        if(type==WeaponMechanism::M1A)for(int i=0;i<recipe.tracks.size();++i)if(recipe.tracks[i].anchor.anchorName=="WeaponMagazineChild1"||recipe.tracks[i].anchor.anchorName=="WeaponMagazineChild2"||recipe.tracks[i].anchor.anchorName=="WeaponMagazineChild3")if(!indices.contains(i))indices.append(i);
        if(_magazineSetup)for(int i=0;i<recipe.tracks.size();++i)if(payloadTrack(i)&&!indices.contains(i))indices.append(i);
        profile={{"recipeId",recipe.id},{"animation",recipe.source["animation"]},{"animationSha256",recipe.source["animationSha256"]},
            {"duration",(rig.motionDuration>0?rig.motionDuration:recipe.duration)*(rig.closedFrame-rig.openFrame)/(recipe.frames-1)},
            {"slideTrack",indices.indexOf(rig.slide)},{"boltTrack",indices.indexOf(rig.bolt)}};
        QJsonArray tracks;
        for(const int index:indices) {
            requireCapture(index>=0&&index<recipe.tracks.size(),"An operating follower has no captured source track");
            const auto& source=recipe.tracks[index];const auto& a=source.anchor;
            const auto& poses=simulationTrackCurve(recipe,rig,index);
            QJsonObject track{{"name",a.anchorName},{"model",a.ownerModel},{"record",a.ownerRecord},{"sourcePath",a.anchorPath},
                {"runtimePath",runtimeTrackPath(index)},{"poses",runtimeCurve(poses,rig.openFrame,rig.closedFrame)}};
            if(type==WeaponMechanism::Ar15&&isSrepConfiguration()&&a.anchorName=="WeaponExtra2")track["boltFollower"]=true;
            if(_magazineSetup?payloadTrack(index):type==WeaponMechanism::M1A&&a.anchorName.startsWith("WeaponMagazineChild")) {
                const auto normalized=[&](int frame){Transformf inverse;requireCapture(invertUniformTransform(recipe.tracks[rail.fullTrack].poses[frame],inverse),"Invalid magazine payload frame");return composed(composed(recipe.tracks[rail.fullTrack].poses[0],inverse),source.poses[frame]);};
                QJsonArray payload;for(int frame=rig.openFrame;frame<=rig.closedFrame;++frame)payload.append(runtimePose(normalized(frame)));
                track["payload"]=true;track["poses"]=payload;track["loadedRest"]=runtimePose(normalized(recipe.frames-1));track["emptyRest"]=runtimePose(normalized(_m1aEmptyMagazineFrame));
            }
            if(type==WeaponMechanism::BoltAction) {
                const int slot=_boltCycle.tracks.indexOf(index);requireCapture(slot>=0&&slot<_boltCycle.poses.size()&&slot<_boltFiredPose.size(),"Incomplete manual bolt follower");
                track["opening"]=runtimeCurve(_boltCycle.poses[slot],_boltCycle.first,_boltCycle.rear);
                track["closing"]=runtimeCurve(_boltCycle.poses[slot],_boltCycle.rear,_boltCycle.last,true);
                track["firedRest"]=runtimePose(_boltFiredPose[slot]);
            }
            tracks.append(track);
        }
        profile["tracks"]=tracks;
        if(type==WeaponMechanism::BoltAction) {
            QJsonArray opening,closing;for(float value:_boltCycle.opening)opening.append(value);for(auto it=_boltCycle.closing.crbegin();it!=_boltCycle.closing.crend();++it)closing.append(1-*it);
            profile["openingProgress"]=opening;profile["closingProgress"]=closing;profile["inputAxis"]=QJsonArray{_boltCycle.inputAxis.x,_boltCycle.inputAxis.y,_boltCycle.inputAxis.z};profile["inputTravel"]=_boltCycle.inputTravel;profile["ejectProgress"]=_boltCycle.ejectProgress;
        } else {
            requireCapture(!_rifleBoltCycle.isEmpty(),"Missing native firing bolt motion");
            profile["fireCycle"]=runtimeCurve(_rifleBoltCycle,0,_rifleBoltCycle.size()-1);profile["fireOpen"]=_rifleBoltOpen;profile["fireDuration"]=_rifleBoltDuration;
        }
    }
    profile["receiver"]=receiverKey;profile["magazine"]=magazineKey;
    if(rig.customSetup){profile["operatingSetup"]=rig.authoring;profile["motionSource"]=rig.mechanicalSource;}
    const auto& operating=recipe.tracks[rig.slide].anchor;
    if(operating.ownerRecord!=receiverKey&&operating.ownerRecord!=magazineKey){selection["operatingPart"]=operating.ownerRecord;profile["operatingPart"]=operating.ownerRecord;}
    const auto interactions=exportInteractionGrips();requireCapture(!interactions.isEmpty(),"Resolve the operating finger assignments before capture");
    const auto fingers=interactions["operatingFingers"].toArray();requireCapture(type==WeaponMechanism::PistolSlide||fingers.size()==2,"Configure operating fingers for both hands before capture");if(!fingers.isEmpty())profile["operatingFingers"]=fingers;

    const auto& full=recipe.tracks[rail.fullTrack];
    QJsonObject magazine{{"record",magazineKey},{"model",anchor.ownerModel},{"capacity",_simulationAmmo.capacity},
        {"rail",magazineRailJson(rail)},{"seat",runtimePose(full.poses[0])},{"fullName",full.anchor.anchorName},
        {"emptyName",recipe.tracks[rail.emptyTrack].anchor.anchorName},{"sourceRecipeId",recipe.id},{"emptyHidePrefix",""}};
    magazine["fullPath"]=runtimeTrackPath(rail.fullTrack);magazine["emptyPath"]=runtimeTrackPath(rail.emptyTrack);
    if(_magazineSetup)magazine["presentationSetup"]=magazineSetupJson(*_magazineSetup);
    if(_magazineSetup&&_magazineSetup->motion=="manual") {
        const auto movement=std::find_if(_mechanisms.cbegin(),_mechanisms.cend(),[&](const auto& m){return m.id==_magazineSetup->movementId;});
        requireCapture(movement!=_mechanisms.cend(),"The authored magazine movement is missing");QString sourceError;
        const auto bytes=encodeMechanism(*movement,sourceError);requireCapture(!bytes.isEmpty(),sourceError);
        magazine["motionSource"]=QJsonObject{{"method","authored-part-local-stages"},{"definition",QJsonDocument::fromJson(bytes).object()}};
    }
    QString error;
    for(const auto hand:{PoseHand::Left,PoseHand::Right}) {
        if(hand==PoseHand::Right&&type==WeaponMechanism::PistolSlide)continue;
        const auto grip=magazineGripFor(weapon,anchor,_partGrips,_magazineGrabGrips,_magazineGrabDefaults,error,hand);
        requireCapture(grip.has_value(),hand==PoseHand::Left?"Configure the left magazine hand before capture":"Configure the right magazine hand before capture");
        const auto bytes=encodeHandPose(grip->pose,error);requireCapture(!bytes.isEmpty(),error);
        magazine[hand==PoseHand::Left?"grip":"rightGrip"]=QJsonDocument::fromJson(bytes).object();
    }
    // This is the native carrier/reference bridge, not the skeleton-binding
    // bridge used by the offline arm solver. Both roots are read in one frame.
    int reference=-1;for(int i=0;i<_handPoseAnchors.size();++i)if(sameGripAnchor(_handPoseAnchors[i].description,anchor)){reference=i;break;}
    requireCapture(reference>=0&&rail.fullTrack<_reloadTrackNodes.size(),"Missing native magazine reference");
    Transformf carrier,referenceWorld,inverse;
    requireCapture(sceneNodeWorld(_sceneNodes[_reloadTrackNodes[rail.fullTrack]],carrier,error)&&sceneNodeWorld(_handPoseAnchors[reference].node,referenceWorld,error)&&invertUniformTransform(carrier,inverse),"Cannot establish the magazine carrier frame");
    magazine["modelInMagazine"]=runtimePose(composed(inverse,referenceWorld));
    const auto referenceSetup=_authoringRoadmaps.value(roadmapKey()).toObject().value("magazineReference").toObject();
    if(!referenceSetup.isEmpty()&&referenceSetup.value("connectionMode")!="source") {
        const auto point=composed(inverse,rail.connection).translation;
        magazine["capturePointInMagazine"]=QJsonArray{point.x,point.y,point.z};
    }
    QJsonArray parts,emptyParts;
    const auto normalizedPart=[&](const ReloadTrack& track,int frame){Transformf inv;requireCapture(invertUniformTransform(full.poses[frame],inv),"Invalid payload carrier");return composed(composed(full.poses[0],inv),track.poses[frame]);};
    // These assemblies synthesize timing-only magazine bones absent from their
    // native meshes. Other established templates retain their complete chain.
    const bool nativeSurfacesOnly=isMk18Configuration()||isSrepConfiguration();
    for(int index=0;index<recipe.tracks.size();++index){const auto& track=recipe.tracks[index];if((_magazineSetup?fullTrack(index):track.anchor.anchorName.startsWith("WeaponMagazine"))&&(!nativeSurfacesOnly||track.surface)) {
        const int frame=sampledMagazinePayload()?_m1aLoadedMagazineFrame:0;const auto pose=sampledMagazinePayload()?normalizedPart(track,frame):track.poses[0];
        if(pose.scale.x>1e-5f)parts.append(QJsonObject{{"name",track.anchor.anchorName},{"pose",runtimePose(pose)}});
        if(_magazineSetup?sampledMagazinePayload()&&index!=rail.fullTrack&&fullTrack(index):type==WeaponMechanism::M1A&&track.anchor.anchorName.startsWith("WeaponMagazineChild")){const auto empty=normalizedPart(track,_m1aEmptyMagazineFrame);if(empty.scale.x>1e-5f)emptyParts.append(QJsonObject{{"name",track.anchor.anchorName},{"pose",runtimePose(empty)}});}
    }}
    magazine["parts"]=parts;if(!emptyParts.isEmpty())magazine["emptyParts"]=emptyParts;
    QJsonArray motion;for(int i=0;i<rail.motion.size();++i)motion.append(QJsonObject{{"progress",rail.motionDistances[i]/rail.motionDistances.back()},{"pose",runtimePose(rail.motion[i])}});magazine["motion"]=motion;
    auto* model=activeModel();const auto root=_sceneNodes[_reloadTrackNodes[rail.fullTrack]];
    const auto below=[&](QModelIndex child,const QModelIndex& parent){for(int i=0;child.isValid()&&i<model->getBlockCount();++i){if(child==parent)return true;child=model->getBlockIndex(model->getParent(child));}return false;};
    QJsonArray payload;for(const auto& visual:_magazineVisuals)if(visual.cartridge&&!visual.nativeOnly&&!visual.empty&&visual.node.isValid()&&below(visual.node,root)){
        QModelIndex candidate=visual.node;
        for(int depth=0;depth<model->getBlockCount();++depth){const auto parent=model->getBlockIndex(model->getParent(candidate));if(!parent.isValid()||parent==root)break;
            const bool body=std::any_of(_magazineVisuals.begin(),_magazineVisuals.end(),[&](const auto& other){return !other.nativeOnly&&!other.empty&&!other.cartridge&&below(other.node,parent);});
            if(body)break;
            candidate=parent;
        }
        const auto name=model->get<QString>(candidate,"Name");requireCapture(!name.isEmpty()&&name!="WeaponMagazine","Payload needs an explicit non-body selector");if(!payload.contains(name))payload.append(name);
    }
    requireCapture(payload.size()<=16,"Payload needs fewer independently named groups for this template");
    if(!payload.isEmpty())magazine["emptyHideNodes"]=payload;
    if(_magazineSetup)for(bool fullBody:{true,false}) {
        const auto carrier=_sceneNodes[_reloadTrackNodes[fullBody?rail.fullTrack:rail.emptyTrack]];QJsonArray excluded;
        for(const auto& visual:_magazineVisuals)if(visual.nativeOnly&&visual.node.isValid()&&below(visual.node,carrier)) {
            QStringList names;auto node=QModelIndex(visual.node);
            for(int depth=0;node.isValid()&&depth<model->getBlockCount();++depth){const auto name=model->get<QString>(node,"Name");requireCapture(!name.isEmpty(),"Excluded magazine pieces need named native ancestry");names.prepend(name);if(node==carrier)break;node=model->getBlockIndex(model->getParent(node));}
            excluded.append(names.join('/'));
        }
        if(!excluded.isEmpty())magazine[fullBody?"excludedFullPieces":"excludedEmptyPieces"]=excluded;
    }
    if(isSrepEmbeddedMagazine())magazine["parkedMagazinePath"]="Weapon/WeaponOptics2/MagHelperRotate/MagHelper";
    else if(isMk18Configuration())magazine["parkedMagazinePath"]="Weapon/WeaponExtra3";
    const auto grabs=exportGrabLocations();requireCapture(type==WeaponMechanism::PistolSlide||!grabs.isEmpty(),"Configure the operating surface grab points before capture");
    const auto sources=sourceEvidence();requireCapture(auditSources(sources)["compatible"].toBool(),"Source resources changed; reload and review the configuration before export");
    QSet<QString> criticalModels{normalizeNifPath(currentWeapon()->baseModelPath),normalizeNifPath(anchor.ownerModel)};
    auto criticalTracks=rig.slideTracks;for(int track:rig.magazineTracks)if(!criticalTracks.contains(track))criticalTracks.append(track);
    if(rig.bolt>=0&&!criticalTracks.contains(rig.bolt))criticalTracks.append(rig.bolt);
    for(int track:criticalTracks)criticalModels.insert(normalizeNifPath(recipe.tracks[track].anchor.ownerModel));
    QJsonArray runtimeSources;for(const auto& value:sources["resources"].toArray()){const auto source=value.toObject();
        if(source["kind"]=="animation"||criticalModels.contains(source["path"].toString()))runtimeSources.append(source);}
    QJsonObject header{{"format",type==WeaponMechanism::PistolSlide?"diamond-pistol-pack":"diamond-weapon-pack"},{"version",1},{"weapon",weapon}};
    QJsonArray catalogSlots;
    if(type==WeaponMechanism::PistolSlide) {
        header["standardReceiver"]=receiverKey;header["strictReceiver"]=true;
        for(const auto* mod:configuredObjectMods()){
            const auto key=poseRecordKey(mod->formId);const auto* slot=configuration()->slot(configurationSlotKey(*mod));
            catalogSlots.append(QJsonObject{{"name",key==receiverKey?"Receiver":key==magazineKey?"Magazine":slot?slot->label:key},{"records",QJsonArray{key}}});
            if(key==magazineKey&&mod->attachPoint)header["magazineAttachPoint"]=poseRecordKey(mod->attachPoint);
        }
    }else {
        header["mechanism"]=type==WeaponMechanism::Ar15?"ar15":type==WeaponMechanism::M1A?"m1a":type==WeaponMechanism::BoltAction?"manual-bolt-action":"reciprocating-bolt";
        header["standardReceiver"]=receiverKey;header["strictReceiver"]=receiver!=nullptr;
        for(const auto& pair:{qMakePair(QString("receiver"),QString("Receiver")),qMakePair(QString("magazine"),QString("Magazine")),qMakePair(QString("operatingPart"),QString("OperatingPart"))})if(selection.contains(pair.first)){
            const auto record=selection[pair.first].toString();catalogSlots.append(QJsonObject{{"name",pair.second},{"records",QJsonArray{record}}});
            if(pair.first!="receiver")for(const auto* mod:configuredObjectMods())if(poseRecordKey(mod->formId)==record&&mod->attachPoint)
                header[pair.first=="magazine"?"magazineAttachPoint":"operatingAttachPoint"]=poseRecordKey(mod->attachPoint);
        }
        if(type==WeaponMechanism::Ar15&&isSrepConfiguration())header["manualBoltFromReload"]=true;
        if(isSrepEmbeddedMagazine())header["embeddedMagazineReceivers"]=QJsonArray{receiverKey};
    }
    header["slots"]=catalogSlots;
    const auto compatibility=sourcePolicyState();const auto policy=compatibility["policy"].toObject();
    for(auto it=policy.begin();it!=policy.end();++it)header[it.key()]=it.value();
    return {{"format","reload-maker-runtime-configuration"},{"version",1},{"weapon",weapon},{"template",templateIdentity(type)},
        {"sourcePolicy",policy},{"sourcePolicyEdited",compatibility["saved"]},
        {"name",currentWeapon()->name},{"profileHeader",header},
        {"selection",selection},{"profile",profile},{"magazine",magazine},{"grabs",grabs},
        {"sounds",exportSounds()},{"sources",sources},{"runtimeSources",runtimeSources},{"worldModelSpace","magazine-carrier"}};
}
}
