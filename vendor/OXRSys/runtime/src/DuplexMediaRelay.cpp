// SPDX-License-Identifier: MPL-2.0
#include "DuplexMediaRelay.h"
#include <oxrsys/protocol/DuplexMedia.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <random>
#include <spdlog/spdlog.h>
using namespace oxr::media;
void DuplexMediaRelay::Start(){if(running_.exchange(true))return;thread_=std::thread(&DuplexMediaRelay::Run,this);}
void DuplexMediaRelay::Stop(){running_=false;if(thread_.joinable())thread_.join();}
void DuplexMediaRelay::SetClient(const std::string& ip){std::lock_guard<std::mutex> l(mutex_);ip_=ip;}
void DuplexMediaRelay::Run(){
 int s=socket(AF_INET,SOCK_DGRAM,0);if(s<0)return;
 timeval timeout{0,20000};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 sockaddr_in bindAddr{};bindAddr.sin_family=AF_INET;bindAddr.sin_port=htons(Port);bindAddr.sin_addr.s_addr=INADDR_ANY;
 if(bind(s,(sockaddr*)&bindAddr,sizeof(bindAddr))<0){spdlog::warn("Duplex media port {} unavailable",Port);close(s);return;}
 struct Peer{sockaddr_in address{};uint64_t token=0;bool wantsMic=false;std::chrono::steady_clock::time_point seen{};};Peer quest,mac;
 auto same=[](const sockaddr_in&a,const sockaddr_in&b){return a.sin_addr.s_addr==b.sin_addr.s_addr&&a.sin_port==b.sin_port;};
 auto active=[](const Peer&p){return p.token&&std::chrono::steady_clock::now()-p.seen<std::chrono::seconds(3);};
 std::random_device random;
 auto send=[&](const Peer&p,char*b,size_t n){((Header*)b)->token=p.token;sendto(s,b,n,0,(const sockaddr*)&p.address,sizeof(p.address));};
 std::string previousIp;
 while(running_){
  std::string ip;{std::lock_guard<std::mutex>l(mutex_);ip=ip_;}
  if(ip!=previousIp){quest={};previousIp=ip;}
  alignas(8) char buffer[1056];sockaddr_in from{};socklen_t len=sizeof(from);
  auto n=recvfrom(s,buffer,sizeof(buffer),0,(sockaddr*)&from,&len);if(n<0||!Valid(buffer,n))continue;
  auto&h=*(Header*)buffer;bool local=ntohl(from.sin_addr.s_addr)==INADDR_LOOPBACK;
  char address[INET_ADDRSTRLEN];inet_ntop(AF_INET,&from.sin_addr,address,sizeof(address));
  if(h.type==HelloMac||h.type==HelloQuest){
   Peer*peer=nullptr;
   if(h.type==HelloMac&&local)peer=&mac;
   if(h.type==HelloQuest&&!ip.empty()&&ip==address)peer=&quest;
   if(!peer)continue;
   if(!same(peer->address,from)||!peer->token)peer->token=((uint64_t)random()<<32)|random()|1;
   peer->address=from;if(h.type==HelloMac)peer->wantsMic=h.frames==1;peer->seen=std::chrono::steady_clock::now();Header ack;ack.type=Ack;ack.token=peer->token;ack.frames=h.type==HelloQuest ? (active(mac)&&mac.wantsMic ? 1:0) : (active(quest) ? 1:0);
   sendto(s,&ack,sizeof(ack),0,(sockaddr*)&from,sizeof(from));continue;
  }
  if(active(mac)&&same(mac.address,from)&&h.token==mac.token&&h.type==Audio){if(active(quest))send(quest,buffer,n);}
  else if(active(quest)&&same(quest.address,from)&&h.token==quest.token){
   if(h.type==Microphone&&active(mac)&&mac.wantsMic)send(mac,buffer,n);
   else if(h.type==Face){sockaddr_in dest{};dest.sin_family=AF_INET;dest.sin_addr.s_addr=htonl(INADDR_LOOPBACK);dest.sin_port=htons(TrackingPort);sendto(s,buffer,n,0,(sockaddr*)&dest,sizeof(dest));}
  }
 }
 close(s);
}
