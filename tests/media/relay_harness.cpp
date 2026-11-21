// SPDX-License-Identifier: MPL-2.0
#include "DuplexMediaRelay.h"
#include <iostream>
#include <thread>
#include <chrono>
int main(){DuplexMediaRelay relay;relay.SetClient("127.0.0.1");relay.Start();std::cout<<"READY"<<std::endl;std::this_thread::sleep_for(std::chrono::seconds(8));relay.Stop();}
