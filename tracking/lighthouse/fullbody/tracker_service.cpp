// GPL-3.0-only: uses the Motoc-derived calibration component.
#define SURVIVE_ENABLE_FULL_API
#include <survive.h>
#include <survive_api.h>
#include "../calibration/motoc_fit.h"
#include "../calibration/pose_history.h"
#include "../calibration/async_fit.h"
#include "pose_guard.h"
#include "common/vrcm_trackers.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <string>
#include <thread>
using namespace vrcm_calibration;
static volatile std::sig_atomic_t quit=0;
static void stop(int) {quit=1;}
static int64_t nowNs() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double epochNow() {return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();}
static uint64_t identifier(const std::string& serial) {uint64_t h=14695981039346656037ull;for(unsigned char c:serial) {h^=c;h*=1099511628211ull;}return h ? h:1;}
static Pose fromWire(const VrcmPose& p) {
    Pose out=Pose::Identity();Eigen::Quaterniond q(p.orientation.w,p.orientation.x,p.orientation.y,p.orientation.z);
    if(!q.coeffs().allFinite() || std::abs(q.squaredNorm()-1)>.01) throw std::runtime_error("Invalid head rotation");
    out.linear()=q.normalized().toRotationMatrix();out.translation()=Eigen::Vector3d(p.position.x,p.position.y,p.position.z);
    if(!valid(out)) throw std::runtime_error("Invalid head pose");return out;
}
static Pose fromSurvive(const SurvivePose& p) {
    Pose out=Pose::Identity();
    Eigen::Quaterniond q(p.Rot[0],p.Rot[1],p.Rot[2],p.Rot[3]);
    if(!q.coeffs().allFinite() || std::abs(q.squaredNorm()-1)>.01) throw std::runtime_error("Invalid tracker rotation");
    // Same basis conversion as Monado's survive_driver.c pose_to_relation.
    out.linear()=(Eigen::Quaterniond(Eigen::AngleAxisd(-M_PI/2,Eigen::Vector3d::UnitX()))*q.normalized()).toRotationMatrix();
    out.translation()=Eigen::Vector3d(p.Pos[0],p.Pos[2],-p.Pos[1]);
    if(!valid(out)) throw std::runtime_error("Invalid tracker pose");return out;
}
static VrcmPose toWire(const Pose& p) {
    Eigen::Quaterniond q(p.rotation());
    return {{float(q.x()),float(q.y()),float(q.z()),float(q.w())},
            {float(p.translation().x()),float(p.translation().y()),float(p.translation().z())}};
}
static void logMessage(SurviveSimpleContext*,SurviveLogLevel,const char *message) {fprintf(stderr,"%s\n",message);}
struct Device {Pose pose=Pose::Identity();int64_t received=0,optical=0,lastGuardLog=0;bool poseTrusted=false;unsigned rejected=0;PoseHistory history;vrcm_tracking::PoseGuard guard;};

// Discover receiver-backed trackers without opening a pose socket or starting calibration.
static int discoverTrackers(const char* config) {
    const char* arguments[] = {"tracker-discovery", "--configfile", config,
        "--lighthouse-gen", "2", "--haptic-on-calibrate", "0", nullptr};
    auto* context = survive_simple_init_with_logger(7, const_cast<char**>(arguments), logMessage);
    if (!context) return 4;

    std::signal(SIGTERM, stop);
    survive_simple_start_thread(context);
    std::set<std::string> serials;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!quit && survive_simple_is_running(context) && std::chrono::steady_clock::now() < deadline) {
        survive_simple_lock(context);
        for (auto* object = survive_simple_get_first_object(context); object;
             object = survive_simple_get_next_object(context, object)) {
            if (survive_simple_object_get_type(object) != SurviveSimpleObject_OBJECT) continue;
            const auto subtype = survive_simple_object_get_subtype(object);
            if (subtype == SURVIVE_OBJECT_SUBTYPE_TRACKER || subtype == SURVIVE_OBJECT_SUBTYPE_TRACKER_GEN2) {
                if (const char* serial = survive_simple_serial_number(object)) serials.insert(serial);
            }
        }
        survive_simple_unlock(context);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool completed = !quit && survive_simple_is_running(context);
    survive_simple_close(context);
    for (const auto& serial : serials) printf("VRONMAC_TRACKER %s\n", serial.c_str());
    return completed ? 0 : 4;
}

int main(int argc,char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--discover")) return discoverTrackers(argv[2]);
    if(argc==2 && !strcmp(argv[1],"--self-test")) {
        SurvivePose raw{};raw.Pos[0]=1;raw.Pos[1]=2;raw.Pos[2]=3;raw.Rot[0]=1;
        Pose p=fromSurvive(raw);
        require((p.translation()-Eigen::Vector3d(1,3,-2)).norm()<1e-9,"Lighthouse coordinate conversion failed");
        VrcmPose wire=toWire(p);Pose restored=fromWire(wire);
        require((p.matrix()-restored.matrix()).norm()<1e-6,"Pose wire conversion failed");
        puts("PASS: Monado Lighthouse basis conversion and native pose serialization");return 0;
    }
    if(argc<3 || (argc>3 && strcmp(argv[3],"manual") && strcmp(argv[3],"continuous"))) {
        fprintf(stderr,"Usage: %s state-directory reference-serial [manual|continuous [body-serial ...]]\n",argv[0]);
        return 2;
    }
    bool manual=argc>3 && !strcmp(argv[3],"manual");
    bool collecting=!manual;
    std::set<std::string> bodySerials;
    for(int i=4;i<argc;++i) bodySerials.insert(argv[i]);
    std::filesystem::path directory=std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    std::string headSerial=argv[2];
    setbuf(stdout,nullptr);std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
    int headSocket=socket(AF_INET,SOCK_DGRAM,0),feed=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(VRCM_HEAD_PORT);
    if(headSocket<0 || feed<0 || bind(headSocket,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {perror("Head pose receiver");return 3;}
    fcntl(headSocket,F_SETFL,O_NONBLOCK);fcntl(feed,F_SETFL,O_NONBLOCK);
    sockaddr_in destination=address;destination.sin_port=htons(VRCM_TRACKER_PORT);
    std::string config=(directory/"lighthouse.config").string();
    // Quest provides the final floor. Keep the raw Lighthouse origin stable,
    // and reject poor camera re-estimates rather than moving the calibration space.
    const char *arguments[]={argv[0],"--configfile",config.c_str(),"--lighthouse-gen","2","--haptic-on-calibrate","0",
        "--gss-auto-floor-height","0","--max-cal-error","0.005",nullptr};
    auto *context=survive_simple_init_with_logger(11,const_cast<char**>(arguments),logMessage);
    if(!context) {close(headSocket);close(feed);return 4;}
    survive_simple_start_thread(context);
    std::map<std::string,Device> devices;
    PoseHistory questHistory;
    Fit fit;AsyncFit fitting;bool calibrated=false,announced=false,ready=false;
    uint64_t calibrationEpoch=0,fitEpoch=0;
    int64_t lastStatus=0,lastControl=0;
    unsigned attempts=0;
    std::string lastError;
    int64_t lastHead=0,lastReference=0,lastCollect=0,lastPublish=0,lagNs=0,lastMaintain=0,badAlignmentSince=0,lastClockWarning=0;
    std::vector<TimedPose> reference;
    puts("Waiting for native Quest poses and Lighthouse tracking. Head reference is excluded from body trackers.");
    while(!quit) {
        int64_t now=nowNs();
        if(now-lastControl>=500000000) {
            std::error_code error;
            if(std::filesystem::remove(directory/"retry",error)) {
                ++calibrationEpoch;attempts=0;lastError.clear();reference.clear();
                calibrated=false;ready=false;announced=false;collecting=true;
                puts("Calibration retry requested.");
            }
            lastControl=now;
        }
        for(int i=0;i<512;++i) {
            char bytes[sizeof(VrcmHeadPacket)+1];int count=int(recv(headSocket,bytes,sizeof(bytes),0));
            if(count<0) break;
            if(count!=sizeof(VrcmHeadPacket)) continue;
            VrcmHeadPacket packet;memcpy(&packet,bytes,sizeof(packet));
            if(packet.magic!=VRCM_HEAD_MAGIC || packet.version!=1 || packet.valid>3 ||
               packet.sampleTimeNs>now+50000000 || now-packet.sampleTimeNs>150000000) continue;
            if(packet.valid) lastHead=now;
            if(!(packet.valid&1)) continue;
            try {questHistory.add(packet.sampleTimeNs,fromWire(packet.pose));lastReference=now;} catch(...) {}
        }
        for(int i=0;i<256;++i) {
            SurviveSimpleEvent event{};auto type=survive_simple_next_event(context,&event);
            if(type==SurviveSimpleEventType_None) break;
            if(type==SurviveSimpleEventType_Shutdown) {quit=1;break;}
            if(type!=SurviveSimpleEventType_PoseUpdateEvent) continue;
            const auto *e=survive_simple_get_pose_updated_event(&event);
            if(survive_simple_object_get_type(e->object)!=SurviveSimpleObject_OBJECT) continue;
            std::string serial=survive_simple_serial_number(e->object);
            if(serial.empty()) continue;
            double epoch=epochNow(),age=epoch-e->time;
            if(age<-.05 || age>.15) {
                if(nowNs()-lastClockWarning>5000000000) {
                    printf("CLOCK: rejected tracker timestamp (age %.1f ms); waiting for synchronized live poses\n",age*1000);
                    lastClockWarning=nowNs();
                }
                continue;
            }
            int64_t sampleTime=nowNs()-int64_t(age*1e9),opticalTime=0;
            survive_simple_lock(context);
            if(auto* object=survive_simple_get_survive_object(e->object)) {
                if(object->activations.last_light) {
                    double light=SurviveSensorActivations_runtime(&object->activations,object->activations.last_light)*1e-6;
                    double lightAge=epoch-light;
                    if(lightAge>=-.05 && lightAge<.15) opticalTime=nowNs()-int64_t(lightAge*1e9);
                }
            }
            survive_simple_unlock(context);
            try {
                Pose p=fromSurvive(e->pose);
                auto &d=devices[serial];
                if(manual || serial!=headSerial) {
                    if(!opticalTime) {d.poseTrusted=false;continue;}
                    auto outcome=d.guard.Observe(sampleTime,p);
                    if(outcome==vrcm_tracking::Outcome::Ignored) continue;
                    if(outcome==vrcm_tracking::Outcome::Rejected) {
                        d.poseTrusted=false;++d.rejected;
                        if(nowNs()-d.lastGuardLog>5000000000) {
                            printf("POSE GUARD: %s rejected %u implausible/discontinuous samples; body pose temporarily untracked\n",serial.c_str(),d.rejected);
                            d.lastGuardLog=nowNs();
                        }
                        continue;
                    }
                }
                d.pose=p;d.received=nowNs();d.optical=opticalTime;d.poseTrusted=true;
                if(opticalTime) d.history.add(sampleTime,p);
            } catch(...) {}
        }
        now=nowNs();auto found=devices.find(headSerial);
        bool tracking=found!=devices.end() && found->second.optical &&
            now-found->second.optical<150000000 && lastReference && now-lastReference<150000000;
        if(collecting && tracking && !calibrated && attempts<3 && !fitting.active() && now-lastCollect>=50000000) {
            if(!announced) {
                puts(manual ? "CALIBRATION: keep the reference and tracker rigidly together and move across several axes."
                            : "CALIBRATION: keep the head tracker fixed; slowly turn/tilt your head and walk a little.");
                announced=true;
            }
            Pose lighthouse,quest;int64_t time=now-150000000;
            if(found->second.history.at(time,lighthouse) && questHistory.at(time,quest)) {
                reference.push_back({time,lighthouse});lastCollect=now;
            }
            if(reference.size()>=200) {
                fitEpoch=calibrationEpoch;
                ++attempts;
                fitting.start([samples=reference,history=questHistory,directory,attempt=attempts]{
                    auto save=[](const auto& poses,const std::filesystem::path& path) {
                        std::ofstream csv(path);csv<<"time_ns,px,py,pz,qw,qx,qy,qz\n"<<std::setprecision(17);
                        for(const auto& sample:poses) {
                            Eigen::Quaterniond q(sample.pose.rotation());const auto& p=sample.pose.translation();
                            csv<<sample.time<<","<<p.x()<<","<<p.y()<<","<<p.z()<<","<<q.w()<<","<<q.x()<<","<<q.y()<<","<<q.z()<<"\n";
                        }
                    };
                    auto prefix="attempt-"+std::to_string(attempt);
                    save(samples,directory/(prefix+"-lighthouse.csv"));
                    save(history.samples(),directory/(prefix+"-quest.csv"));
                    return fitTimedReference(samples,history);
                });
                puts("CALCULATING: finding tracking-space alignment; live tracking continues.");
            }
        }
        if(fitting.ready()) {
            try {
                auto result=fitting.take();
                if(fitEpoch!=calibrationEpoch || !tracking) throw std::runtime_error("Tracking changed during fitting");
                fit=result.fit;lagNs=result.lagNs;calibrated=true;lastError.clear();attempts=0;
                printf("CALIBRATED: %.1f mm RMS, %.2f degrees RMS, Quest timing adjustment %.0f ms; excluded %zu/%zu pose outliers\n",fit.positionRms*1000,fit.angleRms*180/M_PI,double(lagNs)/1e6,result.rejectedSamples,result.matchedSamples);
                std::ofstream profile(directory/"calibration.txt");
                profile<<"VRConMac Motoc calibration v1\n"<<headSerial<<"\n"<<lagNs<<"\n";
                profile<<fit.questFromLighthouse.matrix()<<"\n"<<fit.trackerFromHead.matrix()<<"\n";
                puts("Calibration accepted; checking the body trackers before declaring ready.");
            } catch(const std::runtime_error& error) {
                lastError=error.what();
                printf("CALIBRATION REJECTED (%u/3): %s\n",attempts,lastError.c_str());
                if(attempts>=3) puts("CALIBRATION PAUSED: inspect the reason; press R then Return in the status window to retry.");
                if(reference.size()>100) reference.erase(reference.begin(),reference.end()-100);
            }
        }
        if(!manual && calibrated && tracking && now-lastMaintain>=20000000) {
            int64_t time=now-150000000;Pose lh,q,oldLh,oldQ;
            if(found->second.history.at(time,lh) && questHistory.at(time+lagNs,q) &&
               found->second.history.at(time-50000000,oldLh) && questHistory.at(time+lagNs-50000000,oldQ)) {
                double linear=std::max((lh.translation()-oldLh.translation()).norm(),(q.translation()-oldQ.translation()).norm())/.05;
                double angular=std::max(angle(lh.rotation()*oldLh.rotation().transpose()),angle(q.rotation()*oldQ.rotation().transpose()))/.05;
                Pose desired=q*fit.trackerFromHead.inverse()*lh.inverse();
                bool jump=(desired.translation()-fit.questFromLighthouse.translation()).norm()>.15 ||
                    angle(desired.rotation()*fit.questFromLighthouse.rotation().transpose())>.2;
                if(jump && linear<.5 && angular<.3) {
                    if(!badAlignmentSince) badAlignmentSince=now;
                    if(now-badAlignmentSince>1000000000) {
                        ++calibrationEpoch;calibrated=false;ready=false;reference.clear();announced=false;badAlignmentSince=0;
                        puts("Tracking spaces changed; body poses paused until recalibration.");
                    }
                } else {
                    badAlignmentSince=0;
                    maintain(fit.questFromLighthouse,fit.trackerFromHead,{q,lh},true,0,linear,angular,.02);
                }
            }
            lastMaintain=now;
        }
        if(lastHead && now-lastHead>2000000000 && calibrated) {
            ++calibrationEpoch;calibrated=false;ready=false;reference.clear();questHistory.clear();announced=false;collecting=!manual;
            puts("Quest feed lost; calibration invalidated. Recalibrate after reconnecting.");
        }
        if(now-lastPublish>=10000000) {
            VrcmTrackerPacket packet{};packet.magic=VRCM_TRACKER_MAGIC;packet.version=1;
            packet.flags=calibrated ? VRCM_TRACKER_CALIBRATED:0;packet.sampleTimeNs=now;
            for(const auto &entry:devices) {
                if((!manual && entry.first==headSerial) ||
                   (!bodySerials.empty() && !bodySerials.count(entry.first)) ||
                   packet.count>=VRCM_MAX_TRACKERS) continue;
                auto &t=packet.trackers[packet.count++];t.id=identifier(entry.first);
                snprintf(t.name,sizeof(t.name),"HTC Vive Tracker");snprintf(t.serial,sizeof(t.serial),"%s",entry.first.c_str());
                bool fresh=entry.second.poseTrusted && entry.second.optical && now-entry.second.optical<150000000 && now-entry.second.received<150000000;
                t.flags=calibrated && fresh && lastHead && now-lastHead<150000000 ? 3:0;
                Pose p=calibrated ? fit.questFromLighthouse*entry.second.pose:Pose::Identity();t.pose=toWire(p);
            }
            const size_t expected=bodySerials.empty() ? (argc>3 ? std::max(1u,packet.count) : 3u) : bodySerials.size();
            unsigned tracked=0;
            for(unsigned i=0;i<packet.count;++i) if(packet.trackers[i].flags==3) ++tracked;
            if(calibrated && tracked==expected && !ready) {
                ready=true;
                puts("READY: calibrated body trackers.");
                for(unsigned i=0;i<packet.count;++i) printf("BODY: %s\n",packet.trackers[i].serial);
                printf("HEAD REFERENCE: %s\n",headSerial.c_str());
            }
            if(now-lastStatus>=500000000) {
                bool questConnected=lastHead && now-lastHead<150000000;
                const char* phase=!questConnected ? "Waiting for headset" :
                    calibrated ? (tracked==expected ? "Ready" : "Waiting for body trackers") :
                    !collecting ? "Press Calibrate" :
                    !tracking ? (manual ? "Waiting for reference poses" : "Waiting for head tracker") :
                    fitting.active() ? "Calculating alignment" : !calibrated && attempts>=3 ? "Calibration paused" : !calibrated ? "Collecting calibration" :
                    "Waiting for body trackers";
                std::ofstream status(directory/"status.tmp");
                status<<"{\"phase\":\""<<phase<<"\",\"samples\":"<<reference.size()
                    <<",\"target\":200,\"bodySeen\":"<<packet.count<<",\"bodyTracked\":"<<tracked<<",\"bodyTarget\":"<<expected
                    <<",\"questConnected\":"<<(questConnected ? "true":"false")
                    <<",\"headTrackerTracked\":"<<(tracking ? "true":"false")
                    <<",\"positionRmsMm\":"<<(calibrated ? fit.positionRms*1000:0)
                    <<",\"attempts\":"<<attempts<<",\"lastError\":"<<std::quoted(lastError)
                    <<",\"discoveredSerials\":[";
                bool first = true;
                for (const auto& entry : devices) {
                    if (!first) status << ",";
                    status << std::quoted(entry.first);
                    first = false;
                }
                status << "]}\n";
                status.close();
                std::error_code error;std::filesystem::rename(directory/"status.tmp",directory/"status.json",error);
                lastStatus=now;
            }
            sendto(feed,&packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination));lastPublish=now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    survive_simple_close(context);close(headSocket);close(feed);return 0;
}
