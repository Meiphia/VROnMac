// Native diagnostic for Monado's open-source Lighthouse backend, libsurvive.
// Candidate poses remain in libsurvive's native origin and are not sent to VRChat.
#include <survive_api.h>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>
static volatile std::sig_atomic_t stopping=0;
static void stop(int) { stopping=1; }
static void log_message(SurviveSimpleContext*, SurviveLogLevel, const char *message) {
    std::fprintf(stderr,"%s\n",message);
}
int main(int argc,char **argv) {
    if(argc<3) {std::fprintf(stderr,"Usage: %s seconds pose-output.csv [libsurvive flags]\n",argv[0]); return 2;}
    char *end=nullptr;
    double seconds=std::strtod(argv[1],&end);
    if(!end || *end || !std::isfinite(seconds) || seconds<1 || seconds>120) return 2;
    FILE *output=std::fopen(argv[2],"wb");
    if(!output) {std::perror(argv[2]);return 2;}
    std::setbuf(stdout,nullptr);
    std::signal(SIGINT,stop); std::signal(SIGTERM,stop);
    std::vector<char*> args{argv[0]};
    for(int i=3;i<argc;++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    std::puts("Native Lighthouse solver diagnostic. Poses are not Quest-aligned.");
    SurviveSimpleContext *context=survive_simple_init_with_logger((int)args.size()-1,args.data(),log_message);
    if(!context) {std::fclose(output);return 3;}
    std::fputs("received_ns,serial,solver_time,px,py,pz,qw,qx,qy,qz\n",output);
    auto started=std::chrono::steady_clock::now();
    survive_simple_start_thread(context);
    std::map<std::string,unsigned> counts;
    bool shutdown=false;
    while(!stopping && !shutdown && std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<seconds) {
        SurviveSimpleEvent event{};
        auto type=survive_simple_next_event(context,&event);
        if(type==SurviveSimpleEventType_None) {std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        if(type==SurviveSimpleEventType_Shutdown) {shutdown=true;continue;}
        if(type==SurviveSimpleEventType_ConfigEvent) {
            const auto *e=survive_simple_get_config_event(&event);
            std::printf("CONFIG serial=%s\n",survive_simple_serial_number(e->object));
        }
        if(type!=SurviveSimpleEventType_PoseUpdateEvent) continue;
        const auto *e=survive_simple_get_pose_updated_event(&event);
        if(survive_simple_object_get_type(e->object)==SurviveSimpleObject_LIGHTHOUSE) continue;
        const char *serial=survive_simple_serial_number(e->object);
        bool finite=true;
        for(double v:e->pose.Pos) finite &= std::isfinite(v);
        for(double v:e->pose.Rot) finite &= std::isfinite(v);
        if(!finite) continue;
        unsigned count=++counts[serial];
        if(count==1) std::printf("FIRST_CANDIDATE_POSE serial=%s position=%.3f,%.3f,%.3f\n",serial,e->pose.Pos[0],e->pose.Pos[1],e->pose.Pos[2]);
        auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        std::fprintf(output,"%lld,%s,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
            (long long)ns,serial,e->time,e->pose.Pos[0],e->pose.Pos[1],e->pose.Pos[2],
            e->pose.Rot[0],e->pose.Rot[1],e->pose.Rot[2],e->pose.Rot[3]);
    }
    survive_simple_close(context);
    int failed=std::fclose(output);
    for(const auto &c:counts) std::printf("CANDIDATE_POSES serial=%s count=%u\n",c.first.c_str(),c.second);
    std::puts("Solver capture complete. Pose quality and Quest-space alignment still require verification.");
    return failed ? 5 : counts.empty() ? 4 : 0;
}
