#include "pose_history.h"
#include <fstream>
#include <sstream>
#include <iostream>
using namespace vrcm_calibration;
static std::vector<TimedPose> read(const char* filename) {
    std::ifstream input(filename);require(bool(input),"Cannot open pose capture");
    std::string line;std::getline(input,line);std::vector<TimedPose> poses;
    while(std::getline(input,line)) {
        for(char& c:line) if(c==',') c=' ';
        std::istringstream row(line);int64_t time;double x,y,z,w,qx,qy,qz;
        require(bool(row>>time>>x>>y>>z>>w>>qx>>qy>>qz),"Invalid pose capture row");
        Pose pose=Pose::Identity();pose.translation()=Eigen::Vector3d(x,y,z);
        Eigen::Quaterniond q(w,qx,qy,qz);require(q.coeffs().allFinite() && std::abs(q.squaredNorm()-1)<.01,"Invalid captured rotation");
        pose.linear()=q.normalized().toRotationMatrix();require(valid(pose),"Invalid captured pose");
        poses.push_back({time,pose});
    }
    return poses;
}
int main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"Usage: replay_fit lighthouse.csv quest.csv\n";return 2;}
    try {
        auto reference=read(argv[1]);PoseHistory history;
        for(const auto& p:read(argv[2])) history.add(p.time,p.pose);
        auto result=fitTimedReference(reference,history);
        std::cout<<"PASS: "<<result.fit.positionRms*1000<<" mm RMS, "<<result.fit.angleRms*180/3.141592653589793
                 <<" degrees RMS, lag "<<result.lagNs/1000000<<" ms; excluded "<<result.rejectedSamples<<"/"<<result.matchedSamples<<" pose outliers\n";
        std::cout<<"Quest from Lighthouse:\n"<<result.fit.questFromLighthouse.matrix()<<"\n";return 0;
    } catch(const std::exception& error) {std::cout<<"REJECTED: "<<error.what()<<"\n";return 1;}
}
