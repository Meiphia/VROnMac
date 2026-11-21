#pragma once
#include "vrcm_trackers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vrcm {
class TrackerCache {
    mutable std::mutex mutex_;
    VrcmTrackerPacket packet_{};
    int64_t received_ = 0;
    uint64_t generation_ = 1;
public:
    static bool Validate(const VrcmTrackerPacket &p) {
        if(p.magic!=VRCM_TRACKER_MAGIC || p.version!=VRCM_TRACKER_VERSION ||
           p.count>VRCM_MAX_TRACKERS || p.flags>VRCM_TRACKER_CALIBRATED || p.sampleTimeNs<=0) return false;
        std::set<uint64_t> ids;
        std::set<std::string> serials;
        for(uint32_t i=0;i<p.count;++i) {
            const auto &t=p.trackers[i];
            if(!t.id || !ids.insert(t.id).second || !t.name[0] || !t.serial[0] ||
               !memchr(t.name,0,sizeof(t.name)) || !memchr(t.serial,0,sizeof(t.serial)) ||
               !serials.insert(t.serial).second || t.flags>3) return false;
            const auto &v=t.pose.position; const auto &q=t.pose.orientation;
            if(!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
               std::abs(v.x)>100 || std::abs(v.y)>100 || std::abs(v.z)>100 ||
               !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
            double norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
            if(std::abs(norm-1)>0.01) return false;
        }
        return true;
    }
    bool Update(const VrcmTrackerPacket &p,int64_t now) {
        if(!Validate(p)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if(p.sampleTimeNs<packet_.sampleTimeNs) return false;
        bool changed=p.count!=packet_.count;
        for(uint32_t i=0;!changed && i<p.count;++i)
            changed=p.trackers[i].id!=packet_.trackers[i].id ||
                strcmp(p.trackers[i].name,packet_.trackers[i].name) ||
                strcmp(p.trackers[i].serial,packet_.trackers[i].serial);
        if(changed) ++generation_;
        packet_=p;received_=now;
        if(!(p.flags&VRCM_TRACKER_CALIBRATED))
            for(uint32_t i=0;i<packet_.count;++i) packet_.trackers[i].flags=0;
        return true;
    }
    std::vector<VrcmTracker> Snapshot(int64_t now,uint64_t *generation=nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(generation) *generation=generation_;
        std::vector<VrcmTracker> out(packet_.trackers,packet_.trackers+packet_.count);
        if(!received_ || now<received_ || now-received_>250000000)
            for(auto &t:out) t.flags=0;
        return out;
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(packet_.count) ++generation_;
        packet_={}; received_=0;
    }
};
}
