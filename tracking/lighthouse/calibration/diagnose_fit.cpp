#define main saved_replay_main
#include "replay_fit.cpp"
#undef main
#include <algorithm>
struct Result {int lag;double position,angle,score;};
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    auto reference=read(argv[1]);PoseHistory history;
    for(auto p:read(argv[2])) history.add(p.time,p.pose);
    std::vector<Result> results;
    for(int ms=-500;ms<=500;ms+=10) {
        std::vector<Sample> pairs;
        for(const auto& p:reference) {Pose q;if(history.at(p.time+int64_t(ms)*1000000,q)) pairs.push_back({q,p.pose});}
        if(pairs.size()<180) continue;
        try {auto fit=solve(pairs,false);results.push_back({ms,fit.positionRms*1000,fit.angleRms*180/M_PI,fit.positionRms+.1*fit.angleRms});}
        catch(const std::exception&) {}
    }
    std::sort(results.begin(),results.end(),[](auto a,auto b){return a.score<b.score;});
    for(size_t i=0;i<std::min(size_t(10),results.size());++i)
        std::cout<<results[i].lag<<" ms: "<<results[i].position<<" mm, "<<results[i].angle<<" degrees\n";
}
