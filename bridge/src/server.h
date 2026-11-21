// TCP server the Wine-side runtime connects to. One guest at a time.
#pragma once
#include "../../common/vrcm_protocol.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GuestEye {
	VrcmEyeImage hdr{};
	std::vector<uint8_t> pixels; // height * stride bytes
};

struct GuestFrame {
	VrcmFrame hdr{};
	GuestEye eyes[2];
	int64_t receivedAtNs = 0;
	uint64_t serial = 0; // increments per received frame
};

class BridgeServer {
public:
	explicit BridgeServer(int port);
	~BridgeServer();

	// Info sent to every guest in HELLO_ACK. Set before Start().
	VrcmHelloAck helloAck{};

	bool Start();
	void Stop();

	bool GuestConnected() const { return connected_.load(); }
	bool GuestSessionRunning() const { return guestRunning_.load(); }
	std::string GuestName();

	void SendTick(const VrcmTick& t);

	// Latest frame from the guest. Returns false if none. Swaps into `out` cheaply when newer than
	// `haveSerial`; returns true only if `out` was updated.
	bool TakeLatestFrame(uint64_t haveSerial, std::shared_ptr<GuestFrame>& out);

	bool PopHaptic(VrcmHaptic& h);

	// stats
	std::atomic<uint64_t> framesReceived{ 0 };
	std::atomic<uint64_t> bytesReceived{ 0 };

private:
	void AcceptLoop();
	void ClientLoop(int fd);
	bool SendMsg(uint16_t type, const void* p, uint32_t size);

	int port_;
	int listenFd_ = -1;
	std::atomic<int> clientFd_{ -1 };
	std::atomic<bool> stop_{ false };
	std::atomic<bool> connected_{ false };
	std::atomic<bool> guestRunning_{ false };
	std::thread acceptThread_;
	std::mutex sendMutex_;

	std::mutex frameMutex_;
	std::shared_ptr<GuestFrame> latest_;
	uint64_t serial_ = 0;

	std::mutex hapticMutex_;
	std::deque<VrcmHaptic> haptics_;

	std::mutex nameMutex_;
	std::string guestName_;
};

int64_t MonotonicNs();
