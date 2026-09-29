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
    WeaponSimulation s;check(s.dropMagazine(),"Drop must work at rest");
    check(!s.magazinePresent&&s.fire(),"Removing magazine must not disable visual fire");
    check(!s.grab()&&!s.reload()&&!s.dropMagazine(),"A native action must own the scene until complete");
    check(s.tick(1,.2f)&&!s.magazinePresent,"Fire must preserve a removed magazine");
    check(s.grab(),"Grab failed");s.drag(.6f);s.release(false,true);
    s.tick(.05f,1);check(s.slide>.39f&&s.slide<.41f,"Spring must advance from the current pull");
    check(s.grab(),"Regrab during return failed");s.drag(.9f);s.release(true,true);
    check(s.action==SimulationAction::Idle&&s.slide>.39f&&s.slide<.41f,"Cancel must restore the press-time state");
    check(!s.fire(),"Open slide must reject fire");
    check(s.grab(),"Grab failed");s.drag(1);s.release(false,false);
    check(s.slide==1&&s.action==SimulationAction::Idle,"Hold mode lost its position");
    check(s.reload()&&s.magazinePresent,"Automatic reload must restore the assembly");
    check(s.tick(3,3)&&s.slide==0,"Reload completion must restore idle");
    s.dropMagazine();s.reset();check(s.magazinePresent&&s.slide==0,"Reset failed");
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
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {stateTransitions();check(app.arguments().size()==2,"Pass the Glock project fixture");fixture(app.arguments()[1]);
        QTextStream(stdout)<<"PASS simulation state ownership, drop/fire persistence, regrab/cancel/return/reset, actual slide/barrel/cartridge curves and complete magazine ownership\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<"FAIL "<<e.what()<<'\n';return 1;}
}
