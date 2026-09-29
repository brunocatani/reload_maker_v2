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
    enum class ReloadRelation { Independent, Rigid, Timed };
    struct ReloadMember {
        int track{-1};
        ReloadRelation relation{ReloadRelation::Independent};
        bool grabbable{false};
        QString gripId;
    };
    struct ReloadWindow {
        QString label;
        int first{0},last{0};
        QVector<ReloadMember> members;
        bool returnOnRelease{false}; // Explicit author decision; animation cannot establish a spring.
    };
    struct ReloadRecipe {
        QString id; // Digest of source identity AND captured part trajectories.
        QJsonObject source;
        int revision{0};
        float duration{0};
        int frames{0};
        QVector<ReloadTrack> tracks;
        QVector<ReloadWindow> windows;
    };
    inline constexpr int maximumReloadFrames=8193;
    inline constexpr int maximumReloadTracks=256;
    [[nodiscard]] float reloadPoseDistance(const Transformf& a,const Transformf& b);
    [[nodiscard]] bool reloadPoseVisible(const Transformf& pose);
    [[nodiscard]] Transformf reloadTrackPose(const ReloadRecipe& recipe,int track,float seconds);
    [[nodiscard]] bool analyzeReloadWindows(ReloadRecipe& recipe,QString& error);
    [[nodiscard]] QVector<ReloadMember> reloadMembers(const ReloadRecipe& recipe,int first,int last);
    [[nodiscard]] bool splitReloadWindow(ReloadRecipe& recipe,int window,int frame,QString& error);
    [[nodiscard]] bool mergeReloadWindow(ReloadRecipe& recipe,int window,QString& error);
    [[nodiscard]] QByteArray encodeReloadRecipe(const ReloadRecipe& recipe,QString& error);
    [[nodiscard]] bool decodeReloadRecipe(const QByteArray& bytes,ReloadRecipe& recipe,QString& error);
    [[nodiscard]] QString reloadRecipeIdentity(const ReloadRecipe& recipe);
    class ReloadRecipeLibrary final {
    public:
        explicit ReloadRecipeLibrary(QString directory=defaultDirectory());
        [[nodiscard]] static QString defaultDirectory();
        [[nodiscard]] QString filePath(const QString& id)const;
        [[nodiscard]] bool load(const QString& id,ReloadRecipe& recipe,QString& error)const;
        [[nodiscard]] bool save(ReloadRecipe& recipe,QString& error)const;
    private:
        QString _directory;
    };
}
