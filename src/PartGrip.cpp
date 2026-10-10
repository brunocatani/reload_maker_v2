#include "PartGrip.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QQuaternion>
#include <QSaveFile>
#include <QUuid>
#include <QVector3D>
#include <algorithm>
#include <cmath>

namespace reloadmaker {
    namespace {
        constexpr qint64 maxGripBytes = 160 * 1024;
        QString key(QString text) { return text.replace('\\','/').toLower(); }
        QVector3D v(const Vec3f& p) { return {p.x,p.y,p.z}; }
        Vec3f vec(const QVector3D& p) { return {p.x(),p.y(),p.z()}; }
        QQuaternion q(const Quaternionf& r) { return {r.w,r.x,r.y,r.z}; }
        Quaternionf quat(const QQuaternion& r) { return {r.x(),r.y(),r.z(),r.scalar()}; }
        QQuaternion fromTo(QVector3D a, QVector3D b, const QVector3D& pole) {
            a.normalize(); b.normalize();
            if (QVector3D::dotProduct(a,b) < -0.99999f) {
                const auto axis = (pole-a*QVector3D::dotProduct(a,pole)).normalized();
                return QQuaternion::fromAxisAndAngle(axis,180);
            }
            return QQuaternion::rotationTo(a,b);
        }
        Transformf carry(const QQuaternion& rotation, const QVector3D& from, const QVector3D& to) {
            return {vec(to-rotation.rotatedVector(from)),quat(rotation),{1,1,1}};
        }
    }
    bool sameGripAnchor(const HandPosePlacement& a, const HandPosePlacement& b) {
        return a.anchorKind==b.anchorKind && key(a.anchorName)==key(b.anchorName) &&
            key(a.anchorPath)==key(b.anchorPath) && key(a.ownerModel)==key(b.ownerModel) &&
            key(a.ownerRecord)==key(b.ownerRecord);
    }
    namespace {
        bool samePlacement(const Transformf& a, const Transformf& b) {
            const auto near=[](float x,float y){return std::abs(x-y)<.00002f;};
            if(!near(a.translation.x,b.translation.x)||!near(a.translation.y,b.translation.y)||!near(a.translation.z,b.translation.z)||
               !near(a.scale.x,b.scale.x)||!near(a.scale.y,b.scale.y)||!near(a.scale.z,b.scale.z)) { return false; }
            const auto& x=a.rotation; const auto& y=b.rotation;
            const float sign=x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w<0?-1.f:1.f;
            return near(x.x,sign*y.x)&&near(x.y,sign*y.y)&&near(x.z,sign*y.z)&&near(x.w,sign*y.w);
        }
    }
    bool sameGripFit(const PartGrip& a, const PartGrip& b) {
        if(a.pose.name!=b.pose.name||a.pose.hand!=b.pose.hand||!sameGripAnchor(a.pose.placement,b.pose.placement)||
           !samePlacement(a.pose.placement.handInAnchor,b.pose.placement.handInAnchor)) { return false; }
        for(int i=0;i<15;++i) { if(!samePlacement(a.pose.fingers[i],b.pose.fingers[i])) { return false; } }
        return true;
    }
    int resolveGripAnchor(const PartGrip& grip, const QString& weaponRecord,
                          const QVector<HandPosePlacement>& anchors, QString& error) {
        error.clear();
        if (weaponRecord.isEmpty() || key(weaponRecord)!=key(grip.weaponRecord)) {
            error=QStringLiteral("This part grip belongs to a different weapon record"); return -1;
        }
        int match=-1;
        for (int i=0;i<anchors.size();++i) {
            if (!sameGripAnchor(grip.pose.placement,anchors[i])) { continue; }
            if (match>=0) { error=QStringLiteral("The saved reference part is ambiguous in this assembly"); return -1; }
            match=i;
        }
        if (match<0) { error=QStringLiteral("The saved reference part/model is not installed in this configuration"); }
        return match;
    }
    QByteArray encodePartGrip(const PartGrip& grip, QString& error) {
        const auto pose=encodeHandPose(grip.pose,error);
        if (pose.isEmpty()) { return {}; }
        if (grip.weaponRecord.trimmed().isEmpty() || grip.weaponRecord.size()>512 ||
            QUuid(grip.sourcePoseId).isNull() || QUuid(grip.sourcePoseId).toString(QUuid::WithoutBraces)!=grip.sourcePoseId) {
            error=QStringLiteral("Part grip needs a weapon identity and source pose ID"); return {};
        }
        if(!grip.mechanismId.isEmpty() && (QUuid(grip.mechanismId).isNull() || QUuid(grip.mechanismId).toString(QUuid::WithoutBraces)!=grip.mechanismId)) {error="Invalid shared mechanism ID";return {};}
        QJsonObject object{{"format","reload-maker-part-grip"},{"version",!grip.mechanismId.isEmpty()?3:grip.surfaceContact?2:1},
            {"weaponRecord",grip.weaponRecord},{"sourcePoseId",grip.sourcePoseId},
            {"pose",QJsonDocument::fromJson(pose).object()}};
        if(!grip.mechanismId.isEmpty()) object["mechanismId"]=grip.mechanismId;
        if(grip.surfaceContact) {
            const auto& c=*grip.surfaceContact;
            const auto valid=[](Vec3f v){return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::abs(v.x)<=1e6f && std::abs(v.y)<=1e6f && std::abs(v.z)<=1e6f;};
            if(!valid(c.pointInAnchor) || !valid(c.palmInHand) || c.surfaceName.size()>512) {
                error=QStringLiteral("Invalid surface contact metadata"); return {};
            }
            object["surfaceContact"]=QJsonObject{{"method","generic-palm-center-v1"},
                {"pointInAnchor",QJsonArray{c.pointInAnchor.x,c.pointInAnchor.y,c.pointInAnchor.z}},
                {"palmInHand",QJsonArray{c.palmInHand.x,c.palmInHand.y,c.palmInHand.z}},{"surfaceName",c.surfaceName}};
        }
        const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Indented);
        if (bytes.size()>maxGripBytes) { error=QStringLiteral("Part grip exceeds supported file size"); return {}; }
        return bytes;
    }
    bool decodePartGrip(const QByteArray& bytes, PartGrip& grip, QString& error) {
        error.clear();
        if (bytes.isEmpty() || bytes.size()>maxGripBytes) { error=QStringLiteral("Part-grip file is empty or too large"); return false; }
        QJsonParseError parse;
        const auto doc=QJsonDocument::fromJson(bytes,&parse);
        if (parse.error!=QJsonParseError::NoError || !doc.isObject() ||
            doc["format"]!="reload-maker-part-grip" || (doc["version"].toDouble()!=1 && doc["version"].toDouble()!=2 && doc["version"].toDouble()!=3)) {
            error=QStringLiteral("Invalid or unsupported part-grip file"); return false;
        }
        PartGrip result;
        if (!decodeHandPose(QJsonDocument(doc["pose"].toObject()).toJson(),result.pose,error)) { return false; }
        result.weaponRecord=doc["weaponRecord"].toString(); result.sourcePoseId=doc["sourcePoseId"].toString();
        if(doc["version"].toDouble()==3) {result.mechanismId=doc["mechanismId"].toString();if(result.mechanismId.isEmpty()){error="Shared part definition ID is missing";return false;}}
        else if(doc.object().contains("mechanismId")){error="Shared definitions require grip version 3";return false;}
        if(doc["version"].toDouble()==2 || (doc["version"].toDouble()==3 && doc.object().contains("surfaceContact"))) {
            const auto c=doc["surfaceContact"].toObject();
            const auto readPoint=[](QJsonValue value,Vec3f& out){
                const auto a=value.toArray(); if(a.size()!=3) return false;
                for(const auto& v:a) if(!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble())>1e6) return false;
                out={float(a[0].toDouble()),float(a[1].toDouble()),float(a[2].toDouble())}; return true;
            };
            PartGrip::SurfaceContact contact;
            if(c["method"]!="generic-palm-center-v1" || !readPoint(c["pointInAnchor"],contact.pointInAnchor) || !readPoint(c["palmInHand"],contact.palmInHand)) {
                error=QStringLiteral("Invalid surface contact metadata"); return false;
            }
            contact.surfaceName=c["surfaceName"].toString(); result.surfaceContact=contact;
        } else if(doc.object().contains("surfaceContact")) {
            error=QStringLiteral("Surface contact requires part-grip version 2"); return false;
        }
        if (encodePartGrip(result,error).isEmpty()) { return false; }
        grip=std::move(result); return true;
    }
    PartGripLibrary::PartGripLibrary(QString directory):_directory(std::move(directory)) {}
    QString PartGripLibrary::defaultDirectory() {
        return QDir::cleanPath(QDir(HandPoseLibrary::defaultDirectory()).filePath("../part-grips"));
    }
    QVector<PartGrip> PartGripLibrary::load(QStringList& diagnostics) const {
        diagnostics.clear(); QVector<PartGrip> result;
        for (const auto& info:QDir(_directory).entryInfoList({"*.json"},QDir::Files|QDir::NoSymLinks,QDir::Name)) {
            QFile file(info.absoluteFilePath()); PartGrip grip; QString error;
            if (info.size()>maxGripBytes) { error=QStringLiteral("File too large"); }
            else if (!file.open(QIODevice::ReadOnly)) { error=file.errorString(); }
            else if (decodePartGrip(file.read(maxGripBytes+1),grip,error) && info.completeBaseName()!=grip.pose.id) {
                error=QStringLiteral("Filename does not match grip ID");
            }
            if (error.isEmpty()) { result.append(std::move(grip)); }
            else { diagnostics.append(QStringLiteral("%1: %2").arg(info.fileName(),error)); }
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.pose.createdUtc>b.pose.createdUtc;});
        return result;
    }
    bool PartGripLibrary::save(const PartGrip& grip, QString& error) const {
        const auto bytes=encodePartGrip(grip,error);
        if (bytes.isEmpty()) { return false; }
        QDir directory(_directory);
        if (!directory.mkpath(".")) { error=QStringLiteral("Cannot create part-grip library"); return false; }
        const auto path=directory.filePath(grip.pose.id+".json");
        if (QFileInfo::exists(path)) { error=QStringLiteral("Existing part grips are never overwritten"); return false; }
        QSaveFile file(path); file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
            error=file.errorString(); return false;
        }
        return true;
    }

    bool solvePartGrip(const HkxSkeleton& skeleton, const QVector<Transformf>& nativeLocal,
        const QVector<Transformf>& framedGlobal, const HandPose& pose, const Transformf& handWorld,
        QVector<Transformf>& solvedLocal, QVector<Transformf>& solvedGlobal, QString& error,GripReach reach,bool allowHandScale) {
        error.clear();
        const QString prefix=pose.hand==PoseHand::Left?"LArm_":"RArm_";
        const int upper=skeleton.boneIndex(prefix+"UpperArm");
        const int fore=skeleton.boneIndex(prefix+"ForeArm1");
        const int hand=skeleton.boneIndex(prefix+"Hand");
        Transformf inverse;
        if (nativeLocal.size()!=skeleton.bones.size() || framedGlobal.size()!=nativeLocal.size() ||
            upper<0 || fore<0 || hand<0 || !invertUniformTransform(handWorld,inverse)) {
            error=QStringLiteral("The target or first-person arm chain is invalid"); return false;
        }
        const auto descends=[&](int bone,int ancestor) {
            for (int depth=0;bone>=0 && bone<skeleton.bones.size() && depth<skeleton.bones.size();++depth) {
                if (bone==ancestor) { return true; }
                bone=skeleton.bones[bone].parentIndex;
            }
            return false;
        };
        if (!descends(fore,upper) || !descends(hand,fore) ||
            !invertUniformTransform(framedGlobal[upper],inverse) || !invertUniformTransform(framedGlobal[fore],inverse)) {
            error=QStringLiteral("Unsupported first-person arm hierarchy"); return false;
        }
        // ROCK ArmPresentationPolicy provides the segment ownership and bend-plane
        // construction. Use our quaternion convention; do not copy NiMatrix layout.
        const auto sourceShoulder=v(framedGlobal[upper].translation), e=v(framedGlobal[fore].translation);
        const auto w=v(framedGlobal[hand].translation), target=v(handWorld.translation);
        const float a=(e-sourceShoulder).length(), b=(w-e).length();
        auto s=sourceShoulder;float distance=(target-s).length();
        if(reach==GripReach::FollowTarget&&a>.0001f&&b>.0001f&&std::isfinite(distance)) {
            // A cursor-held object is not constrained by the preview torso.
            // Follow it with the arm base only outside its native reach; keep
            // segment lengths, the exact wrist, weapon and other hand intact.
            const float minimum=std::abs(a-b)+std::min(a,b)*.02f,maximum=a+b-std::min(a,b)*.02f;
            const float reachable=std::clamp(distance,minimum,maximum);
            const auto direction=distance>.0001f?(target-s)/distance:(w-s).normalized();
            s=target-direction*reachable;distance=reachable;
        }
        if (!(a>.0001f && b>.0001f && distance>.0001f) || !std::isfinite(distance) ||
            distance>a+b+.0001f || distance<std::abs(a-b)-.0001f) {
            error=QStringLiteral("Target is outside this arm's reach; move it closer or use the current wrist"); return false;
        }
        if (!allowHandScale && std::abs(handWorld.scale.x-framedGlobal[hand].scale.x)>.001f) {
            error=QStringLiteral("Reference-part scale differs; use the current wrist to preserve hand size"); return false;
        }
        if (!invertUniformTransform(framedGlobal[hand],inverse)) { error=QStringLiteral("Invalid wrist frame"); return false; }
        const Transformf handCarry=composed(handWorld,inverse);
        const auto axis=(target-s)/distance;
        auto oldAxis=(w-sourceShoulder).normalized();
        auto oldPole=(e-sourceShoulder)-oldAxis*QVector3D::dotProduct(e-sourceShoulder,oldAxis);
        if (oldPole.lengthSquared()<.000001f) {
            // A straight source arm has no bend plane. Use its own basis, never a
            // hardcoded world up that changes under weapon/world reframing.
            const auto basis=q(framedGlobal[upper].rotation);
            for (const auto candidate:{basis.rotatedVector({0,1,0}),basis.rotatedVector({0,0,1}),basis.rotatedVector({1,0,0})}) {
                oldPole=candidate-oldAxis*QVector3D::dotProduct(candidate,oldAxis);
                if (oldPole.lengthSquared()>.1f) { break; }
            }
        }
        auto pole=fromTo(w-sourceShoulder,target-s,oldPole).rotatedVector(oldPole);
        pole=(pole-axis*QVector3D::dotProduct(pole,axis)).normalized();
        const float along=(a*a+distance*distance-b*b)/(2*distance);
        const float height=std::sqrt(std::max(0.f,a*a-along*along));
        const auto elbow=s+axis*along+pole*height;
        const auto planeNormal=QVector3D::crossProduct(oldAxis,oldPole).normalized();
        const auto upperCarry=carry(fromTo(e-sourceShoulder,elbow-s,planeNormal),sourceShoulder,s);
        const auto foreCarry=carry(fromTo(w-e,target-elbow,planeNormal),e,elbow);
        auto global=framedGlobal;
        auto local=nativeLocal;
        QVector<bool> changed(global.size(),false);
        if (!applyHandPoseFingers(skeleton,pose,local,error)) { return false; }
        for (int i=0;i<global.size();++i) {
            const auto& name=skeleton.bones[i].name;
            if (!name.startsWith(prefix)) { continue; }
            const QString tail=name.mid(prefix.size());
            if (tail=="UpperArm" || tail=="UpperTwist1" || tail=="UpperTwist2") {
                global[i]=composed(upperCarry,framedGlobal[i]);
            } else if (tail=="ForeArm1" || tail=="ForeArm2" || tail=="ForeArm3") {
                global[i]=composed(foreCarry,framedGlobal[i]);
            } else if (tail=="Hand") { global[i]=composed(handCarry,framedGlobal[i]); }
            else if (tail.startsWith("Finger")) {
                const int parent=skeleton.bones[i].parentIndex;
                if (parent<0 || parent>=i) { error=QStringLiteral("Invalid finger chain"); return false; }
                global[i]=composed(global[parent],local[i]);
            } else { continue; }
            changed[i]=true;
        }
        for (int i=0;i<local.size();++i) {
            const int parent=skeleton.bones[i].parentIndex;
            // Unchanged branches retain native locals, including zero-scaled
            // visibility keys whose children cannot be inverted in world space.
            if (!changed[i] && (parent<0 || (parent<i && !changed[parent]))) { continue; }
            if (parent>=i || parent< -1 || (parent>=0 && !invertUniformTransform(global[parent],inverse))) {
                error=QStringLiteral("Cannot reconstruct solved arm hierarchy"); return false;
            }
            local[i]=parent>=0?composed(inverse,global[i]):global[i];
        }
        solvedLocal=std::move(local); solvedGlobal=std::move(global); return true;
    }
}
