// OpenXR backend: drives a native macOS OpenXR runtime (OXRSys, Meta XR Simulator, ...).
//
// Each host frame: xrWaitFrame -> locate head/eyes/controllers -> send TICK to the guest ->
// upload the newest guest frame (if any) into our swapchains -> xrEndFrame with the pose the
// guest actually rendered with (so the runtime / headset can reproject correctly).

#include "backend.h"
#include "xr_gfx.h"

#include <chrono>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

#define XR_CHECK(call)                                                                          \
	do {                                                                                        \
		XrResult _r = (call);                                                                   \
		if (XR_FAILED(_r)) {                                                                    \
			fprintf(stderr, "[xr] %s failed: %s\n", #call, xr_.ResultString(inst_, _r).c_str()); \
			return false;                                                                       \
		}                                                                                       \
	} while (0)

enum BoolAct { B_TRIGGER_TOUCH, B_STICK_CLICK, B_STICK_TOUCH, B_PRIMARY_CLICK, B_PRIMARY_TOUCH, B_SECONDARY_CLICK,
	B_SECONDARY_TOUCH, B_MENU_CLICK, B_THUMBREST_TOUCH, B_COUNT };
static const char* kBoolNames[B_COUNT] = { "trigger_touch", "stick_click", "stick_touch", "primary_click", "primary_touch",
	"secondary_click", "secondary_touch", "menu_click", "thumbrest_touch" };
static const uint32_t kBoolBits[B_COUNT] = { VRCM_BTN_TRIGGER_TOUCH, VRCM_BTN_STICK_CLICK, VRCM_BTN_STICK_TOUCH,
	VRCM_BTN_PRIMARY_CLICK, VRCM_BTN_PRIMARY_TOUCH, VRCM_BTN_SECONDARY_CLICK, VRCM_BTN_SECONDARY_TOUCH,
	VRCM_BTN_MENU_CLICK, VRCM_BTN_THUMBREST_TOUCH };

class OpenXRBackend : public Backend {
public:
	~OpenXRBackend() override { Shutdown(); }

	bool Init(const BridgeOptions& opt, VrcmHelloAck& ack) override
	{
		opt_ = opt;
		gfx_ = CreateMetalGfx();
		if (!gfx_) {
			fprintf(stderr, "[xr] no graphics backend in this build (the openxr backend needs macOS/Metal)\n");
			return false;
		}

		std::string manifest = opt.runtimeJson.empty() ? DefaultRuntimeManifest() : opt.runtimeJson;
		std::string err;
		if (!xr_.Load(manifest, err)) {
			fprintf(stderr, "[xr] %s\n", err.c_str());
			return false;
		}

		// Instance
		const char* exts[] = { gfx_->ExtensionName() };
		XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
		strcpy(ici.applicationInfo.applicationName, "VRConMac Bridge");
		strcpy(ici.applicationInfo.engineName, "VRConMac");
		ici.applicationInfo.applicationVersion = 1;
		ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
		ici.enabledExtensionCount = 1;
		ici.enabledExtensionNames = exts;
		XR_CHECK(xr_.xrCreateInstance(&ici, &inst_));
		if (!xr_.LoadInstanceFunctions(inst_, err)) {
			fprintf(stderr, "[xr] %s\n", err.c_str());
			return false;
		}

		// System (the headset may not be connected yet: keep asking for a while)
		XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		for (int tries = 0;; tries++) {
			XrResult r = xr_.xrGetSystem(inst_, &sgi, &sys_);
			if (XR_SUCCEEDED(r))
				break;
			if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE || tries > 120) {
				fprintf(stderr, "[xr] xrGetSystem: %s\n", xr_.ResultString(inst_, r).c_str());
				return false;
			}
			if (tries % 10 == 0)
				printf("[xr] waiting for headset (start the OXRSys client on the Quest)...\n");
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
		XR_CHECK(xr_.xrGetSystemProperties(inst_, sys_, &sp));

		uint32_t n = 0;
		XR_CHECK(xr_.xrEnumerateViewConfigurationViews(inst_, sys_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
		std::vector<XrViewConfigurationView> vcv(n, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
		XR_CHECK(xr_.xrEnumerateViewConfigurationViews(inst_, sys_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vcv.data()));
		if (n != 2) {
			fprintf(stderr, "[xr] expected 2 views, got %u\n", n);
			return false;
		}
		maxW_ = vcv[0].maxImageRectWidth;
		recW_ = std::max(64u, (uint32_t)std::lround(vcv[0].recommendedImageRectWidth * opt.renderScale)) & ~1u;
		recH_ = std::max(64u, (uint32_t)std::lround(vcv[0].recommendedImageRectHeight * opt.renderScale)) & ~1u;
		maxH_ = vcv[0].maxImageRectHeight;

		if (!gfx_->Init(xr_, inst_, sys_, err)) {
			fprintf(stderr, "[xr] %s\n", err.c_str());
			return false;
		}

		// Session
		XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
		sci.next = gfx_->SessionBinding();
		sci.systemId = sys_;
		XR_CHECK(xr_.xrCreateSession(inst_, &sci, &session_));

		// Spaces: prefer STAGE (floor origin). Otherwise LOCAL with a faked floor.
		XR_CHECK(xr_.xrEnumerateReferenceSpaces(session_, 0, &n, nullptr));
		std::vector<XrReferenceSpaceType> spaces(n);
		XR_CHECK(xr_.xrEnumerateReferenceSpaces(session_, n, &n, spaces.data()));
		for (auto s : spaces)
			if (s == XR_REFERENCE_SPACE_TYPE_STAGE)
				haveStage_ = true;
		XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		rsci.poseInReferenceSpace.orientation.w = 1;
		rsci.referenceSpaceType = haveStage_ ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
		XR_CHECK(xr_.xrCreateReferenceSpace(session_, &rsci, &base_));
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		XR_CHECK(xr_.xrCreateReferenceSpace(session_, &rsci, &view_));
		floorOffset_ = haveStage_ ? 0.f : opt.floorHeight;

		XrExtent2Df bounds{ 0, 0 };
		if (haveStage_)
			xr_.xrGetReferenceSpaceBoundsRect(session_, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);

		if (!CreateActions())
			return false;

		ack.recommendedWidth = recW_;
		ack.recommendedHeight = recH_;
		ack.maxWidth = maxW_;
		ack.maxHeight = maxH_;
		ack.refreshRateHz = 72.f;
		ack.playAreaWidth = bounds.width > 0 ? bounds.width : 2.f;
		ack.playAreaDepth = bounds.height > 0 ? bounds.height : 2.f;
		snprintf(ack.systemName, sizeof(ack.systemName), "%s via %s", sp.systemName, xr_.runtimeName.c_str());

		printf("[xr] system '%s', recommended %ux%u/eye (max %ux%u), space=%s\n", sp.systemName,
		    vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, maxW_, maxH_,
		    haveStage_ ? "STAGE" : "LOCAL (+fake floor)");
		return true;
	}

	void Run(BridgeServer& server, std::atomic<bool>& quit) override
	{
		auto lastReport = std::chrono::steady_clock::now();
		uint64_t lastRecv = 0, submitted = 0, uploaded = 0;

		while (!quit) {
			if (!PollEvents()) {
				quit = true;
				break;
			}
			if (!running_) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			XrFrameState fs{ XR_TYPE_FRAME_STATE };
			XrResult r = xr_.xrWaitFrame(session_, nullptr, &fs);
			if (XR_FAILED(r)) {
				fprintf(stderr, "[xr] xrWaitFrame: %s\n", xr_.ResultString(inst_, r).c_str());
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			if (fs.predictedDisplayPeriod > 0)
				server.helloAck.refreshRateHz = (float)(1e9 / (double)fs.predictedDisplayPeriod);
			xr_.xrBeginFrame(session_, nullptr);

			VrcmTick tick{};
			tick.frameId = ++frameId_;
			tick.predictedDisplayTime = fs.predictedDisplayTime;
			tick.predictedPeriod = fs.predictedDisplayPeriod;
			tick.shouldRender = fs.shouldRender;
			FillViews(fs.predictedDisplayTime, tick);
			FillHands(fs.predictedDisplayTime, tick);
			server.SendTick(tick);

			VrcmHaptic hp;
			while (server.PopHaptic(hp))
				ApplyHaptic(hp);

			// Newest guest frame
			if (server.TakeLatestFrame(frameSerial_, frame_))
				frameSerial_ = frame_->serial;
			bool fresh = frame_ && server.GuestSessionRunning() && frame_->hdr.eyeCount == 2 &&
			    (MonotonicNs() - frame_->receivedAtNs) < 2'000'000'000LL;

			XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
				{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
			XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
			const XrCompositionLayerBaseHeader* layers[1] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
			uint32_t layerCount = 0;

			// Without a guest, optionally show a local test pattern (checks the Metal/OXRSys path alone).
			const GuestFrame* use = fresh ? frame_.get() : nullptr;
			if (!use && opt_.testPattern && tick.viewsValid) {
				BuildTestPattern(tick);
				use = pattern_.get();
			}

			if (use && fs.shouldRender && EnsureSwapchains(*use)) {
				bool ok = true;
				if (use->serial != uploadedSerial_) {
					for (int e = 0; e < 2 && ok; e++)
						ok = UploadEye(e, use->eyes[e]);
					if (ok) {
						uploadedSerial_ = use->serial;
						uploaded++;
					}
				}
				if (ok) {
					for (int e = 0; e < 2; e++) {
						const VrcmEyeImage& h = use->eyes[e].hdr;
						pv[e].pose = FromWire(h.pose);
						pv[e].fov = { h.fov.angleLeft, h.fov.angleRight, h.fov.angleUp, h.fov.angleDown };
						pv[e].subImage.swapchain = sc_[e];
						pv[e].subImage.imageRect = { { 0, 0 }, { (int32_t)h.width, (int32_t)h.height } };
						pv[e].subImage.imageArrayIndex = 0;
					}
					layer.space = base_;
					layer.viewCount = 2;
					layer.views = pv;
					layerCount = 1;
					submitted++;
				}
			}

			XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
			fei.displayTime = fs.predictedDisplayTime;
			fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			fei.layerCount = layerCount;
			fei.layers = layerCount ? layers : nullptr;
			r = xr_.xrEndFrame(session_, &fei);
			if (XR_FAILED(r))
				fprintf(stderr, "[xr] xrEndFrame: %s\n", xr_.ResultString(inst_, r).c_str());

			auto now = std::chrono::steady_clock::now();
			double secs = std::chrono::duration<double>(now - lastReport).count();
			if (secs >= 5.0) {
				uint64_t recv = server.framesReceived.load();
				printf("[bridge] host %.1f Hz | guest %s, %.1f fps received, %.1f uploaded/s, %.1f MB/s%s\n",
				    server.helloAck.refreshRateHz,
				    server.GuestConnected() ? (server.GuestSessionRunning() ? "running" : "connected") : "not connected",
				    (recv - lastRecv) / secs, uploaded / secs, server.bytesReceived.exchange(0) / secs / 1e6,
				    submitted ? "" : " (nothing submitted)");
				lastRecv = recv;
				uploaded = submitted = 0;
				lastReport = now;
			}
		}
		Shutdown();
	}

private:
	// ---- poses: wire STAGE == our base space, raised by floorOffset_ when base is LOCAL ----
	VrcmPose ToWire(const XrPosef& p) const
	{
		return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w },
			{ p.position.x, p.position.y + floorOffset_, p.position.z } };
	}
	XrPosef FromWire(const VrcmPose& p) const
	{
		return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w },
			{ p.position.x, p.position.y - floorOffset_, p.position.z } };
	}

	bool PollEvents()
	{
		XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
		while (xr_.xrPollEvent(inst_, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
				printf("[xr] session state %d\n", (int)sc->state);
				switch (sc->state) {
				case XR_SESSION_STATE_READY: {
					XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (XR_SUCCEEDED(xr_.xrBeginSession(session_, &bi)))
						running_ = true;
					break;
				}
				case XR_SESSION_STATE_STOPPING:
					xr_.xrEndSession(session_);
					running_ = false;
					break;
				case XR_SESSION_STATE_EXITING:
				case XR_SESSION_STATE_LOSS_PENDING:
					printf("[xr] runtime is ending the session\n");
					return false;
				default:
					break;
				}
			} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
				printf("[xr] instance loss pending\n");
				return false;
			}
			ev = { XR_TYPE_EVENT_DATA_BUFFER };
		}
		return true;
	}

	void FillViews(XrTime t, VrcmTick& tick)
	{
		XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = t;
		vli.space = base_;
		XrViewState vs{ XR_TYPE_VIEW_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t n = 0;
		if (XR_SUCCEEDED(xr_.xrLocateViews(session_, &vli, &vs, 2, &n, views)) && n == 2 &&
		    (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
			tick.viewsValid = 1;
			for (int e = 0; e < 2; e++) {
				tick.eyePose[e] = ToWire(views[e].pose);
				tick.eyeFov[e] = { views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown };
			}
		}
		XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(xr_.xrLocateSpace(view_, base_, t, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			tick.headPose = ToWire(loc.pose);
		else
			tick.headPose = tick.eyePose[0];
	}

	// ---- input ----
	XrAction MakeAction(const char* name, XrActionType type)
	{
		XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
		aci.actionType = type;
		snprintf(aci.actionName, sizeof(aci.actionName), "%s", name);
		snprintf(aci.localizedActionName, sizeof(aci.localizedActionName), "%s", name);
		aci.countSubactionPaths = 2;
		aci.subactionPaths = hands_;
		XrAction a = XR_NULL_HANDLE;
		XrResult r = xr_.xrCreateAction(actionSet_, &aci, &a);
		if (XR_FAILED(r))
			fprintf(stderr, "[xr] xrCreateAction(%s): %s\n", name, xr_.ResultString(inst_, r).c_str());
		return a;
	}

	XrPath P(const char* s)
	{
		XrPath p = XR_NULL_PATH;
		xr_.xrStringToPath(inst_, s, &p);
		return p;
	}

	bool Suggest(const char* profile, const std::vector<std::pair<XrAction, std::string>>& list)
	{
		std::vector<XrActionSuggestedBinding> b;
		for (auto& [a, path] : list)
			if (a)
				b.push_back({ a, P(path.c_str()) });
		XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		sb.interactionProfile = P(profile);
		sb.countSuggestedBindings = (uint32_t)b.size();
		sb.suggestedBindings = b.data();
		XrResult r = xr_.xrSuggestInteractionProfileBindings(inst_, &sb);
		if (XR_FAILED(r))
			printf("[xr] bindings for %s rejected: %s\n", profile, xr_.ResultString(inst_, r).c_str());
		return XR_SUCCEEDED(r);
	}

	bool CreateActions()
	{
		hands_[0] = P("/user/hand/left");
		hands_[1] = P("/user/hand/right");

		XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
		strcpy(asci.actionSetName, "vrconmac");
		strcpy(asci.localizedActionSetName, "VRConMac");
		XR_CHECK(xr_.xrCreateActionSet(inst_, &asci, &actionSet_));

		gripPose_ = MakeAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT);
		aimPose_ = MakeAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT);
		trigger_ = MakeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
		squeeze_ = MakeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
		stick_ = MakeAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
		haptic_ = MakeAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
		for (int i = 0; i < B_COUNT; i++)
			bools_[i] = MakeAction(kBoolNames[i], XR_ACTION_TYPE_BOOLEAN_INPUT);

		auto touch = [&](bool withThumbrest) {
			std::vector<std::pair<XrAction, std::string>> l;
			for (const char* side : { "left", "right" }) {
				std::string h = std::string("/user/hand/") + side;
				bool left = side[0] == 'l';
				l.push_back({ gripPose_, h + "/input/grip/pose" });
				l.push_back({ aimPose_, h + "/input/aim/pose" });
				l.push_back({ trigger_, h + "/input/trigger/value" });
				l.push_back({ squeeze_, h + "/input/squeeze/value" });
				l.push_back({ stick_, h + "/input/thumbstick" });
				l.push_back({ haptic_, h + "/output/haptic" });
				l.push_back({ bools_[B_TRIGGER_TOUCH], h + "/input/trigger/touch" });
				l.push_back({ bools_[B_STICK_CLICK], h + "/input/thumbstick/click" });
				l.push_back({ bools_[B_STICK_TOUCH], h + "/input/thumbstick/touch" });
				l.push_back({ bools_[B_PRIMARY_CLICK], h + (left ? "/input/x/click" : "/input/a/click") });
				l.push_back({ bools_[B_PRIMARY_TOUCH], h + (left ? "/input/x/touch" : "/input/a/touch") });
				l.push_back({ bools_[B_SECONDARY_CLICK], h + (left ? "/input/y/click" : "/input/b/click") });
				l.push_back({ bools_[B_SECONDARY_TOUCH], h + (left ? "/input/y/touch" : "/input/b/touch") });
				if (left)
					l.push_back({ bools_[B_MENU_CLICK], h + "/input/menu/click" });
				if (withThumbrest)
					l.push_back({ bools_[B_THUMBREST_TOUCH], h + "/input/thumbrest/touch" });
			}
			return l;
		};
		if (!Suggest("/interaction_profiles/oculus/touch_controller", touch(true)) &&
		    !Suggest("/interaction_profiles/oculus/touch_controller", touch(false))) {
			fprintf(stderr, "[xr] runtime rejected Oculus Touch bindings; controllers will be limited\n");
		}
		// Fallback for runtimes/simulators that only expose the simple controller
		std::vector<std::pair<XrAction, std::string>> simple;
		for (const char* side : { "left", "right" }) {
			std::string h = std::string("/user/hand/") + side;
			simple.push_back({ gripPose_, h + "/input/grip/pose" });
			simple.push_back({ aimPose_, h + "/input/aim/pose" });
			simple.push_back({ trigger_, h + "/input/select/click" });
			simple.push_back({ bools_[B_MENU_CLICK], h + "/input/menu/click" });
			simple.push_back({ haptic_, h + "/output/haptic" });
		}
		Suggest("/interaction_profiles/khr/simple_controller", simple);

		for (int h = 0; h < 2; h++) {
			XrActionSpaceCreateInfo asi{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
			asi.poseInActionSpace.orientation.w = 1;
			asi.subactionPath = hands_[h];
			asi.action = gripPose_;
			XR_CHECK(xr_.xrCreateActionSpace(session_, &asi, &gripSpace_[h]));
			asi.action = aimPose_;
			XR_CHECK(xr_.xrCreateActionSpace(session_, &asi, &aimSpace_[h]));
		}

		XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		ai.countActionSets = 1;
		ai.actionSets = &actionSet_;
		XR_CHECK(xr_.xrAttachSessionActionSets(session_, &ai));
		return true;
	}

	void FillHands(XrTime t, VrcmTick& tick)
	{
		XrActiveActionSet aas{ actionSet_, XR_NULL_PATH };
		XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
		si.countActiveActionSets = 1;
		si.activeActionSets = &aas;
		if (XR_FAILED(xr_.xrSyncActions(session_, &si)))
			return;

		for (int h = 0; h < 2; h++) {
			VrcmHand& hand = tick.hands[h];
			XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
			gi.subactionPath = hands_[h];

			XrActionStatePose ps{ XR_TYPE_ACTION_STATE_POSE };
			gi.action = gripPose_;
			xr_.xrGetActionStatePose(session_, &gi, &ps);
			hand.active = ps.isActive;

			XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(xr_.xrLocateSpace(gripSpace_[h], base_, t, &loc)) &&
			    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
				hand.grip = ToWire(loc.pose);
				hand.gripValid = 1;
			}
			loc = { XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(xr_.xrLocateSpace(aimSpace_[h], base_, t, &loc)) &&
			    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
				hand.aim = ToWire(loc.pose);
				hand.aimValid = 1;
			}
			if (hand.gripValid || hand.aimValid)
				hand.active = 1;

			XrActionStateFloat fsTrig{ XR_TYPE_ACTION_STATE_FLOAT }, fsSq{ XR_TYPE_ACTION_STATE_FLOAT };
			gi.action = trigger_;
			xr_.xrGetActionStateFloat(session_, &gi, &fsTrig);
			gi.action = squeeze_;
			xr_.xrGetActionStateFloat(session_, &gi, &fsSq);
			hand.trigger = fsTrig.isActive ? fsTrig.currentState : 0.f;
			hand.squeeze = fsSq.isActive ? fsSq.currentState : 0.f;

			XrActionStateVector2f vs{ XR_TYPE_ACTION_STATE_VECTOR2F };
			gi.action = stick_;
			xr_.xrGetActionStateVector2f(session_, &gi, &vs);
			if (vs.isActive) {
				hand.stickX = vs.currentState.x;
				hand.stickY = vs.currentState.y;
			}
			for (int b = 0; b < B_COUNT; b++) {
				if (!bools_[b])
					continue;
				XrActionStateBoolean bs{ XR_TYPE_ACTION_STATE_BOOLEAN };
				gi.action = bools_[b];
				xr_.xrGetActionStateBoolean(session_, &gi, &bs);
				if (bs.isActive && bs.currentState)
					hand.buttons |= kBoolBits[b];
			}
		}
	}

	void ApplyHaptic(const VrcmHaptic& hp)
	{
		if (hp.hand > 1)
			return;
		XrHapticActionInfo hai{ XR_TYPE_HAPTIC_ACTION_INFO };
		hai.action = haptic_;
		hai.subactionPath = hands_[hp.hand];
		if (hp.amplitude <= 0.f) {
			xr_.xrStopHapticFeedback(session_, &hai);
			return;
		}
		XrHapticVibration vib{ XR_TYPE_HAPTIC_VIBRATION };
		vib.amplitude = hp.amplitude;
		vib.duration = hp.durationNs <= 0 ? XR_MIN_HAPTIC_DURATION : hp.durationNs;
		vib.frequency = hp.frequency > 0 ? hp.frequency : XR_FREQUENCY_UNSPECIFIED;
		xr_.xrApplyHapticFeedback(session_, &hai, reinterpret_cast<XrHapticBaseHeader*>(&vib));
	}

	// Pattern: per-eye tint + 8x8 checkerboard + a cross at the image centre. Uploaded once; the
	// pose follows the head every frame, so it should look world-locked... to your face (a HUD).
	void BuildTestPattern(const VrcmTick& tick)
	{
		if (!pattern_) {
			pattern_ = std::make_shared<GuestFrame>();
			pattern_->serial = UINT64_MAX - 1;
			pattern_->hdr.eyeCount = 2;
			for (int e = 0; e < 2; e++) {
				GuestEye& g = pattern_->eyes[e];
				g.hdr.width = recW_;
				g.hdr.height = recH_;
				g.hdr.stride = recW_ * 4;
				g.hdr.format = VRCM_PIXEL_RGBA8_SRGB;
				g.pixels.resize((size_t)g.hdr.stride * recH_);
				for (uint32_t y = 0; y < recH_; y++)
					for (uint32_t x = 0; x < recW_; x++) {
						uint8_t* p = &g.pixels[(size_t)y * g.hdr.stride + x * 4];
						bool check = ((x * 8 / recW_) + (y * 8 / recH_)) & 1;
						bool cross = (x > recW_ / 2 - 3 && x < recW_ / 2 + 3) || (y > recH_ / 2 - 3 && y < recH_ / 2 + 3);
						uint8_t v = cross ? 255 : (check ? 200 : 60);
						p[0] = e == 0 ? v : v / 3;
						p[1] = v / 3;
						p[2] = e == 1 ? v : v / 3;
						p[3] = 255;
					}
			}
			printf("[xr] showing test pattern (%ux%u per eye) until a guest connects\n", recW_, recH_);
		}
		for (int e = 0; e < 2; e++) {
			pattern_->eyes[e].hdr.pose = tick.eyePose[e];
			pattern_->eyes[e].hdr.fov = tick.eyeFov[e];
		}
	}

	// ---- swapchains ----
	bool EnsureSwapchains(const GuestFrame& f)
	{
		for (int e = 0; e < 2; e++) {
			uint32_t w = f.eyes[e].hdr.width, h = f.eyes[e].hdr.height;
			if (sc_[e] && scW_[e] == w && scH_[e] == h)
				continue;
			if (w == 0 || h == 0 || w > maxW_ * 2 || h > maxH_ * 2)
				return false;
			if (sc_[e]) {
				gfx_->ReleaseImages(e);
				xr_.xrDestroySwapchain(sc_[e]);
				sc_[e] = XR_NULL_HANDLE;
			}
			if (!format_) {
				uint32_t n = 0;
				xr_.xrEnumerateSwapchainFormats(session_, 0, &n, nullptr);
				std::vector<int64_t> fmts(n);
				xr_.xrEnumerateSwapchainFormats(session_, n, &n, fmts.data());
				format_ = gfx_->ChooseFormat(fmts, swapRB_);
				if (!format_) {
					fprintf(stderr, "[xr] runtime offers no usable 8-bit sRGB swapchain format\n");
					return false;
				}
				printf("[xr] swapchain format %lld%s\n", (long long)format_, swapRB_ ? " (BGRA)" : "");
			}
			XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
			    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			ci.format = format_;
			ci.sampleCount = 1;
			ci.width = w;
			ci.height = h;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;
			XrResult r = xr_.xrCreateSwapchain(session_, &ci, &sc_[e]);
			if (XR_FAILED(r)) {
				fprintf(stderr, "[xr] xrCreateSwapchain %ux%u: %s\n", w, h, xr_.ResultString(inst_, r).c_str());
				sc_[e] = XR_NULL_HANDLE;
				return false;
			}
			std::string err;
			if (!gfx_->BindImages(xr_, sc_[e], e, err)) {
				fprintf(stderr, "[xr] %s\n", err.c_str());
				return false;
			}
			scW_[e] = w;
			scH_[e] = h;
			uploadedSerial_ = 0;
			printf("[xr] eye %d swapchain %ux%u\n", e, w, h);
		}
		return true;
	}

	bool UploadEye(int e, const GuestEye& eye)
	{
		uint32_t idx = 0;
		XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		if (XR_FAILED(xr_.xrAcquireSwapchainImage(sc_[e], &ai, &idx)))
			return false;
		XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wi.timeout = 100'000'000;
		XrResult r = xr_.xrWaitSwapchainImage(sc_[e], &wi);
		bool ok = r == XR_SUCCESS &&
		    gfx_->Upload(e, idx, eye.pixels.data(), eye.hdr.width, eye.hdr.height, eye.hdr.stride, swapRB_);
		XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xr_.xrReleaseSwapchainImage(sc_[e], &ri);
		return ok;
	}

	void Shutdown()
	{
		if (!inst_)
			return;
		for (int e = 0; e < 2; e++)
			if (sc_[e]) {
				gfx_->ReleaseImages(e);
				xr_.xrDestroySwapchain(sc_[e]);
				sc_[e] = XR_NULL_HANDLE;
			}
		if (session_) {
			if (running_) {
				xr_.xrRequestExitSession(session_);
				running_ = false;
			}
			xr_.xrDestroySession(session_);
			session_ = XR_NULL_HANDLE;
		}
		xr_.xrDestroyInstance(inst_);
		inst_ = XR_NULL_HANDLE;
	}

	BridgeOptions opt_;
	XrApi xr_;
	std::unique_ptr<XrGfx> gfx_;
	XrInstance inst_ = XR_NULL_HANDLE;
	XrSystemId sys_ = XR_NULL_SYSTEM_ID;
	XrSession session_ = XR_NULL_HANDLE;
	XrSpace base_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE;
	bool haveStage_ = false, running_ = false;
	float floorOffset_ = 0.f;
	uint32_t maxW_ = 4096, maxH_ = 4096, recW_ = 1440, recH_ = 1584;
	std::shared_ptr<GuestFrame> pattern_;
	int64_t frameId_ = 0;

	XrPath hands_[2] = {};
	XrActionSet actionSet_ = XR_NULL_HANDLE;
	XrAction gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE,
	         stick_ = XR_NULL_HANDLE, haptic_ = XR_NULL_HANDLE;
	XrAction bools_[B_COUNT] = {};
	XrSpace gripSpace_[2] = {}, aimSpace_[2] = {};

	XrSwapchain sc_[2] = {};
	uint32_t scW_[2] = {}, scH_[2] = {};
	int64_t format_ = 0;
	bool swapRB_ = false;

	std::shared_ptr<GuestFrame> frame_;
	uint64_t frameSerial_ = 0, uploadedSerial_ = 0;
};

} // namespace

std::unique_ptr<Backend> CreateOpenXRBackend() { return std::make_unique<OpenXRBackend>(); }

#ifndef __APPLE__
// Non-Apple builds (CI / testing on Linux) have no Metal glue.
std::unique_ptr<XrGfx> CreateMetalGfx() { return nullptr; }
#endif
