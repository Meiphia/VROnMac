// Test harness: drives vrconmac_xr.dll the way OpenComposite does (temporary D3D11 session, then a
// second session on the "game" device, action system, projection layers), against a running
// bridge (use `vrconmac-bridge --backend mock --dump-dir ...`).
//
// Build:  x86_64-w64-mingw32-g++ -std=c++17 -O1 -I../third_party harness.cpp -o harness.exe -ld3d11 -ldxgi -static
// Run:    wine harness.exe Z:\path\to\vrconmac_xr.dll [frames]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(x)                                                                    \
	do {                                                                            \
		XrResult _r = (x);                                                          \
		if (XR_FAILED(_r)) {                                                        \
			printf("FAIL %s -> %d (line %d)\n", #x, (int)_r, __LINE__);             \
			g_fail++;                                                               \
			return false;                                                           \
		}                                                                           \
	} while (0)
#define EXPECT(cond, msg)                                         \
	do {                                                          \
		if (!(cond)) {                                            \
			printf("FAIL %s (line %d)\n", msg, __LINE__);         \
			g_fail++;                                             \
		} else                                                    \
			printf("ok   %s\n", msg);                             \
	} while (0)

#define FUNCS(X)                                                                                      \
	X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) \
	X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) \
	X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrPollEvent) X(xrCreateReferenceSpace)  \
	X(xrLocateSpace) X(xrLocateViews) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)              \
	X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage)                       \
	X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)     \
	X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrCreateAction)                          \
	X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions)                \
	X(xrGetActionStateFloat) X(xrGetActionStateBoolean) X(xrGetActionStatePose) X(xrCreateActionSpace) \
	X(xrGetCurrentInteractionProfile) X(xrApplyHapticFeedback) X(xrEnumerateBoundSourcesForAction)      \
	X(xrGetD3D11GraphicsRequirementsKHR) X(xrResultToString) X(xrGetReferenceSpaceBoundsRect)

#define DECL(n) static PFN_##n n;
FUNCS(DECL)
static PFN_xrGetInstanceProcAddr gipa;
static XrInstance inst;
static XrSystemId sys;

static bool Load(const char* dll)
{
	HMODULE m = LoadLibraryA(dll);
	if (!m) {
		printf("FAIL LoadLibrary(%s) err %lu\n", dll, GetLastError());
		return false;
	}
	auto neg = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface");
	if (!neg) {
		printf("FAIL no xrNegotiateLoaderRuntimeInterface export\n");
		return false;
	}
	XrNegotiateLoaderInfo li{ XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li), 1, 1,
		XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff) };
	XrNegotiateRuntimeRequest rr{ XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(rr) };
	CHECK(neg(&li, &rr));
	gipa = rr.getInstanceProcAddr;
	gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
	gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
	return xrCreateInstance && xrEnumerateInstanceExtensionProperties;
}

static bool LoadAll()
{
#define LOADF(n)                                             \
	gipa(inst, #n, (PFN_xrVoidFunction*)&n);                \
	if (!n) {                                                \
		printf("FAIL gipa %s\n", #n);                        \
		return false;                                        \
	}
	FUNCS(LOADF)
	return true;
}

static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_profileChanged = false;
static void Pump(XrSession s)
{
	XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto* e = (XrEventDataSessionStateChanged*)&ev;
			g_state = e->state;
			printf("     state -> %d\n", (int)e->state);
			if (e->state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				xrBeginSession(s, &bi);
			} else if (e->state == XR_SESSION_STATE_STOPPING) {
				xrEndSession(s);
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			g_profileChanged = true;
		}
		ev = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

static ID3D11Device* MakeDevice(ID3D11DeviceContext** ctx)
{
	XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	if (XR_FAILED(xrGetD3D11GraphicsRequirementsKHR(inst, sys, &req)))
		return nullptr;
	IDXGIFactory1* f = nullptr;
	CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f);
	IDXGIAdapter1* a = nullptr, *found = nullptr;
	for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
		DXGI_ADAPTER_DESC1 d;
		a->GetDesc1(&d);
		if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) {
			found = a;
			break;
		}
		a->Release();
	}
	f->Release();
	if (!found) {
		printf("FAIL adapter LUID from runtime not found among DXGI adapters\n");
		return nullptr;
	}
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	ID3D11Device* dev = nullptr;
	HRESULT hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, ctx);
	found->Release();
	if (FAILED(hr)) {
		printf("FAIL D3D11CreateDevice 0x%08lx\n", hr);
		return nullptr;
	}
	return dev;
}

static XrSession MakeSession(ID3D11Device* dev)
{
	XrGraphicsBindingD3D11KHR b{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	b.device = dev;
	XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO };
	ci.next = &b;
	ci.systemId = sys;
	XrSession s = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateSession(inst, &ci, &s)))
		return XR_NULL_HANDLE;
	g_state = XR_SESSION_STATE_UNKNOWN;
	for (int i = 0; i < 20 && g_state != XR_SESSION_STATE_READY && g_state < XR_SESSION_STATE_READY; i++) {
		Pump(s);
		Sleep(10);
	}
	return s;
}

static bool EndSessionLikeOC(XrSession s)
{
	CHECK(xrRequestExitSession(s));
	for (int i = 0; i < 10 && g_state != XR_SESSION_STATE_EXITING; i++) {
		Pump(s);
		Sleep(10);
	}
	EXPECT(g_state == XR_SESSION_STATE_EXITING, "session reached EXITING after xrRequestExitSession");
	CHECK(xrDestroySession(s));
	return true;
}

static bool Run(int frames)
{
	// --- instance ---
	uint32_t n = 0;
	CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
	std::vector<XrExtensionProperties> exts(n, { XR_TYPE_EXTENSION_PROPERTIES });
	CHECK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
	EXPECT(n == 1 && strcmp(exts[0].extensionName, "XR_KHR_D3D11_enable") == 0, "advertises XR_KHR_D3D11_enable");

	const char* e[] = { "XR_KHR_D3D11_enable" };
	XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy(ici.applicationInfo.applicationName, "VRConMacHarness");
	ici.applicationInfo.apiVersion = getenv("VRCM_TEST_API11") ? XR_MAKE_VERSION(1, 1, 0) : XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = e;
	CHECK(xrCreateInstance(&ici, &inst));
	if (!LoadAll())
		return false;

	XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	CHECK(xrGetSystem(inst, &sgi, &sys));
	XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
	CHECK(xrGetSystemProperties(inst, sys, &sp));
	printf("     system: %s\n", sp.systemName);
	CHECK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
	std::vector<XrViewConfigurationView> vcv(n, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	CHECK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vcv.data()));
	uint32_t W = vcv[0].recommendedImageRectWidth, H = vcv[0].recommendedImageRectHeight;
	printf("     views: %ux%u\n", W, H);

	// --- OpenComposite-style temporary session, then the real one ---
	ID3D11DeviceContext* tmpCtx = nullptr;
	ID3D11Device* tmpDev = MakeDevice(&tmpCtx);
	if (!tmpDev)
		return false;
	XrSession tmp = MakeSession(tmpDev);
	EXPECT(tmp != XR_NULL_HANDLE, "temporary session created");
	Pump(tmp);
	if (!EndSessionLikeOC(tmp))
		return false;
	tmpCtx->Release();
	tmpDev->Release();

	ID3D11DeviceContext* ctx = nullptr;
	ID3D11Device* dev = MakeDevice(&ctx);
	if (!dev)
		return false;
	XrSession s = MakeSession(dev);
	EXPECT(s != XR_NULL_HANDLE, "game session created");
	Pump(s);

	// --- actions (OC suggests for lots of profiles; only Touch is used) ---
	XrPath left, right;
	CHECK(xrStringToPath(inst, "/user/hand/left", &left));
	CHECK(xrStringToPath(inst, "/user/hand/right", &right));
	XrPath hands[2] = { left, right };
	XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(asci.actionSetName, "test");
	strcpy(asci.localizedActionSetName, "test");
	XrActionSet set;
	CHECK(xrCreateActionSet(inst, &asci, &set));
	auto mk = [&](const char* name, XrActionType t) {
		XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
		strcpy(aci.actionName, name);
		strcpy(aci.localizedActionName, name);
		aci.actionType = t;
		aci.countSubactionPaths = 2;
		aci.subactionPaths = hands;
		XrAction a = XR_NULL_HANDLE;
		xrCreateAction(set, &aci, &a);
		return a;
	};
	XrAction trig = mk("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
	XrAction clickA = mk("a_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
	XrAction grip = mk("grip", XR_ACTION_TYPE_POSE_INPUT);
	XrAction haptic = mk("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
	auto P = [&](const char* s) {
		XrPath p = XR_NULL_PATH;
		xrStringToPath(inst, s, &p);
		return p;
	};
	XrActionSuggestedBinding tb[] = {
		{ trig, P("/user/hand/left/input/trigger/value") }, { trig, P("/user/hand/right/input/trigger/value") },
		{ clickA, P("/user/hand/right/input/a/click") }, { clickA, P("/user/hand/left/input/x/click") },
		{ grip, P("/user/hand/left/input/grip/pose") }, { grip, P("/user/hand/right/input/grip/pose") },
		{ haptic, P("/user/hand/left/output/haptic") }, { haptic, P("/user/hand/right/output/haptic") },
	};
	XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	sb.interactionProfile = P("/interaction_profiles/oculus/touch_controller");
	sb.countSuggestedBindings = sizeof(tb) / sizeof(tb[0]);
	sb.suggestedBindings = tb;
	CHECK(xrSuggestInteractionProfileBindings(inst, &sb));
	XrActionSuggestedBinding vb[] = { { trig, P("/user/hand/left/input/trigger/value") } };
	sb.interactionProfile = P("/interaction_profiles/valve/index_controller");
	sb.countSuggestedBindings = 1;
	sb.suggestedBindings = vb;
	CHECK(xrSuggestInteractionProfileBindings(inst, &sb));

	XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	ai.countActionSets = 1;
	ai.actionSets = &set;
	CHECK(xrAttachSessionActionSets(s, &ai));

	XrSpace stage, local, gripSpace;
	XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rs.poseInReferenceSpace.orientation.w = 1;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	CHECK(xrCreateReferenceSpace(s, &rs, &stage));
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	CHECK(xrCreateReferenceSpace(s, &rs, &local));
	if (getenv("VRCM_TEST_API11")) {
		XrSpace lf;
		rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
		CHECK(xrCreateReferenceSpace(s, &rs, &lf));
		XrSpaceLocation l{ XR_TYPE_SPACE_LOCATION };
		CHECK(xrLocateSpace(lf, stage, 0, &l));
		EXPECT(l.pose.position.y == 0.f, "LOCAL_FLOOR (OpenXR 1.1) sits on the STAGE floor");
	}
	XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
	asi.action = grip;
	asi.subactionPath = right;
	asi.poseInActionSpace.orientation.w = 1;
	CHECK(xrCreateActionSpace(s, &asi, &gripSpace));
	XrExtent2Df bounds;
	CHECK(xrGetReferenceSpaceBoundsRect(s, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds));
	printf("     stage bounds %.2f x %.2f\n", bounds.width, bounds.height);

	// --- swapchains: left in RGBA8 sRGB, right in RGBA16F to exercise conversion ---
	DXGI_FORMAT fmts[2] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT };
	const bool arrayMode = getenv("VRCM_TEST_ARRAY") != nullptr; // Unity single-pass instanced: 1 swapchain, 2 slices
	if (arrayMode)
		printf("     array mode: one swapchain, arraySize=2\n");
	XrSwapchain sc[2];
	std::vector<XrSwapchainImageD3D11KHR> imgs[2];
	std::vector<ID3D11RenderTargetView*> rtvs[2];
	for (int i = 0; i < (arrayMode ? 1 : 2); i++) {
		XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		ci.format = fmts[i];
		ci.sampleCount = 1;
		ci.width = W;
		ci.height = H;
		ci.faceCount = 1;
		ci.arraySize = arrayMode ? 2 : 1;
		ci.mipCount = 1;
		CHECK(xrCreateSwapchain(s, &ci, &sc[i]));
		CHECK(xrEnumerateSwapchainImages(sc[i], 0, &n, nullptr));
		imgs[i].assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		CHECK(xrEnumerateSwapchainImages(sc[i], n, &n, (XrSwapchainImageBaseHeader*)imgs[i].data()));
		for (auto& im : imgs[i]) {
			for (int sl = 0; sl < (arrayMode ? 2 : 1); sl++) {
				ID3D11RenderTargetView* rtv = nullptr;
				D3D11_RENDER_TARGET_VIEW_DESC rd{};
				rd.Format = fmts[i];
				rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				rd.Texture2DArray.FirstArraySlice = sl;
				rd.Texture2DArray.ArraySize = 1;
				D3D11_RENDER_TARGET_VIEW_DESC rd2{};
				rd2.Format = fmts[i];
				rd2.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				if (FAILED(dev->CreateRenderTargetView(im.texture, arrayMode ? &rd : &rd2, &rtv))) {
					printf("FAIL CreateRenderTargetView\n");
					return false;
				}
				rtvs[i].push_back(rtv);
			}
			if (fmts[i] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
				D3D11_RENDER_TARGET_VIEW_DESC lin{};
				lin.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				lin.ViewDimension = arrayMode ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
				lin.Texture2DArray.ArraySize = arrayMode ? 2 : 0;
				ID3D11RenderTargetView* l = nullptr;
				if (FAILED(dev->CreateRenderTargetView(im.texture, &lin, &l))) {
					printf("FAIL linear RTV on sRGB swapchain image (Unity needs this)\n");
					g_fail++;
				} else
					l->Release();
			}
		}
	}

	// --- frame loop ---
	int rendered = 0, triggerActive = 0, gripTracked = 0, aPressed = 0;
	XrTime lastTime = 0;
	bool timeMonotonic = true;
	for (int f = 0; f < frames; f++) {
		Pump(s);
		XrFrameState fs{ XR_TYPE_FRAME_STATE };
		CHECK(xrWaitFrame(s, nullptr, &fs));
		if (fs.predictedDisplayTime <= lastTime)
			timeMonotonic = false;
		lastTime = fs.predictedDisplayTime;
		CHECK(xrBeginFrame(s, nullptr));

		XrActiveActionSet aas{ set, XR_NULL_PATH };
		XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
		si.countActiveActionSets = 1;
		si.activeActionSets = &aas;
		xrSyncActions(s, &si);
		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
		gi.action = trig;
		XrActionStateFloat fsv{ XR_TYPE_ACTION_STATE_FLOAT };
		xrGetActionStateFloat(s, &gi, &fsv);
		if (fsv.isActive)
			triggerActive++;
		gi.action = clickA;
		XrActionStateBoolean bsv{ XR_TYPE_ACTION_STATE_BOOLEAN };
		xrGetActionStateBoolean(s, &gi, &bsv);
		if (bsv.isActive && bsv.currentState)
			aPressed++;
		XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
		xrLocateSpace(gripSpace, stage, fs.predictedDisplayTime, &loc);
		if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
			gripTracked++;
		if (f == 10) {
			XrHapticVibration vib{ XR_TYPE_HAPTIC_VIBRATION };
			vib.amplitude = 0.5f;
			vib.duration = 50000000;
			XrHapticActionInfo hai{ XR_TYPE_HAPTIC_ACTION_INFO };
			hai.action = haptic;
			hai.subactionPath = right;
			xrApplyHapticFeedback(s, &hai, (XrHapticBaseHeader*)&vib);
		}

		XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = fs.predictedDisplayTime;
		vli.space = stage;
		XrViewState vs{ XR_TYPE_VIEW_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		CHECK(xrLocateViews(s, &vli, &vs, 2, &n, views));

		XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
		for (int i = 0; i < 2; i++) {
			if (arrayMode) {
				// one acquire for both eyes; clear slice 0 blue-ish red, slice 1 green
				if (i == 0) {
					uint32_t idx;
					CHECK(xrAcquireSwapchainImage(sc[0], nullptr, &idx));
					XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
					wi.timeout = XR_INFINITE_DURATION;
					CHECK(xrWaitSwapchainImage(sc[0], &wi));
					float c0[4] = { 1.f, 0.f, 0.f, 0.f }, c1[4] = { 0.f, 1.f, 0.f, 0.f }; // alpha 0 like Unity
					ctx->ClearRenderTargetView(rtvs[0][idx * 2 + 0], c0);
					ctx->ClearRenderTargetView(rtvs[0][idx * 2 + 1], c1);
					CHECK(xrReleaseSwapchainImage(sc[0], nullptr));
				}
				pv[i].pose = views[i].pose;
				pv[i].fov = views[i].fov;
				pv[i].subImage.swapchain = sc[0];
				pv[i].subImage.imageArrayIndex = i;
				pv[i].subImage.imageRect = { { 0, 0 }, { (int32_t)W, (int32_t)H } };
				continue;
			}
			uint32_t idx;
			CHECK(xrAcquireSwapchainImage(sc[i], nullptr, &idx));
			XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wi.timeout = XR_INFINITE_DURATION;
			CHECK(xrWaitSwapchainImage(sc[i], &wi));
			// left: linear red 1.0 -> sRGB 255; right: linear 0.2 green -> sRGB ~124
			float col[4] = { i == 0 ? 1.f : 0.f, i == 1 ? 0.2f : 0.f, (f % 60) / 59.f, 1.f };
			ctx->ClearRenderTargetView(rtvs[i][idx], col);
			D3D11_RECT r{ 0, 0, (LONG)W / 4, (LONG)H / 4 };
			(void)r;
			CHECK(xrReleaseSwapchainImage(sc[i], nullptr));
			pv[i].pose = views[i].pose;
			pv[i].fov = views[i].fov;
			pv[i].subImage.swapchain = sc[i];
			pv[i].subImage.imageRect = { { 0, 0 }, { (int32_t)W, (int32_t)H } };
		}
		XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		layer.space = stage;
		layer.viewCount = 2;
		layer.views = pv;
		const XrCompositionLayerBaseHeader* layers[] = { (XrCompositionLayerBaseHeader*)&layer };
		XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = 1;
		fei.layers = layers;
		CHECK(xrEndFrame(s, &fei));
		rendered++;
	}
	Pump(s);
	EXPECT(g_state == XR_SESSION_STATE_FOCUSED, "session reached FOCUSED");
	EXPECT(g_profileChanged, "INTERACTION_PROFILE_CHANGED event delivered");
	XrInteractionProfileState ips{ XR_TYPE_INTERACTION_PROFILE_STATE };
	CHECK(xrGetCurrentInteractionProfile(s, right, &ips));
	char buf[256];
	CHECK(xrPathToString(inst, ips.interactionProfile, sizeof(buf), &n, buf));
	EXPECT(strcmp(buf, "/interaction_profiles/oculus/touch_controller") == 0, "current profile is oculus/touch_controller");
	XrBoundSourcesForActionEnumerateInfo bi{ XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
	bi.action = trig;
	XrPath srcs[20];
	CHECK(xrEnumerateBoundSourcesForAction(s, &bi, 20, &n, srcs));
	EXPECT(n == 2, "trigger has 2 bound sources");
	EXPECT(timeMonotonic, "predictedDisplayTime is monotonic");
	EXPECT(triggerActive > frames / 2, "trigger action active");
	EXPECT(gripTracked > frames / 2, "grip pose tracked");
	EXPECT(aPressed > 0, "A/X click seen at least once");
	printf("     rendered %d frames (trigger active %d, grip tracked %d, A pressed %d)\n", rendered, triggerActive, gripTracked, aPressed);

	for (int i = 0; i < (arrayMode ? 1 : 2); i++) {
		for (auto* r : rtvs[i])
			r->Release();
		CHECK(xrDestroySwapchain(sc[i]));
	}
	if (!EndSessionLikeOC(s))
		return false;
	ctx->Release();
	dev->Release();
	CHECK(xrDestroyInstance(inst));
	return true;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	const char* dll = argc > 1 ? argv[1] : "vrconmac_xr.dll";
	int frames = argc > 2 ? atoi(argv[2]) : 300;
	if (!Load(dll))
		return 1;
	bool ok = Run(frames);
	printf("%s (%d failures)\n", ok && g_fail == 0 ? "PASS" : "FAIL", g_fail);
	return ok && g_fail == 0 ? 0 : 1;
}
