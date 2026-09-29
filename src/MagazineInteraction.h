#pragma once
#include "ReloadRecipe.h"
#include <QVector3D>

namespace reloadmaker {
    struct MagazineRail {
        QString connectionName;
        Transformf connection;
        int fullTrack{-1}, emptyTrack{-1};
        bool sharedBody()const{return fullTrack==emptyTrack;}
        QVector3D outward, handle;
        float travel{0}, captureRadius{0}, releaseMargin{0};
        float magazineMinProjection{0}, weaponMaxProjection{0}, clearance{0};
    };
    enum class MagazinePhase { Seated, Absent, Outgoing, OutsideEmpty, Incoming, Free, Rail };
    struct MagazineMotion {
        MagazinePhase phase{MagazinePhase::Seated};
        QVector3D offset;
        float distance{0}, elapsed{0};
        bool seated()const{return phase==MagazinePhase::Seated;}
        bool visible()const{return phase!=MagazinePhase::Absent;}
        bool empty()const{return phase==MagazinePhase::Outgoing||phase==MagazinePhase::OutsideEmpty;}
        bool animated()const{return phase==MagazinePhase::Outgoing||phase==MagazinePhase::Incoming;}
        bool carried()const{return phase==MagazinePhase::Free||phase==MagazinePhase::Rail;}
        bool busy()const{return animated()||carried();}
        bool animateOut();
        bool animateIn(const MagazineRail& rail);
        bool grab(const MagazineRail& rail);
        void drop();
        void cancel();
        void reset();
        void moveFree(const MagazineRail& rail,const QVector3D& newOffset);
        void moveRail(const MagazineRail& rail,float newDistance);
        void tick(const MagazineRail& rail,float delta);
    };
    bool buildMagazineRail(const ReloadRecipe& recipe,int magazineTrack,const QString& connectionName,
        const Transformf& connection,const QVector<QVector3D>& magazineVertices,
        const QVector<QVector3D>& weaponVertices,MagazineRail& rail,QString& error);
    QString magazinePhaseName(MagazinePhase phase);
    QJsonObject magazineRailJson(const MagazineRail& rail);
}
