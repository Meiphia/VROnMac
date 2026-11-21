// Shared includes, logging, config and small math for the Wine-side runtime.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "../../common/vrcm_protocol.h"

namespace vrcm {

// ---- logging (file next to the DLL + OutputDebugString) ----
void Log(const char* fmt, ...);
#define VLOG(...) ::vrcm::Log(__VA_ARGS__)

// ---- config (vrconmac.ini next to the DLL, overridable by env vars) ----
struct Config {
	std::string host = "127.0.0.1";
	int port = VRCM_DEFAULT_PORT;
	int connectTimeoutMs = 5000;
	int frameLatency = 1;       // 0 = map readback immediately (stalls), 1 = map previous frame's copy
	bool logVerbose = false;
	std::string dllDir;
};
const Config& GetConfig();

// ---- time ----
int64_t NowNs(); // QPC-based monotonic nanoseconds

// ---- math ----
inline XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}
inline XrQuaternionf QConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }
inline XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
	XrQuaternionf p{ v.x, v.y, v.z, 0 };
	XrQuaternionf r = QMul(QMul(q, p), QConj(q));
	return { r.x, r.y, r.z };
}
inline XrVector3f VAdd(const XrVector3f& a, const XrVector3f& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline XrVector3f VSub(const XrVector3f& a, const XrVector3f& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

// a * b : apply b, then a (b expressed in a's frame)
inline XrPosef PMul(const XrPosef& a, const XrPosef& b)
{
	XrPosef r;
	r.orientation = QMul(a.orientation, b.orientation);
	r.position = VAdd(QRot(a.orientation, b.position), a.position);
	return r;
}
inline XrPosef PInv(const XrPosef& p)
{
	XrPosef r;
	r.orientation = QConj(p.orientation);
	r.position = QRot(r.orientation, { -p.position.x, -p.position.y, -p.position.z });
	return r;
}
inline XrPosef PIdentity() { return { { 0, 0, 0, 1 }, { 0, 0, 0 } }; }
inline XrPosef FromWire(const VrcmPose& p)
{
	return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }, { p.position.x, p.position.y, p.position.z } };
}
inline VrcmPose ToWire(const XrPosef& p)
{
	return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }, { p.position.x, p.position.y, p.position.z } };
}

} // namespace vrcm
