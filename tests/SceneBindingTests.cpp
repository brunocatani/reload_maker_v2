#include "ScenePoseBinding.h"
#include "AttachmentConnections.h"

#include <QCoreApplication>
#include <QFile>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QTextStream>

#include <cmath>
#include <functional>
#include <stdexcept>

using namespace reloadmaker;

namespace
{
    void require(bool condition, const QString& message)
    {
        if (!condition) { throw std::runtime_error(message.toStdString()); }
    }

    Transformf transform(Vec3f t, float pitch = 0, float yaw = 0, float roll = 0)
    {
        const auto q = QQuaternion::fromEulerAngles(pitch, yaw, roll);
        return {t, {q.x(), q.y(), q.z(), q.scalar()}, {1, 1, 1}};
    }

    QMatrix4x4 matrix(const Transformf& value)
    {
        QMatrix4x4 result;
        result.translate(value.translation.x, value.translation.y, value.translation.z);
        result.rotate(QQuaternion(value.rotation.w, value.rotation.x, value.rotation.y, value.rotation.z));
        result.scale(value.scale.x, value.scale.y, value.scale.z);
        return result;
    }

    void expect(const QMatrix4x4& actual, const QMatrix4x4& expected)
    {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                require(std::isfinite(actual(row, col)) && std::abs(actual(row, col) - expected(row, col)) < 0.002f,
                        QStringLiteral("Scene matrix mismatch %1,%2: %3 vs %4")
                            .arg(row).arg(col).arg(actual(row, col)).arg(expected(row, col)));
            }
        }
    }

    void connectionOwnershipCases()
    {
        // FAL fields inspected with nif_mcp: the handguard and barrel reuse
        // this name, but P-aBarrel has an empty parent in the handguard only.
        const QString sharedName = QStringLiteral("BarrelWeldedShortLight");
        AttachmentModelScope receiver{10, {{10, "WEAPON"}, {11, "HandmadeARReceiver"}, {12, "WeaponMagazine"}},
            {{"P-Barrel", "HandmadeARReceiver", transform({0, 25, 5})}, {"P-Mag", "WeaponMagazine", {}}}};
        AttachmentModelScope handguard{20, {{20, sharedName}}, {{"P-aBarrel", "", {}}}};
        AttachmentModelScope barrel{30, {{30, sharedName}}, {{"P-Muzzle", sharedName, transform({0, -10, -5})}}};
        AttachmentConnections connections;
        // Reverse insertion order must not change the owner of a named anchor.
        connections.add(barrel);
        connections.add(receiver);
        connections.add(handguard);
        QString error;
        const auto aBarrel = connections.find("p-abarrel", {20, 10}, error);
        require(aBarrel.valid() && error.isEmpty(), "FAL barrel connection missing");
        require(connections.ownerRoot(aBarrel) == 20, "connection forgot its supplying model");
        require(connections.ownerRoot(connections.find("P-aBarrel", {10}, error)) == 20,
                "unique explicit connection without matching slot ancestry was lost");
        require(connections.anchor(aBarrel, 1, error) == 20, "empty parent escaped to global Weapon");
        const auto muzzle = connections.find("P-Muzzle", {30, 20, 10}, error);
        require(connections.anchor(muzzle, 20, error) == 30, "barrel muzzle used handguard's duplicate name");
        const auto mag = connections.find("P-Mag", {10}, error);
        require(connections.anchor(mag, 99, error) == 12, "local receiver magazine replaced by global reference");
        // A real rig reference outside the owner is allowed only when supplied
        // explicitly by the adapter. Ordinary names from other models are not.
        connections.add({40, {{40, "Mount"}}, {{"P-External", "WeaponExtra1", {}}}});
        const auto external = connections.find("P-External", {40}, error);
        require(connections.anchor(external, -1, error) == -1 && !error.isEmpty(), "missing named parent was guessed");
        require(connections.anchor(external, 99, error) == 99 && error.isEmpty(), "explicit rig anchor rejected");
        for (const auto parentPose : {transform({0,25,5}), transform({3,12,5}, 20,-35,10)}) {
            const auto barrelRoot = composed(parentPose, connections.point(aBarrel)->local);
            const auto muzzleRoot = composed(barrelRoot, connections.point(muzzle)->local);
            const auto meshLocal = transform({0,-35,-5}); // authored FAL short-barrel mesh offset
            expect(matrix(barrelRoot), matrix(parentPose));
            expect(matrix(composed(barrelRoot, meshLocal)), matrix(parentPose) * matrix(meshLocal));
            expect(matrix(muzzleRoot), matrix(parentPose) * matrix(connections.point(muzzle)->local));
        }
        connections.add({50, {{50, sharedName}}, {{"P-Muzzle", sharedName, {}}}});
        require(!connections.find("P-Muzzle", {}, error).valid() && !error.isEmpty(), "ambiguous point chose first import");
        require(connections.ownerRoot(connections.find("P-Muzzle", {30}, error)) == 30, "declared owner did not disambiguate point");
        connections.add({60, {{60,"Duplicate"}, {61,"Duplicate"}}, {{"P-Duplicate","Duplicate",{}}}});
        require(connections.anchor(connections.find("P-Duplicate", {60}, error), 99, error) == -1,
                "ambiguous local name fell through to rig anchor");
        connections.add({70, {{70,"Root"}}, {{"P-DuplicatePoint","",{}}, {"P-DuplicatePoint","",{}}}});
        require(!connections.find("P-DuplicatePoint", {70}, error).valid(), "ambiguous points within owner accepted");
        connections.clear();
        require(!connections.find("P-aBarrel", {20}, error).valid(), "old scene connection survived clear");
        require(connections.anchor({}, 99, error) == -1, "invalid match accepted");
    }

    void runCase(const Transformf& magazineRest, const Transformf& connection, bool auxiliaryUnderMagazine)
    {
        HkxSkeleton skeleton;
        const int parents[] = {-1, 0, 1, 2, 3, 2, 0};
        const QStringList names{"Root", "RightHand", "Weapon", "WeaponMagazine", "WeaponMagazineChild1", "WeaponExtra3", "LeftHand"};
        for (int i = 0; i < names.size(); ++i) {
            skeleton.bones.append({names.at(i), static_cast<std::int16_t>(parents[i]), false, {}});
        }
        // The generic reference is deliberately NOT the attachment rest.
        skeleton.bones[3].referenceLocal = transform({-5.72745f, 4.45585f, 2.99770f});
        const Transformf root = transform({30, -20, 50}, 20, -15, 80);
        const Transformf hand = transform({2, 6, 10}, -30, 15, -10);
        const Transformf weapon = transform({1, 2, 3}, 10, -5, 25);
        const Transformf helper = transform({-magazineRest.translation.x,
                                             -magazineRest.translation.y,
                                             -magazineRest.translation.z});
        // Generic assembly graph, not a generated NIF. Real fixtures' rest values
        // were independently inspected through nif_mcp. Node order is not bone order.
        QVector<ScenePoseNode> nodes{
            {-1, 0, root}, {0, 1, hand}, {1, 2, weapon},
            {2, -1, connection}, {3, 3, magazineRest}, {4, 4, {}},
            {5, -1, helper}, {6, -1, transform({0.38f, 20.07f, 0.525f})},
            {3, 5, transform({0, 0, 0})}, {0, 6, transform({-2, 8, 11})},
            // Another physical part driven by the same semantic track, in a
            // separate owner scope. It retains its own bind placement.
            {2, 3, transform({4, 3, 2}, 5, 12, 20)}
        };
        const QVector<int> canonical{0, 1, 2, 4, 5, 8, 9};
        if (auxiliaryUnderMagazine) { nodes[8].parent = 4; }
        QVector<ScenePoseBinding> bindings;
        QString error;
        require(compileSceneBindings(skeleton, 2, nodes, canonical, bindings, error), error);
        QVector<Transformf> state{root, hand, weapon, magazineRest, {}, {}, nodes.at(9).restLocal};
        QVector<Transformf> source = state;
        QVector<Transformf> logical, boundLocal, boundGlobal;
        const int samples[] = {0, 1, 2, 3, 4, 2, 0};
        for (int sample : samples) {
            source = state;
            if (sample > 0 && sample < 4) {
                source[3] = transform({-6.0f * sample, 2.0f * sample, -15.0f * sample}, 10.0f * sample, 35.0f * sample, -20);
                source[4] = transform({-3.16613f, -12.2554f, -11.1858f}, 12, 14, -6);
                source[5] = transform({1, -3, 5}, 20, 30, 40);
            }
            auto posedLocal = source;
            require(buildPoseGlobals(skeleton, posedLocal, logical, error), error);
            const Transformf sourceWeapon = logical.at(2);
            require(reframePose(skeleton, sourceWeapon, {}, posedLocal, logical, error), error);
            require(evaluateSceneBindings(skeleton, bindings, source, state, logical, boundLocal, boundGlobal, error), error);

            // Independent full-matrix reconstruction using the ACTUAL destination
            // parent graph and the produced locals, including untouched helpers.
            auto sceneLocals = nodes;
            for (int i = 0; i < bindings.size(); ++i) { sceneLocals[bindings.at(i).nodeIndex].restLocal = boundLocal.at(i); }
            const std::function<QMatrix4x4(int)> world = [&](int node) {
                return node < 0 ? QMatrix4x4{} : world(sceneLocals.at(node).parent) * matrix(sceneLocals.at(node).restLocal);
            };
            const auto deltaMag = matrix(source.at(3)) * matrix(state.at(3)).inverted();
            const auto magExpected = deltaMag * matrix(connection) * matrix(magazineRest);
            const auto shellExpected = magExpected * matrix(source.at(4)) * matrix(helper) * matrix(nodes.at(7).restLocal);
            expect(world(2), QMatrix4x4{});
            expect(world(4), magExpected);
            expect(world(7), shellExpected);
            expect(world(8), matrix(source.at(5)) * matrix(connection) *
                             (auxiliaryUnderMagazine ? matrix(magazineRest) : QMatrix4x4{}));
            expect(world(10), deltaMag * matrix(nodes.at(10).restLocal));
            expect(world(1), matrix(logical.at(1)));
            expect(world(9), matrix(logical.at(6)));
            for (int i = 0; i < bindings.size(); ++i) { expect(world(bindings.at(i).nodeIndex), matrix(boundGlobal.at(i))); }
        }
        // The former construction nests the attachment under an animated copy
        // of its own bone. Reject it rather than submitting motion twice.
        auto duplicate = nodes;
        duplicate.append({2, 3, skeleton.bones.at(3).referenceLocal});
        duplicate[3].parent = duplicate.size() - 1;
        require(!compileSceneBindings(skeleton, 2, duplicate, canonical, bindings, error), "duplicate animated ancestry accepted");
        auto cycle = nodes;
        cycle[3].parent = 7;
        require(!compileSceneBindings(skeleton, 2, cycle, canonical, bindings, error), "cyclic scene accepted");
        auto ambiguous = nodes;
        ambiguous[5].parent = 2;
        require(!compileSceneBindings(skeleton, 2, ambiguous, canonical, bindings, error), "ambiguous cross-attachment parent accepted");
    }

    void sharedAlignmentCases()
    {
        HkxSkeleton skeleton;
        // Auxiliary precedes the connection bone in HKX; alignment is not a
        // dependency on that bone's ANIMATED output or its evaluation order.
        skeleton.bones = {{"Root", -1, false, {}}, {"Weapon", 0, false, {}},
                          {"Auxiliary", 1, false, {}}, {"Connection", 1, false, {}},
                          {"Cartridge", 3, false, {}}};
        const auto mount = transform({3, -2, 4}, 15, 25, -35);
        const auto mainRest = transform({0.2f, 8.7f, 0.25f}, 5, 12, 25);
        const auto auxRest = transform({1, -4, 3}, 2, 6, 15);
        QVector<ScenePoseNode> nodes{{-1, 0, {}}, {0, 1, {}}, {1, -1, mount},
                                   {2, 2, auxRest, 4}, {2, 3, mainRest, 4}, {4, 4, {}},
                                   {1, 2, transform({2, 3, 4})}};
        const QVector<int> canonical{0, 1, 3, 4, 5};
        QVector<ScenePoseBinding> bindings;
        QString error;
        require(compileSceneBindings(skeleton, 1, nodes, canonical, bindings, error), error);
        QVector<Transformf> state{{}, {}, transform({-16, -24, -109}, 7, -13, 2), mainRest, {}};
        for (int sample : {0, 1, 2, 3, 1, 0}) {
            auto source = state;
            if (sample == 1) { source[2] = transform({-10, -1, -3}, 35, 90, -20); }
            if (sample == 2) { source[2] = {}; source[3] = transform({0, -51, -4}); }
            if (sample == 3) { source[2].scale = {0, 0, 0}; }
            source[4] = transform({2, -4, 1}, 20, -35, 6);
            QVector<Transformf> logical, local, global;
            require(buildPoseGlobals(skeleton, source, logical, error), error);
            require(evaluateSceneBindings(skeleton, bindings, source, state, logical, local, global, error), error);
            auto scene = nodes;
            for (int i = 0; i < bindings.size(); ++i) { scene[bindings[i].nodeIndex].restLocal = local[i]; }
            const std::function<QMatrix4x4(int)> world = [&](int node) {
                return node < 0 ? QMatrix4x4{} : world(scene[node].parent) * matrix(scene[node].restLocal);
            };
            for (int i = 0; i < bindings.size(); ++i) {
                const int node = bindings[i].nodeIndex;
                expect(world(node), matrix(global[i]));
                if (node == 3) {
                    expect(matrix(global[i]), matrix(source[2]) * matrix(state[3]).inverted() * matrix(mount) * matrix(auxRest));
                } else if (node == 4) {
                    expect(matrix(global[i]), matrix(source[3]) * matrix(state[3]).inverted() * matrix(mount) * matrix(mainRest));
                } else if (node == 5) {
                    expect(matrix(global[i]), matrix(source[3]) * matrix(state[3]).inverted() * matrix(mount) * matrix(mainRest) * matrix(source[4]));
                } else if (node == 6) {
                    // A receiver or second model with the same semantic name
                    // is NOT part of this attachment's calibration scope.
                    expect(matrix(global[i]), matrix(source[2]) * matrix(state[2]).inverted() * matrix(nodes[6].restLocal));
                }
            }
        }
        auto invalid = nodes;
        invalid[3].alignmentNode = 99;
        require(!compileSceneBindings(skeleton, 1, invalid, canonical, bindings, error), "missing alignment anchor accepted");
        invalid[3].alignmentNode = 2;
        require(!compileSceneBindings(skeleton, 1, invalid, canonical, bindings, error), "unbound alignment anchor accepted");
        invalid[3].alignmentNode = 5;
        require(!compileSceneBindings(skeleton, 1, invalid, canonical, bindings, error), "different-parent alignment anchor accepted");
    }

    QByteArray bytes(const QString& path)
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot open clip fixture: %1").arg(path));
        return file.readAll();
    }

    void actualClipCase(const HkxSkeleton& skeleton, const QString& idlePath, const QString& actionPath,
                        bool hk433, bool benelli)
    {
        QString error;
        const auto idle = HkxReader::readClip(bytes(idlePath), error);
        require(idle.valid(), error);
        const auto clip = HkxReader::readClip(bytes(actionPath), error);
        require(clip.valid(), error);
        const int weapon = skeleton.boneIndex("Weapon");
        const int mag = skeleton.boneIndex("WeaponMagazine");
        const int auxiliary = skeleton.boneIndex("WeaponExtra3");
        const int hand = skeleton.boneIndex("LArm_Hand");
        const int shell = skeleton.boneIndex("WeaponMagazineChild1");
        require(weapon >= 0 && mag >= 0 && auxiliary >= 0 && hand >= 0 && shell >= 0, "missing fixture rig bones");
        QVector<Transformf> tracks, state, stateGlobal;
        require(samplePose(skeleton, idle, 0, nullptr, tracks, state, stateGlobal, error), error);
        // These destination node values were inspected through nif_mcp. This
        // test constructs an assembly graph, never reads/guesses NIF bytes.
        const Transformf magRest = benelli ? transform({0.219768f, 8.697246f, 0.252462f}) : Transformf{};
        QVector<ScenePoseNode> nodes{{-1, weapon, {}}, {0, mag, magRest, 1},
                                   {0, auxiliary, {}, benelli ? -1 : 1},
                                   {1, shell, {}}, {-1, hand, {}}};
        QVector<int> canonical(skeleton.bones.size(), -1);
        canonical[weapon] = 0; canonical[mag] = 1; canonical[auxiliary] = 2;
        canonical[shell] = 3; canonical[hand] = 4;
        QVector<ScenePoseBinding> bindings;
        require(compileSceneBindings(skeleton, weapon, nodes, canonical, bindings, error), error);
        QVector<int> samples{0, 25, 50, 75, 100, 50, 0};
        for (int sample = 0; sample <= 100; ++sample) { samples.append(sample); }
        for (int sample : samples) {
            QVector<Transformf> source, sourceGlobal, local, global;
            require(samplePose(skeleton, clip, clip.durationSeconds * sample / 100.0f, &state,
                               tracks, source, sourceGlobal, error), error);
            auto fixedLocal = source;
            auto fixedGlobal = sourceGlobal;
            require(reframePose(skeleton, sourceGlobal[weapon], {}, fixedLocal, fixedGlobal, error), error);
            require(evaluateSceneBindings(skeleton, bindings, source, state, fixedGlobal, local, global, error), error);
            const QMatrix4x4 mainExpected = matrix(source[mag]) * matrix(state[mag]).inverted() * matrix(magRest);
            for (int i = 0; i < bindings.size(); ++i) {
                const int node = bindings[i].nodeIndex;
                if (node == 0) { expect(matrix(global[i]), QMatrix4x4{}); }
                if (node == 1) { expect(matrix(global[i]), mainExpected); }
                if (node == 2) {
                    // M4's anchor and auxiliary both have identity idle locals;
                    // the old correct output is identical. Benelli Extra3 is
                    // receiver-owned and retains its original self calibration.
                    const auto calibration = benelli ? state[auxiliary] : state[mag];
                    expect(matrix(global[i]), matrix(source[auxiliary]) * matrix(calibration).inverted());
                    if (hk433 && sample == 0) {
                        require(global[i].translation.z < -100.0f, "HK433 parked magazine pulled back into the weapon");
                    }
                    if (!hk433) {
                        expect(matrix(global[i]), matrix(source[auxiliary]) * matrix(state[auxiliary]).inverted());
                    }
                }
                if (node == 3) { expect(matrix(global[i]), mainExpected * matrix(source[shell]) * matrix(state[shell]).inverted()); }
                if (node == 4) { expect(matrix(global[i]), matrix(fixedGlobal[hand])); }
            }
        }
        QTextStream(stdout) << "PASS actual clip binding, 101 samples + reverse seek: " << actionPath << '\n';
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        connectionOwnershipCases();
        sharedAlignmentCases();
        for (const Transformf connection : {Transformf{}, transform({3, -2, 4}, 15, 25, -35)}) {
            for (bool auxiliaryUnderMagazine : {false, true}) {
                runCase({}, connection, auxiliaryUnderMagazine); // M4 standard/drum bind pivot.
                runCase(transform({0.219768f, 8.697246f, 0.252462f}), connection, auxiliaryUnderMagazine);
            }
            const Transformf root = transform({7, 3, -2}, 10, 35, -20);
            const Transformf anchor = transform({2, 8, 5}, 20, -15, 60);
            Transformf placement;
            require(suppliedAnchorPlacement(root, anchor, connection, placement), "invalid anchor placement");
            expect(matrix(placement) * matrix(root).inverted() * matrix(anchor), matrix(anchor) * matrix(connection));
        }
        if (app.arguments().size() > 1) {
            const auto args = app.arguments();
            require(args.size() == 10 && args[1] == "--clips",
                    "usage: SceneBindingTests --clips skeleton hk433-idle reload reserve m4-idle reload benelli-idle reload");
            QString error;
            const auto skeleton = HkxReader::readSkeleton(bytes(args[2]), error);
            require(skeleton.valid(), error);
            actualClipCase(skeleton, args[3], args[3], true, false);
            actualClipCase(skeleton, args[3], args[4], true, false);
            actualClipCase(skeleton, args[3], args[5], true, false);
            actualClipCase(skeleton, args[6], args[6], false, false);
            actualClipCase(skeleton, args[6], args[7], false, false);
            actualClipCase(skeleton, args[8], args[8], false, true);
            actualClipCase(skeleton, args[8], args[9], false, true);
        }
        QTextStream(stdout) << "PASS real-node rest, nested shell articulation, shared attachment alignment, parked/zero-scale auxiliary, rotated frames, independent owners, reverse seek, unchanged hands and invalid ancestry\n";
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << "FAIL " << error.what() << '\n';
        return 1;
    }
}
