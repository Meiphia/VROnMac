#include "motoc_fit.h"
#include <iostream>
#include <random>
using namespace vrcm_calibration;
static Pose pose(double x,double y,double z,const Eigen::Vector3d &p) {
    Pose out=Pose::Identity();
    out.linear()=(Eigen::AngleAxisd(x,Eigen::Vector3d::UnitX())*
                  Eigen::AngleAxisd(y,Eigen::Vector3d::UnitY())*
                  Eigen::AngleAxisd(z,Eigen::Vector3d::UnitZ())).toRotationMatrix();
    out.translation()=p; return out;
}
int main() {
    Pose origin=pose(.3,-.7,.2,{1.4,-.2,2.1});
    Pose mount=pose(-.2,.4,.1,{.08,.12,-.15});
    std::vector<Sample> samples;
    for(int i=0;i<100;++i) {
        double t=i*.1;
        Pose head=pose(.8*std::sin(t),.9*std::cos(.7*t),.6*std::sin(.4*t),
                       {std::sin(t),1.7+.2*std::cos(t),std::cos(.3*t)});
        samples.push_back({head,origin.inverse()*head*mount.inverse()});
    }
    auto result=solve(samples);
    require((result.questFromLighthouse.matrix()-origin.matrix()).norm()<1e-8,"Known origin transform not recovered");
    require((result.trackerFromHead.matrix()-mount.matrix()).norm()<1e-8,"Mount offset not recovered");
    std::cout<<"PASS: known origin + nonzero rotated mounting offset\n";
    std::mt19937 random(42); std::normal_distribution<double> noise(0,.001);
    auto noisy=samples;
    for(auto &s:noisy) s.lighthouse.translation()+=Eigen::Vector3d(noise(random),noise(random),noise(random));
    result=solve(noisy);
    require((result.questFromLighthouse.translation()-origin.translation()).norm()<.005,"Noisy fit exceeds 5mm");
    std::cout<<"PASS: 1mm measurement noise; residual "<<result.positionRms*1000<<"mm\n";
    auto rejects=[](const std::vector<Sample>& s) {
        try {solve(s);} catch(const std::runtime_error&) {return true;} return false;
    };
    require(rejects(std::vector<Sample>(40,{Pose::Identity(),Pose::Identity()})),"Accepted stationary capture");
    std::cout<<"PASS: stationary capture rejected\n";
    auto bad=samples; bad[0].quest.translation().x()=NAN;
    require(rejects(bad),"Accepted nonfinite pose");
    std::cout<<"PASS: invalid pose rejected\n";
    bad=samples;
    for(size_t i=0;i<bad.size();++i) bad[i].lighthouse=samples[(i+15)%samples.size()].lighthouse;
    require(rejects(bad),"Accepted mismatched timestamps");
    std::cout<<"PASS: inconsistent paired motion rejected\n";
    Pose current=origin; current.translation().x()+=.02;
    for(int i=0;i<500;++i)
        require(maintain(current,mount,samples[50],true,0,0,0,.01),"Valid drift correction rejected");
    require((current.translation()-origin.translation()).norm()<.0002,"Continuous correction did not converge");
    std::cout<<"PASS: head reference corrects gradual tracking-space drift\n";
    Pose before=current;
    require(!maintain(current,mount,samples[50],false,0,0,0,.01),"Accepted lost tracking");
    require(!maintain(current,mount,samples[50],true,10000000,0,0,.01),"Accepted unsynchronized samples");
    require(!maintain(current,mount,samples[50],true,0,1,0,.01),"Accepted fast motion");
    auto jump=samples[50];jump.quest.translation().x()+=1;
    require(!maintain(current,mount,jump,true,0,0,0,.01),"Accepted stage jump");
    require((current.matrix()-before.matrix()).norm()==0,"Rejected update changed calibration");
    std::cout<<"PASS: lost tracking, timing errors, fast motion and stage jumps preserve prior alignment\n";
}
