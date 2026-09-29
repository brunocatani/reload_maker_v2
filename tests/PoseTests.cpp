#include "PoseEvaluator.h"

#include <QCoreApplication>
#include <QFile>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QTextStream>

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void require(bool condition, const QString& message)
    {
        if (!condition) { throw std::runtime_error(message.toStdString()); }
    }

    QMatrix4x4 matrix(const Transformf& value)
    {
        QMatrix4x4 result;
        result.translate(value.translation.x, value.translation.y, value.translation.z);
        result.rotate(QQuaternion(value.rotation.w, value.rotation.x, value.rotation.y, value.rotation.z));
        result.scale(value.scale.x, value.scale.y, value.scale.z);
        return result;
    }

    void expect(const Transformf& actual, const QMatrix4x4& expected, float tolerance = 0.002f)
    {
        const QMatrix4x4 result = matrix(actual);
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                require(std::isfinite(result(row, col)) &&
                        std::abs(result(row, col) - expected(row, col)) < tolerance,
                        QStringLiteral("Pose differs from independent matrix oracle at %1,%2: %3 vs %4")
                            .arg(row).arg(col).arg(result(row, col)).arg(expected(row, col)));
            }
        }
    }

    Transformf transform(Vec3f position, float pitch, float yaw, float roll, float scale = 1)
    {
        const QQuaternion q = QQuaternion::fromEulerAngles(pitch, yaw, roll);
        return { position, {q.x(), q.y(), q.z(), q.scalar()}, {scale, scale, scale} };
    }

    void checkReframe(const HkxSkeleton& skeleton, int weapon,
                      const QVector<Transformf>& sourceLocal,
                      const QVector<Transformf>& sourceGlobal)
    {
        auto local = sourceLocal;
        auto global = sourceGlobal;
        QString error;
        const Transformf destination = transform({3, -7, 2}, 15, 40, -20);
        const QMatrix4x4 correction = matrix(destination) * matrix(sourceGlobal.at(weapon)).inverted();
        require(reframePose(skeleton, sourceGlobal.at(weapon), destination, local, global, error), error);
        expect(global.at(weapon), matrix(destination));
        QVector<Transformf> reconstructed;
        require(buildPoseGlobals(skeleton, local, reconstructed, error), error);
        for (int bone = 0; bone < global.size(); ++bone) {
            expect(global.at(bone), correction * matrix(sourceGlobal.at(bone)));
            expect(reconstructed.at(bone), matrix(global.at(bone)));
            if (skeleton.bones.at(bone).parentIndex >= 0) {
                expect(local.at(bone), matrix(sourceLocal.at(bone)));
            }
        }
        // A different world/camera basis must not alter any weapon-relative contact.
        local = sourceLocal;
        global = sourceGlobal;
        const Transformf worldChange = transform({-41, 23, 67}, 53, -71, 18, 1.3f);
        require(reframePose(skeleton, Transformf{}, worldChange, local, global, error), error);
        const Transformf changedWeapon = global.at(weapon);
        require(reframePose(skeleton, changedWeapon, destination, local, global, error), error);
        for (int bone = 0; bone < global.size(); ++bone) {
            expect(global.at(bone), correction * matrix(sourceGlobal.at(bone)), 0.003f);
        }
    }

    void synthetic()
    {
        HkxSkeleton skeleton;
        skeleton.name = QStringLiteral("Root");
        const int parents[] = {-1, 0, 1, 2, 2, 0, 5, -1};
        for (int i = 0; i < 8; ++i) {
            skeleton.bones.append(HkxBone{QString::number(i), static_cast<std::int16_t>(parents[i]), false,
                transform({float(i * 3), float(9 - i), float(i - 4)}, float(i * 7), float(i * 11), float(i * -13))});
        }
        QVector<Transformf> locals, globals;
        for (const auto& bone : skeleton.bones) { locals.append(bone.referenceLocal); }
        QString error;
        require(buildPoseGlobals(skeleton, locals, globals, error), error);
        checkReframe(skeleton, 2, locals, globals);

        // Full tracks own the hand and finger, irrespective of a distant idle base.
        auto base = locals;
        base[1].translation = {99, 99, 99};
        QVector<Transformf> tracks{transform({1, 2, 3}, 45, 90, 0), transform({4, 5, 6}, 20, 0, -60)};
        const QVector<std::int16_t> mapping{1, 6};
        require(composeLocalPose(skeleton, tracks, mapping, false, &base, locals, error), error);
        expect(locals.at(1), matrix(tracks.at(0)));
        expect(locals.at(6), matrix(tracks.at(1)));
        expect(locals.at(3), matrix(base.at(3)));
        // Explicit additive base, with translation in parent space and rotation base * delta.
        require(composeLocalPose(skeleton, tracks, mapping, true, &base, locals, error), error);
        const auto& b = base.at(1);
        QMatrix4x4 expected;
        expected.translate(b.translation.x + 1, b.translation.y + 2, b.translation.z + 3);
        expected.rotate(QQuaternion(b.rotation.w, b.rotation.x, b.rotation.y, b.rotation.z));
        const auto& d = tracks.at(0).rotation;
        expected.rotate(QQuaternion(d.w, d.x, d.y, d.z));
        expect(locals.at(1), expected);
        require(!composeLocalPose(skeleton, tracks, mapping, true, nullptr, locals, error), "additive without base accepted");
        require(!composeLocalPose(skeleton, tracks, mapping, true, &locals, locals, error), "aliased base accepted");
        require(!composeLocalPose(skeleton, tracks, {1, 99}, false, &base, locals, error), "invalid mapping accepted");
        locals = base;
        locals[3].scale = {0, 0, 0};
        require(buildPoseGlobals(skeleton, locals, globals, error), "authored hidden part rejected");
        checkReframe(skeleton, 2, locals, globals);
        locals = base;
        locals[0].scale = {1, 2, 1};
        require(!buildPoseGlobals(skeleton, locals, globals, error), "unrepresentable scale accepted");
        locals = base;
        locals[0].translation.x = std::numeric_limits<float>::quiet_NaN();
        require(!buildPoseGlobals(skeleton, locals, globals, error), "NaN accepted");
        locals = base;
        skeleton.bones[1].parentIndex = 6;
        require(!buildPoseGlobals(skeleton, locals, globals, error), "unordered/cyclic parent accepted");
    }

    QByteArray read(const QString& path)
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read %1").arg(path));
        return file.readAll();
    }

    void fixture(const QStringList& args)
    {
        QString error;
        const HkxSkeleton skeleton = HkxReader::readSkeleton(read(args.at(1)), error);
        require(skeleton.valid(), error);
        const HkxClip idle = HkxReader::readClip(read(args.at(2)), error);
        require(idle.valid(), error);
        QVector<Transformf> tracks, base, baseGlobal, local, global;
        if (!samplePose(skeleton, idle, 0, nullptr, tracks, base, baseGlobal, error)) {
            require(false, QStringLiteral("Idle %1: %2").arg(args.at(2), error));
        }
        const int weapon = skeleton.boneIndex(QStringLiteral("Weapon"));
        require(weapon >= 0, "Weapon absent");
        for (int arg = 3; arg < args.size(); ++arg) {
            const HkxClip clip = HkxReader::readClip(read(args.at(arg)), error);
            require(clip.valid(), error);
            for (int frame = 0; frame <= 100; ++frame) {
                const float time = clip.durationSeconds * float(frame) / 100;
                if (!samplePose(skeleton, clip, time, &base, tracks, local, global, error)) {
                    require(false, QStringLiteral("Action %1 t=%2: %3").arg(args.at(arg)).arg(time).arg(error));
                }
                checkReframe(skeleton, weapon, local, global);
            }
            // Reverse seek and switching bases must not retain prior selection state.
            QVector<Transformf> againLocal, againGlobal;
            require(samplePose(skeleton, clip, 0, &base, tracks, againLocal, againGlobal, error), error);
            require(samplePose(skeleton, clip, 0, &base, tracks, local, global, error), error);
            for (int bone = 0; bone < local.size(); ++bone) { expect(local.at(bone), matrix(againLocal.at(bone))); }
            QTextStream(stdout) << "PASS 101 frames + reverse seek: " << args.at(arg) << '\n';
        }
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        synthetic();
        if (app.arguments().size() > 1) {
            require(app.arguments().size() >= 4, "usage: PoseTests skeleton.hkx idle.hkx action.hkx ...");
            fixture(app.arguments());
        }
        QTextStream(stdout) << "PASS pose frame, articulation, explicit base, hierarchy and invalid input checks\n";
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << "FAIL " << error.what() << '\n';
        return 1;
    }
}
