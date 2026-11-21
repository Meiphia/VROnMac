// SPDX-License-Identifier: MPL-2.0
#include "MicRing.h"
#include "PcmRing.h"
#include <atomic>
#include <array>
#include <cstring>
#include <cmath>
struct MicState {
 PcmRing<9600> ring;bool primed=false;size_t fade=0;
 std::atomic<uint64_t> rendered{0},underruns{0},trimmed{0};std::atomic<int> peak{0};
};
extern "C" void* vrcm_mic_create(){return new MicState();}
extern "C" void vrcm_mic_destroy(void* state){delete static_cast<MicState*>(state);}
extern "C" void vrcm_mic_push(void* state,const int16_t* data,size_t count){static_cast<MicState*>(state)->ring.Push(data,count);}
extern "C" void vrcm_mic_render(void* state,float* output,size_t frames){
 auto& s=*static_cast<MicState*>(state);std::memset(output,0,frames*sizeof(float));
 if(frames>4096)return;
 auto available=s.ring.Available();
 if(available>4800){s.ring.Trim(1920);s.trimmed.fetch_add(available-1920,std::memory_order_relaxed);s.fade=64;}
 if(!s.primed){if(s.ring.Available()<1920)return;s.primed=true;s.fade=64;}
 std::array<int16_t,4096> samples;auto count=s.ring.Pop(samples.data(),frames);int peak=0;
 for(size_t i=0;i<count;++i){float value=float(samples[i])/32768;peak=std::max(peak,std::abs(int(samples[i])));if(s.fade){value*=float(65-s.fade)/64;--s.fade;}output[i]=value;}
 if(count<frames){size_t fade=std::min(size_t(64),count);for(size_t i=0;i<fade;++i)output[count-fade+i]*=float(fade-i-1)/fade;s.primed=false;s.underruns.fetch_add(1,std::memory_order_relaxed);}
 s.rendered.fetch_add(count,std::memory_order_relaxed);s.peak.store(std::max(s.peak.load(std::memory_order_relaxed),peak),std::memory_order_relaxed);
}
extern "C" void vrcm_mic_stats(void* state,uint64_t* rendered,uint64_t* underruns,uint64_t* trimmed,int* peak){auto&s=*static_cast<MicState*>(state);*rendered=s.rendered.load();*underruns=s.underruns.load();*trimmed=s.trimmed.load();*peak=s.peak.load();}
