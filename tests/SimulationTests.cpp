#include "WeaponSimulation.h"
#include "BoltCycle.h"
#include "OperatingSetup.h"
#include "MagazineGuide.h"
#include "MagazineReference.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTextStream>
#include <QUuid>
#include <stdexcept>
#include <cmath>
using namespace reloadmaker;
namespace {
void check(bool valid,const QString& error){if(!valid)throw std::runtime_error(error.toStdString());}
void grabDeployment() {
    ReloadRecipe r;r.frames=5;r.duration=4;r.tracks.resize(2);
    for(int i=0;i<5;++i) {
        const float y=i<3?0.f:-5.f*(i-2);
        r.tracks[0].poses.append(Transformf{{0,y,0}});
        r.tracks[1].poses.append(Transformf{{2,y,0},i==0?Quaternionf{}:Quaternionf{0,0,.70710678f,.70710678f}});
    }
    SimulationRig rig;rig.bolt=0;rig.slide=1;rig.slideTracks={0,1};rig.closedFrame=0;rig.openFrame=4;
    QString error;check(buildGrabDeployment(r,rig,2,error),error);
    for(float u:{0.f,.25f,.5f,.75f,1.f}) {
        const auto bolt=simulationGrabPose(rig,0,u),handle=simulationGrabPose(rig,1,u);
        check(std::abs(bolt.translation.y+10*u)<.001f,"Source pause compressed the physical pull");
        check(std::abs(handle.translation.y-bolt.translation.y)<.001f&&std::abs(handle.rotation.z-.70710678f)<.001f,"Deployed handle did not stay on the bolt");
    }
    check(simulationGrabPose(rig,0,0).translation.y==0,"Deployment moved the bolt rearward");
    check(simulationTrackPose(r,rig,1,0).rotation.w==1,"Grab deployment changed the folded idle pose");
    // The bolt is already open when a non-reciprocating handle unfolds at its forward stop.
    r.tracks.resize(3);r.tracks[2]=r.tracks[1];rig.slideTracks={1,2};
    r.tracks[1].poses[4].translation.y=0;r.tracks[2].poses[4].translation.y=0;
    check(buildGrabDeployment(r,rig,4,error,true),error);
    for(float u:{0.f,.25f,.5f,.75f,1.f}) {
        check(std::abs(simulationGrabPose(rig,0,u).translation.y+10*u)<.001f,"Independent handle omitted the held bolt curve");
        for(int track:{1,2})check(std::abs(simulationGrabPose(rig,track,u).translation.y+10*u)<.001f,"Independent handle or carriage lost its forward reference");
    }
    OperatingSetup setup;setup.mode="reload";setup.parts.operating=1;setup.parts.bolt=0;setup.parts.magazine=2;setup.parts.followers={1};setup.grabDeploySeconds=2;setup.grabDeployIndependent=true;
    OperatingSetup decoded;check(readOperatingSetup(operatingSetupJson(setup),decoded,error),error);
    check(decoded.grabDeployIndependent,"Independent deployment selection did not round trip");
    setup.mode="clip";setup.clip="handle.hkx";setup.baseline="ready.hkx";
    check(readOperatingSetup(operatingSetupJson(setup),decoded,error),error);
    rig.motionDuration=8;rig.nativeProgress={0,.2f,.5f,.7f,1};
    rig.authoredCurves.insert(1,r.tracks[1].poses);rig.authoredCurves.insert(2,r.tracks[2].poses);
    rig.authoredCurves[1][2].translation.x=9;rig.authoredCurves[2][2].translation.x=10;
    check(buildGrabDeployment(r,rig,4,error,true),error);
    check(std::abs(simulationGrabPose(rig,1,0).translation.x-9)<.001f&&std::abs(simulationGrabPose(rig,2,0).translation.x-10)<.001f,"Deployment sampled the magazine clock instead of the operating clip");
}
void nativeSourceTimes() {
    OperatingSetup setup;setup.mode="clip";setup.clip="source.hkx";setup.baseline="idle.hkx";
    setup.parts.operating=setup.parts.bolt=0;setup.parts.magazine=1;setup.parts.followers={0};
    setup.sourceTimes=true;setup.openSeconds=.15;setup.closedSeconds=0;
    QString error;OperatingSetup decoded;
    const auto document=operatingSetupJson(setup);
    check(document["version"]==2&&!document.contains("openFrame"),"Native time settings retained frame authority");
    check(readOperatingSetup(document,decoded,error),error);
    check(operatingSetupJson(decoded)==document,"Source-time round trip changed the window");
    for(int frames:{249,321,361,401,501}) {
        auto parts=decoded.parts;QVector<float> times;
        check(operatingTimeSamples(decoded,3,frames,parts,times,error),error);
        check(std::abs(times[parts.openFrame]-.15)<1e-6&&times[parts.closedFrame]==0,"Capture size changed a source endpoint");
        for(int i=1;i<times.size();++i)check(times[i]>=times[i-1],"Source window introduced a time reversal");
        ReloadRecipe recipe;recipe.frames=frames;recipe.duration=4;
        SimulationRig rig;rig.motionFrames=361;rig.motionDuration=3;rig.openFrame=342;rig.closedFrame=360;rig.strokeSeconds=.15f;
        check(std::abs(simulationStrokeDuration(recipe,rig)-.15f)<1e-7,"Magazine clock changed operating duration");
    }
    decoded.openSeconds=.123456;decoded.closedSeconds=.287654;
    auto parts=decoded.parts;QVector<float> times;
    check(operatingTimeSamples(decoded,3,361,parts,times,error),error);
    check(std::abs(times[parts.openFrame]-.123456)<1e-6&&std::abs(times[parts.closedFrame]-.287654)<1e-6,"Source times were rounded to frame boundaries");
    decoded.closedSeconds=4;check(!operatingTimeSamples(decoded,3,361,parts,times,error),"Window outside the clip was sampled");
    auto invalid=document;invalid["closedSeconds"]=-1;check(!readOperatingSetup(invalid,decoded,error),"Mixed automatic and explicit times accepted");
    invalid=document;invalid["openSeconds"]=-.5;check(!readOperatingSetup(invalid,decoded,error),"Negative source time accepted");
    invalid=document;invalid["openFrame"]=18;check(!readOperatingSetup(invalid,decoded,error),"Conflicting frame and time authorities accepted");
    // Existing manual movement records keep their positions, assignments and clock.
    setup.sourceTimes=false;setup.mode="manual";setup.parts.openFrame=18;setup.parts.closedFrame=0;setup.movements[0]="authored-movement";
    const auto manual=operatingSetupJson(setup);check(readOperatingSetup(manual,decoded,error),error);
    check(operatingSetupJson(decoded)==manual&&!decoded.sourceTimes,"Manual movement was converted to native source time");
    parts=decoded.parts;check(operatingTimeSamples(decoded,3,361,parts,times,error),error);
    check(parts.openFrame==18&&parts.closedFrame==0,"Manual window changed");
    ReloadRecipe native;native.duration=1;native.frames=5;native.tracks.resize(1);
    for(int i=0;i<5;++i)native.tracks[0].poses.append(Transformf{{float(i),0,0}});
    SimulationRig independent;independent.motionFrames=9;independent.openFrame=8;independent.closedFrame=0;
    check(std::abs(simulationNativeTrackPose(native,independent,0,1).translation.x-4)<1e-6,"Independent clock indexed outside native source");
    independent.motionFrames=3;independent.openFrame=2;independent.nativeProgress={0,.1f,1};
    check(std::abs(simulationNativeFrame(native,independent,1)-.4f)<1e-6,"Fractional source time lost its native reference frame");
    check(std::abs(simulationNativeTrackPose(native,independent,0,.5f).translation.x-.4f)<1e-6,"Native reference pose ignored the source-time mapping");
}
void carrierOwnership() {
    ReloadRecipe recipe;recipe.frames=3;recipe.tracks.resize(3);
    for(int i=0;i<3;++i) {
        auto& track=recipe.tracks[i];track.surface=true;track.parent=i-1;
        track.anchor.anchorName=i==2?"WeaponMagazineChild2":"WeaponMagazine";
        track.poses={Transformf{},Transformf{{0,1,0}},Transformf{{0,2,0}}};
    }
    check(magazineCarrier(recipe,1)==0,"Duplicate rigid magazine node was not reduced to its carrier");
    check(magazineCarrier(recipe,2)==2,"Explicit inner magazine carrier was replaced by its parent");
    recipe.tracks[1].poses[1].translation.x=1;
    check(magazineCarrier(recipe,1)==1,"Independently moving magazine was attached to another carrier");
    check(magazineCarrier(recipe,-1)==-1&&magazineCarrier(recipe,3)==3,"Invalid carrier index was dereferenced");
    recipe.tracks.resize(1);
    recipe.tracks[0].poses[1].rotation={.09983342f,0,0,.99500417f};
    recipe.tracks[0].poses[2].rotation=recipe.tracks[0].poses[1].rotation;
    const QVector<QVector3D> body{{-1,-1,-2},{1,1,2},{1,-1,2}};
    const QVector<QVector3D> weapon{{-3,-1,-3},{3,3,3}};
    MagazineRail rail;QString error;
    check(buildMagazineRail(recipe,0,"Magazine",{},body,weapon,rail,error,false,0),error);
    check((rail.outward-QVector3D(0,1,0)).length()<1e-5f,"Rotating extraction lost its measured near-well input direction");
}
void guideAuthoring() {
    HandPosePlacement anchor{"animated-part","CustomBox","Root/CustomBox","Model.nif","Mod.esp:001234"};
    MagazineReference reference{QString(64,'a'),magazineReferenceKey(anchor),"custom",Transformf{{1,2,3},{0,0,.70710678f,.70710678f}}};
    QString referenceError;MagazineReference parsed;
    check(readMagazineReference(magazineReferenceJson(reference),parsed,referenceError),referenceError);
    check(reloadPoseDistance(reference.connectionInCarrier,parsed.connectionInCarrier)<1e-5f,"Connection frame changed during round trip");
    auto renamed=anchor;renamed.anchorPath+="/Other";check(magazineReferenceKey(anchor)!=magazineReferenceKey(renamed),"Reference identity omitted its exact path");
    auto invalid=magazineReferenceJson(reference);invalid["anchorKey"]="CustomBox";check(!readMagazineReference(invalid,parsed,referenceError),"Ambiguous named reference accepted");
    MagazineRail original;original.connection.rotation={0,0,.70710678f,.70710678f};original.outward={0,0,-1};original.travel=10;original.captureRadius=1;original.releaseMargin=.6f;
    MagazineGuide guide{QString(64,'a'),QVector3D(1,0,0),17,1.25f,.8f};QString error;MagazineGuide decoded;
    check(readMagazineGuide(magazineGuideJson(guide),decoded,error),error);
    auto changed=original;check(applyMagazineGuide(decoded,changed,error),error);
    check((changed.outward-QVector3D(0,1,0)).length()<.0001f,"Guide ignored the connection rotation");
    check((magazineAnimationOffset(changed,changed.travel)-changed.outward*changed.travel).length()<.0001f,"Straight visual entry did not follow the guide");
    guide.travel=23;guide.outwardInConnection={0,1,0};check(applyMagazineGuide(guide,changed,error),error);
    check((magazineAnimationOffset(changed,changed.travel)-changed.outward*changed.travel).length()<.0001f,"Repeated straight guide edit retained the old endpoint");
    auto linear=changed;check(applyMagazineGuide(guide,linear,error,true),error);
    for(float u:{0.f,.25f,.75f,1.f}) {
        const auto delta=magazineAnimationDelta(linear,u*linear.travel);
        const auto expected=linear.outward*(u*linear.travel);
        check(reloadPoseDistance(delta,Transformf{{expected.x(),expected.y(),expected.z()}})<.0001f,"Explicit linear presentation ignored its authored insertion axis");
    }
    MagazineMotion mag;mag.phase=MagazinePhase::Free;mag.offset=changed.outward*11+QVector3D(0,0,3);mag.moveFree(changed,changed.outward*11-QVector3D(0,0,3));
    check(mag.phase==MagazinePhase::Rail&&std::abs(mag.distance-11)<.001f,"Custom guide did not capture at the swept entry depth");
    mag.moveRail(changed,0);check(mag.seated(),"Custom guide did not seat");
    for(const auto& pair:{qMakePair(QString("travel"),QJsonValue(0)),qMakePair(QString("captureRadius"),QJsonValue(30)),qMakePair(QString("releaseMargin"),QJsonValue(-1)),qMakePair(QString("outwardInConnection"),QJsonValue(QJsonArray{0,0,0}))}) {
        auto bad=magazineGuideJson(guide);bad[pair.first]=pair.second;check(!readMagazineGuide(bad,decoded,error),"Invalid guide was accepted");
    }
}
void stateTransitions() {
    WeaponSimulation s;s.configureAmmunition(17,true);check(s.dropMagazine(),"Drop must work at rest");
    check(!s.magazinePresent()&&s.fire(),"Removing magazine must retain the chambered shot");
    check(!s.grab()&&!s.reload()&&!s.dropMagazine(),"A native action must own the scene until complete");
    check(s.tick(1,.2f)&&!s.magazinePresent(),"Fire must preserve a removed magazine");
    check(s.grab(),"Grab failed");s.drag(.6f);s.release(false,true);
    s.tick(.05f,1);check(s.slide>.39f&&s.slide<.41f,"Spring must advance from the current pull");
    check(s.grab(),"Regrab during return failed");s.drag(.9f);s.release(true,true);
    check(s.action==SimulationAction::Idle&&s.slide>.39f&&s.slide<.41f,"Cancel must restore the press-time state");
    check(!s.fire(),"Open slide must reject fire");
    check(s.grab(),"Grab failed");s.drag(1);s.release(false,false);
    check(s.slide==1&&s.action==SimulationAction::Idle,"Hold mode lost its position");
    check(s.reload()&&s.magazinePresent(),"Automatic reload must restore the assembly");
    check(s.tick(3,3)&&s.slide==0,"Reload completion must restore idle");
    s.dropMagazine();s.reset();check(s.magazinePresent()&&s.slide==0,"Reset failed");
}
void fixture(const QString& path) {
    QFile f(path);check(f.open(QIODevice::ReadOnly),"Missing fixture");
    auto json=QJsonDocument::fromJson(f.readAll()).object();if(json.contains("reloadDraft"))json=json["reloadDraft"].toObject();
    ReloadRecipe r;QString error;check(decodeReloadRecipe(QJsonDocument(json).toJson(),r,error),error);
    SimulationRig rig;check(buildSimulationRig(r,rig,error),error);
    check(rig.slide==0&&rig.magazine==4,"Glock owner identity mismatch");
    check(rig.openFrame==248&&rig.closedFrame==260,"Wrong stable closing stroke");
    check(rig.slideTracks==QVector<int>({0,1,2,6}),"Must retain barrel/catch/cartridge and exclude spare magazine");
    check(rig.magazineTracks==QVector<int>({3,4,5,6,7,8,9}),"Magazine ownership omitted a sibling or descendant");
    const auto slide=simulationTrackPose(r,rig,0,1),barrel=simulationTrackPose(r,rig,1,1);
    check(slide.translation.y< -3&&std::abs(barrel.rotation.x)>.02f,"Barrel tilt was replaced with rigid slide translation");
    check(reloadPoseDistance(simulationTrackPose(r,rig,6,.5f),simulationTrackPose(r,rig,6,0))>.1f,"Cartridge trajectory is missing");
    const auto original=rig;SimulationParts roles;roles.operating=rig.slide;roles.bolt=rig.bolt;roles.magazine=rig.magazine;roles.followers=rig.slideTracks;roles.openFrame=rig.openFrame;roles.closedFrame=rig.closedFrame;
    auto renamed=r;renamed.tracks[roles.operating].anchor.anchorName="AuthoredOperatingPart";
    SimulationRig authored;check(buildSimulationRig(renamed,authored,error,WeaponMechanism::PistolSlide,false,{},&roles),error);
    for(int track:roles.followers)for(float u:{0.f,.2f,.5f,.8f,1.f})check(reloadPoseDistance(simulationTrackPose(r,original,track,u),simulationTrackPose(renamed,authored,track,u))<.0001f,"Explicit operating roles changed a complete native follower curve");
    roles.curves[roles.operating]=r.tracks[roles.operating].poses;
    for(int frame=roles.openFrame;frame<=roles.closedFrame;++frame)roles.curves[roles.operating][frame].translation.x+=.2f*(roles.closedFrame-frame)/(roles.closedFrame-roles.openFrame);
    check(buildSimulationRig(r,authored,error,WeaponMechanism::PistolSlide,false,{},&roles),error);
    check(std::abs(simulationTrackPose(r,authored,roles.operating,1).translation.x-simulationTrackPose(r,original,roles.operating,1).translation.x-.2f)<.0001f,"Authored movement override was ignored");
    check(reloadPoseDistance(simulationTrackPose(r,authored,roles.followers[1],.5f),simulationTrackPose(r,original,roles.followers[1],.5f))<.0001f,"Independent follower motion was overwritten");
    OperatingSetup setup;setup.recipeId=r.id;setup.parts=roles;OperatingSetup decoded;
    check(readOperatingSetup(operatingSetupJson(setup),decoded,error),error);
    auto invalid=operatingSetupJson(setup);auto duplicate=invalid["followers"].toArray();duplicate.append(duplicate[0]);invalid["followers"]=duplicate;check(!readOperatingSetup(invalid,decoded,error),"Duplicate operating follower accepted");
    auto missing=roles;missing.followers.removeAll(missing.operating);check(!buildSimulationRig(r,authored,error,WeaponMechanism::PistolSlide,false,{},&missing),"Operating group omitted its primary part");
    auto outside=roles;outside.closedFrame=r.frames;check(!buildSimulationRig(r,authored,error,WeaponMechanism::PistolSlide,false,{},&outside),"Out-of-range movement window accepted");
    auto bad=r;bad.tracks[rig.slide].poses.fill(bad.tracks[rig.slide].poses.back(),r.frames);
    check(!buildSimulationRig(bad,rig,error),"Static slide must reject simulation");
    bad=r;bad.tracks[4].anchor.ownerRecord=bad.tracks[0].anchor.ownerRecord;bad.tracks[4].anchor.ownerModel=bad.tracks[0].anchor.ownerModel;
    check(!buildSimulationRig(bad,rig,error),"Shared receiver owner must not be hidden as magazine");
    check(buildSimulationRig(bad,rig,error,WeaponMechanism::PistolSlide,false,{},&roles),error);
    check(rig.magazineBranchOnly&&rig.magazineTracks==QVector<int>({4,5,6,7,8,9}),"Explicit shared magazine must exclude the receiver and sibling parts");
    auto nested=bad;nested.tracks[roles.operating].parent=roles.magazine;
    check(!buildSimulationRig(nested,rig,error,WeaponMechanism::PistolSlide,false,{},&roles),"Magazine carrier containing the operating part was accepted");
    check(buildSimulationRig(bad,rig,error,WeaponMechanism::ReciprocatingBolt),error);
    check(rig.slideTracks==QVector<int>({0})&&rig.magazineTracks==QVector<int>({4,5,6,7,8,9}),"Reciprocating rifle must separate the bolt from magazine descendants despite their shared receiver");
    QVector<QVector3D> body,frame;
    for(float x:{-1.f,1.f})for(float y:{-2.f,2.f})for(float z:{-5.f,3.f})body.append({x,y,z});
    for(float x:{-2.f,2.f})for(float y:{-3.f,14.f})for(float z:{-6.f,5.f})frame.append({x,y,z});
    MagazineRail rail;check(buildMagazineRail(r,4,"P-Mag",{},body,frame,rail,error),error);
    auto noExtraction=r;noExtraction.tracks[rail.emptyTrack].poses.fill(noExtraction.tracks[rail.emptyTrack].poses.front(),r.frames);
    MagazineRail authoredDirection;
    check(!buildMagazineRail(noExtraction,4,"Authored",{},body,frame,authoredDirection,error,false,rail.emptyTrack),"Static source invented an extraction direction");
    check(buildMagazineRail(noExtraction,4,"Authored",{},body,frame,authoredDirection,error,false,rail.emptyTrack,&rail.outward),error);
    check(authoredDirection.outward==rail.outward&&std::abs(authoredDirection.travel-rail.travel)<.0001f,"Authored direction did not preserve complete geometry clearance");
    const QVector3D invalidDirection{};
    check(!buildMagazineRail(noExtraction,4,"Authored",{},body,frame,authoredDirection,error,false,rail.emptyTrack,&invalidDirection),"Zero authored direction accepted");
    PartMechanism movement;movement.id=QUuid::createUuid().toString(QUuid::WithoutBraces);movement.name="Authored magazine extraction";
    movement.weaponRecord=r.source["weaponRecord"].toString();movement.anchor=r.tracks[4].anchor;
    Transformf inverse;check(invertUniformTransform(r.tracks[4].poses.front(),inverse),"Fixture has invalid rest frame");
    const auto local=rotated(inverse.rotation,{rail.outward.x()*rail.travel*3,rail.outward.y()*rail.travel*3,rail.outward.z()*rail.travel*3});
    const float components[]{local.x,local.y,local.z};for(int axis=0;axis<3;++axis)if(std::abs(components[axis])>.0001f){MechanismStage stage;stage.axis=axis;stage.to=components[axis];movement.stages.append(stage);}
    MechanismStage tilt;tilt.rotation=true;tilt.axis=0;tilt.to=20;movement.stages.append(tilt);
    auto manual=rail;check(buildManualMagazineAnimation(r,movement,body,manual,error),error);
    check(manual.motion.size()>16&&std::abs(manual.motion.back().rotation.w)<.999f,"Saved Move/Rotate magazine path lost rotation");
    check(manual.outward==rail.outward&&manual.travel==rail.travel,"Visible movement changed the hand guide");
    check(reloadPoseDistance(magazineAnimationDelta(manual,0),{})<1e-5f,"Authored magazine is not seated at zero");
    auto badMovement=movement;badMovement.anchor.anchorPath+="/unrelated";
    check(!buildManualMagazineAnimation(r,badMovement,body,manual,error),"Other-part magazine movement accepted");
    badMovement=movement;badMovement.stages={MechanismStage{}};badMovement.stages[0].to=.001f;
    check(!buildManualMagazineAnimation(r,badMovement,body,manual,error),"Incomplete extraction accepted");
    check(rail.fullTrack==4&&rail.emptyTrack==3,"Wrong full/empty branch");
    check(rail.outward.y()<-.2f&&rail.outward.z()<-.9f,"Rail lost the weapon-specific tilted extraction axis");
    check(rail.magazineMinProjection+rail.travel>rail.weaponMaxProjection,"Magazine does not completely clear the weapon");
    // Rotating the weapon and its attachment must rotate the rail, not retain
    // a hardcoded global-down or screen-space direction.
    const Transformf rotatedFrame{{3,-7,5},{0,0,.70710678f,.70710678f}};
    auto rotatedRecipe=r;for(auto& t:rotatedRecipe.tracks)for(auto& pose:t.poses)pose=composed(rotatedFrame,pose);
    const auto transform=[&](const QVector<QVector3D>& input){QVector<QVector3D> result;for(const auto& p:input){const auto q=composed(rotatedFrame,Transformf{{p.x(),p.y(),p.z()}}).translation;result.append({q.x,q.y,q.z});}return result;};
    MagazineRail rotatedRail;check(buildMagazineRail(rotatedRecipe,4,"P-Mag",rotatedFrame,transform(body),transform(frame),rotatedRail,error),error);
    const auto direction=rotated(rotatedFrame.rotation,{rail.outward.x(),rail.outward.y(),rail.outward.z()});
    check((rotatedRail.outward-QVector3D(direction.x,direction.y,direction.z)).length()<1e-4f&&std::abs(rail.travel-rotatedRail.travel)<1e-3f,"Connection frame covariance failed");
    MagazineMotion magazine;check(magazine.animateOut(),"Mag Out rejected seated magazine");
    magazine.tick(rail,.03f);check(magazine.empty()&&magazine.distance>0&&magazine.distance<rail.travel*.1f,"Empty extraction did not animate");
    magazine.tick(rail,.03f);check(magazine.phase==MagazinePhase::OutsideEmpty&&std::abs(magazine.distance-rail.travel*.1f)<1e-5f,"Mag Out did not finish the short release stroke");
    check(magazine.animateIn(rail)&&!magazine.empty(),"Mag In must use the full variant");magazine.tick(rail,.6f);check(magazine.seated(),"Mag In did not seat");
    magazine.drop();check(magazine.grab(rail),"Grab failed");const auto entrance=rail.outward*rail.travel;
    magazine.moveFree(rail,entrance+QVector3D(rail.captureRadius*5,0,0));check(magazine.phase==MagazinePhase::Free,"Captured outside the entrance volume");
    magazine.moveFree(rail,entrance-QVector3D(rail.captureRadius*5,0,0));check(magazine.phase==MagazinePhase::Rail,"Fast swept entry was missed");
    magazine.moveRail(rail,rail.travel+rail.releaseMargin*.5f);check(magazine.phase==MagazinePhase::Rail,"Capture jitter released the rail");
    magazine.moveRail(rail,rail.travel+rail.releaseMargin+1);check(magazine.phase==MagazinePhase::Free,"Withdrawal could not disengage");
    magazine.moveFree(rail,entrance);magazine.moveRail(rail,rail.travel*.4f);check((magazine.offset-rail.outward*magazine.distance).length()<1e-5f,"Guided movement left its axis");
    magazine.moveRail(rail,0);check(magazine.seated()&&magazine.offset.isNull(),"Cursor insertion did not latch");
    const auto lateral=QVector3D::crossProduct(rail.outward,{0,1,0}).normalized()*rail.captureRadius*3;
    for(const float fraction:{.15f,.5f,.85f,-1.f,2.f}) {
        magazine.drop();magazine.grab(rail);
        const auto middle=rail.outward*(rail.travel*fraction);
        magazine.offset=middle+lateral;
        magazine.moveFree(rail,middle-lateral);
        if(fraction<0||fraction>1)check(magazine.phase==MagazinePhase::Free,"Captured beyond finite guide");
        else {
            check(magazine.phase==MagazinePhase::Rail&&std::abs(magazine.distance-rail.travel*fraction)<1e-4f,"Side entry lost capture depth");
            magazine.moveRail(rail,0);check(magazine.seated(),"Side entry failed to seat");
        }
    }
    magazine.drop();magazine.grab(rail);magazine.cancel();check(!magazine.visible()&&!magazine.carried(),"Cancel left cursor ownership or geometry behind");
}
void ammunitionTransitions() {
    WeaponSimulation s;s.configureAmmunition(3,true);
    const auto fire=[&]{check(s.fire(),"Loaded shot rejected");check(s.tick(1,.2f),"Shot failed to finish");};
    check(s.magazineRounds==2&&s.chambered&&s.canFire(),"Reset must split native loaded capacity into magazine and chamber");
    fire();check(s.magazineRounds==1&&s.chambered&&s.shotsFired==1,"First shot did not feed exactly once");
    fire();check(s.magazineRounds==0&&s.chambered&&!s.slideLocked,"Last chambered round must remain fireable");
    fire();check(s.slideLocked&&s.slide==1&&!s.chambered&&s.shotsFired==3&&!s.fire(),"Empty pistol must lock and reject firing");
    check(s.grab(),"Locked slide grab failed");s.drag(1);s.release(false,true);
    check(s.slideLocked&&s.action==SimulationAction::Idle,"Touching the catch must not release it");
    check(s.dropMagazine()&&s.slideLocked,"Removing empty magazine released the slide");
    MagazineRail rail;rail.travel=10;rail.outward={0,0,-1};rail.captureRadius=1;rail.releaseMargin=2;
    check(s.magazineIn(rail),"Fresh magazine rejected");s.magazine.tick(rail,1);
    check(s.magazineRounds==3&&!s.chambered&&s.slideLocked&&!s.canFire(),"Seating must not chamber or unlock");
    check(s.slideRelease(),"Slide release rejected");s.tick(1,1);
    check(s.magazineRounds==2&&s.chambered&&s.slide==0&&!s.slideLocked,"Slide release failed to transfer a single round");
    fire();fire();fire();check(s.slideLocked,"Second cycle failed to lock");
    s.dropMagazine();check(s.grabMagazine(rail),"Cursor magazine failed");s.magazine.moveFree(rail,rail.outward*rail.travel);s.magazine.moveRail(rail,0);
    check(s.magazinePresent()&&s.magazineRounds==3&&s.slideLocked&&!s.chambered,"Cursor seating changed chamber/catch");
    s.grab();s.drag(1.08f);s.release(true,true);
    check(s.slideLocked&&s.magazineRounds==3&&!s.chambered,"Cancelled slingshot changed ammo or catch");
    s.grab();s.drag(1.08f);s.release(false,true);s.tick(1,1);
    check(!s.slideLocked&&s.chambered&&s.magazineRounds==2,"Slingshot failed to chamber");
    s.grab();s.drag(.5f);s.release(false,true);s.tick(1,1);
    check(s.chambered&&s.magazineRounds==2&&s.roundsEjected==0,"Partial pull changed ammo");
    s.grab();s.drag(1);check(!s.chambered&&s.roundsEjected==1,"Full rack did not eject chamber");s.drag(1);
    check(s.roundsEjected==1,"Repeated pull ejected twice");s.release(true,true);
    check(s.chambered&&s.magazineRounds==2&&s.roundsEjected==0,"Cancel did not restore ammunition");
    s.grab();s.drag(1);s.release(false,true);s.tick(.1f,1);s.grab();s.drag(.9f);s.release(false,true);s.tick(1,1);
    check(s.chambered&&s.magazineRounds==1&&s.roundsEjected==1,"Regrab during feed duplicated a round");
    s.dropMagazine();check(s.chambered&&s.magazineRounds==0,"Tactical drop lost chamber");fire();
    check(!s.slideLocked&&s.slide==0&&!s.fire(),"Absent magazine must allow only the chambered shot, without follower lock");
    s.grab();s.drag(1);s.release(false,true);s.tick(1,1);check(!s.chambered,"Racking empty weapon fabricated a round");
    s.grabMagazine(rail);s.cancelMagazine();check(s.magazineRounds==0&&!s.magazine.visible(),"Cancelled spare retained ammo");
    check(s.reload(),"Automatic reload failed");check(!s.chambered&&s.magazineRounds==0,"Automatic reload credited ammunition before completion");
    s.tick(1,1);check(s.chambered&&s.magazineRounds==2,"Automatic empty reload count wrong");
    s.dropMagazine();s.magazineIn(rail);s.magazine.tick(rail,1);
    check(s.chambered&&s.magazineRounds==3,"Tactical fresh magazine must retain the extra chambered round");
    s.reset();s.configureAmmunition(1,false);fire();check(!s.slideLocked&&!s.chambered,"Non-pistol must not receive pistol follower lock");
    s.configureAmmunition(1,true);fire();s.slideRelease();s.tick(1,1);
    check(!s.chambered&&s.slide==0&&!s.canFire(),"Release over empty magazine fabricated a round");
    s.grab();s.drag(1);s.release(false,true);check(s.slideLocked,"Empty seated follower must relock on full rack");
    s.configureAmmunition(0,true);check(!s.fire()&&!s.reload()&&!s.magazineIn(rail),"Unknown ammunition must not fabricate firing or replenishment");
}
void nativeReloadEvents() {
    WeaponSimulation s;s.configureAmmunition(17,true);
    check(s.reload(.4f,1.8f)&&s.reloadStartedLoaded&&!s.reloadStartedOpen,"Loaded native reload classification wrong");
    s.tick(.2f,3);check(s.chambered&&s.magazineRounds==16,"Native reload changed ammo before cycle");
    s.tick(.3f,3);check(!s.chambered&&s.roundsEjected==1&&s.reloadCycled,"Native rack failed to eject live chamber");
    s.tick(1.4f,3);check(s.chambered&&s.magazineRounds==16&&s.reloadFed&&!s.canFire(),"Completion marker must feed once while animation retains control");
    s.tick(2,3);check(s.canFire()&&s.magazineRounds==16&&s.roundsEjected==1,"End of clip refilled/ejected twice");
    s.configureAmmunition(1,true);s.fire();s.tick(1,1);
    check(s.reload(.4f,.8f)&&s.reloadStartedOpen&&!s.reloadStartedLoaded,"Empty open reload classification wrong");
    s.tick(2,1);check(s.chambered&&s.magazineRounds==0&&s.roundsEjected==0,"Large frame step lost empty reload events");
    s.reset();check(!s.reloadCycled&&!s.reloadFed&&s.reloadRackAt<0,"Reset retained reload event state");
}
void ar15Transitions() {
    WeaponSimulation s;s.configureAmmunition(3,false,WeaponMechanism::Ar15);s.fireCycleSeconds=.1f;
    check(s.fire(),"AR fire rejected");s.triggerHeld=true;
    check(s.tick(1,2)&&s.shotsFired==3&&s.slideLocked&&s.slide==1&&s.handle==0,"Automatic AR fire must consume each round and hold only the bolt open");
    check(s.grab(),"AR charging-handle grab failed");s.drag(.5f);
    check(s.slide==1&&s.handle==.5f&&s.slideLocked,"Partial handle pull released the bolt catch");
    s.release(false,true);s.tick(1,1);check(s.handle==0&&s.slide==1&&s.slideLocked,"Empty charging handle did not return independently");
    MagazineRail rail;rail.travel=10;rail.outward={0,0,-1};rail.captureRadius=1;rail.releaseMargin=2;
    check(s.dropMagazine()&&s.grabMagazine(rail),"AR spare magazine rejected");
    s.magazine.moveFree(rail,rail.outward*rail.travel);s.magazine.moveRail(rail,0);
    check(s.slideLocked&&!s.chambered&&s.magazineRounds==3,"Insertion should not close the bolt");
    s.grab();s.drag(1);s.release(true,true);
    check(s.slideLocked&&s.slide==1&&s.handle==0&&!s.chambered,"Cancel did not restore AR bolt/handle/ammo");
    s.grab();s.drag(1);s.release(false,true);s.tick(1,1);
    check(s.chambered&&s.magazineRounds==2&&s.slide==0&&s.handle==0&&!s.slideLocked,"AR pull/release did not feed exactly once");
    s.grab();s.drag(1);check(s.roundsEjected==1&&!s.chambered,"AR rack did not eject the chambered round");
    s.release(false,true);s.tick(1,1);check(s.chambered&&s.magazineRounds==1,"AR rack double fed");
    s.fire();s.triggerHeld=true;s.tick(1,1);check(s.slideLocked,"Second empty cycle failed");
    s.dropMagazine();s.magazineIn(rail);s.magazine.tick(rail,1);check(s.slideRelease(),"AR bolt release rejected");s.tick(1,1);
    check(s.chambered&&s.magazineRounds==2&&s.handle==0,"Bolt release moved the charging handle or lost a round");
    s.fire();s.triggerHeld=true;s.tick(.11f,1);s.triggerHeld=false;const int shots=s.shotsFired;s.tick(1,1);
    check(s.shotsFired==shots&&s.action==SimulationAction::Idle,"Trigger release did not stop repeat shots");
    s.reset();s.dropMagazine();s.fire();s.tick(1,1);check(!s.slideLocked&&!s.chambered,"Absent magazine fabricated follower lock");
    s.reset();s.grab();s.drag(.4f);s.release(false,false);check(!s.canFire(),"Open charging handle allowed fire");
}
void boltActionTransitions() {
    WeaponSimulation automatic;automatic.configureAmmunition(20,false,WeaponMechanism::ReciprocatingBolt);automatic.fireCycleSeconds=.1f;
    check(automatic.fire(),"Reciprocating rifle fire failed");automatic.triggerHeld=true;automatic.tick(3,5);
    check(automatic.shotsFired==20&&automatic.slideLocked&&!automatic.chambered,"Reciprocating automatic rifle lost its measured cadence or last-shot lock");
    automatic.reset();automatic.fire();automatic.tick(.11f,5);
    check(automatic.shotsFired==1&&automatic.chambered&&automatic.action==SimulationAction::Idle,"Semiautomatic reciprocating rifle repeated a shot");
    WeaponSimulation s;s.configureAmmunition(5,false,WeaponMechanism::BoltAction);s.boltEjectProgress=.8f;
    check(s.fire()&&s.tick(1,1),"Bolt-action shot failed");
    check(s.spentCase&&!s.chambered&&s.magazineRounds==4&&s.slide==0&&!s.slideLocked&&!s.canFire(),"Firing automatically cycled the bolt action");
    s.grab();s.drag(.4f);s.release(false,true);s.tick(3,1);
    check(s.slide==.4f&&s.spentCase&&s.casesEjected==0,"Partial bolt must hold without ejecting or springing closed");
    s.grab();s.drag(.85f);check(!s.spentCase&&s.casesEjected==1,"Source ejection threshold was not honored");
    s.drag(.9f);check(s.casesEjected==1,"One opening ejected multiple cases");s.release(true,true);
    check(s.slide==.4f&&s.spentCase&&s.casesEjected==0,"Cancelled bolt pull lost case/position state");
    s.grab();s.drag(.85f);s.drag(0);s.release(false,true);
    check(!s.chambered&&s.magazineRounds==4,"Short stroke fed a round without reaching the rear");
    s.grab();s.drag(1);s.release(false,true);s.tick(5,1);
    check(s.slide==1&&s.boltClosing&&!s.chambered&&!s.slideRelease(),"Manual bolt acquired an automatic release/return");
    s.grab();s.drag(.2f);check(!s.canFire()&&!s.chambered,"Unlocked bolt allowed firing or credited chambering");
    s.drag(0);s.release(false,true);check(s.canFire()&&s.chambered&&s.magazineRounds==3,"Full bolt cycle failed to chamber exactly once");
    s.grab();s.drag(1);check(s.roundsEjected==1&&s.casesEjected==1,"Live round and spent case were not distinguished");
    s.release(true,true);check(s.chambered&&s.roundsEjected==0&&s.magazineRounds==3,"Cancelled live-round ejection consumed ammunition");
    s.reset();check(s.boltEjectProgress==.8f&&!s.spentCase&&!s.boltClosing,"Reset lost the source threshold or retained transient state");
    s.dropMagazine();s.fire();s.tick(1,1);s.grab();s.drag(1);s.drag(0);s.release(false,true);
    check(!s.canFire()&&!s.chambered&&!s.slideLocked&&s.casesEjected==1,"No-magazine bolt cycle fabricated a round or follower lock");
    MagazineRail rail;rail.travel=10;rail.outward={0,0,-1};rail.captureRadius=1;
    s.grab();s.drag(1);s.release(false,true);s.grabMagazine(rail);s.magazine.moveFree(rail,rail.outward*rail.travel);s.magazine.moveRail(rail,0);
    check(!s.chambered&&s.slide==1,"Fresh magazine automatically closed/chambered the bolt");
    s.grab();s.drag(0);s.release(false,true);check(s.chambered&&s.magazineRounds==4,"Open-bolt magazine insertion did not feed on locking");
    s.fire();s.tick(1,1);s.reload(.4f,.8f);s.tick(2,1);
    check(!s.spentCase&&s.chambered&&s.magazineRounds==4&&s.casesEjected==2,"Native full reload lost spent-case/feed events");
    s.reset();s.boltEjectProgress=1;s.fire();s.tick(1,1);s.grab();s.drag(.999f);s.drag(0);s.release(false,true);
    check(s.spentCase&&!s.chambered&&s.magazineRounds==4,"Rear tolerance fed through an unejected case");
}
void boltCycleFixture(const QString& path) {
    QFile file(path);check(file.open(QIODevice::ReadOnly),"Missing Timberwolf native-cycle fixture");
    const auto json=QJsonDocument::fromJson(file.readAll()).object();BoltCycle c;c.duration=float(json["duration"].toDouble());QString error;
    for(const auto& values:json["tracks"].toArray()) {
        c.tracks.append(c.tracks.size());QVector<Transformf> poses;
        for(const auto& value:values.toArray()){Transformf pose;check(decodePoseTransform(value,pose,error),error);poses.append(pose);}c.poses.append(poses);
    }
    const float eject=float(json["ejectSeconds"].toDouble());check(analyzeBoltCycle(c,eject,error),error);
    check(c.inputTravel>8&&c.inputTravel<9&&c.ejectProgress>.8f&&c.ejectProgress<1,"Timberwolf source stroke or ejection lost");
    check(reloadPoseDistance(c.poses[2].front(),c.poses[2].back())>.3f,"Fixture lacks the fired-to-cocked pin change");
    check(std::abs(c.poses[0][c.rear].rotation.w)<.72f,"Native bolt lift rotation was lost");
    for(int t:c.tracks) {
        for(int f=c.first;f<=c.rear;++f)check(reloadPoseDistance(boltCyclePose(c,t,c.opening[f-c.first],false),c.poses[t][f])<.005f,"Opening lost a native follower sample");
        for(int f=c.rear;f<=c.last;++f)check(reloadPoseDistance(boltCyclePose(c,t,1-c.closing[f-c.rear],true),c.poses[t][f])<.005f,"Closing lost a native follower sample");
    }
    check(!analyzeBoltCycle(c,-1,error),"Missing ejection event accepted");
    check(!analyzeBoltCycle(c,c.duration,error),"Ejection after locking accepted");
    c.poses[1].removeLast();check(!analyzeBoltCycle(c,eject,error),"Mismatched follower samples accepted");
}
void ar15Fixture(const QString& path) {
    QFile file(path);check(file.open(QIODevice::ReadOnly),"Missing RU556 fixture");
    ReloadRecipe recipe;QString error;check(decodeReloadRecipe(file.readAll(),recipe,error),error);
    SimulationRig rig;check(buildSimulationRig(recipe,rig,error,WeaponMechanism::Ar15),error);
    check(rig.slide==1&&rig.bolt==0&&rig.magazine==4,"RU556 part roles are wrong");
    check(rig.slideTracks==QVector<int>({1})&&!rig.magazineTracks.contains(rig.bolt)&&!rig.magazineTracks.contains(2),"Magazine/handle ownership included the receiver bolt or release");
    check(reloadPoseDistance(simulationTrackPose(recipe,rig,1,0),simulationTrackPose(recipe,rig,1,1))>3.8f,"RU556 handle lost its source travel");
    auto staticHandle=recipe;const auto rest=staticHandle.tracks[1].poses.back();staticHandle.tracks[1].poses.fill(rest,recipe.frames);
    const auto boltRest=staticHandle.tracks[0].poses.back();
    for(int f=0;f<recipe.frames;++f){auto& p=staticHandle.tracks[0].poses[f];p=boltRest;p.translation.y+=recipe.tracks[1].poses[f].translation.y-rest.translation.y;}
    check(!buildSimulationRig(staticHandle,rig,error,WeaponMechanism::Ar15),"Static handle silently acquired an unrequested repair");
    check(buildSimulationRig(staticHandle,rig,error,WeaponMechanism::Ar15,true),error);
    const auto& bolt=staticHandle.tracks[0].poses;
    for(int f=0;f<recipe.frames;++f){const auto& h=rig.derivedHandle[f];check(std::abs((h.translation.y-rest.translation.y)-(bolt[f].translation.y-bolt.back().translation.y))<.0001f,"Derived handle lost native bolt travel");}
    check(reloadPoseDistance(staticHandle.tracks[1].poses.front(),rest)==0&&!simulationRigJson(staticHandle,rig,true)["derivedHandleMotion"].toObject().isEmpty(),"Static source was mutated or derivation provenance lost");
    // Rework 14, items 9 and 11: strokes and the parts a bolt drives. The
    // RU556 reload strokes its handle, never its bolt, so nothing follows the bolt.
    check(reloadTrackStroke(recipe,1)>3.8f&&reloadTrackStroke(recipe,0)<kCompleteStroke&&reloadTrackStroke(staticHandle,1)==0,"RU556 reload strokes");
    check(surfaceTrack(recipe,"WeaponExtra1")==1&&surfaceTrack(recipe,"weaponbolt")==0&&surfaceTrack(recipe,"WeaponMissing")<0,"Native bone tracks");
    SimulationRig plain;check(buildSimulationRig(recipe,plain,error,WeaponMechanism::Ar15),error);
    check(boltDrivenTracks(recipe,plain).isEmpty(),"A part followed the RU556's static bolt");
    // A release that moves only while the bolt is back is driven by it; one
    // that moves while the bolt rests is not.
    const auto releaseRest=staticHandle.tracks[2].poses.back();
    // A part that moves while the bolt is back but against its travel (a
    // spare carried during a locked-back reload) is not.
    auto driven=staticHandle,independent=staticHandle,opposed=staticHandle;
    float boltStroke=0;
    for(const auto& pose:bolt)boltStroke=std::max(boltStroke,reloadPoseDistance(pose,bolt.back()));
    for(int f=0;f<recipe.frames;++f) {
        const float boltTravel=reloadPoseDistance(bolt[f],bolt.back());
        driven.tracks[2].poses[f]=releaseRest;driven.tracks[2].poses[f].translation.z+=.2f*boltTravel;
        independent.tracks[2].poses[f]=releaseRest;independent.tracks[2].poses[f].translation.z+=boltTravel<.01f&&f<recipe.frames-1?.5f:0.f;
        opposed.tracks[2].poses[f]=releaseRest;opposed.tracks[2].poses[f].translation.z+=boltTravel>.2f*boltStroke?10*(1-boltTravel/boltStroke)+1:0.f;
    }
    SimulationRig drivenRig,independentRig,opposedRig;
    check(buildSimulationRig(driven,drivenRig,error,WeaponMechanism::Ar15,true)&&buildSimulationRig(independent,independentRig,error,WeaponMechanism::Ar15,true)&&
          buildSimulationRig(opposed,opposedRig,error,WeaponMechanism::Ar15,true),error);
    check(boltDrivenTracks(driven,drivenRig)==QVector<int>({2}),"A part moving only with the bolt was not driven");
    check(boltDrivenTracks(independent,independentRig).isEmpty(),"A part moving while the bolt rests was driven");
    check(boltDrivenTracks(opposed,opposedRig).isEmpty(),"A part moving against the bolt's travel was driven");
    SimulationRig m1aRig=drivenRig;m1aRig.mechanism=WeaponMechanism::M1A;
    check(boltDrivenTracks(driven,m1aRig).isEmpty(),"An M1A rig took bolt-driven parts");
}
void m1aFixture(const QString& path) {
    QFile file(path);check(file.open(QIODevice::ReadOnly),"Missing M1A fixture");
    ReloadRecipe r;QString error;check(decodeReloadRecipe(file.readAll(),r,error),error);
    SimulationRig rig;check(buildSimulationRig(r,rig,error,WeaponMechanism::M1A),error);
    check(rig.slide==1&&rig.bolt==0&&rig.magazine==4&&rig.slideTracks==QVector<int>({0,1}),"M1A operating handle/bolt roles were lost");
    check(!rig.magazineTracks.contains(0)&&!rig.magazineTracks.contains(2),"M1A magazine contains receiver parts");
    const auto open=simulationTrackPose(r,rig,0,1),closed=simulationTrackPose(r,rig,0,0);
    const float dot=std::abs(open.rotation.x*closed.rotation.x+open.rotation.y*closed.rotation.y+open.rotation.z*closed.rotation.z+open.rotation.w*closed.rotation.w);
    check(dot<.99f&&reloadPoseDistance(open,closed)>5,"M1A bolt rotation/stroke was flattened");
    QVector<QVector3D> body,frame;
    for(float x:{-1.f,1.f})for(float y:{7.f,14.f})for(float z:{-4.f,3.f})body.append({x,y,z});
    for(float x:{-2.f,2.f})for(float y:{-15.f,30.f})for(float z:{-6.f,5.f})frame.append({x,y,z});
    MagazineRail rail;check(buildMagazineRail(r,4,"P-Mag",{},body,frame,rail,error,true),error);
    check(buildMagazineAnimation(r,1.9f,body,rail,error),error);
    check(rail.motion.size()>20&&reloadPoseDistance(magazineAnimationDelta(rail,0),{})<1e-5f,"Guide did not preserve a seated identity");
    bool rotates=false;for(const auto& delta:rail.motion)rotates|=std::abs(delta.rotation.w)<.995f;
    check(rotates,"M1A magazine insertion lost native rotation");
    for(const auto& p:body){const auto q=composed(magazineAnimationDelta(rail,rail.travel),Transformf{{p.x(),p.y(),p.z()}}).translation;
        check(QVector3D::dotProduct({q.x,q.y,q.z},rail.outward)>rail.weaponMaxProjection,"Curved magazine did not fully clear the weapon");}
    MagazineMotion mag;mag.drop();mag.grab(rail);
    const auto middle=magazineInputOffset(rail,rail.travel*.7f),side=QVector3D::crossProduct(rail.outward,{0,1,0}).normalized()*rail.captureRadius*3;
    mag.offset=middle+side;mag.moveFree(rail,middle-side);check(mag.phase==MagazinePhase::Rail&&std::abs(mag.distance-rail.travel*.7f)<1e-4f,"Angled straight-guide entry lost its depth");
    for(float progress:{.1f,.4f,.8f,.4f}) {
        mag.moveRail(rail,rail.travel*progress);
        check((mag.offset-rail.outward*mag.distance).length()<1e-5f,"Input followed the curved animation instead of a straight line");
    }
    for(int i=0;i<rail.motion.size();++i) {
        const float input=rail.travel*rail.motionDistances[i]/rail.motionDistances.back();
        check(reloadPoseDistance(magazineAnimationDelta(rail,input),rail.motion[i])<.0005f,"Linear input lost a native motion sample");
    }
    check((magazineAnimationOffset(rail,rail.travel*.4f)-magazineInputOffset(rail,rail.travel*.4f)).length()>.1f,"Animation was flattened into the input line");
    auto adjusted=rail;
    MagazineGuide authored{QString(64,'b'),rail.outward,rail.travel*.25f,rail.captureRadius*.2f,rail.releaseMargin*.2f};
    // This fixture's connection is identity. Shortening clips the actual
    // source approach; extending and shortening again cannot lose its keys.
    check(applyMagazineGuide(authored,adjusted,error),error);
    const auto shortened=adjusted;
    check(std::abs(QVector3D::dotProduct(magazineAnimationOffset(adjusted,adjusted.travel),adjusted.outward)-adjusted.travel)<.001f,"Short rail kept the old visual entrance");
    check(reloadPoseDistance(magazineAnimationDelta(adjusted,0),{})<1e-5f,"Short rail moved its seated endpoint");
    check(adjusted.motion.size()<rail.motion.size(),"Short rail kept the complete old visual curve");
    authored.travel=rail.travel*1.4f;check(applyMagazineGuide(authored,adjusted,error),error);
    authored.travel=shortened.travel;check(applyMagazineGuide(authored,adjusted,error),error);
    check(adjusted.motion.size()==shortened.motion.size(),"Repeated length edits lost native curve keys");
    for(int i=0;i<adjusted.motion.size();++i)check(reloadPoseDistance(adjusted.motion[i],shortened.motion[i])<.0005f,"Repeated length edits accumulated transforms");
    // A Barrett-style insertion rotates right up to its seated frame, so
    // there may be no rotation-aligned sample within the near-well interval.
    auto rocking=r;
    const Quaternionf tilt{.09983342f,0,0,.99500417f};
    for(int f=0;f<rocking.frames-1;++f)rocking.tracks[4].poses[f].rotation=multiplied(rocking.tracks[4].poses.back().rotation,tilt);
    MagazineRail rockingRail;check(buildMagazineRail(rocking,4,"P-Mag",{},body,frame,rockingRail,error,true),error);
    check(buildMagazineAnimation(rocking,1.9f,body,rockingRail,error),error);
    check(rockingRail.motion.size()>20&&std::abs(rockingRail.outward.length()-1)<.0001f,"Rotating approach lost its finite straight input");
    mag.moveRail(rail,rail.travel+rail.releaseMargin+1);check(mag.phase==MagazinePhase::Free,"Straight guide could not disengage");
    check((mag.offset-magazineAnimationOffset(rail,mag.distance)).length()<1e-4f,"Disengaging snapped the visual magazine to the input line");
    mag.moveFree(rail,magazineInputOffset(rail,rail.travel));mag.moveRail(rail,0);check(mag.seated(),"Straight guide did not seat");
    const auto exported=magazineRailJson(rail);
    check(exported["version"]==3&&exported["inputPath"]=="straight"&&!exported.contains("guide"),"Export confuses input with visual motion");
    WeaponSimulation s;s.configureAmmunition(2,false,WeaponMechanism::M1A);
    s.fire();s.tick(1,.74f);s.fire();s.tick(1,.74f);check(s.slideLocked,"M1A last shot did not hold open");
    s.dropMagazine();s.grabMagazine(rail);s.magazine.moveFree(rail,magazineInputOffset(rail,rail.travel));s.magazine.moveRail(rail,0);
    s.grab();s.drag(1.08f);s.release(true,true);check(s.slideLocked&&!s.chambered,"M1A cancel lost its catch/chamber state");
    s.grab();s.drag(1.08f);s.release(false,true);s.tick(1,1);check(s.chambered&&s.magazineRounds==1&&!s.slideLocked,"M1A release did not feed once");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(app.arguments().contains("--grab-deployment")){try{grabDeployment();QTextStream(stdout)<<"PASS folding handle deployment, independent carriage and native clip clock\n";return 0;}catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}}
    guideAuthoring();carrierOwnership();nativeSourceTimes();
    try {grabDeployment();stateTransitions();ammunitionTransitions();nativeReloadEvents();ar15Transitions();boltActionTransitions();check(app.arguments().size()>=2,"Pass the Glock project fixture");fixture(app.arguments()[1]);if(app.arguments().size()>2)ar15Fixture(app.arguments()[2]);if(app.arguments().size()>3)m1aFixture(app.arguments()[3]);if(app.arguments().size()>4)boltCycleFixture(app.arguments()[4]);
        QTextStream(stdout)<<"PASS simulation state ownership, drop/fire persistence, regrab/cancel/return/reset, actual slide/barrel/cartridge curves and complete magazine ownership\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}
}
