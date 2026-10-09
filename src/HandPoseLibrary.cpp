#include "HandPoseLibrary.h"
#include "StudioPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuaternion>
#include <QRegularExpression>
#include <QMatrix4x4>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace reloadmaker {
    namespace {
        constexpr qint64 kMaximumPoseBytes = 128 * 1024;
        const QString kConvention = QStringLiteral("fallout4-first-person-native-parent-local-v1");

        void require(bool condition, const char* error) {
            if (!condition) { throw std::runtime_error(error); }
        }
        bool validTransform(const Transformf& value) {
            Transformf inverse;
            const auto& q = value.rotation;
            const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
            return value.scale.x > 0 && std::abs(norm - 1.0f) < 0.001f && invertUniformTransform(value, inverse);
        }
        QString side(PoseHand hand) { return hand == PoseHand::Left ? QStringLiteral("left") : QStringLiteral("right"); }
        QString wrist(PoseHand hand) { return hand == PoseHand::Left ? QStringLiteral("LArm_Hand") : QStringLiteral("RArm_Hand"); }
        QString canonicalId(const QString& id) { return QUuid(id).toString(QUuid::WithoutBraces); }
        bool validId(const QString& id) { return !QUuid(id).isNull() && canonicalId(id) == id; }

        std::array<int, 15> fingerIndices(const HkxSkeleton& skeleton, PoseHand hand) {
            require(hand == PoseHand::Left || hand == PoseHand::Right, "Invalid hand side");
            std::array<int, 15> indices{};
            for (int i = 0; i < 15; ++i) {
                const QString name = handPoseBoneName(hand, i);
                const int bone = skeleton.boneIndex(name);
                require(bone >= 0, "The current skeleton is missing a required finger joint");
                const int parent = skeleton.bones[bone].parentIndex;
                const QString expected = i % 3 == 0 ? wrist(hand) : handPoseBoneName(hand, i - 1);
                require(parent >= 0 && parent < skeleton.bones.size() &&
                        skeleton.bones[parent].name.compare(expected, Qt::CaseInsensitive) == 0,
                        "Finger hierarchy does not match the saved native-local convention");
                indices[i] = bone;
            }
            return indices;
        }
        QJsonObject transformJson(const Transformf& value) {
            require(validTransform(value), "Pose contains a non-finite, non-uniform or invalid transform");
            // ROCKProviderTransform is rotate[9] row-major, translate[3], scale.
            // rotated() gives the columns of the same active rotation matrix.
            const auto x = rotated(value.rotation, {1,0,0});
            const auto y = rotated(value.rotation, {0,1,0});
            const auto z = rotated(value.rotation, {0,0,1});
            return {{"rotate", QJsonArray{x.x,y.x,z.x,x.y,y.y,z.y,x.z,y.z,z.z}},
                    {"translate", QJsonArray{value.translation.x, value.translation.y, value.translation.z}},
                    {"scale", value.scale.x}};
        }
        float number(const QJsonValue& value) {
            require(value.isDouble(), "Transform component is not a number");
            const double n = value.toDouble();
            require(std::isfinite(n) && std::abs(n) <= std::numeric_limits<float>::max(), "Transform component is not finite");
            return static_cast<float>(n);
        }
        Transformf readTransform(const QJsonValue& value) {
            require(value.isObject(), "Missing transform object");
            const auto object = value.toObject();
            const auto rotation = object["rotate"].toArray();
            const auto translation = object["translate"].toArray();
            require(rotation.size() == 9 && translation.size() == 3, "Invalid transform dimensions");
            QMatrix3x3 matrix;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) { matrix(r,c) = number(rotation[r*3+c]); }
            }
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    float dot = 0;
                    for (int k = 0; k < 3; ++k) { dot += matrix(r,k) * matrix(c,k); }
                    require(std::abs(dot - (r == c ? 1.0f : 0.0f)) < 0.001f, "Rotation matrix is not orthonormal");
                }
            }
            const float determinant = matrix(0,0)*(matrix(1,1)*matrix(2,2)-matrix(1,2)*matrix(2,1))
                - matrix(0,1)*(matrix(1,0)*matrix(2,2)-matrix(1,2)*matrix(2,0))
                + matrix(0,2)*(matrix(1,0)*matrix(2,1)-matrix(1,1)*matrix(2,0));
            require(std::abs(determinant - 1) < 0.001f, "Reflected rotation matrix is not supported");
            const auto q = QQuaternion::fromRotationMatrix(matrix).normalized();
            const float scale = number(object["scale"]);
            const Transformf result{{number(translation[0]), number(translation[1]), number(translation[2])},
                                    {q.x(),q.y(),q.z(),q.scalar()}, {scale,scale,scale}};
            require(validTransform(result), "Invalid transform scale or rotation");
            return result;
        }
        void validateIdentity(const HandPose& pose) {
            require(validId(pose.id), "Pose ID must be a canonical UUID");
            require(!pose.name.trimmed().isEmpty() && pose.name.size() <= 128, "Pose name must contain 1–128 characters");
            require(pose.notes.size() <= 4096, "Pose notes are too long");
            require(QDateTime::fromString(pose.createdUtc, Qt::ISODateWithMs).isValid(), "Invalid capture timestamp");
            require(pose.hand == PoseHand::Left || pose.hand == PoseHand::Right, "Invalid hand side");
            require(pose.placement.anchorKind == "weapon" || pose.placement.anchorKind == "installed-part" ||
                    pose.placement.anchorKind == "animated-part", "Invalid placement anchor kind");
            require(!pose.placement.anchorName.isEmpty(), "Placement anchor name is missing");
        }
    }

    QString handPoseBoneName(PoseHand hand, int fingerIndex) {
        if (fingerIndex < 0 || fingerIndex >= 15) { return {}; }
        return QStringLiteral("%1Arm_Finger%2%3").arg(hand == PoseHand::Left ? "L" : "R")
            .arg(fingerIndex / 3 + 1).arg(fingerIndex % 3 + 1);
    }

    bool encodePoseTransform(const Transformf& value,QJsonObject& json,QString& error) {
        error.clear(); try {json=transformJson(value);return true;}
        catch(const std::exception& e){error=QString::fromUtf8(e.what());return false;}
    }
    bool decodePoseTransform(const QJsonValue& json,Transformf& value,QString& error) {
        error.clear(); try {value=readTransform(json);return true;}
        catch(const std::exception& e){error=QString::fromUtf8(e.what());return false;}
    }

    bool captureHandPose(const HkxSkeleton& skeleton, const QVector<Transformf>& locals,
        const QVector<Transformf>& globals, const Transformf& anchorWorld, HandPose& pose, QString& error) {
        error.clear();
        try {
            require(locals.size() == skeleton.bones.size() && globals.size() == locals.size(), "No complete evaluated pose is available");
            const auto indices = fingerIndices(skeleton, pose.hand);
            const int hand = skeleton.boneIndex(wrist(pose.hand));
            const int weapon = skeleton.boneIndex("Weapon");
            require(hand >= 0 && weapon >= 0, "Required hand/Weapon frame is missing");
            Transformf inverseAnchor, inverseWeapon;
            require(validTransform(anchorWorld) && invertUniformTransform(anchorWorld, inverseAnchor), "Selected part has no usable frame (it may be hidden or zero-scaled)");
            require(validTransform(globals[weapon]) && invertUniformTransform(globals[weapon], inverseWeapon), "Weapon frame is not invertible");
            HandPose result = pose;
            for (int i = 0; i < 15; ++i) {
                require(validTransform(locals[indices[i]]), "A finger joint has an invalid transform");
                result.fingers[i] = locals[indices[i]];
            }
            result.placement.handInAnchor = composed(inverseAnchor, globals[hand]);
            result.placement.handInWeapon = composed(inverseWeapon, globals[hand]);
            require(validTransform(result.placement.handInAnchor) && validTransform(result.placement.handInWeapon), "Invalid captured wrist placement");
            pose = std::move(result);
            return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }

    bool mirrorHandPose(const HkxSkeleton& skeleton,const HandPose& source,const Transformf& anchorInWeapon,HandPose& target,QString& error) {
        error.clear();
        try {
            const auto other=source.hand==PoseHand::Left?PoseHand::Right:PoseHand::Left;
            const auto from=fingerIndices(skeleton,source.hand),to=fingerIndices(skeleton,other);
            const int sourceHand=skeleton.boneIndex(wrist(source.hand)),targetHand=skeleton.boneIndex(wrist(other));
            require(sourceHand>=0&&targetHand>=0,"Missing mirrored wrist");
            // Only bone bases are needed. Reference-pose scale/translation on
            // unrelated helper bones may not describe a renderable pose.
            QVector<Transformf> local,reference;for(const auto& b:skeleton.bones){Transformf basis;basis.rotation=normalized(b.referenceLocal.rotation);local.append(basis);}
            if(!buildPoseGlobals(skeleton,local,reference,error))return false;
            const auto matrix=[](const Transformf& t){QMatrix4x4 m;m.translate(t.translation.x,t.translation.y,t.translation.z);m.rotate(QQuaternion(t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z));m.scale(t.scale.x);return m;};
            QMatrix4x4 reflection;reflection.scale(-1,1,1);
            const auto mirrored=[&](const Transformf& value,int a,int b) {
                auto ra=reference[a],rb=reference[b];ra.translation={};rb.translation={};ra.scale=rb.scale={1,1,1};
                const auto m=reflection*matrix(value)*matrix(ra).inverted()*reflection*matrix(rb);
                const float scale=value.scale.x;QMatrix3x3 rotation;
                for(int r=0;r<3;++r)for(int c=0;c<3;++c)rotation(r,c)=m(r,c)/scale;
                const auto q=QQuaternion::fromRotationMatrix(rotation).normalized();
                return Transformf{{m(0,3),m(1,3),m(2,3)},{q.x(),q.y(),q.z(),q.scalar()},{scale,scale,scale}};
            };
            const auto hand=composed(anchorInWeapon,source.placement.handInAnchor);
            const auto mirroredHand=mirrored(hand,sourceHand,targetHand);
            Transformf inverse;require(invertUniformTransform(anchorInWeapon,inverse),"Invalid magazine frame");
            HandPose result=source;result.hand=other;
            // A mirrored label describes the target hand, not the history of
            // previous mirrors. Also collapse labels saved by older builds.
            QString name=source.name.trimmed();
            name.remove(QRegularExpression(QStringLiteral("(?:\\s*[·-]\\s*(?:left|right)(?: hand)?)+$"),QRegularExpression::CaseInsensitiveOption));
            result.name=name.trimmed()+(other==PoseHand::Left?QStringLiteral(" · left"):QStringLiteral(" · right"));
            result.placement.handInAnchor=composed(inverse,mirroredHand);result.placement.handInWeapon=mirroredHand;
            std::array<Transformf,15> original,converted;
            for(int i=0;i<15;++i) {
                original[i]=composed(i%3?original[i-1]:hand,source.fingers[i]);
                converted[i]=mirrored(original[i],from[i],to[i]);
                require(invertUniformTransform(i%3?converted[i-1]:mirroredHand,inverse),"Invalid mirrored finger parent");
                result.fingers[i]=composed(inverse,converted[i]);require(validTransform(result.fingers[i]),"Invalid mirrored finger transform");
            }
            require(validTransform(result.placement.handInAnchor),"Invalid mirrored wrist transform");target=std::move(result);return true;
        }catch(const std::exception& e){error=QString::fromUtf8(e.what());return false;}
    }
    bool handPoseFingersFor(const HkxSkeleton& skeleton,const HandPose& source,PoseHand hand,HandPose& target,QString& error) {
        auto fingers=source;fingers.placement={};
        if(source.hand==hand){target=std::move(fingers);error.clear();return true;}
        if(!mirrorHandPose(skeleton,fingers,{},target,error))return false;
        target.placement={};return true;
    }
    bool applyHandPoseFingers(const HkxSkeleton& skeleton, const HandPose& pose,
        QVector<Transformf>& locals, QString& error) {
        error.clear();
        try {
            require(locals.size() == skeleton.bones.size(), "No complete target pose is available");
            const auto indices = fingerIndices(skeleton, pose.hand);
            for (const auto& transform : pose.fingers) { require(validTransform(transform), "Saved finger transform is invalid"); }
            for (int i = 0; i < 15; ++i) { locals[indices[i]] = pose.fingers[i]; }
            return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }

    QByteArray encodeHandPose(const HandPose& pose, QString& error) {
        error.clear();
        try {
            validateIdentity(pose);
            QJsonArray fingers;
            for (int i = 0; i < 15; ++i) {
                auto joint = transformJson(pose.fingers[i]);
                joint["bone"] = handPoseBoneName(pose.hand, i);
                joint["parent"] = i % 3 == 0 ? wrist(pose.hand) : handPoseBoneName(pose.hand, i - 1);
                fingers.append(joint);
            }
            const auto& p = pose.placement;
            QJsonObject object{{"format", "reload-maker-hand-pose"}, {"version", 1}, {"id", pose.id},
                {"name", pose.name}, {"notes", pose.notes}, {"createdUtc", pose.createdUtc}, {"hand", side(pose.hand)},
                {"convention", kConvention}, {"units", "fallout4-game-units"}, {"rotationLayout", "row-major-3x3"},
                {"fingerLocalTransformMask", 0x7fff}, {"fingerLocalTransforms", fingers}, {"source", pose.source},
                {"placement", QJsonObject{{"anchorKind", p.anchorKind}, {"anchorName", p.anchorName},
                    {"anchorPath", p.anchorPath}, {"ownerModel", p.ownerModel}, {"ownerRecord", p.ownerRecord},
                    {"handInAnchor", transformJson(p.handInAnchor)}, {"handInWeapon", transformJson(p.handInWeapon)}}}};
            const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
            require(bytes.size() <= kMaximumPoseBytes, "Pose metadata exceeds the supported file size");
            return bytes;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return {}; }
    }

    bool decodeHandPose(const QByteArray& bytes, HandPose& pose, QString& error) {
        error.clear();
        try {
            require(!bytes.isEmpty() && bytes.size() <= kMaximumPoseBytes, "Pose file is empty or too large");
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(bytes, &parseError);
            require(parseError.error == QJsonParseError::NoError && document.isObject(), "Invalid pose JSON");
            const auto object = document.object();
            require(object["format"] == "reload-maker-hand-pose" && object["version"].isDouble() && object["version"].toDouble() == 1,
                    "Unsupported hand-pose file format/version");
            require(object["convention"] == kConvention && object["units"] == "fallout4-game-units" &&
                    object["rotationLayout"] == "row-major-3x3", "Unsupported pose coordinate convention");
            require(object["hand"] == "left" || object["hand"] == "right", "Invalid hand side");
            require(object["fingerLocalTransformMask"].toInt(-1) == 0x7fff, "Pose must contain all 15 finger joints");
            const auto fingers = object["fingerLocalTransforms"].toArray();
            require(fingers.size() == 15, "Pose must contain exactly 15 finger joints");
            HandPose result;
            result.id = object["id"].toString(); result.name = object["name"].toString();
            result.notes = object["notes"].toString(); result.createdUtc = object["createdUtc"].toString();
            result.hand = object["hand"] == "left" ? PoseHand::Left : PoseHand::Right;
            result.source = object["source"].toObject();
            for (int i = 0; i < 15; ++i) {
                const auto joint = fingers[i].toObject();
                require(joint["bone"] == handPoseBoneName(result.hand, i) &&
                        joint["parent"] == (i % 3 == 0 ? wrist(result.hand) : handPoseBoneName(result.hand, i - 1)),
                        "Finger order or parent convention does not match");
                result.fingers[i] = readTransform(fingers[i]);
            }
            const auto p = object["placement"].toObject();
            result.placement = {p["anchorKind"].toString(), p["anchorName"].toString(), p["anchorPath"].toString(),
                p["ownerModel"].toString(), p["ownerRecord"].toString(), readTransform(p["handInAnchor"]), readTransform(p["handInWeapon"])};
            validateIdentity(result);
            pose = std::move(result);
            return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }

    HandPoseLibrary::HandPoseLibrary(QString directory) : _directory(std::move(directory)) {}
    QString HandPoseLibrary::defaultDirectory() {
        return studioDataPath("workspace/hand-poses");
    }
    QVector<HandPose> HandPoseLibrary::load(QStringList& diagnostics) const {
        diagnostics.clear();
        QVector<HandPose> result;
        const QDir dir(_directory);
        if (!dir.exists()) { return result; }
        const auto files = dir.entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const auto& info : files) {
            QFile file(info.absoluteFilePath());
            HandPose pose;
            QString error;
            if (info.size() > kMaximumPoseBytes) { error = QStringLiteral("File too large"); }
            else if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); }
            else if (decodeHandPose(file.read(kMaximumPoseBytes + 1), pose, error) && info.completeBaseName() != pose.id) {
                error = QStringLiteral("Filename does not match the pose ID");
            }
            if (!error.isEmpty()) { diagnostics.append(QStringLiteral("%1: %2").arg(info.fileName(), error)); }
            else { result.append(std::move(pose)); }
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.createdUtc > b.createdUtc; });
        return result;
    }
    bool HandPoseLibrary::save(const HandPose& pose, QString& error) const {
        const auto bytes = encodeHandPose(pose, error);
        if (bytes.isEmpty()) { return false; }
        QDir dir(_directory);
        if (!dir.mkpath(".")) { error = QStringLiteral("Cannot create pose library: %1").arg(_directory); return false; }
        const QString path = dir.filePath(pose.id + ".json");
        if (QFileInfo::exists(path)) { error = QStringLiteral("This pose ID already exists; existing captures are never overwritten"); return false; }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = QStringLiteral("Cannot save pose: %1").arg(file.errorString()); return false;
        }
        return true;
    }
}
