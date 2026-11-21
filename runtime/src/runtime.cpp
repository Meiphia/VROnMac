// VRConMac Wine-side OpenXR runtime.
//
// A Windows OpenXR runtime DLL that implements just enough of OpenXR 1.0 + XR_KHR_D3D11_enable for
// OpenComposite, and forwards everything to the native macOS bridge over TCP:
//   - head/eye/controller poses and frame timing come from the bridge's TICK messages
//   - eye images are read back from D3D11 swapchains and shipped as RGBA8 in FRAME messages
//   - the OpenXR action system is implemented here against a fixed Oculus Touch layout
//
// Only one instance and one session at a time (which is all OpenComposite uses).

#include "common.h"
#include "frame_diagnostics.h"
#include "pixels.h"
#include "transport.h"

#include <openxr/openxr_reflection.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

using namespace vrcm;

// ============================================================================================
// Globals & helpers
// ============================================================================================

static std::recursive_mutex g_lock;
#define LOCK() std::lock_guard<std::recursive_mutex> _guard(g_lock)

static const XrSystemId kSystemId = 0x56524D;

template <typename T>
static XrResult TwoCall(uint32_t capacity, uint32_t* countOut, T* out, const std::vector<T>& items)
{
	if (!countOut)
		return XR_ERROR_VALIDATION_FAILURE;
	*countOut = (uint32_t)items.size();
	if (capacity == 0)
		return XR_SUCCESS;
	if (capacity < items.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	if (!out)
		return XR_ERROR_VALIDATION_FAILURE;
	for (size_t i = 0; i < items.size(); i++)
		out[i] = items[i];
	return XR_SUCCESS;
}

static XrResult TwoCallString(uint32_t capacity, uint32_t* countOut, char* buffer, const std::string& s)
{
	if (!countOut)
		return XR_ERROR_VALIDATION_FAILURE;
	*countOut = (uint32_t)s.size() + 1;
	if (capacity == 0)
		return XR_SUCCESS;
	if (capacity < s.size() + 1)
		return XR_ERROR_SIZE_INSUFFICIENT;
	memcpy(buffer, s.c_str(), s.size() + 1);
	return XR_SUCCESS;
}

// ---- path table ----
static std::vector<std::string> g_paths; // XrPath = index + 1
static std::unordered_map<std::string, XrPath> g_pathIds;

static bool ValidPathString(const char* s)
{
	if (!s || s[0] != '/')
		return false;
	size_t n = strlen(s);
	if (n >= XR_MAX_PATH_LENGTH || s[n - 1] == '/')
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/';
		if (!ok)
			return false;
		if (c == '/' && i + 1 < n && s[i + 1] == '/')
			return false;
	}
	return true;
}

static XrPath PathFor(const std::string& s)
{
	auto it = g_pathIds.find(s);
	if (it != g_pathIds.end())
		return it->second;
	g_paths.push_back(s);
	XrPath id = (XrPath)g_paths.size();
	g_pathIds[s] = id;
	return id;
}

static const std::string* PathString(XrPath p)
{
	if (p == XR_NULL_PATH || p > g_paths.size())
		return nullptr;
	return &g_paths[(size_t)p - 1];
}

// ============================================================================================
// Input model: fixed Oculus Touch layout
// ============================================================================================

static const char* kTouchProfile = "/interaction_profiles/oculus/touch_controller";

enum Comp {
	C_NONE,
	C_TRIGGER_VALUE, C_TRIGGER_TOUCH,
	C_SQUEEZE_VALUE,
	C_STICK, C_STICK_X, C_STICK_Y, C_STICK_CLICK, C_STICK_TOUCH,
	C_PRIMARY_CLICK, C_PRIMARY_TOUCH, C_SECONDARY_CLICK, C_SECONDARY_TOUCH,
	C_MENU_CLICK, C_SYSTEM_CLICK, C_THUMBREST_TOUCH,
	C_GRIP_POSE, C_AIM_POSE, C_HAPTIC,
};

enum CompKind { K_BOOL, K_FLOAT, K_VEC2, K_POSE, K_HAPTIC };

static CompKind KindOf(Comp c)
{
	switch (c) {
	case C_TRIGGER_VALUE: case C_SQUEEZE_VALUE: case C_STICK_X: case C_STICK_Y: return K_FLOAT;
	case C_STICK: return K_VEC2;
	case C_GRIP_POSE: case C_AIM_POSE: return K_POSE;
	case C_HAPTIC: return K_HAPTIC;
	default: return K_BOOL;
	}
}

struct Binding {
	int hand; // 0 left, 1 right
	Comp comp;
	XrPath path;
};

// Parse "/user/hand/<side>/input/<id>[/<component>]" or ".../output/haptic" against the Touch profile.
static bool ParseTouchBinding(const std::string& p, Binding& out)
{
	int hand;
	std::string rest;
	if (p.rfind("/user/hand/left/", 0) == 0) { hand = 0; rest = p.substr(16); }
	else if (p.rfind("/user/hand/right/", 0) == 0) { hand = 1; rest = p.substr(17); }
	else return false;

	if (rest == "output/haptic") { out = { hand, C_HAPTIC, 0 }; return true; }
	if (rest.rfind("input/", 0) != 0) return false;
	rest = rest.substr(6);

	std::string id = rest, comp;
	size_t slash = rest.find('/');
	if (slash != std::string::npos) { id = rest.substr(0, slash); comp = rest.substr(slash + 1); }

	Comp c = C_NONE;
	bool primary = (hand == 0 && id == "x") || (hand == 1 && id == "a");
	bool secondary = (hand == 0 && id == "y") || (hand == 1 && id == "b");
	if (primary || secondary) {
		if (comp == "" || comp == "click") c = primary ? C_PRIMARY_CLICK : C_SECONDARY_CLICK;
		else if (comp == "touch") c = primary ? C_PRIMARY_TOUCH : C_SECONDARY_TOUCH;
	} else if (id == "menu" && hand == 0) {
		if (comp == "" || comp == "click") c = C_MENU_CLICK;
	} else if (id == "system" && hand == 1) {
		if (comp == "" || comp == "click") c = C_SYSTEM_CLICK;
	} else if (id == "trigger") {
		if (comp == "" || comp == "value") c = C_TRIGGER_VALUE;
		else if (comp == "touch") c = C_TRIGGER_TOUCH;
	} else if (id == "squeeze") {
		if (comp == "" || comp == "value") c = C_SQUEEZE_VALUE;
	} else if (id == "thumbstick") {
		if (comp == "") c = C_STICK;
		else if (comp == "x") c = C_STICK_X;
		else if (comp == "y") c = C_STICK_Y;
		else if (comp == "click") c = C_STICK_CLICK;
		else if (comp == "touch") c = C_STICK_TOUCH;
	} else if (id == "thumbrest") {
		if (comp == "touch") c = C_THUMBREST_TOUCH;
	} else if (id == "grip") {
		if (comp == "" || comp == "pose") c = C_GRIP_POSE;
	} else if (id == "aim") {
		if (comp == "" || comp == "pose") c = C_AIM_POSE;
	}
	if (c == C_NONE) return false;
	out = { hand, c, 0 };
	return true;
}

static bool BtnBit(const VrcmHand& h, uint32_t bit) { return (h.buttons & bit) != 0; }

static float ReadFloat(const VrcmHand& h, Comp c)
{
	switch (c) {
	case C_TRIGGER_VALUE: return h.trigger;
	case C_SQUEEZE_VALUE: return h.squeeze;
	case C_STICK_X: return h.stickX;
	case C_STICK_Y: return h.stickY;
	case C_TRIGGER_TOUCH: return BtnBit(h, VRCM_BTN_TRIGGER_TOUCH) ? 1.f : 0.f;
	case C_STICK_CLICK: return BtnBit(h, VRCM_BTN_STICK_CLICK) ? 1.f : 0.f;
	case C_STICK_TOUCH: return BtnBit(h, VRCM_BTN_STICK_TOUCH) ? 1.f : 0.f;
	case C_PRIMARY_CLICK: return BtnBit(h, VRCM_BTN_PRIMARY_CLICK) ? 1.f : 0.f;
	case C_PRIMARY_TOUCH: return BtnBit(h, VRCM_BTN_PRIMARY_TOUCH) ? 1.f : 0.f;
	case C_SECONDARY_CLICK: return BtnBit(h, VRCM_BTN_SECONDARY_CLICK) ? 1.f : 0.f;
	case C_SECONDARY_TOUCH: return BtnBit(h, VRCM_BTN_SECONDARY_TOUCH) ? 1.f : 0.f;
	case C_MENU_CLICK: return BtnBit(h, VRCM_BTN_MENU_CLICK) ? 1.f : 0.f;
	case C_SYSTEM_CLICK: return BtnBit(h, VRCM_BTN_SYSTEM_CLICK) ? 1.f : 0.f;
	case C_THUMBREST_TOUCH: return BtnBit(h, VRCM_BTN_THUMBREST_TOUCH) ? 1.f : 0.f;
	default: return 0.f;
	}
}

// ============================================================================================
// Objects
// ============================================================================================

struct Session;
struct ActionSet;

struct ActionState {
	// value storage for any action type
	bool b = false;
	float f = 0.f;
	XrVector2f v{ 0, 0 };
	bool active = false;
	bool changed = false;
	XrTime lastChange = 0;
};

struct Action {
	ActionSet* set;
	std::string name;
	XrActionType type;
	std::vector<XrPath> subactions;
	std::vector<Binding> bindings;  // active bindings (from the Touch profile) after attach
	ActionState state[3];           // 0 = any, 1 = left, 2 = right
};

struct ActionSet {
	std::string name;
	uint32_t priority = 0;
	std::vector<Action*> actions;
	bool attached = false;
};

struct Instance {
	std::string appName;
	XrVersion apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::set<std::string> enabledExts;
	std::deque<XrEventDataBuffer> events;
	std::map<XrPath, std::vector<std::pair<Action*, XrPath>>> suggested; // profile -> (action, binding)
	std::vector<ActionSet*> actionSets;
	bool queriedD3D11 = false;
	Session* session = nullptr;
};

struct SwapchainImage {
	ID3D11Texture2D* tex = nullptr;
};

struct Swapchain {
	Session* session;
	XrSwapchainCreateInfo info{};
	DXGI_FORMAT format;
	std::vector<SwapchainImage> images;
	uint32_t nextAcquire = 0;
	std::deque<uint32_t> acquired;
	int lastReleased = -1;
	// readback ring
	bool readback = false;
	static const int kStaging = 3;
	uint32_t slices = 1;                    // arraySize (2 for single-pass instanced)
	std::vector<ID3D11Texture2D*> staging;  // kStaging * slices, index slot * slices + slice
	int stagingNext = 0;
	int lastStagingSlot = -1; // slot holding the copy of lastReleased
	ID3D11Texture2D* Staging(int slot, uint32_t slice) const { return staging[(size_t)slot * slices + slice]; }
};

enum SpaceKind { SK_REFERENCE, SK_ACTION };

struct Space {
	Session* session;
	SpaceKind kind;
	XrReferenceSpaceType refType = XR_REFERENCE_SPACE_TYPE_STAGE;
	Action* action = nullptr;
	XrPath subaction = XR_NULL_PATH;
	XrPosef offset = PIdentity();
};

struct PendingEye {
	Swapchain* sc;
	int stagingSlot;
	uint32_t slice;
	XrRect2Di rect;
	XrPosef poseStage;
	XrFovf fov;
};

struct PendingFrame {
	bool valid = false;
	int64_t frameId = 0;
	XrTime displayTime = 0;
	int eyeCount = 0;
	PendingEye eyes[2];
};

struct Session {
	Instance* instance;
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;
	bool exitRequested = false;
	bool attached = false;
	bool syncedOnce = false;
	std::set<Swapchain*> swapchains;
	std::set<Space*> spaces;

	// frame loop
	int64_t lastWaitedFrameId = -1;
	uint64_t bridgeGeneration = 0;
	VrcmTick waitedTick{};
	bool haveWaitedTick = false;
	XrTime synthTime = 0;
	int64_t frameBegun = 0;
	uint64_t framesEnded = 0;
	FrameDiagnostics diagnostics;
	PendingFrame pending;

	// LOCAL space origin within STAGE (set from the first head pose we see)
	bool haveLocalOrigin = false;
	XrPosef localOrigin = PIdentity();

	// latest input snapshot (copied on xrSyncActions)
	VrcmTick inputTick{};
	bool haveInputTick = false;
	XrPath currentProfile[2] = { XR_NULL_PATH, XR_NULL_PATH };
};

static Instance* g_instance = nullptr;

static Instance* AsInstance(XrInstance h)
{
	Instance* i = reinterpret_cast<Instance*>(h);
	return (i && i == g_instance) ? i : nullptr;
}
static Session* AsSession(XrSession h)
{
	Session* s = reinterpret_cast<Session*>(h);
	return (s && g_instance && g_instance->session == s) ? s : nullptr;
}
#define GET_INSTANCE(var, h) \
	Instance* var = AsInstance(h); \
	if (!var) return XR_ERROR_HANDLE_INVALID
#define GET_SESSION(var, h) \
	Session* var = AsSession(h); \
	if (!var) return XR_ERROR_HANDLE_INVALID

static XrTime CurrentTime()
{
	VrcmTick t;
	if (BridgeClient::Get().LatestTick(t))
		return t.predictedDisplayTime;
	return NowNs();
}

static void PushEvent(Instance* inst, const void* ev, size_t size)
{
	XrEventDataBuffer buf{};
	memcpy(&buf, ev, std::min(size, sizeof(buf)));
	inst->events.push_back(buf);
}

static void SetSessionState(Session* s, XrSessionState st)
{
	if (s->state == st)
		return;
	s->state = st;
	XrEventDataSessionStateChanged ev{ XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
	ev.session = reinterpret_cast<XrSession>(s);
	ev.state = st;
	ev.time = CurrentTime();
	PushEvent(s->instance, &ev, sizeof(ev));
	VLOG("Session state -> %d", (int)st);
}

// ============================================================================================
// Spaces
// ============================================================================================

static bool GetTickFor(XrTime time, VrcmTick& t)
{
	if (time > 0)
		return BridgeClient::Get().TickNearest(time, t);
	return BridgeClient::Get().LatestTick(t);
}

static void UpdateLocalOrigin(Session* s, const VrcmTick& t)
{
	if (s->haveLocalOrigin || !t.viewsValid)
		return;
	// LOCAL: origin at the initial head position, yaw-only orientation
	XrPosef head = FromWire(t.headPose);
	const XrQuaternionf& q = head.orientation;
	float yaw = std::atan2(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.y * q.y + q.x * q.x));
	s->localOrigin.orientation = { 0, std::sin(yaw / 2), 0, std::cos(yaw / 2) };
	s->localOrigin.position = head.position;
	s->haveLocalOrigin = true;
	VLOG("LOCAL origin set at (%.2f, %.2f, %.2f) yaw %.1f deg", head.position.x, head.position.y, head.position.z,
	    yaw * 57.2958f);
}

// Pose of `sp` expressed in STAGE at `time`. Returns false if untracked.
static bool SpaceInStage(Space* sp, XrTime time, XrPosef& out)
{
	Session* s = sp->session;
	VrcmTick t{};
	bool haveTick = GetTickFor(time, t);
	if (haveTick)
		UpdateLocalOrigin(s, t);

	XrPosef base = PIdentity();
	bool valid = true;
	if (sp->kind == SK_REFERENCE) {
		switch (sp->refType) {
		case XR_REFERENCE_SPACE_TYPE_STAGE: base = PIdentity(); break;
		case XR_REFERENCE_SPACE_TYPE_LOCAL: base = s->localOrigin; break;
		case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR: // LOCAL's x/z and yaw, at floor height
			base = s->localOrigin;
			base.position.y = 0.f;
			break;
		case XR_REFERENCE_SPACE_TYPE_VIEW:
			if (!haveTick || !t.viewsValid) valid = false;
			else base = FromWire(t.headPose);
			break;
		default: valid = false;
		}
	} else {
		// pose action: use the snapshot from the last xrSyncActions if we have it, else latest tick
		const VrcmTick& src = s->haveInputTick ? s->inputTick : t;
		if (!s->haveInputTick && !haveTick)
			return false;
		valid = false;
		for (const Binding& b : sp->action->bindings) {
			if (sp->subaction != XR_NULL_PATH) {
				const std::string* sub = PathString(sp->subaction);
				if (!sub) continue;
				if ((b.hand == 0) != (*sub == "/user/hand/left")) continue;
			}
			const VrcmHand& h = src.hands[b.hand];
			if (!h.active) continue;
			// Prefer the freshest pose for rendering: pull from the time-matched tick when available
			const VrcmHand& hp = haveTick ? t.hands[b.hand] : h;
			if (b.comp == C_GRIP_POSE && hp.gripValid) { base = FromWire(hp.grip); valid = true; break; }
			if (b.comp == C_AIM_POSE && hp.aimValid) { base = FromWire(hp.aim); valid = true; break; }
		}
	}
	out = PMul(base, sp->offset);
	return valid;
}

static XrResult XRAPI_CALL rt_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location)
{
	LOCK();
	Space* a = reinterpret_cast<Space*>(space);
	Space* b = reinterpret_cast<Space*>(baseSpace);
	if (!a || !b || !location)
		return XR_ERROR_HANDLE_INVALID;
	XrPosef pa, pb;
	bool va = SpaceInStage(a, time, pa);
	bool vb = SpaceInStage(b, time, pb);
	location->locationFlags = 0;
	if (va && vb) {
		location->pose = PMul(PInv(pb), pa);
		location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
		    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	} else {
		location->pose = PIdentity();
	}
	// velocity chained struct: report unknown
	XrBaseOutStructure* next = reinterpret_cast<XrBaseOutStructure*>(location->next);
	while (next) {
		if (next->type == XR_TYPE_SPACE_VELOCITY) {
			auto* vel = reinterpret_cast<XrSpaceVelocity*>(next);
			vel->velocityFlags = 0;
			vel->linearVelocity = { 0, 0, 0 };
			vel->angularVelocity = { 0, 0, 0 };
		}
		next = next->next;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* out)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !out || info->spaceCount != out->locationCount)
		return XR_ERROR_VALIDATION_FAILURE;
	Space* b = reinterpret_cast<Space*>(info->baseSpace);
	XrPosef pb;
	bool vb = b && SpaceInStage(b, info->time, pb);
	for (uint32_t i = 0; i < info->spaceCount; i++) {
		Space* a = reinterpret_cast<Space*>(info->spaces[i]);
		XrPosef pa;
		bool va = a && SpaceInStage(a, info->time, pa);
		XrSpaceLocationData& d = out->locations[i];
		if (va && vb) {
			d.pose = PMul(PInv(pb), pa);
			d.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
			    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
		} else {
			d.pose = PIdentity();
			d.locationFlags = 0;
		}
	}
	for (auto* n = reinterpret_cast<XrBaseOutStructure*>(out->next); n; n = n->next)
		if (n->type == XR_TYPE_SPACE_VELOCITIES) {
			auto* v = reinterpret_cast<XrSpaceVelocities*>(n);
			for (uint32_t i = 0; i < v->velocityCount; i++)
				v->velocities[i] = { 0, { 0, 0, 0 }, { 0, 0, 0 } };
		}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateReferenceSpaces(XrSession session, uint32_t cap, uint32_t* count, XrReferenceSpaceType* out)
{
	LOCK();
	GET_SESSION(s, session);
	std::vector<XrReferenceSpaceType> types = { XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
		XR_REFERENCE_SPACE_TYPE_STAGE };
	if (XR_VERSION_MINOR(s->instance->apiVersion) >= 1)
		types.push_back(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR); // core in OpenXR 1.1
	return TwoCall<XrReferenceSpaceType>(cap, count, out, types);
}

static XrResult XRAPI_CALL rt_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* ci, XrSpace* space)
{
	LOCK();
	GET_SESSION(s, session);
	if (!ci || !space)
		return XR_ERROR_VALIDATION_FAILURE;
	bool ok = ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW || ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
	    ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE || ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
	if (!ok) {
		VLOG("xrCreateReferenceSpace: unsupported type %d", (int)ci->referenceSpaceType);
		return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
	}
	Space* sp = new Space{ s, SK_REFERENCE, ci->referenceSpaceType, nullptr, XR_NULL_PATH, ci->poseInReferenceSpace };
	s->spaces.insert(sp);
	*space = reinterpret_cast<XrSpace>(sp);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType type, XrExtent2Df* bounds)
{
	LOCK();
	GET_SESSION(s, session);
	if (!bounds)
		return XR_ERROR_VALIDATION_FAILURE;
	if (type == XR_REFERENCE_SPACE_TYPE_STAGE) {
		const VrcmHelloAck& a = BridgeClient::Get().Info();
		bounds->width = a.playAreaWidth > 0 ? a.playAreaWidth : 2.f;
		bounds->height = a.playAreaDepth > 0 ? a.playAreaDepth : 2.f;
		return XR_SUCCESS;
	}
	bounds->width = bounds->height = 0;
	return XR_SPACE_BOUNDS_UNAVAILABLE;
}

static XrResult XRAPI_CALL rt_xrDestroySpace(XrSpace space)
{
	LOCK();
	Space* sp = reinterpret_cast<Space*>(space);
	if (!sp)
		return XR_ERROR_HANDLE_INVALID;
	if (g_instance && g_instance->session == sp->session)
		sp->session->spaces.erase(sp);
	delete sp;
	return XR_SUCCESS;
}

// ============================================================================================
// Instance / system
// ============================================================================================

static XrResult XRAPI_CALL rt_xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t cap, uint32_t* count, XrExtensionProperties* props)
{
	if (layerName)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	XrExtensionProperties d3d{ XR_TYPE_EXTENSION_PROPERTIES };
	strcpy(d3d.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
	d3d.extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
	if (!count)
		return XR_ERROR_VALIDATION_FAILURE;
	*count = 1;
	if (cap == 0)
		return XR_SUCCESS;
	if (!props)
		return XR_ERROR_VALIDATION_FAILURE;
	props[0].extensionVersion = d3d.extensionVersion;
	strcpy(props[0].extensionName, d3d.extensionName);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateApiLayerProperties(uint32_t cap, uint32_t* count, XrApiLayerProperties*)
{
	if (!count)
		return XR_ERROR_VALIDATION_FAILURE;
	*count = 0;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrCreateInstance(const XrInstanceCreateInfo* ci, XrInstance* instance)
{
	LOCK();
	if (!ci || !instance)
		return XR_ERROR_VALIDATION_FAILURE;
	if (g_instance)
		return XR_ERROR_LIMIT_REACHED;
	if (XR_VERSION_MAJOR(ci->applicationInfo.apiVersion) != 1)
		return XR_ERROR_API_VERSION_UNSUPPORTED;

	Instance* inst = new Instance();
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
		std::string e = ci->enabledExtensionNames[i];
		if (e != XR_KHR_D3D11_ENABLE_EXTENSION_NAME) {
			VLOG("xrCreateInstance: unsupported extension %s", e.c_str());
			delete inst;
			return XR_ERROR_EXTENSION_NOT_PRESENT;
		}
		inst->enabledExts.insert(e);
	}
	inst->appName = ci->applicationInfo.applicationName;
	inst->apiVersion = ci->applicationInfo.apiVersion;
	g_instance = inst;
	*instance = reinterpret_cast<XrInstance>(inst);
	VLOG("xrCreateInstance: app='%s' engine='%s' api=%u.%u", ci->applicationInfo.applicationName,
	    ci->applicationInfo.engineName, XR_VERSION_MAJOR(ci->applicationInfo.apiVersion),
	    XR_VERSION_MINOR(ci->applicationInfo.apiVersion));
	return XR_SUCCESS;
}

static void DestroySessionInternal(Session* s);

static XrResult XRAPI_CALL rt_xrDestroyInstance(XrInstance instance)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (inst->session)
		DestroySessionInternal(inst->session);
	for (ActionSet* as : inst->actionSets) {
		for (Action* a : as->actions)
			delete a;
		delete as;
	}
	delete inst;
	g_instance = nullptr;
	BridgeClient::Get().Disconnect();
	VLOG("xrDestroyInstance");
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* props)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!props)
		return XR_ERROR_VALIDATION_FAILURE;
	props->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
	strcpy(props->runtimeName, "VRConMac (Wine -> macOS bridge)");
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrPollEvent(XrInstance instance, XrEventDataBuffer* ev)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!ev)
		return XR_ERROR_VALIDATION_FAILURE;
	if (inst->events.empty())
		return XR_EVENT_UNAVAILABLE;
	*ev = inst->events.front();
	inst->events.pop_front();
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrResultToString(XrInstance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
#define RES_CASE(name, val) \
	case name: strcpy(buffer, #name); return XR_SUCCESS;
	switch (value) {
		XR_LIST_ENUM_XrResult(RES_CASE)
	default:
		snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s_%d", value < 0 ? "XR_UNKNOWN_FAILURE" : "XR_UNKNOWN_SUCCESS", (int)value);
		return XR_SUCCESS;
	}
#undef RES_CASE
}

static XrResult XRAPI_CALL rt_xrStructureTypeToString(XrInstance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
#define ST_CASE(name, val) \
	case name: strcpy(buffer, #name); return XR_SUCCESS;
	switch (value) {
		XR_LIST_ENUM_XrStructureType(ST_CASE)
	default:
		snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)value);
		return XR_SUCCESS;
	}
#undef ST_CASE
}

static XrResult XRAPI_CALL rt_xrGetSystem(XrInstance instance, const XrSystemGetInfo* info, XrSystemId* systemId)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!info || !systemId)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	if (!BridgeClient::Get().EnsureConnected(inst->appName.c_str()))
		return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
	*systemId = kSystemId;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* props)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	if (!props)
		return XR_ERROR_VALIDATION_FAILURE;
	const VrcmHelloAck& a = BridgeClient::Get().Info();
	props->systemId = kSystemId;
	props->vendorId = 0x4D45; // 'ME'
	snprintf(props->systemName, sizeof(props->systemName), "VRConMac: %s", a.systemName);
	props->graphicsProperties.maxSwapchainImageWidth = std::max(a.maxWidth, 4096u);
	props->graphicsProperties.maxSwapchainImageHeight = std::max(a.maxHeight, 4096u);
	props->graphicsProperties.maxLayerCount = 16;
	props->trackingProperties.orientationTracking = XR_TRUE;
	props->trackingProperties.positionTracking = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count, XrViewConfigurationType* out)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	return TwoCall<XrViewConfigurationType>(cap, count, out, { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO });
}

static XrResult XRAPI_CALL rt_xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type, XrViewConfigurationProperties* props)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	props->viewConfigurationType = type;
	props->fovMutable = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type, uint32_t cap, uint32_t* count, XrViewConfigurationView* views)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	if (!count)
		return XR_ERROR_VALIDATION_FAILURE;
	*count = 2;
	if (cap == 0)
		return XR_SUCCESS;
	if (cap < 2)
		return XR_ERROR_SIZE_INSUFFICIENT;
	const VrcmHelloAck& a = BridgeClient::Get().Info();
	for (int i = 0; i < 2; i++) {
		views[i].recommendedImageRectWidth = a.recommendedWidth ? a.recommendedWidth : 1440;
		views[i].recommendedImageRectHeight = a.recommendedHeight ? a.recommendedHeight : 1584;
		views[i].maxImageRectWidth = std::max(a.maxWidth, views[i].recommendedImageRectWidth);
		views[i].maxImageRectHeight = std::max(a.maxHeight, views[i].recommendedImageRectHeight);
		views[i].recommendedSwapchainSampleCount = 1;
		views[i].maxSwapchainSampleCount = 4;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId, XrViewConfigurationType, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* out)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	return TwoCall<XrEnvironmentBlendMode>(cap, count, out, { XR_ENVIRONMENT_BLEND_MODE_OPAQUE });
}

static XrResult XRAPI_CALL rt_xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D11KHR* req)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	if (!req)
		return XR_ERROR_VALIDATION_FAILURE;

	// OpenComposite looks this LUID up among the DXGI adapters to build its temporary device,
	// so it must be a real adapter: report the first one Wine exposes.
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) {
		VLOG("CreateDXGIFactory1 failed");
		return XR_ERROR_RUNTIME_FAILURE;
	}
	IDXGIAdapter1* adapter = nullptr;
	HRESULT hr = factory->EnumAdapters1(0, &adapter);
	if (FAILED(hr) || !adapter) {
		factory->Release();
		VLOG("No DXGI adapters");
		return XR_ERROR_RUNTIME_FAILURE;
	}
	DXGI_ADAPTER_DESC1 desc;
	adapter->GetDesc1(&desc);
	req->adapterLuid = desc.AdapterLuid;
	req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	char name[128];
	WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
	VLOG("D3D11 requirements: adapter '%s'", name);
	adapter->Release();
	factory->Release();
	inst->queriedD3D11 = true;
	return XR_SUCCESS;
}

// ============================================================================================
// Session
// ============================================================================================

static XrResult XRAPI_CALL rt_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* ci, XrSession* session)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!ci || !session)
		return XR_ERROR_VALIDATION_FAILURE;
	if (ci->systemId != kSystemId)
		return XR_ERROR_SYSTEM_INVALID;
	if (inst->session)
		return XR_ERROR_LIMIT_REACHED;

	const XrGraphicsBindingD3D11KHR* binding = nullptr;
	for (auto* n = reinterpret_cast<const XrBaseInStructure*>(ci->next); n; n = n->next)
		if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
			binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(n);
	if (!binding || !binding->device) {
		VLOG("xrCreateSession: no D3D11 binding");
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	if (!inst->queriedD3D11)
		return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
	if (!BridgeClient::Get().EnsureConnected(inst->appName.c_str()))
		return XR_ERROR_RUNTIME_UNAVAILABLE;

	Session* s = new Session();
	s->instance = inst;
	s->device = binding->device;
	s->device->AddRef();
	s->device->GetImmediateContext(&s->ctx);
	inst->session = s;
	*session = reinterpret_cast<XrSession>(s);
	VLOG("xrCreateSession: device %p", (void*)s->device);

	SetSessionState(s, XR_SESSION_STATE_IDLE);
	SetSessionState(s, XR_SESSION_STATE_READY);
	return XR_SUCCESS;
}

static void DestroySwapchainInternal(Swapchain* sc);

static void DestroySessionInternal(Session* s)
{
	for (Swapchain* sc : std::set<Swapchain*>(s->swapchains))
		DestroySwapchainInternal(sc);
	for (Space* sp : s->spaces)
		delete sp;
	if (s->ctx)
		s->ctx->Release();
	if (s->device)
		s->device->Release();
	if (s->running)
		BridgeClient::Get().SendSessionState(false);
	s->instance->session = nullptr;
	for (ActionSet* as : s->instance->actionSets)
		as->attached = false;
	delete s;
}

static XrResult XRAPI_CALL rt_xrDestroySession(XrSession session)
{
	LOCK();
	GET_SESSION(s, session);
	VLOG("xrDestroySession");
	DestroySessionInternal(s);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrBeginSession(XrSession session, const XrSessionBeginInfo* info)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	if (s->running)
		return XR_ERROR_SESSION_RUNNING;
	if (s->state != XR_SESSION_STATE_READY)
		return XR_ERROR_SESSION_NOT_READY;
	if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	s->running = true;
	s->framesEnded = 0;
	s->diagnostics.Start();
	s->pending.valid = false;
	BridgeClient::Get().SendSessionState(true);
	VLOG("xrBeginSession");
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEndSession(XrSession session)
{
	LOCK();
	GET_SESSION(s, session);
	if (!s->running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (s->state != XR_SESSION_STATE_STOPPING)
		return XR_ERROR_SESSION_NOT_STOPPING;
	s->running = false;
	s->diagnostics.Stop();
	s->pending.valid = false;
	BridgeClient::Get().SendSessionState(false);
	SetSessionState(s, XR_SESSION_STATE_IDLE);
	if (s->exitRequested)
		SetSessionState(s, XR_SESSION_STATE_EXITING);
	else
		SetSessionState(s, XR_SESSION_STATE_READY);
	VLOG("xrEndSession");
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrRequestExitSession(XrSession session)
{
	LOCK();
	GET_SESSION(s, session);
	if (!s->running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	s->exitRequested = true;
	if (s->state == XR_SESSION_STATE_FOCUSED)
		SetSessionState(s, XR_SESSION_STATE_VISIBLE);
	if (s->state == XR_SESSION_STATE_VISIBLE)
		SetSessionState(s, XR_SESSION_STATE_SYNCHRONIZED);
	SetSessionState(s, XR_SESSION_STATE_STOPPING);
	return XR_SUCCESS;
}

// ============================================================================================
// Swapchains
// ============================================================================================

static const std::vector<int64_t> kFormats = {
	DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
	DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
	DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
	DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
};

static XrResult XRAPI_CALL rt_xrEnumerateSwapchainFormats(XrSession session, uint32_t cap, uint32_t* count, int64_t* out)
{
	LOCK();
	GET_SESSION(s, session);
	return TwoCall<int64_t>(cap, count, out, kFormats);
}

// Real D3D11 runtimes create swapchain textures with the TYPELESS variant so apps can create
// sRGB *and* linear views (Unity does exactly that). The exact format is what views/staging use.
static DXGI_FORMAT TypelessOf(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
	default: return f;
	}
}

static XrResult XRAPI_CALL rt_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* swapchain)
{
	LOCK();
	GET_SESSION(s, session);
	if (!ci || !swapchain)
		return XR_ERROR_VALIDATION_FAILURE;
	if (std::find(kFormats.begin(), kFormats.end(), ci->format) == kFormats.end()) {
		VLOG("xrCreateSwapchain: unsupported format %lld", (long long)ci->format);
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	if (ci->width == 0 || ci->height == 0 || ci->arraySize == 0 || ci->mipCount == 0 || ci->sampleCount == 0 ||
	    (ci->faceCount != 1 && ci->faceCount != 6))
		return XR_ERROR_VALIDATION_FAILURE;

	auto sc = std::make_unique<Swapchain>();
	sc->session = s;
	sc->info = *ci;
	sc->format = (DXGI_FORMAT)ci->format;
	bool depth = IsDepthFormat(sc->format);

	D3D11_TEXTURE2D_DESC td{};
	td.Width = ci->width;
	td.Height = ci->height;
	td.MipLevels = ci->mipCount;
	td.ArraySize = ci->arraySize * ci->faceCount;
	td.Format = TypelessOf(sc->format);
	td.SampleDesc.Count = ci->sampleCount;
	td.Usage = D3D11_USAGE_DEFAULT;
	if (depth)
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL | ((ci->sampleCount == 1) ? D3D11_BIND_SHADER_RESOURCE : 0);
	else
		td.BindFlags = D3D11_BIND_RENDER_TARGET | ((ci->sampleCount == 1) ? D3D11_BIND_SHADER_RESOURCE : 0);
	if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
		td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
	if (ci->faceCount == 6)
		td.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

	const int kImages = 3;
	for (int i = 0; i < kImages; i++) {
		ID3D11Texture2D* tex = nullptr;
		HRESULT hr = s->device->CreateTexture2D(&td, nullptr, &tex);
		if (FAILED(hr)) {
			VLOG("xrCreateSwapchain: CreateTexture2D failed 0x%08lx (fmt %d typeless %d %ux%u)", hr, (int)sc->format, (int)td.Format, td.Width, td.Height);
			for (auto& im : sc->images)
				im.tex->Release();
			return XR_ERROR_RUNTIME_FAILURE;
		}
		sc->images.push_back({ tex });
	}

	if (!depth && ci->sampleCount == 1 && ci->faceCount == 1 && ci->arraySize <= 4 && IsReadbackFormat(sc->format)) {
		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = ci->width;
		sd.Height = ci->height;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Format = sc->format;
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sc->readback = true;
		sc->slices = ci->arraySize;
		sc->staging.assign((size_t)Swapchain::kStaging * sc->slices, nullptr);
		for (size_t i = 0; i < sc->staging.size(); i++) {
			if (FAILED(s->device->CreateTexture2D(&sd, nullptr, &sc->staging[i]))) {
				VLOG("xrCreateSwapchain: staging texture failed; this swapchain won't be streamed");
				sc->readback = false;
				break;
			}
		}
	}

	VLOG("xrCreateSwapchain: %ux%u fmt=%d (texture %d) usage=0x%llx samples=%u mips=%u array=%u faces=%u readback=%d",
	    ci->width, ci->height, (int)ci->format, (int)td.Format, (unsigned long long)ci->usageFlags, ci->sampleCount,
	    ci->mipCount, ci->arraySize, ci->faceCount, (int)sc->readback);

	Swapchain* raw = sc.release();
	s->swapchains.insert(raw);
	*swapchain = reinterpret_cast<XrSwapchain>(raw);
	return XR_SUCCESS;
}

static void DestroySwapchainInternal(Swapchain* sc)
{
	for (auto& im : sc->images)
		if (im.tex)
			im.tex->Release();
	for (auto* st : sc->staging)
		if (st)
			st->Release();
	sc->staging.clear();
	Session* s = sc->session;
	s->swapchains.erase(sc);
	// drop a pending frame that references this swapchain
	for (int i = 0; i < s->pending.eyeCount; i++)
		if (s->pending.eyes[i].sc == sc)
			s->pending.valid = false;
	delete sc;
}

static Swapchain* AsSwapchain(XrSwapchain h)
{
	Swapchain* sc = reinterpret_cast<Swapchain*>(h);
	if (!sc || !g_instance || !g_instance->session || !g_instance->session->swapchains.count(sc))
		return nullptr;
	return sc;
}

static XrResult XRAPI_CALL rt_xrDestroySwapchain(XrSwapchain swapchain)
{
	LOCK();
	Swapchain* sc = AsSwapchain(swapchain);
	if (!sc)
		return XR_ERROR_HANDLE_INVALID;
	DestroySwapchainInternal(sc);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
	LOCK();
	Swapchain* sc = AsSwapchain(swapchain);
	if (!sc)
		return XR_ERROR_HANDLE_INVALID;
	if (!count)
		return XR_ERROR_VALIDATION_FAILURE;
	*count = (uint32_t)sc->images.size();
	if (cap == 0)
		return XR_SUCCESS;
	if (cap < sc->images.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	auto* d3d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
	for (size_t i = 0; i < sc->images.size(); i++) {
		if (d3d[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
		d3d[i].texture = sc->images[i].tex;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
	LOCK();
	Swapchain* sc = AsSwapchain(swapchain);
	if (!sc)
		return XR_ERROR_HANDLE_INVALID;
	if (!index)
		return XR_ERROR_VALIDATION_FAILURE;
	if (sc->acquired.size() >= sc->images.size())
		return XR_ERROR_CALL_ORDER_INVALID;
	*index = sc->nextAcquire;
	sc->acquired.push_back(sc->nextAcquire);
	sc->nextAcquire = (sc->nextAcquire + 1) % (uint32_t)sc->images.size();
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*)
{
	LOCK();
	Swapchain* sc = AsSwapchain(swapchain);
	if (!sc)
		return XR_ERROR_HANDLE_INVALID;
	if (sc->acquired.empty())
		return XR_ERROR_CALL_ORDER_INVALID;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*)
{
	LOCK();
	Swapchain* sc = AsSwapchain(swapchain);
	if (!sc)
		return XR_ERROR_HANDLE_INVALID;
	if (sc->acquired.empty())
		return XR_ERROR_CALL_ORDER_INVALID;
	uint32_t idx = sc->acquired.front();
	sc->acquired.pop_front();
	sc->lastReleased = (int)idx;

	// Kick off the GPU->staging copy now; it's mapped in xrEndFrame (this frame or the next).
	if (sc->readback && sc->session->running) {
		int slot = sc->stagingNext;
		sc->stagingNext = (sc->stagingNext + 1) % Swapchain::kStaging;
		for (uint32_t sl = 0; sl < sc->slices; sl++) {
			FrameDiagnostics::Operation operation(sc->session->diagnostics, FrameDiagnostics::Stage::Copy);
			sc->session->ctx->CopySubresourceRegion(sc->Staging(slot, sl), 0, 0, 0, 0, sc->images[idx].tex,
			    D3D11CalcSubresource(0, sl, sc->info.mipCount), nullptr);
		}
		sc->lastStagingSlot = slot;
	}
	return XR_SUCCESS;
}

// ============================================================================================
// Frame loop
// ============================================================================================

static XrResult XRAPI_CALL rt_xrWaitFrame(XrSession session, const XrFrameWaitInfo*, XrFrameState* frameState)
{
	Session* s;
	int64_t after;
	BridgeClient& bc = BridgeClient::Get();
	const auto generation = bc.ConnectionGeneration();
	{
		LOCK();
		s = AsSession(session);
		if (!s)
			return XR_ERROR_HANDLE_INVALID;
		if (!s->running)
			return XR_ERROR_SESSION_NOT_RUNNING;
		if (!frameState)
			return XR_ERROR_VALIDATION_FAILURE;
		after = s->bridgeGeneration == generation ? s->lastWaitedFrameId : -1;
		if (!bc.IsConnected()) bc.RequestReconnect(s->instance->appName);
	}

	// Pace off the bridge's frame clock; don't hold the runtime lock while waiting.
	VrcmTick t{};
	bool got = bc.IsConnected() && bc.WaitTick(after, 100, t);

	LOCK();
	if (!AsSession(session))
		return XR_ERROR_SESSION_LOST;
	if (got) {
		s->bridgeGeneration = generation;
		s->lastWaitedFrameId = t.frameId;
		s->waitedTick = t;
		s->haveWaitedTick = true;
		UpdateLocalOrigin(s, t);
		frameState->predictedDisplayTime = t.predictedDisplayTime;
		frameState->predictedDisplayPeriod = t.predictedPeriod;
		frameState->shouldRender = (t.shouldRender && (s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED ||
		                                                  s->state == XR_SESSION_STATE_SYNCHRONIZED))
		    ? XR_TRUE
		    : XR_FALSE;
		// Before the first xrEndFrame the state is READY; let the app render anyway so we reach SYNCHRONIZED.
		if (s->framesEnded == 0)
			frameState->shouldRender = XR_TRUE;
	} else {
		// No bridge tick: free-run at ~72 Hz on our own clock so the app keeps going.
		const int64_t period = 13888889;
		int64_t now = NowNs();
		if (s->synthTime == 0 || s->synthTime < now)
			s->synthTime = now + period;
		else
			s->synthTime += period;
		frameState->predictedDisplayTime = s->synthTime;
		frameState->predictedDisplayPeriod = period;
		frameState->shouldRender = XR_FALSE;
		if (!s->haveWaitedTick) {
			s->waitedTick = VrcmTick{};
			s->waitedTick.frameId = after;
		}
		s->waitedTick.predictedDisplayTime = s->synthTime;
		static int warned = 0;
		if (warned++ < 5)
			VLOG("xrWaitFrame: no tick from bridge (connected=%d), free-running", (int)bc.IsConnected());
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrBeginFrame(XrSession session, const XrFrameBeginInfo*)
{
	LOCK();
	GET_SESSION(s, session);
	if (!s->running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	XrResult r = s->frameBegun ? XR_FRAME_DISCARDED : XR_SUCCESS;
	s->frameBegun = 1;
	return r;
}

// Map the staging copies referenced by `pf` and ship them to the bridge.
static void ShipFrame(Session* s, const PendingFrame& pf)
{
	BridgeClient& bc = BridgeClient::Get();
	if (!bc.IsConnected())
		return;

	size_t total = sizeof(VrcmFrame);
	for (int i = 0; i < pf.eyeCount; i++)
		total += sizeof(VrcmEyeImage) + (size_t)pf.eyes[i].rect.extent.width * pf.eyes[i].rect.extent.height * 4;

	std::vector<uint8_t>& buf = [&]() -> std::vector<uint8_t>& {
		FrameDiagnostics::Operation operation(s->diagnostics, FrameDiagnostics::Stage::BeginFrameBuffer);
		return bc.BeginFrame(total);
	}();
	VrcmFrame fh{};
	fh.frameId = pf.frameId;
	fh.displayTime = pf.displayTime;
	fh.eyeCount = (uint32_t)pf.eyeCount;
	memcpy(buf.data(), &fh, sizeof(fh));
	size_t off = sizeof(fh);

	for (int i = 0; i < pf.eyeCount; i++) {
		const PendingEye& e = pf.eyes[i];
		uint32_t w = (uint32_t)e.rect.extent.width, h = (uint32_t)e.rect.extent.height;
		VrcmEyeImage eh{};
		eh.pose = ToWire(e.poseStage);
		eh.fov = { e.fov.angleLeft, e.fov.angleRight, e.fov.angleUp, e.fov.angleDown };
		eh.width = w;
		eh.height = h;
		eh.stride = w * 4;
		eh.format = VRCM_PIXEL_RGBA8_SRGB;
		memcpy(buf.data() + off, &eh, sizeof(eh));
		off += sizeof(eh);

		ID3D11Texture2D* st = e.sc->Staging(e.stagingSlot, e.slice);
		D3D11_MAPPED_SUBRESOURCE m{};
		HRESULT hr;
		{
			FrameDiagnostics::Operation operation(s->diagnostics, FrameDiagnostics::Stage::Map);
			hr = s->ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
		}
		if (SUCCEEDED(hr)) {
			uint8_t* px = buf.data() + off;
			ConvertToRGBA8sRGB(e.sc->format, (const uint8_t*)m.pData, m.RowPitch, (uint32_t)e.rect.offset.x,
			    (uint32_t)e.rect.offset.y, w, h, px);
		{
			FrameDiagnostics::Operation operation(s->diagnostics, FrameDiagnostics::Stage::Unmap);
			s->ctx->Unmap(st, 0);
		}
			// Projection layers are opaque unless the app asks otherwise; apps often leave alpha at 0.
			for (size_t p = 3, n = (size_t)w * h * 4; p < n; p += 4)
				px[p] = 255;
			// Diagnostics: how bright are the first frames? (tells "game renders black" from "pipe broken")
			if (s->framesEnded < 3 || s->framesEnded % 720 == 0) {
				uint64_t sum[3] = { 0, 0, 0 }, nonzero = 0, samples = 0;
				for (uint32_t y = 0; y < h; y += 16)
					for (uint32_t x = 0; x < w; x += 16) {
						const uint8_t* q = px + ((size_t)y * w + x) * 4;
						sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2];
						nonzero += (q[0] | q[1] | q[2]) != 0;
						samples++;
					}
				VLOG("eye %d (slice %u, fmt %d) mean RGB %.1f %.1f %.1f, non-black %.1f%%", i, e.slice, (int)e.sc->format,
				    (double)sum[0] / samples, (double)sum[1] / samples, (double)sum[2] / samples, 100.0 * nonzero / samples);
			}
		} else {
			memset(buf.data() + off, 0, (size_t)w * h * 4);
			static int warned = 0;
			if (warned++ < 5)
				VLOG("Map(staging) failed 0x%08lx", hr);
		}
		off += (size_t)w * h * 4;
	}
	FrameDiagnostics::Operation operation(s->diagnostics, FrameDiagnostics::Stage::CommitFrame);
	bc.CommitFrame();
}

static XrResult XRAPI_CALL rt_xrEndFrame(XrSession session, const XrFrameEndInfo* info)
{
	LOCK();
	GET_SESSION(s, session);
	if (!s->running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!s->frameBegun)
		return XR_ERROR_CALL_ORDER_INVALID;
	s->frameBegun = 0;

	// Build this frame's submission from the first projection layer.
	PendingFrame cur;
	cur.frameId = s->waitedTick.frameId;
	cur.displayTime = info->displayTime;
	for (uint32_t li = 0; li < info->layerCount && !cur.valid; li++) {
		const XrCompositionLayerBaseHeader* l = info->layers[li];
		if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
			continue;
		auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(l);
		Space* sp = reinterpret_cast<Space*>(proj->space);
		XrPosef spaceInStage;
		if (!sp || !SpaceInStage(sp, info->displayTime, spaceInStage))
			spaceInStage = PIdentity();
		int n = (int)std::min<uint32_t>(proj->viewCount, 2);
		bool ok = n == 2;
		for (int v = 0; v < n && ok; v++) {
			const XrCompositionLayerProjectionView& pv = proj->views[v];
			Swapchain* sc = AsSwapchain(pv.subImage.swapchain);
			if (!sc || !sc->readback || sc->lastStagingSlot < 0) {
				ok = false;
				break;
			}
			XrRect2Di r = pv.subImage.imageRect;
			if (r.extent.width <= 0 || r.extent.height <= 0) {
				r.offset = { 0, 0 };
				r.extent = { (int32_t)sc->info.width, (int32_t)sc->info.height };
			}
			r.extent.width = std::min<int32_t>(r.extent.width, (int32_t)sc->info.width - r.offset.x);
			r.extent.height = std::min<int32_t>(r.extent.height, (int32_t)sc->info.height - r.offset.y);
			uint32_t slice = pv.subImage.imageArrayIndex < sc->slices ? pv.subImage.imageArrayIndex : 0;
			cur.eyes[v] = { sc, sc->lastStagingSlot, slice, r, PMul(spaceInStage, pv.pose), pv.fov };
		}
		if (ok) {
			cur.eyeCount = 2;
			cur.valid = true;
		}
	}

	if (cur.valid) {
		FrameDiagnostics::Operation operation(s->diagnostics, FrameDiagnostics::Stage::Flush);
		s->ctx->Flush(); // get the staging copies moving
	}

	if (GetConfig().frameLatency == 0) {
		if (cur.valid)
			ShipFrame(s, cur);
	} else {
		if (s->pending.valid)
			ShipFrame(s, s->pending);
		s->pending = cur;
	}

	s->framesEnded++;
	s->diagnostics.FrameCompleted();
	if (s->framesEnded == 1) {
		SetSessionState(s, XR_SESSION_STATE_SYNCHRONIZED);
		SetSessionState(s, XR_SESSION_STATE_VISIBLE);
		SetSessionState(s, XR_SESSION_STATE_FOCUSED);
	}
	if (s->framesEnded == 1 || s->framesEnded % 600 == 0)
		VLOG("xrEndFrame #%llu (frameId %lld, layers %u, streamed=%d)", (unsigned long long)s->framesEnded,
		    (long long)cur.frameId, info->layerCount, (int)cur.valid);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* viewState, uint32_t cap, uint32_t* count, XrView* views)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !viewState || !count)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	*count = 2;
	if (cap == 0)
		return XR_SUCCESS;
	if (cap < 2)
		return XR_ERROR_SIZE_INSUFFICIENT;

	VrcmTick t{};
	bool have = GetTickFor(info->displayTime, t);
	Space* sp = reinterpret_cast<Space*>(info->space);
	XrPosef spaceInStage;
	bool spaceValid = sp && SpaceInStage(sp, info->displayTime, spaceInStage);

	viewState->viewStateFlags = 0;
	if (have && t.viewsValid && spaceValid) {
		viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
		    XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
	}
	XrPosef inv = spaceValid ? PInv(spaceInStage) : PIdentity();
	for (int i = 0; i < 2; i++) {
		if (have) {
			views[i].pose = PMul(inv, FromWire(t.eyePose[i]));
			views[i].fov = { t.eyeFov[i].angleLeft, t.eyeFov[i].angleRight, t.eyeFov[i].angleUp, t.eyeFov[i].angleDown };
		} else {
			views[i].pose = PIdentity();
			views[i].pose.position.x = i == 0 ? -0.032f : 0.032f;
			views[i].fov = { -0.8f, 0.8f, 0.8f, -0.8f };
		}
	}
	return XR_SUCCESS;
}

// ============================================================================================
// Paths
// ============================================================================================

static XrResult XRAPI_CALL rt_xrStringToPath(XrInstance instance, const char* pathString, XrPath* path)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!path)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!ValidPathString(pathString))
		return XR_ERROR_PATH_FORMAT_INVALID;
	*path = PathFor(pathString);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrPathToString(XrInstance instance, XrPath path, uint32_t cap, uint32_t* count, char* buffer)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	const std::string* s = PathString(path);
	if (!s)
		return XR_ERROR_PATH_INVALID;
	return TwoCallString(cap, count, buffer, *s);
}

// ============================================================================================
// Actions
// ============================================================================================

static XrResult XRAPI_CALL rt_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* ci, XrActionSet* actionSet)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!ci || !actionSet)
		return XR_ERROR_VALIDATION_FAILURE;
	ActionSet* as = new ActionSet();
	as->name = ci->actionSetName;
	as->priority = ci->priority;
	inst->actionSets.push_back(as);
	*actionSet = reinterpret_cast<XrActionSet>(as);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroyActionSet(XrActionSet actionSet)
{
	LOCK();
	ActionSet* as = reinterpret_cast<ActionSet*>(actionSet);
	if (!as || !g_instance)
		return XR_ERROR_HANDLE_INVALID;
	auto& v = g_instance->actionSets;
	v.erase(std::remove(v.begin(), v.end(), as), v.end());
	for (auto& kv : g_instance->suggested) {
		auto& list = kv.second;
		list.erase(std::remove_if(list.begin(), list.end(), [&](auto& p) { return p.first->set == as; }), list.end());
	}
	for (Action* a : as->actions)
		delete a;
	delete as;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* ci, XrAction* action)
{
	LOCK();
	ActionSet* as = reinterpret_cast<ActionSet*>(actionSet);
	if (!as)
		return XR_ERROR_HANDLE_INVALID;
	if (!ci || !action)
		return XR_ERROR_VALIDATION_FAILURE;
	if (as->attached)
		return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
	Action* a = new Action();
	a->set = as;
	a->name = ci->actionName;
	a->type = ci->actionType;
	for (uint32_t i = 0; i < ci->countSubactionPaths; i++)
		a->subactions.push_back(ci->subactionPaths[i]);
	as->actions.push_back(a);
	*action = reinterpret_cast<XrAction>(a);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrDestroyAction(XrAction action)
{
	LOCK();
	Action* a = reinterpret_cast<Action*>(action);
	if (!a)
		return XR_ERROR_HANDLE_INVALID;
	auto& v = a->set->actions;
	v.erase(std::remove(v.begin(), v.end(), a), v.end());
	if (g_instance)
		for (auto& kv : g_instance->suggested) {
			auto& list = kv.second;
			list.erase(std::remove_if(list.begin(), list.end(), [&](auto& p) { return p.first == a; }), list.end());
		}
	delete a;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* sb)
{
	LOCK();
	GET_INSTANCE(inst, instance);
	if (!sb)
		return XR_ERROR_VALIDATION_FAILURE;
	const std::string* prof = PathString(sb->interactionProfile);
	if (!prof)
		return XR_ERROR_PATH_INVALID;
	// Lenient on purpose: OpenComposite aborts on any failure here, and we only ever use Touch.
	std::vector<std::pair<Action*, XrPath>> list;
	for (uint32_t i = 0; i < sb->countSuggestedBindings; i++)
		list.push_back({ reinterpret_cast<Action*>(sb->suggestedBindings[i].action), sb->suggestedBindings[i].binding });
	inst->suggested[sb->interactionProfile] = list;
	if (GetConfig().logVerbose || *prof == kTouchProfile)
		VLOG("Suggested %u bindings for %s", sb->countSuggestedBindings, prof->c_str());
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	if (s->attached)
		return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;

	XrPath touch = PathFor(kTouchProfile);
	auto it = s->instance->suggested.find(touch);
	int bound = 0;
	for (uint32_t i = 0; i < info->countActionSets; i++) {
		ActionSet* as = reinterpret_cast<ActionSet*>(info->actionSets[i]);
		as->attached = true;
		for (Action* a : as->actions) {
			a->bindings.clear();
			for (auto& st : a->state)
				st = ActionState{};
			if (it == s->instance->suggested.end())
				continue;
			for (auto& pr : it->second) {
				if (pr.first != a)
					continue;
				const std::string* bp = PathString(pr.second);
				Binding b;
				if (bp && ParseTouchBinding(*bp, b)) {
					b.path = pr.second;
					a->bindings.push_back(b);
					bound++;
				} else if (bp) {
					VLOG("Ignoring unknown Touch binding %s for action %s", bp->c_str(), a->name.c_str());
				}
			}
		}
	}
	s->attached = true;
	bool haveTouch = it != s->instance->suggested.end();
	s->currentProfile[0] = s->currentProfile[1] = haveTouch ? touch : XR_NULL_PATH;
	VLOG("xrAttachSessionActionSets: %u sets, %d Touch bindings active%s", info->countActionSets, bound,
	    haveTouch ? "" : " (app suggested no Touch bindings!)");

	XrEventDataInteractionProfileChanged ev{ XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED };
	ev.session = session;
	PushEvent(s->instance, &ev, sizeof(ev));
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath, XrInteractionProfileState* state)
{
	LOCK();
	GET_SESSION(s, session);
	if (!state)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!s->attached)
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	const std::string* p = PathString(topLevelUserPath);
	state->interactionProfile = XR_NULL_PATH;
	if (!p)
		return XR_ERROR_PATH_INVALID;
	if (*p == "/user/hand/left")
		state->interactionProfile = s->currentProfile[0];
	else if (*p == "/user/hand/right")
		state->interactionProfile = s->currentProfile[1];
	return XR_SUCCESS;
}

static void UpdateActionState(Action* a, const VrcmTick& t, XrTime now)
{
	for (int slot = 0; slot < 3; slot++) {
		ActionState next{};
		for (const Binding& b : a->bindings) {
			if (slot == 1 && b.hand != 0) continue;
			if (slot == 2 && b.hand != 1) continue;
			const VrcmHand& h = t.hands[b.hand];
			if (!h.active) continue;
			CompKind k = KindOf(b.comp);
			if (k == K_POSE || k == K_HAPTIC) {
				next.active = true;
				continue;
			}
			next.active = true;
			switch (a->type) {
			case XR_ACTION_TYPE_BOOLEAN_INPUT: {
				float v = k == K_VEC2 ? std::hypot(h.stickX, h.stickY) : ReadFloat(h, b.comp);
				next.b = next.b || v > 0.5f;
				break;
			}
			case XR_ACTION_TYPE_FLOAT_INPUT: {
				float v = k == K_VEC2 ? std::hypot(h.stickX, h.stickY) : ReadFloat(h, b.comp);
				if (std::fabs(v) > std::fabs(next.f)) next.f = v;
				break;
			}
			case XR_ACTION_TYPE_VECTOR2F_INPUT:
				if (k == K_VEC2) {
					if (std::hypot(h.stickX, h.stickY) >= std::hypot(next.v.x, next.v.y))
						next.v = { h.stickX, h.stickY };
				}
				break;
			default:
				break;
			}
		}
		ActionState& cur = a->state[slot];
		bool changed = false;
		if (next.active && cur.active) {
			changed = (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT && next.b != cur.b) ||
			    (a->type == XR_ACTION_TYPE_FLOAT_INPUT && next.f != cur.f) ||
			    (a->type == XR_ACTION_TYPE_VECTOR2F_INPUT && (next.v.x != cur.v.x || next.v.y != cur.v.y));
		}
		next.changed = changed;
		next.lastChange = changed ? now : cur.lastChange;
		cur = next;
	}
}

static XrResult XRAPI_CALL rt_xrSyncActions(XrSession session, const XrActionsSyncInfo* info)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!s->attached)
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	VrcmTick t{};
	if (!BridgeClient::Get().LatestTick(t))
		return s->state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
	s->inputTick = t;
	s->haveInputTick = true;
	XrTime now = t.predictedDisplayTime;

	if (s->state != XR_SESSION_STATE_FOCUSED) {
		// Not focused: every action is inactive.
		for (uint32_t i = 0; i < info->countActiveActionSets; i++) {
			ActionSet* as = reinterpret_cast<ActionSet*>(info->activeActionSets[i].actionSet);
			for (Action* a : as->actions)
				for (auto& st : a->state)
					st = ActionState{};
		}
		return XR_SESSION_NOT_FOCUSED;
	}

	for (uint32_t i = 0; i < info->countActiveActionSets; i++) {
		ActionSet* as = reinterpret_cast<ActionSet*>(info->activeActionSets[i].actionSet);
		if (!as || !as->attached)
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		for (Action* a : as->actions)
			UpdateActionState(a, t, now);
	}
	return XR_SUCCESS;
}

static int SlotFor(Action* a, XrPath sub, XrResult& err)
{
	err = XR_SUCCESS;
	if (sub == XR_NULL_PATH)
		return 0;
	if (std::find(a->subactions.begin(), a->subactions.end(), sub) == a->subactions.end()) {
		err = XR_ERROR_PATH_UNSUPPORTED;
		return 0;
	}
	const std::string* p = PathString(sub);
	if (p && *p == "/user/hand/left") return 1;
	if (p && *p == "/user/hand/right") return 2;
	return -1; // a subaction path we don't model (head, gamepad...): always inactive
}

#define GET_ACTION_SLOT(info, expectedType)                                   \
	Action* a = reinterpret_cast<Action*>((info)->action);                   \
	if (!a) return XR_ERROR_HANDLE_INVALID;                                   \
	if (a->type != (expectedType)) return XR_ERROR_ACTION_TYPE_MISMATCH;      \
	if (!a->set->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;            \
	XrResult serr;                                                            \
	int slot = SlotFor(a, (info)->subactionPath, serr);                       \
	if (serr != XR_SUCCESS) return serr;                                      \
	ActionState st = slot >= 0 ? a->state[slot] : ActionState{}

static XrResult XRAPI_CALL rt_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* state)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	GET_ACTION_SLOT(info, XR_ACTION_TYPE_BOOLEAN_INPUT);
	state->currentState = st.b;
	state->changedSinceLastSync = st.changed;
	state->lastChangeTime = st.lastChange;
	state->isActive = st.active;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* state)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	GET_ACTION_SLOT(info, XR_ACTION_TYPE_FLOAT_INPUT);
	state->currentState = st.f;
	state->changedSinceLastSync = st.changed;
	state->lastChangeTime = st.lastChange;
	state->isActive = st.active;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* info, XrActionStateVector2f* state)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	GET_ACTION_SLOT(info, XR_ACTION_TYPE_VECTOR2F_INPUT);
	state->currentState = st.v;
	state->changedSinceLastSync = st.changed;
	state->lastChangeTime = st.lastChange;
	state->isActive = st.active;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* state)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	GET_ACTION_SLOT(info, XR_ACTION_TYPE_POSE_INPUT);
	state->isActive = st.active;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* ci, XrSpace* space)
{
	LOCK();
	GET_SESSION(s, session);
	if (!ci || !space)
		return XR_ERROR_VALIDATION_FAILURE;
	Action* a = reinterpret_cast<Action*>(ci->action);
	if (!a)
		return XR_ERROR_HANDLE_INVALID;
	if (a->type != XR_ACTION_TYPE_POSE_INPUT)
		return XR_ERROR_ACTION_TYPE_MISMATCH;
	Space* sp = new Space{ s, SK_ACTION, XR_REFERENCE_SPACE_TYPE_STAGE, a, ci->subactionPath, ci->poseInActionSpace };
	s->spaces.insert(sp);
	*space = reinterpret_cast<XrSpace>(sp);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL rt_xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo* info, uint32_t cap, uint32_t* count, XrPath* sources)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	Action* a = reinterpret_cast<Action*>(info->action);
	if (!a)
		return XR_ERROR_HANDLE_INVALID;
	if (!a->set->attached)
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	std::vector<XrPath> out;
	for (const Binding& b : a->bindings)
		if (std::find(out.begin(), out.end(), b.path) == out.end())
			out.push_back(b.path);
	return TwoCall<XrPath>(cap, count, sources, out);
}

static XrResult XRAPI_CALL rt_xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo* info, uint32_t cap, uint32_t* count, char* buffer)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	const std::string* p = PathString(info->sourcePath);
	if (!p)
		return XR_ERROR_PATH_INVALID;
	std::string name;
	if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT)
		name += p->find("/left") != std::string::npos ? "Left Hand " : "Right Hand ";
	if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT)
		name += "Oculus Touch Controller ";
	if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT) {
		size_t in = p->find("/input/");
		name += in != std::string::npos ? p->substr(in + 7) : *p;
	}
	if (!name.empty() && name.back() == ' ')
		name.pop_back();
	return TwoCallString(cap, count, buffer, name);
}

static void SendHapticFor(Action* a, XrPath sub, float amplitude, int64_t duration, float freq)
{
	bool sent[2] = { false, false };
	XrResult err;
	int slot = SlotFor(a, sub, err);
	for (const Binding& b : a->bindings) {
		if (b.comp != C_HAPTIC || sent[b.hand])
			continue;
		if (slot == 1 && b.hand != 0) continue;
		if (slot == 2 && b.hand != 1) continue;
		if (slot < 0) continue;
		VrcmHaptic h{ (uint32_t)b.hand, amplitude, duration, freq };
		BridgeClient::Get().SendHaptic(h);
		sent[b.hand] = true;
	}
}

static XrResult XRAPI_CALL rt_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* info, const XrHapticBaseHeader* haptic)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info || !haptic)
		return XR_ERROR_VALIDATION_FAILURE;
	Action* a = reinterpret_cast<Action*>(info->action);
	if (!a)
		return XR_ERROR_HANDLE_INVALID;
	if (a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
		return XR_ERROR_ACTION_TYPE_MISMATCH;
	if (haptic->type != XR_TYPE_HAPTIC_VIBRATION)
		return XR_ERROR_VALIDATION_FAILURE;
	auto* v = reinterpret_cast<const XrHapticVibration*>(haptic);
	if (s->state == XR_SESSION_STATE_FOCUSED)
		SendHapticFor(a, info->subactionPath, v->amplitude, v->duration, v->frequency);
	return s->state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

static XrResult XRAPI_CALL rt_xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* info)
{
	LOCK();
	GET_SESSION(s, session);
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	Action* a = reinterpret_cast<Action*>(info->action);
	if (!a)
		return XR_ERROR_HANDLE_INVALID;
	SendHapticFor(a, info->subactionPath, 0.f, 0, 0.f);
	return XR_SUCCESS;
}

// ============================================================================================
// Dispatch
// ============================================================================================

static XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

struct FnEntry {
	const char* name;
	PFN_xrVoidFunction fn;
	bool nullInstanceOk;
};

#define FN(n) { #n, reinterpret_cast<PFN_xrVoidFunction>(rt_##n), false }
#define FN0(n) { #n, reinterpret_cast<PFN_xrVoidFunction>(rt_##n), true }

static const FnEntry kFunctions[] = {
	FN0(xrGetInstanceProcAddr),
	FN0(xrEnumerateInstanceExtensionProperties),
	FN0(xrEnumerateApiLayerProperties),
	FN0(xrCreateInstance),
	FN(xrDestroyInstance),
	FN(xrGetInstanceProperties),
	FN(xrPollEvent),
	FN(xrResultToString),
	FN(xrStructureTypeToString),
	FN(xrGetSystem),
	FN(xrGetSystemProperties),
	FN(xrEnumerateEnvironmentBlendModes),
	FN(xrCreateSession),
	FN(xrDestroySession),
	FN(xrEnumerateReferenceSpaces),
	FN(xrCreateReferenceSpace),
	FN(xrGetReferenceSpaceBoundsRect),
	FN(xrCreateActionSpace),
	FN(xrLocateSpace),
	FN(xrLocateSpaces),
	FN(xrDestroySpace),
	FN(xrEnumerateViewConfigurations),
	FN(xrGetViewConfigurationProperties),
	FN(xrEnumerateViewConfigurationViews),
	FN(xrEnumerateSwapchainFormats),
	FN(xrCreateSwapchain),
	FN(xrDestroySwapchain),
	FN(xrEnumerateSwapchainImages),
	FN(xrAcquireSwapchainImage),
	FN(xrWaitSwapchainImage),
	FN(xrReleaseSwapchainImage),
	FN(xrBeginSession),
	FN(xrEndSession),
	FN(xrRequestExitSession),
	FN(xrWaitFrame),
	FN(xrBeginFrame),
	FN(xrEndFrame),
	FN(xrLocateViews),
	FN(xrStringToPath),
	FN(xrPathToString),
	FN(xrCreateActionSet),
	FN(xrDestroyActionSet),
	FN(xrCreateAction),
	FN(xrDestroyAction),
	FN(xrSuggestInteractionProfileBindings),
	FN(xrAttachSessionActionSets),
	FN(xrGetCurrentInteractionProfile),
	FN(xrGetActionStateBoolean),
	FN(xrGetActionStateFloat),
	FN(xrGetActionStateVector2f),
	FN(xrGetActionStatePose),
	FN(xrSyncActions),
	FN(xrEnumerateBoundSourcesForAction),
	FN(xrGetInputSourceLocalizedName),
	FN(xrApplyHapticFeedback),
	FN(xrStopHapticFeedback),
	FN(xrGetD3D11GraphicsRequirementsKHR),
};

static XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
	if (!name || !function)
		return XR_ERROR_VALIDATION_FAILURE;
	*function = nullptr;
	for (const FnEntry& e : kFunctions) {
		if (strcmp(e.name, name) != 0)
			continue;
		if (instance == XR_NULL_HANDLE && !e.nullInstanceOk)
			return XR_ERROR_HANDLE_INVALID;
		if (strcmp(name, "xrGetD3D11GraphicsRequirementsKHR") == 0) {
			LOCK();
			Instance* inst = AsInstance(instance);
			if (!inst || !inst->enabledExts.count(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
				return XR_ERROR_FUNCTION_UNSUPPORTED;
		}
		*function = e.fn;
		return XR_SUCCESS;
	}
	VLOG("xrGetInstanceProcAddr: unsupported %s", name);
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* request)
{
	if (!loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
	    request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
	    loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
	    loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION ||
	    loaderInfo->minApiVersion > XR_API_VERSION_1_0) {
		VLOG("Loader negotiation failed");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	request->runtimeApiVersion = XR_API_VERSION_1_0;
	request->getInstanceProcAddr = rt_xrGetInstanceProcAddr;
	VLOG("VRConMac runtime loaded (bridge %s:%d, frame_latency=%d)", GetConfig().host.c_str(), GetConfig().port,
	    GetConfig().frameLatency);
	return XR_SUCCESS;
}
