#include "pose_history.h"
#include <iostream>
#include <fstream>
#include <iomanip>
using namespace vrcm_calibration;
int main(int argc,char** argv) {
    Pose origin=Pose::Identity(),mount=Pose::Identity();
    origin.linear()=Eigen::AngleAxisd(.7,Eigen::Vector3d(.2,1,.3).normalized()).toRotationMatrix();
    origin.translation()=Eigen::Vector3d(1.4,.2,-2);
    mount.linear()=Eigen::AngleAxisd(.4,Eigen::Vector3d(1,.5,.3).normalized()).toRotationMatrix();
    mount.translation()=Eigen::Vector3d(.04,.1,-.13);
    PoseHistory quest;std::vector<TimedPose> reference;
    for(int i=0;i<1250;++i) {
        double t=i*.01;
        Pose head=Pose::Identity();
        head.linear()=(Eigen::AngleAxisd(.6*std::sin(t),Eigen::Vector3d::UnitX())*
                       Eigen::AngleAxisd(.8*std::cos(.8*t),Eigen::Vector3d::UnitY())*
                       Eigen::AngleAxisd(.4*std::sin(.6*t),Eigen::Vector3d::UnitZ())).toRotationMatrix();
        head.translation()=Eigen::Vector3d(std::sin(t),1.7+.1*std::cos(t),std::cos(.4*t));
        int64_t stamp=1000000000+int64_t(i)*10000000;
        quest.add(stamp+30000000,head); // Synthetic Quest stream arrives 30ms later.
        if(i>=100 && i<1100 && i%5==0)
            reference.push_back({stamp,origin.inverse()*head*mount.inverse()});
    }
    auto result=fitTimedReference(reference,quest);
    require(result.lagNs==30000000,"Did not recover known 30ms timing difference");
    require((result.fit.questFromLighthouse.matrix()-origin.matrix()).norm()<1e-7,"Timing-aligned fit recovered wrong origin");
    std::cout<<"PASS: synchronized interpolation recovers 30ms stream delay and the known origin transform\n";
    PoseHistory broken;broken.add(1,Pose::Identity());broken.add(100000001,Pose::Identity());Pose pose;
    require(!broken.at(50000001,pose),"Interpolated across missing tracking data");
    require(!quest.at(1,pose),"Extrapolated outside pose history");
    std::cout<<"PASS: tracking gaps and out-of-range queries rejected\n";
    std::vector<TimedPose> still;
    PoseHistory stillQuest;
    for(int i=0;i<250;++i) {
        int64_t stamp=1000000000+int64_t(i)*10000000;
        stillQuest.add(stamp,Pose::Identity());
        if(i<200) still.push_back({stamp,Pose::Identity()});
    }
    bool explained=false;
    try {fitTimedReference(still,stillQuest);}
    catch(const std::runtime_error& error) {explained=std::string(error.what()).find("rotation")!=std::string::npos;}
    require(explained,"Lost the specific reason for rejecting a stationary capture");
    std::cout<<"PASS: rejection explains insufficient head rotation\n";
    if(argc==2) {
        auto save=[](const auto& poses,const std::string& filename) {
            std::ofstream out(filename);out<<"time_ns,px,py,pz,qw,qx,qy,qz\n"<<std::setprecision(17);
            for(const auto& sample:poses) {
                Eigen::Quaterniond q(sample.pose.rotation());auto p=sample.pose.translation();
                out<<sample.time<<","<<p.x()<<","<<p.y()<<","<<p.z()<<","<<q.w()<<","<<q.x()<<","<<q.y()<<","<<q.z()<<"\n";
            }
        };
        save(reference,std::string(argv[1])+"-lighthouse.csv");
        save(quest.samples(),std::string(argv[1])+"-quest.csv");
    }
}
