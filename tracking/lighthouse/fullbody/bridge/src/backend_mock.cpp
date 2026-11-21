// Mock backend: a fake headset that sways its head, waves its controllers and presses buttons.
// Lets you test the whole Wine side (runtime DLL + OpenComposite + game) without a headset or an
// XR runtime, and optionally dumps the eye images it receives to disk.

#include "backend.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

namespace {

VrcmPose MakePose(float yaw, float pitch, float x, float y, float z)
{
	// yaw around +Y then pitch around +X
	float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
	VrcmPose p;
	p.orientation = { cy * sp, sy * cp, -sy * sp, cy * cp };
	p.position = { x, y, z };
	return p;
}

VrcmVec3 Rotate(const VrcmQuat& q, VrcmVec3 v)
{
	// v' = v + w*t + cross(q.xyz, t), t = 2*cross(q.xyz, v)
	float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
	return { v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
		v.z + q.w * tz + (q.x * ty - q.y * tx) };
}

class MockBackend : public Backend {
public:
	bool Init(const BridgeOptions& opt, VrcmHelloAck& ack) override
	{
		opt_ = opt;
		ack.recommendedWidth = (uint32_t)(1800 * opt.renderScale) & ~1u;
		ack.recommendedHeight = (uint32_t)(1920 * opt.renderScale) & ~1u;
		ack.maxWidth = 4096;
		ack.maxHeight = 4096;
		ack.refreshRateHz = 72.f;
		ack.playAreaWidth = 2.5f;
		ack.playAreaDepth = 2.5f;
		snprintf(ack.systemName, sizeof(ack.systemName), "Mock Headset");
		return true;
	}

	void Run(BridgeServer& server, std::atomic<bool>& quit) override
	{
		const int64_t period = (int64_t)(1e9 / 72.0);
		int64_t next = MonotonicNs();
		int64_t frameId = 0;
		int64_t lastReport = next, lastDump = 0;
		uint64_t lastFrames = 0, haveSerial = 0;
		std::shared_ptr<GuestFrame> frame;

		while (!quit) {
			next += period;
			int64_t now = MonotonicNs();
			if (next > now)
				std::this_thread::sleep_for(std::chrono::nanoseconds(next - now));
			else
				next = now;

			float t = (float)(now / 1e9);
			VrcmTick tick{};
			tick.frameId = ++frameId;
			tick.predictedDisplayTime = next + period * 2;
			tick.predictedPeriod = period;
			tick.shouldRender = 1;
			tick.viewsValid = 1;
			tick.headPose = MakePose(0.4f * std::sin(t * 0.5f), 0.1f * std::sin(t * 0.7f), 0.f, 1.65f, 0.f);
			const float ipd = 0.064f;
			for (int e = 0; e < 2; e++) {
				VrcmVec3 off = Rotate(tick.headPose.orientation, { e == 0 ? -ipd / 2 : ipd / 2, 0, 0 });
				tick.eyePose[e] = tick.headPose;
				tick.eyePose[e].position = { tick.headPose.position.x + off.x, tick.headPose.position.y + off.y,
					tick.headPose.position.z + off.z };
				tick.eyeFov[e] = { e == 0 ? -0.96f : -0.79f, e == 0 ? 0.79f : 0.96f, 0.84f, -0.96f };
			}
			for (int h = 0; h < 2; h++) {
				VrcmHand& hand = tick.hands[h];
				float side = h == 0 ? -1.f : 1.f;
				hand.active = hand.gripValid = hand.aimValid = 1;
				hand.grip = MakePose(0, -0.5f, side * 0.2f, 1.2f + 0.1f * std::sin(t + h), -0.35f);
				hand.aim = MakePose(0, -0.2f, side * 0.2f, 1.2f + 0.1f * std::sin(t + h), -0.40f);
				hand.trigger = 0.5f + 0.5f * std::sin(t * 2.f);
				hand.squeeze = 0.5f + 0.5f * std::cos(t * 2.f);
				hand.stickX = 0.f;
				hand.stickY = 0.f;
				hand.buttons = ((int)t % 4 == 0) ? VRCM_BTN_PRIMARY_CLICK | VRCM_BTN_PRIMARY_TOUCH : 0;
				if (hand.trigger > 0.1f)
					hand.buttons |= VRCM_BTN_TRIGGER_TOUCH;
			}
			server.SendTick(tick);

			VrcmHaptic hp;
			while (server.PopHaptic(hp))
				if (opt_.verbose)
					printf("[mock] haptic hand=%u amp=%.2f dur=%lldns\n", hp.hand, hp.amplitude, (long long)hp.durationNs);

			if (server.TakeLatestFrame(haveSerial, frame))
				haveSerial = frame->serial;

			if (now - lastReport > 2'000'000'000LL) {
				uint64_t f = server.framesReceived.load();
				double secs = (now - lastReport) / 1e9;
				printf("[mock] guest=%s session=%s  frames/s=%.1f  MB/s=%.1f", server.GuestConnected() ? "yes" : "no",
				    server.GuestSessionRunning() ? "running" : "-", (f - lastFrames) / secs,
				    server.bytesReceived.exchange(0) / secs / 1e6);
				if (frame && frame->hdr.eyeCount == 2) {
					int64_t lag = frameId - frame->hdr.frameId;
					printf("  eye %ux%u  frame lag=%lld ticks", frame->eyes[0].hdr.width, frame->eyes[0].hdr.height,
					    (long long)lag);
				}
				printf("\n");
				lastFrames = f;
				lastReport = now;
			}

			if (!opt_.dumpDir.empty() && frame && frame->hdr.eyeCount == 2 &&
			    now - lastDump > (int64_t)(opt_.dumpEverySec * 1e9)) {
				lastDump = now;
				for (int e = 0; e < 2; e++)
					DumpPPM(frame->eyes[e], opt_.dumpDir + "/eye" + std::to_string(e) + "_" + std::to_string(dumpCount_) + ".ppm");
				dumpCount_++;
			}
		}
	}

private:
	static void DumpPPM(const GuestEye& e, const std::string& path)
	{
		FILE* f = fopen(path.c_str(), "wb");
		if (!f) {
			printf("[mock] cannot write %s\n", path.c_str());
			return;
		}
		fprintf(f, "P6\n%u %u\n255\n", e.hdr.width, e.hdr.height);
		std::vector<uint8_t> row(e.hdr.width * 3);
		for (uint32_t y = 0; y < e.hdr.height; y++) {
			const uint8_t* src = e.pixels.data() + (size_t)y * e.hdr.stride;
			for (uint32_t x = 0; x < e.hdr.width; x++) {
				row[x * 3 + 0] = src[x * 4 + 0];
				row[x * 3 + 1] = src[x * 4 + 1];
				row[x * 3 + 2] = src[x * 4 + 2];
			}
			fwrite(row.data(), 1, row.size(), f);
		}
		fclose(f);
		printf("[mock] wrote %s\n", path.c_str());
	}

	BridgeOptions opt_;
	int dumpCount_ = 0;
};

} // namespace

std::unique_ptr<Backend> CreateMockBackend() { return std::make_unique<MockBackend>(); }
