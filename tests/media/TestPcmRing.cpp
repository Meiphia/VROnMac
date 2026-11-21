#include <oxrsys/protocol/PcmRing.h>
#include <cassert>
#include <thread>
#include <iostream>
int main(){PcmRing<16> ring;int16_t a[20],b[20]{};for(int i=0;i<20;i++)a[i]=i;ring.Push(a,8);assert(ring.Pop(b,5)==5&&b[4]==4);ring.Push(a+8,8);assert(ring.Pop(b,16)==11&&b[0]==5&&b[10]==15);ring.Push(a,17);assert(ring.Available()==0);ring.Push(a,16);ring.Trim(4);assert(ring.Pop(b,10)==4&&b[0]==12);PcmRing<1024> stress;std::thread producer([&]{for(int i=0;i<100000;i++){int16_t v=i%30000;while(stress.Available()>=1024)std::this_thread::yield();stress.Push(&v,1);}});for(int i=0;i<100000;i++){int16_t v;while(stress.Pop(&v,1)==0)std::this_thread::yield();assert(v==i%30000);}producer.join();std::cout<<"PASS: PCM wrap, overflow, underrun, backlog trim, concurrent sequence\n";}
