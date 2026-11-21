// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <oxrsys/protocol/DuplexMedia.h>
#include <aaudio/AAudio.h>
#include <atomic>
#include <array>
#include <thread>
#include <mutex>
#include <string>
#include <oxrsys/protocol/PcmRing.h>
class DuplexClient {
public:
 ~DuplexClient(){Stop();} void Start(const char*ip);void Stop();void SetFace(const oxr::media::FaceSample&face);
private:
 void Run(std::string ip); bool OpenAudio();void CloseAudio();
 static aaudio_data_callback_result_t Output(AAudioStream*,void*,void*,int32_t);
 static aaudio_data_callback_result_t Input(AAudioStream*,void*,void*,int32_t);
 static void Error(AAudioStream*,void*,aaudio_result_t);
 std::atomic<bool> running_{false},audioError_{false};std::thread thread_;AAudioStream*out_=nullptr,*in_=nullptr;
 PcmRing<19200> playback_,microphone_;bool primed_=false,wantsMic_=false;
 std::mutex faceMutex_;oxr::media::FaceSample face_;uint64_t faceGeneration_=0;
};
