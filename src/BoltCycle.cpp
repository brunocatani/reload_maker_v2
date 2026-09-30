#include "BoltCycle.h"
#include <algorithm>
#include <cmath>

namespace reloadmaker {
bool analyzeBoltCycle(BoltCycle& c,float ejectSeconds,QString& error) {
    error.clear();
    if(c.poses.isEmpty()||c.tracks.size()!=c.poses.size()||c.duration<=0||c.poses.front().size()<3){error="Incomplete native bolt cycle.";return false;}
    const int frames=c.poses.front().size();
    for(const auto& track:c.poses)if(track.size()!=frames){error="Bolt follower sample counts differ.";return false;}
    QVector<float> distance;float travel=0;
    for(int f=0;f<frames;++f) {
        float d=0;
        for(const auto& track:c.poses) {
            if(!reloadPoseVisible(track[f])){error="Bolt part disappears during its cycle.";return false;}
            d=std::max(d,reloadPoseDistance(track.front(),track[f]));
        }
        distance.append(d);travel=std::max(travel,d);
    }
    if(travel<.15f){error="Native bolt cycle has no measurable stroke.";return false;}
    c.first=0;
    for(int f=1;f<frames;++f)if(distance[f]>travel*.002f){c.first=f-1;break;}
    c.rear=int(std::max_element(distance.cbegin(),distance.cend())-distance.cbegin());
    c.last=-1;
    // Cocking changes the firing pin's rest pose, so the completed cycle need
    // not return every follower to its starting transform.
    for(int f=c.rear+1;f<frames;++f) {
        float d=0;for(const auto& track:c.poses)d=std::max(d,reloadPoseDistance(track.back(),track[f]));
        if(d<=travel*.002f){c.last=f;break;}
    }
    const float ejectFrame=ejectSeconds/c.duration*(frames-1);
    if(c.rear<=c.first||c.last<=c.rear||!std::isfinite(ejectFrame)||ejectFrame<c.first||ejectFrame>c.rear){error=QString("Incomplete bolt cycle: opening %1, rear %2, locking %3, ejection %4 (of %5 frames).").arg(c.first).arg(c.rear).arg(c.last).arg(ejectFrame).arg(frames);return false;}
    const auto lengths=[&](int first,int last) {
        QVector<float> result{0};float total=0;
        for(int f=first+1;f<=last;++f) {
            float step=0;for(const auto& track:c.poses)step=std::max(step,reloadPoseDistance(track[f-1],track[f]));
            total+=step;result.append(total);
        }
        for(float& p:result)p/=total;
        return result;
    };
    c.opening=lengths(c.first,c.rear);c.closing=lengths(c.rear,c.last);
    const float local=ejectFrame-c.first;const int a=int(local),b=std::min(a+1,int(c.opening.size()-1));
    c.ejectProgress=c.opening[a]+(c.opening[b]-c.opening[a])*(local-a);
    const auto aPoint=c.poses.front()[c.first].translation,bPoint=c.poses.front()[c.rear].translation;
    const Vec3f delta{bPoint.x-aPoint.x,bPoint.y-aPoint.y,bPoint.z-aPoint.z};
    c.inputTravel=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    if(c.inputTravel<.15f){error="Bolt cycle has no rearward input axis.";return false;}
    c.inputAxis={delta.x/c.inputTravel,delta.y/c.inputTravel,delta.z/c.inputTravel};return true;
}
Transformf boltCyclePose(const BoltCycle& c,int track,float progress,bool closing) {
    const int index=c.tracks.indexOf(track);if(index<0)return {};
    const auto& curve=closing?c.closing:c.opening;
    const float target=closing?1-std::clamp(progress,0.f,1.f):std::clamp(progress,0.f,1.f);
    const int last=std::min(int(std::upper_bound(curve.cbegin(),curve.cend(),target)-curve.cbegin()),int(curve.size()-1));
    const int first=std::max(0,last-1),start=closing?c.rear:c.first;
    const float span=curve[last]-curve[first];
    return interpolatedTransform(c.poses[index][start+first],c.poses[index][start+last],span>0?(target-curve[first])/span:0);
}
}
