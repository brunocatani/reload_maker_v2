#include "HandPoseLibrary.h"
#include "ScenePoseBinding.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace reloadmaker;
namespace {
    void require(bool value, const QString& message) { if (!value) { throw std::runtime_error(message.toStdString()); } }
    Transformf transform(Vec3f t, float angle = 0) {
        const auto q = QQuaternion::fromEulerAngles(angle, angle * .7f, angle * -.2f);
        return {t,{q.x(),q.y(),q.z(),q.scalar()},{1,1,1}};
    }
    QMatrix4x4 matrix(const Transformf& t) {
        QMatrix4x4 result; result.translate(t.translation.x,t.translation.y,t.translation.z);
        result.rotate(QQuaternion(t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z));
        result.scale(t.scale.x,t.scale.y,t.scale.z); return result;
    }
    void expect(const Transformf& a, const Transformf& b) {
        const auto ma=matrix(a), mb=matrix(b);
        for (int i=0;i<16;++i) { require(std::isfinite(ma.constData()[i]) && std::abs(ma.constData()[i]-mb.constData()[i]) < .0003f, "Pose transform mismatch"); }
    }
    HandPose identity(PoseHand hand) {
        HandPose result; result.hand=hand; result.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.name="Bolt pinch"; result.createdUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        result.placement.anchorKind="animated-part"; result.placement.anchorName="WeaponBolt";
        return result;
    }
    QByteArray read(const QString& path) {
        QFile f(path); require(f.open(QIODevice::ReadOnly), "Cannot open fixture: " + path); return f.readAll();
    }
    void write(const QString& path, const QByteArray& bytes) {
        QFile f(path); require(f.open(QIODevice::WriteOnly), "Cannot write fixture"); require(f.write(bytes)==bytes.size(),"Short fixture write");
    }
    HkxSkeleton rig() {
        HkxSkeleton skeleton;
        skeleton.bones={{"Root",-1,false,{}},{"RArm_Hand",0,false,{}},{"Weapon",1,false,{}},{"LArm_Hand",0,false,{}},{"WeaponBolt",2,false,{}}};
        for (const auto side : {PoseHand::Left,PoseHand::Right}) {
            for (int i=0;i<15;++i) {
                const int parent=i%3==0 ? (side==PoseHand::Left?3:1) : skeleton.bones.size()-1;
                skeleton.bones.append({handPoseBoneName(side,i),static_cast<std::int16_t>(parent),false,{}});
            }
        }
        return skeleton;
    }
    void roundTripAndPreview(const HkxSkeleton& skeleton, const QVector<Transformf>& locals, PoseHand side) {
        QString error;
        QVector<Transformf> globals;
        require(buildPoseGlobals(skeleton,locals,globals,error),error);
        const int weapon=skeleton.boneIndex("Weapon");
        const auto anchor=composed(globals[weapon],transform({2,-5,7},37));
        auto pose=identity(side);
        require(captureHandPose(skeleton,locals,globals,anchor,pose,error),error);
        const auto encoded=encodeHandPose(pose,error);
        require(!encoded.isEmpty(),error);
        const auto json=QJsonDocument::fromJson(encoded).object();
        const auto rotation=json["fingerLocalTransforms"].toArray()[0].toObject()["rotate"].toArray();
        const auto expectedRotation=matrix(pose.fingers[0]);
        for(int r=0;r<3;++r) for(int c=0;c<3;++c) {
            require(std::abs(rotation[r*3+c].toDouble()-expectedRotation(r,c)) < .0001,"ROCK row-major matrix orientation transposed");
        }
        HandPose decoded;
        require(decodeHandPose(encoded,decoded,error),error);
        for(int i=0;i<15;++i) expect(pose.fingers[i],decoded.fingers[i]);
        expect(pose.placement.handInAnchor,decoded.placement.handInAnchor);
        expect(pose.placement.handInWeapon,decoded.placement.handInWeapon);
        const int hand=skeleton.boneIndex(side==PoseHand::Left?"LArm_Hand":"RArm_Hand");
        expect(composed(anchor,decoded.placement.handInAnchor),globals[hand]);
        expect(composed(globals[weapon],decoded.placement.handInWeapon),globals[hand]);

        auto fixedLocals=locals;
        auto fixedGlobals=globals;
        require(reframePose(skeleton,globals[weapon],{},fixedLocals,fixedGlobals,error),error);
        Transformf inverse;
        require(invertUniformTransform(globals[weapon],inverse),"Invalid fixture Weapon");
        auto fixedPose=identity(side);
        require(captureHandPose(skeleton,fixedLocals,fixedGlobals,composed(inverse,anchor),fixedPose,error),error);
        expect(fixedPose.placement.handInAnchor,pose.placement.handInAnchor);
        expect(fixedPose.placement.handInWeapon,pose.placement.handInWeapon);
        for(int i=0;i<15;++i) expect(fixedPose.fingers[i],pose.fingers[i]);

        QVector<Transformf> destination(locals.size(),transform({3,4,-8},-41));
        const auto before=destination;
        require(applyHandPoseFingers(skeleton,decoded,destination,error),error);
        for(int bone=0;bone<destination.size();++bone) {
            int finger=-1;
            for(int i=0;i<15;++i) if(skeleton.bones[bone].name==handPoseBoneName(side,i)) finger=i;
            expect(destination[bone],finger>=0 ? decoded.fingers[finger] : before[bone]);
        }
        auto bad=skeleton;
        bad.bones[skeleton.boneIndex(handPoseBoneName(side,14))].name="Missing";
        destination=before;
        require(!applyHandPoseFingers(bad,decoded,destination,error),"Missing bone accepted");
        for(int i=0;i<destination.size();++i) expect(destination[i],before[i]);
        bad=skeleton;
        bad.bones[skeleton.boneIndex(handPoseBoneName(side,1))].parentIndex=hand;
        require(!applyHandPoseFingers(bad,decoded,destination,error),"Incompatible hierarchy accepted");
    }
    void previewBindingTests(const HkxSkeleton& skeleton, const QVector<Transformf>& source, PoseHand side) {
        QString error;
        QVector<Transformf> sourceGlobal;
        require(buildPoseGlobals(skeleton,source,sourceGlobal,error),error);
        auto pose=identity(side);
        require(captureHandPose(skeleton,source,sourceGlobal,sourceGlobal[2],pose,error),error);
        for(auto& finger:pose.fingers) finger=transform({.8f,1.4f,-.2f},67);
        QVector<ScenePoseNode> nodes;
        QVector<int> canonical;
        for(int i=0;i<skeleton.bones.size();++i) {
            nodes.append({skeleton.bones[i].parentIndex,i,source[i]}); canonical.append(i);
        }
        QVector<ScenePoseBinding> bindings;
        require(compileSceneBindings(skeleton,2,nodes,canonical,bindings,error),error);
        // Exercise the controller's split: native locals still drive parts;
        // only presentation globals contain the selected finger override.
        for(const auto frame:{sourceGlobal[2],transform({-4,8,3},-23)}) {
            QVector<Transformf> before, preview, restored, boundLocal;
            const auto evaluate=[&](bool overrideFingers,QVector<Transformf>& output) {
                auto local=source;
                QVector<Transformf> global;
                if(overrideFingers) require(applyHandPoseFingers(skeleton,pose,local,error),error);
                require(buildPoseGlobals(skeleton,local,global,error),error);
                require(reframePose(skeleton,frame,{},local,global,error),error);
                require(evaluateSceneBindings(skeleton,bindings,source,source,global,boundLocal,output,error),error);
            };
            evaluate(false,before); evaluate(true,preview); evaluate(false,restored);
            for(int i=0;i<bindings.size();++i) {
                expect(before[i],restored[i]);
                const QString name=skeleton.bones[bindings[i].boneIndex].name;
                if(!name.startsWith(side==PoseHand::Left?"LArm_Finger":"RArm_Finger")) expect(before[i],preview[i]);
            }
            bool changed=false;
            for(int i=0;i<bindings.size();++i) {
                if(skeleton.bones[bindings[i].boneIndex].name==handPoseBoneName(side,0)) {
                    changed=matrix(before[i])!=matrix(preview[i]);
                }
            }
            require(changed,"Saved fingers never reached scene bindings");
        }
    }
    void storageTests() {
        QTemporaryDir temporary; require(temporary.isValid(),"No temporary folder");
        HandPoseLibrary library(temporary.filePath("poses"));
        auto pose=identity(PoseHand::Right);
        pose.name="../name is not a filename"; pose.notes="Kept across restarts";
        for(int i=0;i<15;++i) pose.fingers[i]=transform({float(i),1,2},float(i*9));
        QString error;
        require(library.save(pose,error),error);
        const auto file=QDir(library.directory()).filePath(pose.id+".json");
        const auto original=read(file);
        require(!library.save(pose,error),"Existing capture overwritten");
        require(read(file)==original,"Existing capture changed on failed save");
        HandPoseLibrary restarted(library.directory());
        QStringList diagnostics;
        auto loaded=restarted.load(diagnostics);
        require(loaded.size()==1 && diagnostics.isEmpty() && loaded[0].name==pose.name && loaded[0].notes==pose.notes,"Restart lost saved pose");
        for(int i=0;i<15;++i) expect(loaded[0].fingers[i],pose.fingers[i]);
        pose.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        require(library.save(pose,error),error);
        require(restarted.load(diagnostics).size()==2,"Duplicate labels collapsed independent captures");
        auto object=QJsonDocument::fromJson(original).object();
        const auto reject=[&](QJsonObject bad) {
            HandPose existing=pose;
            require(!decodeHandPose(QJsonDocument(bad).toJson(),existing,error) && !error.isEmpty(),"Malformed pose accepted");
            require(existing.id==pose.id,"Decode failure partially changed output");
        };
        auto bad=object; bad["version"]=2; reject(bad);
        bad=object; bad["convention"]="controller-local"; reject(bad);
        bad=object; bad["hand"]="both"; reject(bad);
        bad=object; bad["fingerLocalTransformMask"]=1; reject(bad);
        auto fingers=object["fingerLocalTransforms"].toArray(); fingers.removeLast();
        bad=object; bad["fingerLocalTransforms"]=fingers; reject(bad);
        fingers=object["fingerLocalTransforms"].toArray();
        auto joint=fingers[0].toObject(); joint["bone"]="LArm_Finger11"; fingers[0]=joint;
        bad=object; bad["fingerLocalTransforms"]=fingers; reject(bad);
        joint=object["fingerLocalTransforms"].toArray()[0].toObject(); joint["rotate"]=QJsonArray{-1,0,0,0,1,0,0,0,1}; fingers[0]=joint;
        bad=object; bad["fingerLocalTransforms"]=fingers; reject(bad);
        pose.fingers[0].translation.x=std::numeric_limits<float>::quiet_NaN();
        require(!library.save(pose,error),"NaN transform saved");
        write(QDir(library.directory()).filePath("broken.json"),"{bad JSON");
        require(restarted.load(diagnostics).size()==2 && diagnostics.size()==1,"Corrupt file hid valid captures or was silently accepted");
        write(temporary.filePath("blocked"),"preserve");
        HandPoseLibrary blocked(temporary.filePath("blocked/poses"));
        pose=identity(PoseHand::Left);
        require(!blocked.save(pose,error) && read(temporary.filePath("blocked"))=="preserve","Save failure damaged existing path");
    }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto skeleton=rig();
        QVector<Transformf> locals;
        for(int i=0;i<skeleton.bones.size();++i) locals.append(transform({float(i*.2),float(i*.1),float(i*-.05)},float(i*5)));
        for(const auto side:{PoseHand::Left,PoseHand::Right}) {
            roundTripAndPreview(skeleton,locals,side);
            previewBindingTests(skeleton,locals,side);
        }
        storageTests();
        const auto args=app.arguments();
        if(args.size()>1) {
            require(args.size()==4,"usage: HandPoseTests skeleton idle reload");
            QString error;
            const auto rig=HkxReader::readSkeleton(read(args[1]),error); require(rig.valid(),error);
            const auto idle=HkxReader::readClip(read(args[2]),error); require(idle.valid(),error);
            const auto clip=HkxReader::readClip(read(args[3]),error); require(clip.valid(),error);
            QVector<Transformf> tracks,base,global;
            require(samplePose(rig,idle,0,nullptr,tracks,base,global,error),error);
            for(const auto side:{PoseHand::Left,PoseHand::Right}) {
                roundTripAndPreview(rig,base,side);
                for(float fraction:{0.f,.25f,.5f,.75f,1.f}) {
                    QVector<Transformf> pose;
                    require(samplePose(rig,clip,clip.durationSeconds*fraction,&base,tracks,pose,global,error),error);
                    roundTripAndPreview(rig,pose,side);
                }
            }
        }
        QTextStream(stdout)<<"PASS hand capture, ROCK matrix layout, frame-invariant placement, one-hand preview, JSON round trip, restart persistence and failure safety\n";
        return 0;
    } catch(const std::exception& error) { QTextStream(stderr)<<"FAIL "<<error.what()<<'\n'; return 1; }
}
