#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

int64_t MonotonicNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

BridgeServer::BridgeServer(int port) : port_(port) {}
BridgeServer::~BridgeServer() { Stop(); }

bool BridgeServer::Start()
{
	listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd_ < 0) {
		perror("socket");
		return false;
	}
	int one = 1;
	setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port_);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (const char* any = getenv("VRCONMAC_LISTEN_ANY"); any && any[0] == '1')
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
		perror("bind");
		close(listenFd_);
		listenFd_ = -1;
		return false;
	}
	if (listen(listenFd_, 2) != 0) {
		perror("listen");
		return false;
	}
	printf("[bridge] listening on %s:%d\n", addr.sin_addr.s_addr == htonl(INADDR_ANY) ? "0.0.0.0" : "127.0.0.1", port_);
	acceptThread_ = std::thread(&BridgeServer::AcceptLoop, this);
	return true;
}

void BridgeServer::Stop()
{
	if (stop_.exchange(true))
		return;
	int c = clientFd_.exchange(-1);
	if (c >= 0) {
		VrcmMsgHeader h{ VRCM_MAGIC, VRCM_MSG_SHUTDOWN, 0, 0 };
		(void)!write(c, &h, sizeof(h));
		shutdown(c, SHUT_RDWR);
		close(c);
	}
	if (listenFd_ >= 0) {
		shutdown(listenFd_, SHUT_RDWR);
		close(listenFd_);
		listenFd_ = -1;
	}
	if (acceptThread_.joinable())
		acceptThread_.join();
}

std::string BridgeServer::GuestName()
{
	std::lock_guard<std::mutex> l(nameMutex_);
	return guestName_;
}

static bool ReadAll(int fd, void* dst, size_t n)
{
	char* p = (char*)dst;
	while (n) {
		ssize_t r = recv(fd, p, n, 0);
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

static bool WriteAll(int fd, const void* src, size_t n)
{
	const char* p = (const char*)src;
	while (n) {
#ifdef MSG_NOSIGNAL
		ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
#else
		ssize_t r = send(fd, p, n, 0);
#endif
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

bool BridgeServer::SendMsg(uint16_t type, const void* p, uint32_t size)
{
	std::lock_guard<std::mutex> l(sendMutex_);
	int fd = clientFd_.load();
	if (fd < 0)
		return false;
	VrcmMsgHeader h{ VRCM_MAGIC, type, 0, size };
	return WriteAll(fd, &h, sizeof(h)) && (size == 0 || WriteAll(fd, p, size));
}

void BridgeServer::SendTick(const VrcmTick& t)
{
	if (connected_)
		SendMsg(VRCM_MSG_TICK, &t, sizeof(t));
}

void BridgeServer::AcceptLoop()
{
	while (!stop_) {
		sockaddr_in peer{};
		socklen_t len = sizeof(peer);
		int fd = accept(listenFd_, (sockaddr*)&peer, &len);
		if (fd < 0) {
			if (stop_)
				break;
			continue;
		}
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		int rcvbuf = 8 * 1024 * 1024;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

		if (clientFd_.load() >= 0) {
			printf("[bridge] rejecting second guest connection\n");
			close(fd);
			continue;
		}
		ClientLoop(fd); // one guest at a time; handled on this thread
	}
}

void BridgeServer::ClientLoop(int fd)
{
	VrcmMsgHeader h;
	if (!ReadAll(fd, &h, sizeof(h)) || h.magic != VRCM_MAGIC || h.type != VRCM_MSG_HELLO || h.size < sizeof(VrcmHello)) {
		printf("[bridge] bad hello, dropping connection\n");
		close(fd);
		return;
	}
	std::vector<uint8_t> buf(h.size);
	if (!ReadAll(fd, buf.data(), buf.size())) {
		close(fd);
		return;
	}
	VrcmHello hello;
	memcpy(&hello, buf.data(), sizeof(hello));
	hello.appName[sizeof(hello.appName) - 1] = 0;
	{
		std::lock_guard<std::mutex> l(nameMutex_);
		guestName_ = hello.appName;
	}
	if (hello.protocolVersion != VRCM_PROTOCOL_VERSION)
		printf("[bridge] WARNING: guest protocol %u, bridge %u\n", hello.protocolVersion, VRCM_PROTOCOL_VERSION);

	clientFd_ = fd;
	VrcmHelloAck ack = helloAck;
	ack.protocolVersion = VRCM_PROTOCOL_VERSION;
	if (!SendMsg(VRCM_MSG_HELLO_ACK, &ack, sizeof(ack))) {
		clientFd_ = -1;
		close(fd);
		return;
	}
	connected_ = true;
	printf("[bridge] guest connected: '%s'\n", hello.appName);

	while (!stop_) {
		if (!ReadAll(fd, &h, sizeof(h)) || h.magic != VRCM_MAGIC)
			break;
		bytesReceived += sizeof(h) + h.size;
		if (h.type == VRCM_MSG_FRAME) {
			auto f = std::make_shared<GuestFrame>();
			if (h.size < sizeof(VrcmFrame) || !ReadAll(fd, &f->hdr, sizeof(VrcmFrame)))
				break;
			uint32_t remaining = h.size - sizeof(VrcmFrame);
			bool ok = true;
			if (f->hdr.eyeCount > 2) {
				printf("[bridge] bad eyeCount %u\n", f->hdr.eyeCount);
				break;
			}
			for (uint32_t i = 0; i < f->hdr.eyeCount && ok; i++) {
				GuestEye& e = f->eyes[i];
				if (remaining < sizeof(VrcmEyeImage) || !ReadAll(fd, &e.hdr, sizeof(VrcmEyeImage))) {
					ok = false;
					break;
				}
				remaining -= sizeof(VrcmEyeImage);
				size_t bytes = (size_t)e.hdr.stride * e.hdr.height;
				if (bytes > remaining || e.hdr.stride < e.hdr.width * 4) {
					printf("[bridge] bad eye image header\n");
					ok = false;
					break;
				}
				e.pixels.resize(bytes);
				if (!ReadAll(fd, e.pixels.data(), bytes)) {
					ok = false;
					break;
				}
				remaining -= (uint32_t)bytes;
			}
			if (!ok)
				break;
			if (remaining) {
				std::vector<uint8_t> skip(remaining);
				if (!ReadAll(fd, skip.data(), remaining))
					break;
			}
			f->receivedAtNs = MonotonicNs();
			{
				std::lock_guard<std::mutex> l(frameMutex_);
				f->serial = ++serial_;
				latest_ = std::move(f);
			}
			framesReceived++;
			continue;
		}

		buf.resize(h.size);
		if (h.size && !ReadAll(fd, buf.data(), h.size))
			break;
		switch (h.type) {
		case VRCM_MSG_HAPTIC:
			if (h.size >= sizeof(VrcmHaptic)) {
				VrcmHaptic hp;
				memcpy(&hp, buf.data(), sizeof(hp));
				std::lock_guard<std::mutex> l(hapticMutex_);
				if (haptics_.size() < 64)
					haptics_.push_back(hp);
			}
			break;
		case VRCM_MSG_SESSION_STATE:
			if (h.size >= sizeof(VrcmSessionState)) {
				VrcmSessionState s;
				memcpy(&s, buf.data(), sizeof(s));
				guestRunning_ = s.running != 0;
				printf("[bridge] guest session %s\n", s.running ? "running" : "stopped");
			}
			break;
		default:
			break;
		}
	}

	printf("[bridge] guest disconnected\n");
	connected_ = false;
	guestRunning_ = false;
	{
		std::lock_guard<std::mutex> l(sendMutex_);
		clientFd_ = -1;
	}
	close(fd);
	std::lock_guard<std::mutex> l(frameMutex_);
	latest_.reset();
}

bool BridgeServer::TakeLatestFrame(uint64_t haveSerial, std::shared_ptr<GuestFrame>& out)
{
	std::lock_guard<std::mutex> l(frameMutex_);
	if (!latest_ || latest_->serial <= haveSerial)
		return false;
	out = latest_;
	return true;
}

bool BridgeServer::PopHaptic(VrcmHaptic& h)
{
	std::lock_guard<std::mutex> l(hapticMutex_);
	if (haptics_.empty())
		return false;
	h = haptics_.front();
	haptics_.pop_front();
	return true;
}
