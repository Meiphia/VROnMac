// vrconmac-bridge: native macOS side of VRConMac.
//
//   vrconmac-bridge [--backend openxr|mock] [--runtime-json PATH] [--port N]
//                   [--render-scale F] [--floor-height M] [--dump-dir DIR] [--test-pattern] [--verbose]

#include "backend.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::atomic<bool> g_quit{ false };
static void OnSignal(int) { g_quit = true; }

static void Usage()
{
	printf("vrconmac-bridge - streams a Wine-side OpenXR app into a native macOS OpenXR runtime\n\n"
	       "  --backend openxr|mock   openxr (default): drive a real runtime (OXRSys, Meta XR Simulator)\n"
	       "                          mock: fake headset, no XR runtime needed (for testing the guest side)\n"
	       "  --runtime-json PATH     runtime manifest (default: $XR_RUNTIME_JSON, then\n"
	       "                          ~/.config/openxr/1/active_runtime.json)\n"
	       "  --port N                TCP port the Wine-side runtime connects to (default %d)\n"
	       "  --render-scale F        scale the per-eye resolution the game renders at (default 1.0;\n"
	       "                          0.5-0.7 is a good start - frames are copied through the CPU)\n"
	       "  --floor-height M        eye height to fake a floor when the runtime has no STAGE (1.6)\n"
	       "  --dump-dir DIR          mock: save received eye images as PPM files\n"
	       "  --test-pattern          openxr: show a test pattern while no game is connected\n"
	       "  --verbose\n",
	    VRCM_DEFAULT_PORT);
}

int main(int argc, char** argv)
{
	BridgeOptions opt;
	for (int i = 1; i < argc; i++) {
		auto arg = [&](const char* n) { return strcmp(argv[i], n) == 0 && i + 1 < argc; };
		if (arg("--backend")) opt.backend = argv[++i];
		else if (arg("--runtime-json")) opt.runtimeJson = argv[++i];
		else if (arg("--port")) opt.port = atoi(argv[++i]);
		else if (arg("--render-scale")) opt.renderScale = (float)atof(argv[++i]);
		else if (arg("--floor-height")) opt.floorHeight = (float)atof(argv[++i]);
		else if (arg("--dump-dir")) opt.dumpDir = argv[++i];
		else if (arg("--dump-every")) opt.dumpEverySec = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--verbose")) opt.verbose = true;
		else if (!strcmp(argv[i], "--test-pattern")) opt.testPattern = true;
		else { Usage(); return strcmp(argv[i], "--help") == 0 ? 0 : 2; }
	}
	if (opt.renderScale <= 0.05f || opt.renderScale > 2.f) {
		fprintf(stderr, "--render-scale out of range\n");
		return 2;
	}
	setvbuf(stdout, nullptr, _IOLBF, 0);
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	signal(SIGPIPE, SIG_IGN);

	std::unique_ptr<Backend> backend;
	if (opt.backend == "mock")
		backend = CreateMockBackend();
	else if (opt.backend == "openxr")
		backend = CreateOpenXRBackend();
	if (!backend) {
		fprintf(stderr, "backend '%s' not available in this build\n", opt.backend.c_str());
		return 1;
	}

	BridgeServer server(opt.port);
	if (!backend->Init(opt, server.helloAck)) {
		fprintf(stderr, "[bridge] backend init failed\n");
		return 1;
	}
	printf("[bridge] guest will render %ux%u per eye @ %.1f Hz (%s)\n", server.helloAck.recommendedWidth,
	    server.helloAck.recommendedHeight, server.helloAck.refreshRateHz, server.helloAck.systemName);
	if (!server.Start())
		return 1;

	backend->Run(server, g_quit);
	server.Stop();
	printf("[bridge] bye\n");
	return 0;
}
