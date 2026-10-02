#include "AuthoringProgress.h"
#include <QCoreApplication>
#include <QSet>
#include <QTextStream>
#include <stdexcept>
using namespace reloadmaker;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        std::array<AuthoringStepInput,12> inputs;for(int i=0;i<12;++i)inputs[i]={true,true,QString(),{{"input",i}}};
        const auto initial=authoringProgress(inputs,{});QJsonObject reviews;for(int i=0;i<12;++i)reviews[QString::number(i)]=initial[i].toObject()["fingerprint"];
        check(validAuthoringReviews(reviews),"Cannot read valid reviews");for(const auto& item:authoringProgress(inputs,reviews))check(item.toObject()["status"]=="reviewed","Unchanged data lost its review");
        auto changed=inputs;changed[6].evidence["travel"]=7;const auto guide=authoringProgress(changed,reviews);
        for(int i=0;i<12;++i)check(guide[i].toObject()["status"]==(QSet<int>{6,9,11}.contains(i)?"needs-review":"reviewed"),"Guide invalidated unrelated hand/part reviews or retained dependent review");
        changed=inputs;changed[10].evidence["hullEdit"]=1;const auto collision=authoringProgress(changed,reviews);
        for(int i=0;i<12;++i)check(collision[i].toObject()["status"]==(i>=10?"needs-review":"reviewed"),"Collision invalidation escaped its dependent steps");
        changed=inputs;changed[4].evidence["stroke"]=2;const auto motion=authoringProgress(changed,reviews);
        for(int i=0;i<12;++i)check(motion[i].toObject()["status"]==(QSet<int>{4,8,9,11}.contains(i)?"needs-review":"reviewed"),"Operating motion did not invalidate point/sound checks precisely");
        changed=inputs;changed[0].evidence["modelHash"]=QString(64,'a');for(const auto& item:authoringProgress(changed,reviews))check(item.toObject()["status"]=="needs-review","Source change retained stale review");
        changed=inputs;changed[7].available=false;check(authoringProgress(changed,reviews)[7].toObject()["status"]=="blocked","Unavailable step advertised review readiness");
        auto bad=reviews;bad["12"]=QString(64,'a');check(!validAuthoringReviews(bad),"Out-of-range review accepted");bad=reviews;bad["1"]="bad";check(!validAuthoringReviews(bad),"Invalid review hash accepted");
        const auto policy=collisionDraftPolicy("recipe",{{"hash","a"}},{});check(policy!=collisionDraftPolicy("recipe",{{"hash","b"}},{}),"Collision proof ignored source changes");
        QJsonObject drafts{{"full",QJsonObject{{"policy",policy},{"edit",QString(32,'a')},{"usable",true}}}};check(validCollisionDrafts(drafts),"Valid collision draft rejected");drafts["unknown"]=drafts["full"];check(!validCollisionDrafts(drafts),"Unknown collision state accepted");
        QTextStream(stdout)<<"PASS source/guide/motion/collision review invalidation and bounded saved metadata\n";return 0;
    }catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 1;}
}
