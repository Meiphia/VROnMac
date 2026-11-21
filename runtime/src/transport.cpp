#include "transport.h"

#include <chrono>

namespace vrcm {

BridgeClient& BridgeClient::Get()
{
	static BridgeClient* inst = new BridgeClient(); // intentionally leaked: avoid shutdown-order issues
	return *inst;
}

void BridgeClient::RequestReconnect(const std::string& appName)
{
	if (connected_ || NowNs() < retryAfter_ || reconnecting_.exchange(true))
		return;
	retryAfter_ = NowNs() + 1000000000;
	try {
		std::thread([this, appName] {
			try {
				if (EnsureConnected(appName.c_str()))
					SendSessionState(true);
			} catch (...) {
				VLOG("Bridge reconnect failed");
			}
			reconnecting_ = false;
		}).detach();
	} catch (...) {
		reconnecting_ = false;
	}
}

bool BridgeClient::EnsureConnected(const char* appName)
{
	std::lock_guard<std::mutex> lock(connectMutex_);
	if (connected_.load())
		return true;

	// Clean up a previous dead connection
	if (recvThread_.joinable() || sendThread_.joinable()) {
		stop_ = true;
		mailCv_.notify_all();
		if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_BOTH);
		if (recvThread_.joinable()) recvThread_.join();
		if (sendThread_.joinable()) sendThread_.join();
		if (sock_ != INVALID_SOCKET) closesocket(sock_.exchange(INVALID_SOCKET));
		{
			std::lock_guard<std::mutex> lock(mailMutex_);
			pending_.clear();
			hasPending_ = false;
		}
	}
	stop_ = false;

	static bool wsaInit = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
	if (!wsaInit) {
		VLOG("WSAStartup failed");
		return false;
	}

	const Config& cfg = GetConfig();
	char portStr[16];
	snprintf(portStr, sizeof(portStr), "%d", cfg.port);

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.connectTimeoutMs);
	SOCKET s = INVALID_SOCKET;
	int attempts = 0;
	while (s == INVALID_SOCKET) {
		addrinfo hints{}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		if (getaddrinfo(cfg.host.c_str(), portStr, &hints, &res) == 0 && res) {
			s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
			if (s != INVALID_SOCKET && connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
				closesocket(s);
				s = INVALID_SOCKET;
			}
			freeaddrinfo(res);
		}
		attempts++;
		if (s == INVALID_SOCKET) {
			if (std::chrono::steady_clock::now() > deadline) {
				VLOG("Could not connect to bridge at %s:%d after %d attempts (is vrconmac-bridge running?)",
				    cfg.host.c_str(), cfg.port, attempts);
				return false;
			}
			Sleep(250);
		}
	}

	BOOL nodelay = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
	int sndbuf = 8 * 1024 * 1024;
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
	sock_ = s;
	DWORD handshakeTimeout = 1500;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&handshakeTimeout, sizeof(handshakeTimeout));

	VrcmHello hello{};
	hello.protocolVersion = VRCM_PROTOCOL_VERSION;
	strncpy(hello.appName, appName ? appName : "unknown", sizeof(hello.appName) - 1);
	if (!SendRaw(VRCM_MSG_HELLO, &hello, sizeof(hello))) {
		VLOG("Failed to send HELLO");
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return false;
	}

	VrcmMsgHeader hdr{};
	if (!RecvAll(&hdr, sizeof(hdr)) || hdr.magic != VRCM_MAGIC || hdr.type != VRCM_MSG_HELLO_ACK || hdr.size != sizeof(VrcmHelloAck)) {
		VLOG("Bad HELLO_ACK from bridge (magic=%08x type=%u size=%u)", hdr.magic, hdr.type, hdr.size);
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return false;
	}
	std::vector<uint8_t> buf(hdr.size);
	if (!RecvAll(buf.data(), buf.size())) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return false;
	}
	VrcmHelloAck nextAck{};
	memcpy(&nextAck, buf.data(), sizeof(nextAck));
	if (nextAck.protocolVersion != VRCM_PROTOCOL_VERSION) {
		VLOG("Protocol mismatch: bridge=%u runtime=%u", nextAck.protocolVersion, VRCM_PROTOCOL_VERSION);
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
		return false;
	}

	handshakeTimeout = 0;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&handshakeTimeout, sizeof(handshakeTimeout));
	// Keep negotiated properties stable for the lifetime of the OpenXR session.
	if (!generation_) ack_ = nextAck;

	VLOG("Connected to bridge '%s' at %s:%d: %ux%u per eye @ %.1f Hz, play area %.2fx%.2f m",
	    ack_.systemName, cfg.host.c_str(), cfg.port, ack_.recommendedWidth, ack_.recommendedHeight,
	    ack_.refreshRateHz, ack_.playAreaWidth, ack_.playAreaDepth);

	{
		std::lock_guard<std::mutex> tl(tickMutex_);
		haveTick_ = false;
		historyCount_ = historyHead_ = 0;
	}
	++generation_;
	connected_ = true;
	recvThread_ = std::thread(&BridgeClient::RecvLoop, this);
	sendThread_ = std::thread(&BridgeClient::SendLoop, this);
	return true;
}

void BridgeClient::Disconnect()
{
	std::lock_guard<std::mutex> lock(connectMutex_);
	stop_ = true;
	connected_ = false;
	mailCv_.notify_all();
	tickCv_.notify_all();
	if (sock_ != INVALID_SOCKET) {
		shutdown(sock_, SD_BOTH);
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
	}
	if (recvThread_.joinable()) recvThread_.join();
	if (sendThread_.joinable()) sendThread_.join();
}

bool BridgeClient::RecvAll(void* dst, size_t n)
{
	char* p = (char*)dst;
	while (n > 0) {
		int r = recv(sock_, p, (int)std::min<size_t>(n, 1 << 20), 0);
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

static bool SendAll(SOCKET s, const void* src, size_t n)
{
	const char* p = (const char*)src;
	while (n > 0) {
		int r = send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

bool BridgeClient::SendRaw(uint16_t type, const void* payload, uint32_t size)
{
	return SendRaw2(type, payload, size, nullptr, 0);
}

bool BridgeClient::SendRaw2(uint16_t type, const void* a, uint32_t aSize, const void* b, uint32_t bSize)
{
	std::lock_guard<std::mutex> lock(sendMutex_);
	if (sock_ == INVALID_SOCKET)
		return false;
	VrcmMsgHeader hdr{ VRCM_MAGIC, type, 0, aSize + bSize };
	if (!SendAll(sock_, &hdr, sizeof(hdr)))
		return false;
	if (aSize && !SendAll(sock_, a, aSize))
		return false;
	if (bSize && !SendAll(sock_, b, bSize))
		return false;
	return true;
}

void BridgeClient::RecvLoop()
{
	std::vector<uint8_t> buf;
	uint64_t ticks = 0;
	while (!stop_) {
		VrcmMsgHeader hdr;
		if (!RecvAll(&hdr, sizeof(hdr)) || hdr.magic != VRCM_MAGIC) {
			if (!stop_)
				VLOG("Bridge connection lost (recv)");
			break;
		}
		buf.resize(hdr.size);
		if (hdr.size && !RecvAll(buf.data(), hdr.size)) {
			if (!stop_)
				VLOG("Bridge connection lost (payload)");
			break;
		}
		switch (hdr.type) {
		case VRCM_MSG_TICK:
			if (hdr.size >= sizeof(VrcmTick)) {
				std::lock_guard<std::mutex> lock(tickMutex_);
				memcpy(&tick_, buf.data(), sizeof(VrcmTick));
				haveTick_ = true;
				history_[historyHead_] = tick_;
				historyHead_ = (historyHead_ + 1) % kHistory;
				if (historyCount_ < kHistory)
					historyCount_++;
				ticks++;
				if (ticks == 1)
					VLOG("First TICK received (frame %lld)", (long long)tick_.frameId);
			}
			tickCv_.notify_all();
			break;
		case VRCM_MSG_SHUTDOWN:
			VLOG("Bridge requested shutdown");
			stop_ = true;
			break;
		default:
			break;
		}
	}
	connected_ = false;
	tickCv_.notify_all();
	mailCv_.notify_all();
}

void BridgeClient::SendLoop()
{
	std::vector<uint8_t> local;
	auto lastReport = std::chrono::steady_clock::now();
	while (true) {
		{
			std::unique_lock<std::mutex> lock(mailMutex_);
			mailCv_.wait(lock, [&] { return hasPending_ || stop_ || !connected_; });
			if (stop_ || !connected_)
				break;
			local.swap(pending_);
			hasPending_ = false;
		}
		if (!SendRaw(VRCM_MSG_FRAME, local.data(), (uint32_t)local.size())) {
			VLOG("Bridge connection lost (send)");
			connected_ = false;
			break;
		}
		framesSent_++;
		auto now = std::chrono::steady_clock::now();
		if (now - lastReport > std::chrono::seconds(10)) {
			lastReport = now;
			VLOG("Frames sent: %llu, dropped (bridge/socket too slow): %llu",
			    (unsigned long long)framesSent_.load(), (unsigned long long)framesDropped_.load());
		}
	}
}

std::vector<uint8_t>& BridgeClient::BeginFrame(size_t bytes)
{
	writing_.resize(bytes);
	return writing_;
}

void BridgeClient::CommitFrame()
{
	{
		std::lock_guard<std::mutex> lock(mailMutex_);
		if (hasPending_)
			framesDropped_++;
		pending_.swap(writing_);
		hasPending_ = true;
	}
	mailCv_.notify_one();
}

bool BridgeClient::LatestTick(VrcmTick& out) const
{
	std::lock_guard<std::mutex> lock(tickMutex_);
	if (!haveTick_)
		return false;
	out = tick_;
	return true;
}

bool BridgeClient::TickNearest(int64_t time, VrcmTick& out) const
{
	std::lock_guard<std::mutex> lock(tickMutex_);
	if (!haveTick_)
		return false;
	out = tick_;
	int64_t best = INT64_MAX;
	for (int i = 0; i < historyCount_; i++) {
		int64_t d = history_[i].predictedDisplayTime - time;
		if (d < 0)
			d = -d;
		if (d < best) {
			best = d;
			out = history_[i];
		}
	}
	return true;
}

bool BridgeClient::WaitTick(int64_t afterFrameId, int timeoutMs, VrcmTick& out)
{
	std::unique_lock<std::mutex> lock(tickMutex_);
	bool ok = tickCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
	    [&] { return (haveTick_ && tick_.frameId > afterFrameId) || !connected_.load(); });
	if (haveTick_)
		out = tick_;
	return ok && haveTick_ && tick_.frameId > afterFrameId;
}

void BridgeClient::SendHaptic(const VrcmHaptic& h)
{
	if (connected_)
		SendRaw(VRCM_MSG_HAPTIC, &h, sizeof(h));
}

void BridgeClient::SendSessionState(bool running)
{
	VrcmSessionState s{ running ? 1u : 0u };
	if (connected_)
		SendRaw(VRCM_MSG_SESSION_STATE, &s, sizeof(s));
}

} // namespace vrcm
