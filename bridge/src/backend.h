#pragma once
#include "server.h"

#include <atomic>
#include <memory>
#include <string>

struct BridgeOptions {
	std::string backend = "openxr";  // openxr | mock
	std::string runtimeJson;         // OpenXR runtime manifest (OXRSys, Meta XR Simulator...)
	int port = VRCM_DEFAULT_PORT;
	float renderScale = 1.0f;        // scales the per-eye size we ask the guest to render
	float floorHeight = 1.6f;        // used when the runtime has no STAGE space
	std::string dumpDir;             // mock: write received eye images here as PPM
	float dumpEverySec = 5.0f;
	bool testPattern = false;        // openxr: show a pattern when no guest frames arrive
	bool verbose = false;
};

// A backend owns the host-side XR loop: it produces TICKs and consumes guest frames.
class Backend {
public:
	virtual ~Backend() = default;
	// Bring up the XR side and fill in what the guest needs to know (sizes, refresh, bounds).
	virtual bool Init(const BridgeOptions& opt, VrcmHelloAck& ack) = 0;
	// Run until `quit` is set.
	virtual void Run(BridgeServer& server, std::atomic<bool>& quit) = 0;
};

std::unique_ptr<Backend> CreateMockBackend();
std::unique_ptr<Backend> CreateOpenXRBackend(); // nullptr when not built (non-Apple)
