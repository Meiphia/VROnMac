#pragma once
#include "motoc_fit.h"
#include <cstdint>
#include <deque>
#include <map>
#include <string>
namespace vrcm_calibration {
struct TimedPose { int64_t time; Pose pose; };
class PoseHistory {
    std::deque<TimedPose> data_;
public:
    void clear() { data_.clear(); }
    const std::deque<TimedPose>& samples() const { return data_; }
    void add(int64_t time,const Pose& pose) {
        if(!valid(pose) || (!data_.empty() && time<=data_.back().time)) return;
        data_.push_back({time,pose});
        while(data_.size()>4000) data_.pop_front();
    }
    bool at(int64_t time,Pose& out) const {
        if(data_.size()<2 || time<data_.front().time || time>data_.back().time) return false;
        auto end=std::lower_bound(data_.begin(),data_.end(),time,[](const TimedPose& p,int64_t t){return p.time<t;});
        if(end==data_.end()) return false;
        if(end->time==time) {out=end->pose;return true;}
        if(end==data_.begin()) return false;
        auto begin=std::prev(end);
        if(end->time-begin->time>50000000) return false;
        double t=double(time-begin->time)/(end->time-begin->time);
        out=Pose::Identity();out.translation()=(1-t)*begin->pose.translation()+t*end->pose.translation();
        out.linear()=Eigen::Quaterniond(begin->pose.rotation()).slerp(t,Eigen::Quaterniond(end->pose.rotation())).toRotationMatrix();
        return true;
    }
};
struct TimedFit { Fit fit; int64_t lagNs; size_t rejectedSamples=0,matchedSamples=0; };
inline TimedFit fitTimedReference(const std::vector<TimedPose>& reference,const PoseHistory& quest) {
    require(reference.size()>=180 && reference.size()<=600,"Need a synchronized reference window");
    TimedFit best{};double bestScore=1e10;bool found=false;
    std::map<std::string,unsigned> rejected;
    double badScore=1e10;std::string badResidual;
    for(int ms=-60;ms<=60;ms+=10) {
        std::vector<Sample> samples;
        for(const auto& s:reference) {
            Pose q;
            if(quest.at(s.time+int64_t(ms)*1000000,q)) samples.push_back({q,s.pose});
        }
        if(samples.size()<180) {++rejected["Not enough time-matched headset samples"];continue;}
        try {
            auto robust=solveRobust(samples,180);Fit candidate=robust.fit;
            double score=candidate.positionRms+.1*candidate.angleRms;
            if(score<bestScore) {bestScore=score;best={candidate,int64_t(ms)*1000000,robust.rejectedSamples,samples.size()};found=true;}
        } catch(const ResidualError& error) {
            if(error.score<badScore) {badScore=error.score;badResidual=error.what();}
        } catch(const std::runtime_error& error) {++rejected[error.what()];}
    }
    if (!found) {
        std::string reason="No acceptable time-matched calibration";unsigned count=0;
        for (const auto& error:rejected) if(error.second>count) {count=error.second;reason=error.first;}
        if(!badResidual.empty()) reason=badResidual;
        throw std::runtime_error(reason);
    }
    return best;
}
}
