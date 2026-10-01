#include "ScopeProfile.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    ScopeProfile p;p.name="Custom optic";p.model="weapons/test/scope.nif";p.shape="Lens:0";p.lens.translation={-1,3,8};p.lens.rotation={0,0,.70710677f,.70710677f};p.radius=1.7f;
    ScopeProfile decoded;QString error;auto j=scopeProfileJson(p);check(readScopeProfile(j,decoded,error),"Round trip failed");check(scopeProfileJson(decoded)==j,"Round trip changed lens frame");
    j["model"]="Meshes\\Weapons\\Test\\Scope_1.nif";check(readScopeProfile(j,decoded,error)&&decoded.model==p.model,"Model identity normalization failed");
    const auto before=scopeProfileJson(decoded);
    for(const auto key:{"radius","aspect","eyeRelief","cameraForward"}){auto bad=j;auto lens=bad["lens"].toObject();lens[key]=-1;bad["lens"]=lens;check(!readScopeProfile(bad,decoded,error),"Invalid dimensions accepted");check(scopeProfileJson(decoded)==before,"Failed parse changed prior profile");}
    auto bad=j;auto lens=bad["lens"].toObject();lens["rotation"]=QJsonArray{0,0,0,0};bad["lens"]=lens;check(!readScopeProfile(bad,decoded,error),"Zero rotation accepted");
    for(const auto model:{"../scope.nif","weapons/../scope.nif","C:/scope.nif","/scope.nif","weapons//scope.nif"}){bad=j;bad["model"]=model;check(!readScopeProfile(bad,decoded,error),"Non-resource path accepted");}
    bad=j;bad["shape"]=QString(64,'x');check(!readScopeProfile(bad,decoded,error),"Runtime shape buffer overflow accepted");
    QTextStream(stdout)<<"PASS scope frame round trip, model identity, invalid records and atomic rejection\n";return 0;
}catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}}
