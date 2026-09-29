#pragma once
#include "PartGrip.h"
#include <QMatrix4x4>
#include <QPointF>
#include <QSizeF>
#include <QVector3D>

namespace reloadmaker {
    struct GripRay { QVector3D origin, direction; };
    [[nodiscard]] Vec3f genericPalmPoint(const HandPose& pose);
    [[nodiscard]] bool projectGripPoint(const QMatrix4x4& worldToClip, const QSizeF& size,
                                        const QVector3D& world, QPointF& screen, float* depth=nullptr);
    [[nodiscard]] bool unprojectGripPoint(const QMatrix4x4& worldToClip, const QSizeF& size,
                                          const QPointF& screen, float depth, QVector3D& world);
    [[nodiscard]] bool gripRay(const QMatrix4x4& worldToClip, const QSizeF& size,
                              const QPointF& screen, GripRay& ray);
    [[nodiscard]] bool gripAxisParameter(const GripRay& ray,const QVector3D& pivot,const QVector3D& axis,float& parameter);
    [[nodiscard]] bool gripPlanePoint(const GripRay& ray,const QVector3D& pivot,const QVector3D& normal,QVector3D& point);
    [[nodiscard]] Transformf placePalmAt(const Transformf& hand,const Vec3f& palmInHand,const Vec3f& target);
    [[nodiscard]] Transformf rotateGripAbout(const Transformf& hand,const Vec3f& pivot,const Vec3f& axis,float degrees);
}
