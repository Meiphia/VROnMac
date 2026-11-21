#include "motoc_fit.h"
#include <iostream>
using namespace vrcm_calibration;
int main() {
    Pose origin=Pose::Identity(),mount=Pose::Identity();
    origin.linear()=Eigen::AngleAxisd(.7,Eigen::Vector3d(.2,1,.3).normalized()).toRotationMatrix();
    origin.translation()=Eigen::Vector3d(1.4,.2,-2);
    mount.linear()=Eigen::AngleAxisd(.4,Eigen::Vector3d(1,.5,.3).normalized()).toRotationMatrix();
    mount.translation()=Eigen::Vector3d(.04,.1,-.13);
    std::vector<Sample> clean;
    for(int i=0;i<200;++i) {
        double t=i*.05;Pose head=Pose::Identity();
        head.linear()=(Eigen::AngleAxisd(.6*std::sin(t),Eigen::Vector3d::UnitX())*
                       Eigen::AngleAxisd(.8*std::cos(.8*t),Eigen::Vector3d::UnitY())*
                       Eigen::AngleAxisd(.4*std::sin(.6*t),Eigen::Vector3d::UnitZ())).toRotationMatrix();
        head.translation()=Eigen::Vector3d(std::sin(t),1.7+.1*std::cos(t),std::cos(.4*t));
        clean.push_back({head,origin.inverse()*head*mount.inverse()});
    }
    auto result=solveRobust(clean,180);
    require(result.rejectedSamples==0,"Trimmed clean samples");
    require((result.fit.questFromLighthouse.matrix()-origin.matrix()).norm()<1e-7,"Clean origin changed");
    std::cout<<"PASS: clean data retains every sample and recovers known alignment\n";
    auto corrupt=[](std::vector<Sample> data,int interval) {
        for(size_t i=3;i<data.size();i+=interval) {
            auto axis=Eigen::Vector3d(1,.3+.01*i,.2).normalized();
            data[i].lighthouse.linear()*=Eigen::AngleAxisd(1.2,axis).toRotationMatrix();
            data[i].lighthouse.translation()+=Eigen::Vector3d(.06,-.03,.04);
        }
        return data;
    };
    auto bad=corrupt(clean,20);bool ordinaryRejected=false;
    try {solve(bad);} catch(const std::runtime_error&) {ordinaryRejected=true;}
    require(ordinaryRejected,"Ordinary fitter unexpectedly accepted large corrections");
    result=solveRobust(bad,180);
    require(result.rejectedSamples<=20,"Exceeded 10 percent trimming budget");
    require((result.fit.questFromLighthouse.matrix()-origin.matrix()).norm()<1e-6,"Outliers corrupted recovered origin");
    require((result.fit.trackerFromHead.matrix()-mount.matrix()).norm()<1e-6,"Outliers corrupted mount offset");
    std::cout<<"PASS: 5 percent large orientation/position glitches rejected; known origin and mount recovered\n";
    auto rejects=[](const auto& data,size_t minimum=180) {
        try {solveRobust(data,minimum);} catch(const std::runtime_error&) {return true;}return false;
    };
    require(rejects(corrupt(clean,4)),"Accepted 25 percent contaminated capture");
    std::cout<<"PASS: excessive contamination still fails unchanged accuracy gates\n";
    bad=clean;bad[0].quest.translation().x()=NAN;
    require(rejects(bad),"Trimmed a nonfinite input instead of rejecting capture");
    require(rejects(std::vector<Sample>(200,{Pose::Identity(),Pose::Identity()})),"Accepted stationary capture");
    require(rejects(std::vector<Sample>(clean.begin(),clean.begin()+179)),"Accepted fewer than 180 samples");
    std::cout<<"PASS: nonfinite, stationary and insufficient captures rejected\n";
}
