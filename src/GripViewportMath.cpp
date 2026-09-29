#include "GripViewportMath.h"
#include <QQuaternion>
#include <cmath>

namespace reloadmaker {
    namespace {
        bool finite(const QVector3D& v) { return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()); }
    }
    Vec3f genericPalmPoint(const HandPose& pose) {
        // Halfway from the wrist to the mean of the four non-thumb knuckle roots.
        // This is a generic skeletal palm center, not inferred surface contact.
        Vec3f p;
        for(int finger:{3,6,9,12}) {
            p.x+=pose.fingers[finger].translation.x*.125f;
            p.y+=pose.fingers[finger].translation.y*.125f;
            p.z+=pose.fingers[finger].translation.z*.125f;
        }
        return p;
    }
    bool projectGripPoint(const QMatrix4x4& camera,const QSizeF& size,const QVector3D& world,QPointF& screen,float* depth) {
        const auto clip=camera*QVector4D(world,1);
        if(size.width()<=0 || size.height()<=0 || !finite(world) || !std::isfinite(clip.w()) || clip.w()<=.000001f) return false;
        const auto ndc=clip.toVector3DAffine();
        if(!finite(ndc) || ndc.z()< -1 || ndc.z()>1) return false;
        screen={(ndc.x()+1)*size.width()*.5,(1-ndc.y())*size.height()*.5};
        if(depth) *depth=(ndc.z()+1)*.5f;
        return true;
    }
    bool unprojectGripPoint(const QMatrix4x4& camera,const QSizeF& size,const QPointF& screen,float depth,QVector3D& world) {
        if(size.width()<=0 || size.height()<=0 || !std::isfinite(depth) || depth<0 || depth>1) return false;
        bool invertible=false; const auto inverse=camera.inverted(&invertible);
        if(!invertible) return false;
        const auto result=inverse*QVector4D(float(screen.x()/size.width()*2-1),float(1-screen.y()/size.height()*2),depth*2-1,1);
        if(!std::isfinite(result.w()) || std::abs(result.w())<.000001f) return false;
        const auto point=result.toVector3DAffine(); if(!finite(point)) return false;
        world=point; return true;
    }
    bool gripRay(const QMatrix4x4& camera,const QSizeF& size,const QPointF& screen,GripRay& ray) {
        QVector3D a,b;
        if(!unprojectGripPoint(camera,size,screen,0,a) || !unprojectGripPoint(camera,size,screen,1,b) || (b-a).lengthSquared()<1e-10f) return false;
        ray={a,(b-a).normalized()}; return true;
    }
    bool gripAxisParameter(const GripRay& ray,const QVector3D& pivot,const QVector3D& axis,float& parameter) {
        const auto relative=pivot-ray.origin;
        const float b=QVector3D::dotProduct(axis,ray.direction), denominator=1-b*b;
        if(denominator<.0001f) return false;
        const float value=(b*QVector3D::dotProduct(ray.direction,relative)-QVector3D::dotProduct(axis,relative))/denominator;
        if(!std::isfinite(value)) return false;
        parameter=value; return true;
    }
    bool gripPlanePoint(const GripRay& ray,const QVector3D& pivot,const QVector3D& normal,QVector3D& point) {
        const float denominator=QVector3D::dotProduct(ray.direction,normal);
        if(std::abs(denominator)<.0001f) return false;
        const float t=QVector3D::dotProduct(pivot-ray.origin,normal)/denominator;
        if(t<0 || !std::isfinite(t)) return false;
        point=ray.origin+ray.direction*t; return finite(point);
    }
    Transformf placePalmAt(const Transformf& hand,const Vec3f& palm,const Vec3f& target) {
        const auto offset=rotated(hand.rotation,{palm.x*hand.scale.x,palm.y*hand.scale.y,palm.z*hand.scale.z});
        auto result=hand; result.translation={target.x-offset.x,target.y-offset.y,target.z-offset.z}; return result;
    }
    Transformf rotateGripAbout(const Transformf& hand,const Vec3f& pivot,const Vec3f& axis,float degrees) {
        const auto q=QQuaternion::fromAxisAndAngle(QVector3D(axis.x,axis.y,axis.z).normalized(),degrees);
        const Quaternionf rotation{q.x(),q.y(),q.z(),q.scalar()};
        const auto offset=rotated(rotation,{hand.translation.x-pivot.x,hand.translation.y-pivot.y,hand.translation.z-pivot.z});
        auto result=hand; result.translation={pivot.x+offset.x,pivot.y+offset.y,pivot.z+offset.z};
        result.rotation=multiplied(rotation,hand.rotation); return result;
    }
}
