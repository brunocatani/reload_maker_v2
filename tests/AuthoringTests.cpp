#include "areas/project/EspLibrary.h"
#include "CollisionDraftPolicy.h"
#include "SourcePolicy.h"
#include "areas/project/CaptureInputs.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        // Dependency changes select captures; saved drafts do not alter the
        // game's active fits. An overridden default must stay independent.
        const QString weapon="test.esp:000001", a="test.esp:000010", b="test.esp:000011";
        const QStringList moving{"test.esp:000020"};
        QJsonObject before{{"magazineGrabGrips",QJsonObject{{weapon+'|'+a+"|left","own-a"}}},
            {"magazineGrabDefaults",QJsonObject{{weapon+"|left","default"}}}};
        const auto inputs=[&](const QJsonObject& r,const QString& mag){return captureInputs(r,weapon,mag,moving);};
        auto changed=before;
        changed["magazineGrabGrips"]=QJsonObject{{weapon+'|'+a+"|left","edited-a"}};
        check(inputs(before,a)!=inputs(changed,a)&&inputs(before,b)==inputs(changed,b),"Specific magazine fit refreshed unrelated magazines");
        check(magazineOnlyCaptureChange(inputs(before,a),inputs(changed,a)),"Magazine fit queued every receiver");
        changed=before;changed["magazineGrabDefaults"]=QJsonObject{{weapon+"|left","new-default"}};
        check(inputs(before,a)==inputs(changed,a)&&inputs(before,b)!=inputs(changed,b),"Default refresh ignored inheritance or overwrote explicit fit");
        changed=before;changed["magazineGrabGrips"]=QJsonObject{};
        check(inputs(before,a)!=inputs(changed,a),"Removing a fit did not refresh the fallback");
        changed=before;changed["gripDraft"]=QJsonObject{{"name","unfinished"}};
        check(inputs(before,a)==inputs(changed,a),"Unfinished draft recaptured active behavior");
        const auto scope=weapon+"/@magazine:";
        QJsonObject plans{{weapon+"/@anchors",QJsonObject{{"magazineScope",scope+a}}},
            {scope+a,QJsonObject{{"magazineGuide",1}}},{scope+b,QJsonObject{{"magazineGuide",2}}}};
        before["authoringRoadmaps"]=plans;changed=before;
        plans[scope+a]=QJsonObject{{"magazineGuide",3}};changed["authoringRoadmaps"]=plans;
        check(inputs(before,a)!=inputs(changed,a)&&inputs(before,b)==inputs(changed,b),"Guide refresh ignored per-magazine override");
        check(magazineOnlyCaptureChange(inputs(before,a),inputs(changed,a)),"Magazine guide queued every receiver");
        check(inputs(before,"test.esp:000012")!=inputs(changed,"test.esp:000012"),"Anchor guide change missed a dependent magazine");
        before=changed;plans[weapon+"/@moving:"+moving.front()]=QJsonObject{{"operating",1}};changed["authoringRoadmaps"]=plans;
        check(inputs(before,a)!=inputs(changed,a)&&inputs(before,b)!=inputs(changed,b),"Operating part change missed its magazines");
        check(!magazineOnlyCaptureChange(inputs(before,a),inputs(changed,a)),"Moving-part change incorrectly collapsed receiver captures");
        QString policyError;
        check(auxiliaryReloadMode("1cas_servicerifle.ESP:00C7C0") && auxiliaryReloadMode("1CAS_ServiceRifle.esp:00d847"), "Bundled auxiliary selectors lost their regular reload fallback");
        check(!auxiliaryReloadMode("Other.esp:00c7c0") && !auxiliaryReloadMode("1CAS_ServiceRifle.esp:000801"), "Auxiliary selector matched an unrelated part");
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
