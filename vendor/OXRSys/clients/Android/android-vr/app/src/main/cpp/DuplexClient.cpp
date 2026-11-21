// SPDX-License-Identifier: MPL-2.0
#include "DuplexClient.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <android/log.h>
using namespace oxr::media;
void DuplexClient::Start(const char*ip){Stop();running_=true;thread_=std::thread(&DuplexClient::Run,this,std::string(ip));}
void DuplexClient::Stop(){running_=false;if(thread_.joinable())thread_.join();}
void DuplexClient::SetFace(const FaceSample&f){std::unique_lock<std::mutex>l(faceMutex_,std::try_to_lock);if(l.owns_lock()){face_=f;++faceGeneration_;}}
aaudio_data_callback_result_t DuplexClient::Output(AAudioStream*,void*ctx,void*data,int32_t frames){auto&self=*(DuplexClient*)ctx;auto*p=(int16_t*)data;size_t n=(size_t)frames*2;memset(p,0,n*2);self.playback_.Trim(4800);if(!self.primed_&&self.playback_.Available()>=1920)self.primed_=true;if(self.primed_&&self.playback_.Pop(p,n)<n)self.primed_=false;return AAUDIO_CALLBACK_RESULT_CONTINUE;}
aaudio_data_callback_result_t DuplexClient::Input(AAudioStream*,void*ctx,void*data,int32_t frames){((DuplexClient*)ctx)->microphone_.Push((int16_t*)data,frames);return AAUDIO_CALLBACK_RESULT_CONTINUE;}
void DuplexClient::Error(AAudioStream*,void*ctx,aaudio_result_t){((DuplexClient*)ctx)->audioError_=true;}
bool DuplexClient::OpenAudio(){
 auto open=[&](bool input,AAudioStream**stream){AAudioStreamBuilder*b=nullptr;AAudio_createStreamBuilder(&b);if(!b)return;
 AAudioStreamBuilder_setDirection(b,input?AAUDIO_DIRECTION_INPUT:AAUDIO_DIRECTION_OUTPUT);AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_I16);AAudioStreamBuilder_setSampleRate(b,48000);AAudioStreamBuilder_setChannelCount(b,input?1:2);AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);AAudioStreamBuilder_setPerformanceMode(b,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
 AAudioStreamBuilder_setDataCallback(b,input?Input:Output,this);AAudioStreamBuilder_setErrorCallback(b,Error,this);
 auto result=AAudioStreamBuilder_openStream(b,stream);AAudioStreamBuilder_delete(b);
 if(result==AAUDIO_OK){__android_log_print(ANDROID_LOG_INFO,"VRConMac-Media","Opened %s: %d Hz, %d channels",input?"microphone":"headphones",AAudioStream_getSampleRate(*stream),AAudioStream_getChannelCount(*stream));if(AAudioStream_getSampleRate(*stream)!=48000||AAudioStream_getChannelCount(*stream)!=(input?1:2)||AAudioStream_getFormat(*stream)!=AAUDIO_FORMAT_PCM_I16){AAudioStream_close(*stream);*stream=nullptr;return;}if(AAudioStream_requestStart(*stream)!=AAUDIO_OK){AAudioStream_close(*stream);*stream=nullptr;}}
 };
 if(!out_)open(false,&out_);if(wantsMic_&&!in_)open(true,&in_);return out_&&(!wantsMic_||in_);
}
void DuplexClient::CloseAudio(){for(auto*p:{&out_,&in_})if(*p){AAudioStream_requestStop(*p);AAudioStream_close(*p);*p=nullptr;}primed_=false;}
void DuplexClient::Run(std::string ip){
 int s=socket(AF_INET,SOCK_DGRAM,0);if(s<0)return;timeval timeout{0,2000};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 sockaddr_in server{};server.sin_family=AF_INET;server.sin_port=htons(Port);if(inet_pton(AF_INET,ip.c_str(),&server.sin_addr)!=1){close(s);return;}connect(s,(sockaddr*)&server,sizeof(server));
 auto lastAck=std::chrono::steady_clock::time_point{};
 uint64_t token=0,lastFace=0;uint32_t sequence=0,lastAudio=0;bool haveAudio=false;auto helloAt=std::chrono::steady_clock::time_point{},audioAt=helloAt;
 while(running_){auto now=std::chrono::steady_clock::now();if(now-helloAt>std::chrono::seconds(1)){Header h;h.type=HelloQuest;send(s,&h,sizeof(h),0);helloAt=now;}
  if(audioError_.exchange(false))CloseAudio();if(now-audioAt>std::chrono::seconds(2)){OpenAudio();audioAt=now;}
  alignas(8) char buffer[1056];auto n=recv(s,buffer,sizeof(buffer),0);if(n>0&&Valid(buffer,n)){auto&h=*(Header*)buffer;if(h.type==Ack){lastAck=now;if(token!=h.token){haveAudio=false;token=h.token;__android_log_print(ANDROID_LOG_INFO,"VRConMac-Media","Runtime media handshake established");}wantsMic_=h.frames==1;if(!wantsMic_&&in_){AAudioStream_requestStop(in_);AAudioStream_close(in_);in_=nullptr;microphone_.Trim(0);}}else if(h.type==Audio&&h.token==token&&token&&(!haveAudio||(int32_t)(h.sequence-lastAudio)>0)){playback_.Push((int16_t*)(buffer+sizeof(h)),480);lastAudio=h.sequence;haveAudio=true;}}
  if(now-lastAck>std::chrono::seconds(3)){wantsMic_=false;if(in_){AAudioStream_requestStop(in_);AAudioStream_close(in_);in_=nullptr;}microphone_.Trim(0);}
  if(!token)continue;
  microphone_.Trim(2400);
  while(microphone_.Available()>=240){Header h;h.type=Microphone;h.token=token;h.sequence=sequence++;h.frames=240;h.channels=1;h.bytes=480;memcpy(buffer,&h,sizeof(h));microphone_.Pop((int16_t*)(buffer+sizeof(h)),240);send(s,buffer,sizeof(h)+480,0);}
  {std::unique_lock<std::mutex>l(faceMutex_,std::try_to_lock);if(l.owns_lock()&&faceGeneration_!=lastFace){Header h;h.type=Face;h.token=token;h.sequence=sequence++;h.bytes=sizeof(face_);memcpy(buffer,&h,sizeof(h));memcpy(buffer+sizeof(h),&face_,sizeof(face_));lastFace=faceGeneration_;l.unlock();send(s,buffer,sizeof(h)+sizeof(FaceSample),0);}}
 }
 CloseAudio();close(s);
}
