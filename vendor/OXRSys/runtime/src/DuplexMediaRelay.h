// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
class DuplexMediaRelay {
public:
 ~DuplexMediaRelay(){Stop();}
 void Start(); void Stop(); void SetClient(const std::string& ip);
private:
 void Run(); std::atomic<bool> running_{false}; std::thread thread_; std::mutex mutex_; std::string ip_;
};
