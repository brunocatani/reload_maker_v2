#pragma once
#include "HandPoseLibrary.h"

namespace reloadmaker {
    enum class ReleaseRule { Hold, StageStart, Rest };
    struct MechanismStage {
        QString forwardName{"Move"}, reverseName{"Return"};
        bool rotation{false};
        int axis{1};
        float from{0}, to{1};
        Vec3f pivot{}; // In the part-local frame at this stage's start, NOT the grip point.
        ReleaseRule release{ReleaseRule::Hold};
        float returnRate{10}; // Game units/s or degrees/s; kinematic authoring rate, not spring stiffness.
    };
    struct MechanismFollower {
        HandPosePlacement anchor;
        Transformf inPartAtRest{};
    };
    struct PartMechanism {
        QString id, name, weaponRecord;
        int revision{0};
        HandPosePlacement anchor;
        Transformf partInWeaponAtRest{};
        QVector<MechanismFollower> followers;
        QVector<MechanismStage> stages; // Ordered forward path; reverse traverses the same path backwards.
    };
    struct MechanismState {
        float path{0}; // Stage index + normalized progress within that stage.
        bool returning{false};
        float returnTarget{0};
        int activeStage{0}; // The stage last driven; endpoint release uses its policy.
    };
    [[nodiscard]] bool validateMechanism(const PartMechanism& part,QString& error);
    // Installed/animated labels may describe the same owner-qualified scene frame.
    [[nodiscard]] bool sameMechanismPart(const HandPosePlacement& a,const HandPosePlacement& b);
    [[nodiscard]] QByteArray encodeMechanism(const PartMechanism& part,QString& error);
    [[nodiscard]] bool decodeMechanism(const QByteArray& bytes,PartMechanism& part,QString& error);
    [[nodiscard]] Transformf mechanismDelta(const PartMechanism& part,float path);
    [[nodiscard]] MechanismStage mechanismStageFromDrag(const MechanismStage& initial,int axis,bool rotation,float delta);
    void releaseMechanism(const PartMechanism& part,MechanismState& state);
    void advanceMechanism(const PartMechanism& part,MechanismState& state,float seconds);
    class MechanismLibrary final {
    public:
        explicit MechanismLibrary(QString directory);
        [[nodiscard]] static QString defaultDirectory();
        [[nodiscard]] QVector<PartMechanism> load(QStringList& diagnostics) const;
        // Stable IDs/revisions: all grips for a part share edits. Atomic optimistic
        // update; a stale editor or corrupt existing file cannot overwrite it.
        [[nodiscard]] bool save(PartMechanism& part,QString& error) const;
    private:
        QString _directory;
    };
}
