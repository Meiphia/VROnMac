#define main saved_replay_main
#include "replay_fit.cpp"
#undef main
#include <numeric>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    auto reference=read(argv[1]);PoseHistory history;for(auto p:read(argv[2])) history.add(p.time,p.pose);
    for(int lag=-60;lag<=60;lag+=10) {
        std::vector<Sample> samples;for(const auto& p:reference) {Pose q;if(history.at(p.time+int64_t(lag)*1000000,q)) samples.push_back({q,p.pose});}
        if(samples.size()<180) continue;
        try {
            Fit fit=solve(samples,false);size_t keep=std::max(size_t(180),samples.size()-samples.size()/10);
            for(int iteration=0;iteration<4;++iteration) {
                std::vector<std::pair<double,size_t>> scores;
                for(size_t i=0;i<samples.size();++i) {
                    Pose predicted=fit.questFromLighthouse*samples[i].lighthouse*fit.trackerFromHead;
                    double p=(predicted.translation()-samples[i].quest.translation()).norm()/.03;
                    double a=angle(predicted.rotation().transpose()*samples[i].quest.rotation())/.05;
                    scores.push_back({p*p+a*a,i});
                }
                std::sort(scores.begin(),scores.end());std::vector<Sample> kept;
                std::vector<size_t> indices;for(size_t i=0;i<keep;++i) indices.push_back(scores[i].second);
                std::sort(indices.begin(),indices.end());for(auto i:indices) kept.push_back(samples[i]);
                fit=solve(kept,false);
            }
            std::cout<<lag<<" ms: "<<fit.positionRms*1000<<" mm, "<<fit.angleRms*180/M_PI<<" degrees; "<<keep<<"/"<<samples.size()<<" samples\n";
        } catch(const std::exception& error) {std::cout<<lag<<" ms: "<<error.what()<<"\n";}
    }
}
