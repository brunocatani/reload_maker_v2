#include "ReloadMakerController.h"
#include "AuthoringProgress.h"
#include "ReloadBundle.h"
#include "ReloadTemplates.h"
#include "CollisionPanel.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QComboBox>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace reloadmaker {
void ReloadMakerController::rememberCollisionDraft() {
    if(!_collisions||!_reloadRecipe||!currentWeapon())return;
    const auto& profile=_collisions->profile();const auto reference=profile.referencePose;const auto presentation=reference["presentation"].toString();
    if(!profile.valid()||reference["space"]!="magazine-carrier"||(presentation!="full"&&presentation!="empty")||reference["sourceRecipeId"]!=_reloadRecipe->id)return;
    const auto key=profile.weapon.toLower()+"/"+profile.configuration;if(key!=roadmapKey())return;
    auto plan=_authoringRoadmaps.value(key).toObject();if(!plan.contains("step"))plan["step"]=10;
    auto drafts=plan.value("collisionDrafts").toObject();drafts[presentation]=QJsonObject{
        {"policy",collisionDraftPolicy(_reloadRecipe->id,reference["sources"].toObject(),reference["presentationSetup"].toObject())},
        {"usable",std::any_of(profile.hulls.cbegin(),profile.hulls.cend(),[&](const auto& hull){return hull.enabled&&std::any_of(profile.sources.cbegin(),profile.sources.cend(),[&](const auto& source){return source.key==hull.source&&source.worldIncluded;});})},
        {"edit",QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-')}};
    plan["collisionDrafts"]=drafts;_authoringRoadmaps[key]=plan;
}
QJsonArray ReloadMakerController::roadmapProgress() const {
    std::array<AuthoringStepInput,12> steps;const auto saved=_authoringRoadmaps.value(roadmapKey()).toObject();
    const bool loaded=currentWeapon()&&configuration()&&!_loadThread&&!_reloadCapturing&&!_reloadCaptureQueued&&!_rebuildTimer->isActive();
    const bool captured=loaded&&_reloadRecipe.has_value(),rig=captured&&_simulationRig.has_value()&&_simulationRig->recipeId==_reloadRecipe->id,magazine=rig&&_magazineRail.has_value()&&_magazineError.isEmpty();
    QJsonObject sources;QString sourceError;
    if(captured)try{sources=sourceEvidence();}catch(const std::exception& e){sourceError=e.what();}
    steps[0]={true,captured&&!sources.isEmpty()&&saved["sources"]==sources,
        !sourceError.isEmpty()?sourceError:captured?"Record and review the resolved source inputs. Check source changes again before exporting.":"Connect a source and load the weapon.",sources};
    steps[1]={loaded,captured,captured?"Native assembly and reload capture are available.":"Complete the assembly and wait for its native capture.",
        {{"choices",loaded?configuration()->savedChoices(*currentPlugin()):QJsonObject{}},{"recipe",captured?_reloadRecipe->id:QString()},{"ammunition",rig?_simulationAmmo.json():QJsonObject{}}}};
    steps[2]={loaded,!assignedTemplate().isEmpty(),assignedTemplate().isEmpty()?"Choose the intended supported behavior template.":"Template saved for this weapon.",{{"template",assignedTemplate()}}};
    QJsonArray followers;if(rig)for(int track:_simulationRig->slideTracks)followers.append(track);
    const QJsonObject roles=rig?QJsonObject{{"operating",_simulationRig->slide},{"bolt",_simulationRig->bolt},{"magazine",_simulationRig->magazine},{"followers",followers}}:QJsonObject{};
    steps[3]={captured,rig,rig?"Review the native operating part and every independent follower.":_simulationError,roles};
    QCryptographicHash motionHash(QCryptographicHash::Sha256);
    const auto motion=[&](const QVector<Transformf>& curve){for(const auto& p:curve){const std::array<float,10> values{p.translation.x,p.translation.y,p.translation.z,p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w,p.scale.x,p.scale.y,p.scale.z};motionHash.addData(QByteArrayView(reinterpret_cast<const char*>(values.data()),sizeof(values)));}};
    if(rig){for(int track:_simulationRig->slideTracks)motion(simulationTrackCurve(*_reloadRecipe,*_simulationRig,track));if(_simulationRig->bolt>=0&&!_simulationRig->slideTracks.contains(_simulationRig->bolt))motion(simulationTrackCurve(*_reloadRecipe,*_simulationRig,_simulationRig->bolt));if(isRifleConfiguration())motion(_rifleBoltCycle);if(isBoltActionConfiguration()){for(const auto& curve:_boltCycle.poses)motion(curve);for(const auto* progress:{&_boltCycle.opening,&_boltCycle.closing})motionHash.addData(QByteArrayView(reinterpret_cast<const char*>(progress->constData()),progress->size()*sizeof(float)));}}
    steps[4]={captured,rig&&_simulationError.isEmpty(),_simulationError.isEmpty()?"Test the complete opening, closing, firing and ejection movement.":_simulationError,
        {{"setup",rig?_simulationRig->authoring:QJsonObject{}},{"source",rig?_simulationRig->mechanicalSource:QJsonObject{}},{"fireSource",isRifleConfiguration()?_rifleBoltSource:QString()},{"motionHash",QString::fromLatin1(motionHash.result().toHex())},
         {"openFrame",rig?_simulationRig->openFrame:-1},{"closedFrame",rig?_simulationRig->closedFrame:-1},{"fireDuration",isRifleConfiguration()?_rifleBoltDuration:0},
         {"boltTravel",isBoltActionConfiguration()?_boltCycle.inputTravel:0},{"boltEjection",isBoltActionConfiguration()?_boltCycle.ejectProgress:0}}};
    QJsonObject presentation=saved.value("magazineSetup").toObject();if(magazine){presentation["fullTrack"]=_magazineRail->fullTrack;presentation["emptyTrack"]=_magazineRail->emptyTrack;presentation["loadedFrame"]=sampledMagazinePayload()?_m1aLoadedMagazineFrame:0;presentation["emptyFrame"]=sampledMagazinePayload()?_m1aEmptyMagazineFrame:0;}
    if(saved.contains("magazineReference"))presentation["reference"]=saved.value("magazineReference");
    steps[5]={rig,magazine,_magazineError.isEmpty()?"Inspect full, empty and pre-feed presentations around the native carrier.":_magazineError,presentation};
    steps[6]={magazine,magazine,"Test pickup along the finite straight guide and inspect the independent visible movement.",magazine?magazineRailJson(*_magazineRail):QJsonObject{}};
    QJsonArray hands;bool fitted=magazine;QString handError;
    if(magazine)for(const auto hand:{PoseHand::Left,PoseHand::Right}) {
        if(hand==PoseHand::Right&&simulationMechanism()==WeaponMechanism::PistolSlide)continue;
        QString error;const auto fit=magazineGripFor(poseRecordKey(currentWeapon()->formId),simulationMagazineAnchor(),_partGrips,_magazineGrabGrips,_magazineGrabDefaults,error,hand);
        if(!fit){fitted=false;handError=error.isEmpty()?"Assign every required magazine hand.":error;continue;}
        hands.append(QJsonDocument::fromJson(encodeHandPose(fit->pose,error)).object());
    }
    auto operating=rig?exportInteractionGrips():QJsonObject{};operating.remove("grabLocations");
    const bool fingers=rig&&_operatingFingerError.isEmpty()&&(simulationMechanism()==WeaponMechanism::PistolSlide||operating["operatingFingers"].toArray().size()==2);
    steps[7]={captured,fitted&&fingers,!handError.isEmpty()?handError:!fingers?(_operatingFingerError.isEmpty()?QString("Assign operating fingers for both hands."):_operatingFingerError):QString("Inspect required magazine hands and live-wrist operating fingers."),{{"magazineHands",hands},{"operating",operating},{"handMesh",_bodyCombo?_bodyCombo->currentData().toString():QString()}}};
    const auto locations=rig?exportGrabLocations():QJsonObject{};
    steps[8]={rig,rig&&(simulationMechanism()==WeaponMechanism::PistolSlide||!locations.isEmpty()),locations.isEmpty()?"No picked policy. Pistol templates may retain default part acquisition; other templates need surface points.":"Verify picked points on the actual part while closed, partly open and fully open.",locations};
    const auto sounds=rig?exportSounds():QJsonObject{};
    steps[9]={rig,rig&&!sounds.isEmpty()&&_soundError.isEmpty(),_soundError.isEmpty()?"Audition directional crossings, hold/reversal and dry-fire events.":_soundError,sounds};
    const auto drafts=saved.value("collisionDrafts").toObject();const auto policy=collisionDraftPolicy(captured?_reloadRecipe->id:QString(),sources,saved.value("magazineSetup").toObject());
    const bool collisionReady=magazine&&drafts["full"].toObject()["usable"].toBool()&&drafts["empty"].toObject()["usable"].toBool()&&drafts["full"].toObject()["policy"]==policy&&drafts["empty"].toObject()["policy"]==policy;
    steps[10]={magazine,collisionReady,collisionReady?"Review both collision drafts. Finished NIFs and cook receipts are still validated independently when saving/exporting the pack.":"Generate or import full and empty magazine collision drafts for this source and presentation.",drafts};
    const bool exportable=rig&&magazine&&fitted&&fingers&&steps[0].ready&&steps[2].ready&&steps[4].ready&&steps[8].ready&&steps[9].ready&&collisionReady;
    steps[11]={rig,exportable,exportable?"Test Operate, save the selected configuration, then export the complete pack. Author review does not bypass validation.":"Resolve the preceding source, motion, hand, point or sound requirements before export.",{{"ready",exportable}}};
    return authoringProgress(steps,saved.value("reviews").toObject());
}
}
