#include "OperatingGrabLocations.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    OperatingGrabLocations g{"Test.esp:001234","bolt","Weapons/Test.nif","Root/Handle","Handle",{}};
    g.points.append({"c419c058-91e8-4ffc-8476-106eb1c37f0a","Bolt knob",{12,3,-4},2.5f,3});
    g.points.append({"54677caa-c671-4200-8634-09d29b1bf3ba","Left side",{-5,2,1},1.5f,1});
    OperatingGrabLocations decoded;QString error;check(readGrabLocations(grabLocationsJson(g),decoded,error),"Round trip rejected");
    check(decoded.points.size()==2&&decoded.points[0].point.x==12&&decoded.points[1].hands==1,"Locations or hand isolation lost");
    auto duplicate=g;duplicate.points[1].id=duplicate.points[0].id;check(!readGrabLocations(grabLocationsJson(duplicate),decoded,error),"Duplicate ID accepted");
    auto invalid=g;invalid.points[0].radius=-1;check(!readGrabLocations(grabLocationsJson(invalid),decoded,error),"Invalid radius accepted");
    auto json=grabLocationsJson(g);auto points=json["points"].toArray();auto p=points[0].toObject();p["pointInSource"]=QJsonArray{1,"2",3};points[0]=p;json["points"]=points;check(!readGrabLocations(json,decoded,error),"Non-numeric coordinate accepted");
    g.points.clear();check(readGrabLocations(grabLocationsJson(g),decoded,error)&&decoded.points.empty(),"Explicit no-grab configuration lost");
    QTextStream(stdout)<<"PASS source-local location persistence, multiple points, hand eligibility, invalid fields and explicit empty configuration\n";return 0;
}catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}}
