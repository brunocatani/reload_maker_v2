#include "AuthoringPaths.h"
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
        const QJsonObject source{{"mode","mo2-profile"},{"profile","game test"}};
        const auto path=authoringProjectPath("Example Weapon.esp:000AbC",source);
        check(path.startsWith("workspace/projects/example weapon.esp/000abc/")&&path.endsWith("/authoring.project.json"),"Authoring path lost originating plugin/weapon grouping");
        check(path==authoringProjectPath("example weapon.ESP:000abc",source),"Record case split one authoring identity");
        check(path!=authoringProjectPath("Example Weapon.esp:000abc",{{"mode","mo2-profile"},{"profile","overhaul"}}),"Two source profiles share a save destination");
        for(const auto* invalid:{"../Mod.esp:000abc","Folder\\Mod.esp:000abc","Mod.esp:1234567","Mod:000abc"})
            check(authoringProjectPath(invalid,source).isEmpty(),"Unsafe or ambiguous weapon namespace accepted");
        QJsonObject drafts{{"full",QJsonObject{{"edit",QString(32,'a')},{"usable",true}}}};check(validCollisionDrafts(drafts),"Valid collision draft rejected");drafts["unknown"]=drafts["full"];check(!validCollisionDrafts(drafts),"Unknown collision state accepted");
        QTextStream(stdout)<<"PASS source rules, authoring paths and bounded collision draft metadata\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}
}
