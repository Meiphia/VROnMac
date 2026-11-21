#pragma once
#include "pose_history.h"
#include <future>
#include <functional>
#include <thread>
namespace vrcm_calibration {
class AsyncFit {
    std::future<TimedFit> future_;
public:
    bool active() const { return future_.valid(); }
    void start(std::function<TimedFit()> work) {
        require(!active(),"Calibration is already running");
        future_=std::async(std::launch::async,std::move(work));
    }
    bool ready() const {
        return active() && future_.wait_for(std::chrono::seconds(0))==std::future_status::ready;
    }
    TimedFit take() { require(ready(),"Calibration result is not ready");return future_.get(); }
};
}
