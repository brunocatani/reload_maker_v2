#include "WeaponSimulation.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTextStream>
#include <stdexcept>
#include <cmath>
using namespace reloadmaker;
namespace {
void check(bool valid,const QString& error){if(!valid)throw std::runtime_error(error.toStdString());}
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
    auto bad=r;bad.tracks[rig.slide].poses.fill(bad.tracks[rig.slide].poses.back(),r.frames);
    check(!buildSimulationRig(bad,rig,error),"Static slide must reject simulation");
    bad=r;bad.tracks[4].anchor.ownerRecord=bad.tracks[0].anchor.ownerRecord;bad.tracks[4].anchor.ownerModel=bad.tracks[0].anchor.ownerModel;
    check(!buildSimulationRig(bad,rig,error),"Shared receiver owner must not be hidden as magazine");
    QVector<QVector3D> body,frame;
    for(float x:{-1.f,1.f})for(float y:{-2.f,2.f})for(float z:{-5.f,3.f})body.append({x,y,z});
    for(float x:{-2.f,2.f})for(float y:{-3.f,14.f})for(float z:{-6.f,5.f})frame.append({x,y,z});
    MagazineRail rail;check(buildMagazineRail(r,4,"P-Mag",{},body,frame,rail,error),error);
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
    magazine.tick(rail,.3f);check(magazine.empty()&&magazine.distance>0&&magazine.distance<rail.travel,"Empty extraction did not animate");
    magazine.tick(rail,.3f);check(magazine.phase==MagazinePhase::OutsideEmpty&&magazine.distance==rail.travel,"Mag Out did not clear the frame");
    check(magazine.animateIn(rail)&&!magazine.empty(),"Mag In must use the full variant");magazine.tick(rail,.6f);check(magazine.seated(),"Mag In did not seat");
    magazine.drop();check(magazine.grab(rail),"Grab failed");const auto entrance=rail.outward*rail.travel;
    magazine.moveFree(rail,entrance+QVector3D(rail.captureRadius*5,0,0));check(magazine.phase==MagazinePhase::Free,"Captured outside the entrance volume");
    magazine.moveFree(rail,entrance-QVector3D(rail.captureRadius*5,0,0));check(magazine.phase==MagazinePhase::Rail,"Fast swept entry was missed");
    magazine.moveRail(rail,rail.travel+rail.releaseMargin*.5f);check(magazine.phase==MagazinePhase::Rail,"Capture jitter released the rail");
    magazine.moveRail(rail,rail.travel+rail.releaseMargin+1);check(magazine.phase==MagazinePhase::Free,"Withdrawal could not disengage");
    magazine.moveFree(rail,entrance);magazine.moveRail(rail,rail.travel*.4f);check((magazine.offset-rail.outward*magazine.distance).length()<1e-5f,"Guided movement left its axis");
    magazine.moveRail(rail,0);check(magazine.seated()&&magazine.offset.isNull(),"Cursor insertion did not latch");
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
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {stateTransitions();ammunitionTransitions();check(app.arguments().size()==2,"Pass the Glock project fixture");fixture(app.arguments()[1]);
        QTextStream(stdout)<<"PASS simulation state ownership, drop/fire persistence, regrab/cancel/return/reset, actual slide/barrel/cartridge curves and complete magazine ownership\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}
}
