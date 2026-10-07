#include "areas/project/EspLibrary.h"
#include "CollisionDraftPolicy.h"
#include "SourcePolicy.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        QString policyError;
        const QJsonObject sourceRules{{"sourceOverride",QJsonObject{{"marker","Addon.esp:000800"},{"present",false}}},{"blockedMods",QJsonArray{"Example.esp:000801"}},{"magazineCollections",QJsonArray{"Example.esp:000802"}}};
        check(validSourcePolicy(sourceRules,policyError),"Valid add-on/blocked/group rules rejected");
        auto conflicting=sourceRules;conflicting["magazineCollections"]=QJsonArray{"example.ESP:000801"};check(!validSourcePolicy(conflicting,policyError),"One record accepted with incompatible roles");
        conflicting=sourceRules;conflicting["sourceOverride"]=QJsonObject{{"marker","Addon.esp:000000"},{"present",true}};check(!validSourcePolicy(conflicting,policyError),"Zero dependency record accepted");
        // One ESP project per originating plugin, whatever source loaded it.
        application.setProperty("studioRoot","C:/workspace-root");
        check(espProjectKey("Example Weapon.esp")=="example weapon.esp","ESP project key lost the originating plugin");
        check(espProjectKey("example weapon.ESP")==espProjectKey("Example Weapon.esp"),"Plugin case split one ESP project");
        check(espProjectFolder("Example Weapon.esp").endsWith("/workspace/library/plugins/example weapon.esp"),"ESP project outside the library");
        for(const auto* invalid:{"../Mod.esp","Folder\\Mod.esp","Mod.esp:000abc","Mod","Mod.esp.json",""})
            check(espProjectKey(invalid).isEmpty(),"Unsafe or ambiguous ESP project name accepted");
        QJsonObject drafts{{"full",QJsonObject{{"edit",QString(32,'a')},{"usable",true}}}};check(validCollisionDrafts(drafts),"Valid collision draft rejected");drafts["unknown"]=drafts["full"];check(!validCollisionDrafts(drafts),"Unknown collision state accepted");
        QTextStream(stdout)<<"PASS source rules, ESP project keys and bounded collision draft metadata\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}
}
