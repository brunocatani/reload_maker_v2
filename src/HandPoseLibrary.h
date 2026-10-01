#pragma once

#include "PoseEvaluator.h"
#include <QJsonObject>
#include <array>

namespace reloadmaker {
    enum class PoseHand { Left, Right };

    struct HandPosePlacement {
        QString anchorKind; // weapon, installed-part, animated-part
        QString anchorName;
        QString anchorPath; // descriptive node-name path, never an executable path
        QString ownerModel;
        QString ownerRecord;
        Transformf handInAnchor{};
        Transformf handInWeapon{};
    };

    struct HandPose {
        QString id;
        QString name;
        QString notes;
        QString createdUtc;
        PoseHand hand{PoseHand::Right};
        // Native parent-local joints, thumb through pinky, proximal to distal.
        // Matches ROCK's exact 15-transform route, not its lossy curl controls.
        std::array<Transformf, 15> fingers{};
        HandPosePlacement placement;
        QJsonObject source; // provenance only; never required to reuse the fingers
    };

    [[nodiscard]] QString handPoseBoneName(PoseHand hand, int fingerIndex);
    [[nodiscard]] bool captureHandPose(const HkxSkeleton& skeleton,
        const QVector<Transformf>& locals, const QVector<Transformf>& globals,
        const Transformf& anchorWorld, HandPose& pose, QString& error);
    [[nodiscard]] bool applyHandPoseFingers(const HkxSkeleton& skeleton,
        const HandPose& pose, QVector<Transformf>& locals, QString& error);
    // Reflect spatial contact across the weapon's X plane while converting
    // every joint through the actual opposite-hand reference basis.
    [[nodiscard]] bool mirrorHandPose(const HkxSkeleton& skeleton,const HandPose& source,
        const Transformf& anchorInWeapon,HandPose& target,QString& error);
    // Convert only the finger chain through the target hand's skeleton bases.
    // Saved wrist placement is deliberately excluded from this operation.
    [[nodiscard]] bool handPoseFingersFor(const HkxSkeleton& skeleton,const HandPose& source,
        PoseHand hand,HandPose& target,QString& error);

    [[nodiscard]] QByteArray encodeHandPose(const HandPose& pose, QString& error);
    [[nodiscard]] bool encodePoseTransform(const Transformf& value,QJsonObject& json,QString& error);
    [[nodiscard]] bool decodePoseTransform(const QJsonValue& json,Transformf& value,QString& error);
    [[nodiscard]] bool decodeHandPose(const QByteArray& bytes, HandPose& pose, QString& error);

    class HandPoseLibrary final {
    public:
        explicit HandPoseLibrary(QString directory);
        [[nodiscard]] static QString defaultDirectory();
        [[nodiscard]] const QString& directory() const { return _directory; }
        [[nodiscard]] QVector<HandPose> load(QStringList& diagnostics) const;
        // Append-only snapshots: a name is a label, never a filename or an
        // overwrite key. Existing UUID files are never replaced by capture.
        [[nodiscard]] bool save(const HandPose& pose, QString& error) const;
    private:
        QString _directory;
    };
}
