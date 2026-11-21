// TCP client to the macOS bridge. A receiver thread keeps the latest TICK; a sender thread
// ships eye frames from a "latest wins" mailbox so the game's render thread never blocks on
// the socket.
#pragma once
#include "common.h"
#include "../../common/tracker_cache.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace vrcm {

class BridgeClient {
public:
	static BridgeClient& Get();

	// Connects + handshakes if not already connected. Thread-safe.
	bool EnsureConnected(const char* appName);
	void RequestReconnect(const std::string& appName);
	uint64_t ConnectionGeneration() const { return generation_.load(); }
	bool IsConnected() const { return connected_.load(); }
	const VrcmHelloAck& Info() const { return ack_; }

	// Latest tick. Returns false if none has arrived yet.
	bool LatestTick(VrcmTick& out) const;
	// Tick whose predictedDisplayTime is closest to `time` (falls back to latest).
	bool TickNearest(int64_t time, VrcmTick& out) const;
	// Block until a tick with frameId > afterFrameId arrives (or timeout). Returns false on timeout.
	bool WaitTick(int64_t afterFrameId, int timeoutMs, VrcmTick& out);

	// Frame mailbox: caller fills a buffer obtained with BeginFrame, then Commit.
	// Returns a buffer of at least `bytes`, already containing room for the payload.
	std::vector<uint8_t>& BeginFrame(size_t bytes);
	void CommitFrame();

	void SendHaptic(const VrcmHaptic& h);
	void SendSessionState(bool running);

	void Disconnect();
	std::vector<VrcmTracker> Trackers(uint64_t* generation=nullptr) const { return trackers_.Snapshot(NowNs(),generation); }

private:
	BridgeClient() = default;
	void PollTrackers();
	TrackerCache trackers_;
	SOCKET trackerSocket_ = INVALID_SOCKET;
	bool SendRaw(uint16_t type, const void* payload, uint32_t size);
	bool SendRaw2(uint16_t type, const void* a, uint32_t aSize, const void* b, uint32_t bSize);
	bool RecvAll(void* dst, size_t n);
	void RecvLoop();
	void SendLoop();

	mutable std::mutex connectMutex_;
	std::atomic<bool> connected_{ false };
	std::atomic<SOCKET> sock_{INVALID_SOCKET};
	std::atomic<uint64_t> generation_{0};
	std::atomic<bool> reconnecting_{false};
	std::atomic<int64_t> retryAfter_{0};
	VrcmHelloAck ack_{};

	std::mutex sendMutex_; // serialises writes to the socket

	mutable std::mutex tickMutex_;
	std::condition_variable tickCv_;
	VrcmTick tick_{};
	bool haveTick_ = false;
	static const int kHistory = 32;
	VrcmTick history_[kHistory]{};
	int historyCount_ = 0, historyHead_ = 0;

	std::thread recvThread_, sendThread_;
	std::atomic<bool> stop_{ false };

	// mailbox
	std::mutex mailMutex_;
	std::condition_variable mailCv_;
	std::vector<uint8_t> writing_, pending_;
	bool hasPending_ = false;
	std::atomic<uint64_t> framesSent_{ 0 }, framesDropped_{ 0 };
};

} // namespace vrcm
