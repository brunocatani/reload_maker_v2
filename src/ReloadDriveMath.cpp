#include "ReloadDriveMath.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace reloadmaker {
    float reloadCurveTime(const QVector<QPointF>& points,const QVector<float>& times,QPointF cursor,float previous,float endpointSnapFraction){
        if(points.size()!=times.size()||points.size()<2)return previous;
        double nearest=std::numeric_limits<double>::infinity();float answer=previous;
        for(int i=1;i<points.size();++i){
            const auto a=points[i-1],delta=points[i]-a;
            if(!std::isfinite(a.x())||!std::isfinite(a.y())||!std::isfinite(delta.x())||!std::isfinite(delta.y()))continue;
            const double length=delta.x()*delta.x()+delta.y()*delta.y();if(length<.0001)continue;
            const auto offset=cursor-a;const double fraction=std::clamp((offset.x()*delta.x()+offset.y()*delta.y())/length,0.,1.);
            const auto diff=cursor-(a+delta*fraction);const double distance=diff.x()*diff.x()+diff.y()*diff.y();
            const float time=times[i-1]+float(fraction)*(times[i]-times[i-1]);
            if(distance<nearest-1. || (std::abs(distance-nearest)<=1.&&std::abs(time-previous)<std::abs(answer-previous))){nearest=distance;answer=time;}
        }
        if(endpointSnapFraction>0&&endpointSnapFraction<.1f){
            const auto [lo,hi]=std::minmax_element(times.cbegin(),times.cend());const float tolerance=(*hi-*lo)*endpointSnapFraction;
            if(answer<=*lo+tolerance)answer=*lo;else if(answer>=*hi-tolerance)answer=*hi;
        }
        return answer;
    }
}
