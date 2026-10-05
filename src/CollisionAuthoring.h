#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QVector3D>
#include <QMatrix4x4>
#include <QVector>
#include <array>
#include <atomic>
#include <memory>

namespace reloadmaker {
struct CollisionMesh {
    QString key,name,model,record,path,material;
    bool worldIncluded{true};
    QMatrix4x4 frame;
    QJsonObject render; // Populated only by an explicit prepared-model capture.
    QVector<QVector3D> vertices;
    QVector<std::array<quint32,3>> triangles;
};
struct CollisionHull {
    QString source,name;
    QVector<QVector3D> points;
    QVector<std::array<quint32,3>> faces;
    QVector<std::array<quint32,2>> edges;
    bool enabled{true};
};
struct CollisionSettings {
    double gap{1},dedup{.14},error{.5},radius{.01},scale{70},mass{2};
    int targetPoints{96};
    quint32 material{346811165},layer{4};
};
struct CollisionProfile {
    QString weapon,configuration,target,origin,sourceHash;
    QJsonObject referencePose;
    QVector<CollisionMesh> sources;
    QVector<CollisionHull> hulls;
    QStringList warnings;
    CollisionSettings settings;
    qsizetype inputTriangles{};
    bool valid()const{return !sources.isEmpty()&&!hulls.isEmpty();}
};
struct CollisionResult {CollisionProfile profile;QString error;};
bool collisionSettingsValid(const CollisionSettings& settings,QString& error);
bool collisionHullFaces(CollisionHull& hull,QString& error);
CollisionResult bakeCollision(CollisionProfile input,const std::shared_ptr<std::atomic<bool>>& cancelled={});
QJsonObject collisionJson(const CollisionProfile& profile);
QJsonObject collisionCookInput(const CollisionProfile& profile,QString& error);
bool readCollisionJson(const QJsonObject& json,CollisionProfile& profile,QString& error);
QJsonObject collisionSummary(const CollisionProfile& profile);
}
