#include "async_fit.h"
#include <iostream>
using namespace vrcm_calibration;
int main() {
    AsyncFit fit;std::promise<void> started,release;auto gate=release.get_future().share();
    fit.start([&]{started.set_value();gate.wait();return TimedFit{Fit{},30000000};});
    started.get_future().wait();
    require(fit.active() && !fit.ready(),"Slow fitting blocked or falsely completed");
    // Input reception and publishing continue while the worker is deliberately blocked.
    PoseHistory history;
    for(int i=0;i<1000;++i) history.add(1+i*1000000,Pose::Identity());
    Pose pose;require(history.at(500000001,pose),"Input history stopped during fitting");
    require(!fit.ready(),"Worker unexpectedly bypassed its release gate");
    release.set_value();
    while(!fit.ready()) std::this_thread::yield();
    require(fit.take().lagNs==30000000 && !fit.active(),"Worker result was lost");
    fit.start([]()->TimedFit{throw std::runtime_error("bad fit");});
    while(!fit.ready()) std::this_thread::yield();
    bool rejected=false;try{fit.take();}catch(const std::runtime_error&){rejected=true;}
    require(rejected && !fit.active(),"Failed fit did not release worker state");
    std::cout<<"PASS: input processing continues during a blocked fit; completion and failure release worker state\n";
}
