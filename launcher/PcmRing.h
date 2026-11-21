// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstddef>
// Single producer/consumer. Only consumer advances the read cursor.
template<size_t N> class PcmRing {
 std::array<int16_t,N> data_{};std::atomic<uint64_t> read_{0},write_{0};
public:
 size_t Available()const{return write_.load(std::memory_order_acquire)-read_.load(std::memory_order_acquire);}
 void Push(const int16_t*p,size_t n){auto w=write_.load(std::memory_order_relaxed);auto r=read_.load(std::memory_order_acquire);if(n>N-(w-r))return;for(size_t i=0;i<n;i++)data_[(w+i)%N]=p[i];write_.store(w+n,std::memory_order_release);}
 size_t Pop(int16_t*p,size_t n){auto r=read_.load(std::memory_order_relaxed),w=write_.load(std::memory_order_acquire);n=std::min(n,(size_t)(w-r));for(size_t i=0;i<n;i++)p[i]=data_[(r+i)%N];read_.store(r+n,std::memory_order_release);return n;}
 void Trim(size_t keep){auto r=read_.load(std::memory_order_relaxed),w=write_.load(std::memory_order_acquire);if(w-r>keep)read_.store(w-keep,std::memory_order_release);}
};
