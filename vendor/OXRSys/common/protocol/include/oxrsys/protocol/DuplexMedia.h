// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
namespace oxr::media {
constexpr uint32_t Magic=0x314d4356; // VCM1, little endian
#ifndef VRCONMAC_MEDIA_PORT
#define VRCONMAC_MEDIA_PORT 47900
#endif
#ifndef VRCONMAC_TRACKING_PORT
#define VRCONMAC_TRACKING_PORT 47901
#endif
constexpr uint16_t Port=VRCONMAC_MEDIA_PORT, TrackingPort=VRCONMAC_TRACKING_PORT;
enum Type:uint16_t {HelloQuest=1,Audio=2,Microphone=3,Face=4,Ack=6,HelloMac=7};
#pragma pack(push,1)
struct Header { uint32_t magic=Magic; uint16_t version=1,type=0; uint32_t sequence=0,frames=0,sampleRate=48000; uint16_t channels=0,bytes=0; uint64_t token=0; };
struct FaceSample { uint32_t flags=0,count=70; uint64_t time=0; float eye[2][4]={{0,0,0,1},{0,0,0,1}}; float confidence[2]={}; float weights[70]={}; };
#pragma pack(pop)
static_assert(sizeof(Header)==32 && sizeof(FaceSample)==336);
inline bool Valid(const void* data,size_t size) {
 if(size<sizeof(Header)||size>1056)return false;
 Header h;memcpy(&h,data,sizeof(h));
 if(h.magic!=Magic||h.version!=1||size!=sizeof(h)+h.bytes)return false;
 if(h.type==HelloQuest||h.type==HelloMac||h.type==Ack)return h.bytes==0;
 if(h.type==Audio||h.type==Microphone)return h.sampleRate==48000&&h.frames==240&&h.channels==(h.type==Audio?2:1)&&h.bytes==h.frames*h.channels*2;
 if(h.type!=Face||h.bytes!=sizeof(FaceSample))return false;
 FaceSample f;memcpy(&f,(const char*)data+sizeof(h),sizeof(f));
 if(f.count!=70||(f.flags&~3u))return false;
 for(auto w:f.weights)if(!std::isfinite(w)||w<0||w>1)return false;
 for(auto& eye:f.eye)for(float q:eye)if(!std::isfinite(q))return false;
 for(float c:f.confidence)if(!std::isfinite(c)||c<0||c>1)return false;
 return true;
}
}
