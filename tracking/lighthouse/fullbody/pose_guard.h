#pragma once
#include "../calibration/motoc_fit.h"
namespace vrcm_tracking {
using vrcm_calibration::Pose;
using vrcm_calibration::angle;
enum class Outcome { Ignored, Accepted, Rejected };
// Reject transient solver excursions; accepted poses are never averaged/delayed.
// After a genuine tracking discontinuity require a coherent 100ms new segment.
class PoseGuard {
    Pose good_=Pose::Identity(),pending_=Pose::Identity();
    Eigen::Vector3d velocity_=Eigen::Vector3d::Zero();
    int64_t goodTime_=0,lastTime_=0,pendingTime_=0,pendingSince_=0;
    unsigned pendingCount_=0;
    static bool plausible(const Pose& previous,const Pose& next,double dt,
                          const Eigen::Vector3d& velocity,bool predictive) {
        if(dt<=0 || dt>.15)return false;
        double distance=(next.translation()-previous.translation()).norm();
        if(distance>.04+12*dt)return false;
        if(angle(previous.rotation().transpose()*next.rotation())>.35+70*dt)return false;
        return !predictive ||
            (next.translation()-previous.translation()-velocity*dt).norm()<.04+75*dt*dt;
    }
public:
    Outcome Observe(int64_t time,const Pose& pose) {
        if(time<=lastTime_ || !vrcm_calibration::valid(pose))return Outcome::Ignored;
        lastTime_=time;
        if(!goodTime_) {good_=pose;goodTime_=time;return Outcome::Accepted;}
        double dt=double(time-goodTime_)/1e9;
        if(plausible(good_,pose,dt,velocity_,true)) {
            auto measured=(pose.translation()-good_.translation())/dt;
            double alpha=dt/(.02+dt);
            velocity_=(1-alpha)*velocity_+alpha*measured;
            good_=pose;goodTime_=time;pendingCount_=0;
            return Outcome::Accepted;
        }
        double pendingDt=double(time-pendingTime_)/1e9;
        if(!pendingCount_ || !plausible(pending_,pose,pendingDt,Eigen::Vector3d::Zero(),false)) {
            pendingSince_=time;pendingCount_=1;
        } else ++pendingCount_;
        pending_=pose;pendingTime_=time;
        if(pendingCount_>=5 && time-pendingSince_>=100000000) {
            good_=pose;goodTime_=time;velocity_.setZero();pendingCount_=0;
            return Outcome::Accepted;
        }
        return Outcome::Rejected;
    }
};
}
