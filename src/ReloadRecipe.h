#pragma once
#include "HandPoseLibrary.h"
#include <QVector>

namespace reloadmaker {
    struct ReloadTrack {
        HandPosePlacement anchor;
        QString label;
        int parent{-1}; // Another captured track, not an engine/scene index.
        bool surface{false};
        QVector<Transformf> poses; // Frame-aligned, complete Weapon-local transforms, including scale.
    };
    // The learned base reload: every animated weapon part's Weapon-local pose
    // at every captured frame.
    struct ReloadRecipe {
        QString id; // Digest of source identity AND captured part trajectories.
        QJsonObject source;
        float duration{0};
        int frames{0};
        QVector<ReloadTrack> tracks;
    };
    // Frames first to last, inclusive, over which one part moves.
    struct ReloadSpan {
        int first{0};
        int last{0};
    };
    inline constexpr int maximumReloadFrames=8193;
    inline constexpr int maximumReloadTracks=256;
    [[nodiscard]] float reloadPoseDistance(const Transformf& a,const Transformf& b);
    [[nodiscard]] bool reloadPoseVisible(const Transformf& pose);
    [[nodiscard]] Transformf reloadTrackPose(const ReloadRecipe& recipe,int track,float seconds);
    // Checks a finished capture and gives it its identity.
    [[nodiscard]] bool identifyReloadRecipe(ReloadRecipe& recipe,QString& error);
    // When one part moves during the reload, as spans between its rests.
    [[nodiscard]] QVector<ReloadSpan> reloadMotionSpans(const ReloadRecipe& recipe,int track);
    [[nodiscard]] QByteArray encodeReloadRecipe(const ReloadRecipe& recipe,QString& error);
    [[nodiscard]] bool decodeReloadRecipe(const QByteArray& bytes,ReloadRecipe& recipe,QString& error);
    [[nodiscard]] QString reloadRecipeIdentity(const ReloadRecipe& recipe);
}
