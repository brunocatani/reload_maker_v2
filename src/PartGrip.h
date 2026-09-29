#pragma once
#include "HandPoseLibrary.h"
#include <optional>

namespace reloadmaker {
    struct PartGrip {
        HandPose pose; // Frozen fingers and editable placement; independent of source pose file.
        QString weaponRecord;
        QString sourcePoseId;
        QString mechanismId{}; // Stable shared part definition; no per-hand copy of movement rules.
        struct SurfaceContact {
            Vec3f pointInAnchor{}; // Original clicked surface point; fine-tuning does not overwrite it.
            Vec3f palmInHand{}; // Generic initial contact used as the manipulation pivot.
            QString surfaceName;
        };
        std::optional<SurfaceContact> surfaceContact{};
    };
    [[nodiscard]] QByteArray encodePartGrip(const PartGrip& grip, QString& error);
    [[nodiscard]] bool decodePartGrip(const QByteArray& bytes, PartGrip& grip, QString& error);
    [[nodiscard]] bool sameGripAnchor(const HandPosePlacement& a, const HandPosePlacement& b);
    // Reject both missing and ambiguous parts. Never substitute a same-named bone
    // owned by another model, nor a different weapon after load-order changes.
    [[nodiscard]] int resolveGripAnchor(const PartGrip& grip, const QString& weaponRecord,
                                        const QVector<HandPosePlacement>& anchors, QString& error);
    class PartGripLibrary final {
    public:
        explicit PartGripLibrary(QString directory);
        [[nodiscard]] static QString defaultDirectory();
        [[nodiscard]] QVector<PartGrip> load(QStringList& diagnostics) const;
        [[nodiscard]] bool save(const PartGrip& grip, QString& error) const;
    private:
        QString _directory;
    };
    // Presentation only: world-space arm solve followed by reconstruction of
    // locals against the actual target globals. Non-arm globals (especially the
    // Weapon child of RArm_Hand) stay exactly where native evaluation put them.
    // Unreachable/invalid targets fail without changing either output.
    [[nodiscard]] bool solvePartGrip(const HkxSkeleton& skeleton,
        const QVector<Transformf>& nativeLocal, const QVector<Transformf>& framedGlobal,
        const HandPose& pose, const Transformf& handWorld,
        QVector<Transformf>& solvedLocal, QVector<Transformf>& solvedGlobal, QString& error);
}
