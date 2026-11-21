// VRConMac wire protocol, shared by the Wine-side OpenXR runtime (guest) and the macOS bridge (host).
//
// Transport: one TCP connection, guest = client, bridge = server (default 127.0.0.1:47811).
// Every message is a MsgHeader followed by `size` payload bytes. All values little-endian
// (both ends are little-endian: x86-64 guest under FEX, arm64 host).
//
// Flow:
//   guest -> HELLO
//   bridge -> HELLO_ACK            (view sizes, refresh rate, play-area bounds)
//   bridge -> TICK ... (every host frame, after xrWaitFrame/xrLocateViews/xrSyncActions)
//   guest  -> SESSION_STATE        (1 when the guest session is running, 0 when it stops)
//   guest  -> FRAME                (eye images rendered for a given tick)
//   guest  -> HAPTIC
//
// Poses are expressed in the bridge's STAGE space (floor-level origin), in metres.
// Times are the bridge's XrTime (nanoseconds); the guest treats them as opaque.

#pragma once
#include <stdint.h>

#define VRCM_MAGIC 0x4D435256u /* 'VRCM' */
#define VRCM_PROTOCOL_VERSION 1u
#define VRCM_DEFAULT_PORT 47811

#pragma pack(push, 1)

typedef struct VrcmVec3 { float x, y, z; } VrcmVec3;
typedef struct VrcmQuat { float x, y, z, w; } VrcmQuat;
typedef struct VrcmPose { VrcmQuat orientation; VrcmVec3 position; } VrcmPose;
typedef struct VrcmFov { float angleLeft, angleRight, angleUp, angleDown; } VrcmFov;

enum VrcmMsgType {
	VRCM_MSG_HELLO = 1,
	VRCM_MSG_HELLO_ACK = 2,
	VRCM_MSG_TICK = 3,
	VRCM_MSG_FRAME = 4,
	VRCM_MSG_HAPTIC = 5,
	VRCM_MSG_SESSION_STATE = 6,
	VRCM_MSG_SHUTDOWN = 7,
};

typedef struct VrcmMsgHeader {
	uint32_t magic;
	uint16_t type;
	uint16_t reserved;
	uint32_t size; // payload bytes following this header
} VrcmMsgHeader;

typedef struct VrcmHello {
	uint32_t protocolVersion;
	char appName[128];
} VrcmHello;

typedef struct VrcmHelloAck {
	uint32_t protocolVersion;
	uint32_t recommendedWidth;  // per eye, already scaled by the bridge's --render-scale
	uint32_t recommendedHeight;
	uint32_t maxWidth;
	uint32_t maxHeight;
	float refreshRateHz;
	float playAreaWidth;  // metres, X
	float playAreaDepth;  // metres, Z
	char systemName[128];
} VrcmHelloAck;

// Button bits in VrcmHand.buttons
enum VrcmButton {
	VRCM_BTN_PRIMARY_CLICK = 1u << 0,   // A (right) / X (left)
	VRCM_BTN_PRIMARY_TOUCH = 1u << 1,
	VRCM_BTN_SECONDARY_CLICK = 1u << 2, // B (right) / Y (left)
	VRCM_BTN_SECONDARY_TOUCH = 1u << 3,
	VRCM_BTN_MENU_CLICK = 1u << 4,      // left only on Touch
	VRCM_BTN_SYSTEM_CLICK = 1u << 5,
	VRCM_BTN_STICK_CLICK = 1u << 6,
	VRCM_BTN_STICK_TOUCH = 1u << 7,
	VRCM_BTN_TRIGGER_TOUCH = 1u << 8,
	VRCM_BTN_THUMBREST_TOUCH = 1u << 9,
};

typedef struct VrcmHand {
	uint32_t active;     // controller connected/tracked
	uint32_t gripValid;  // pose validity
	uint32_t aimValid;
	uint32_t buttons;    // VrcmButton bits
	float trigger;       // 0..1
	float squeeze;       // 0..1
	float stickX, stickY; // -1..1
	VrcmPose grip;
	VrcmPose aim;
} VrcmHand;

typedef struct VrcmTick {
	int64_t frameId;              // monotonically increasing, echoed back in VrcmFrame
	int64_t predictedDisplayTime; // host XrTime
	int64_t predictedPeriod;      // ns
	uint32_t shouldRender;
	uint32_t viewsValid;          // 0 if head tracking is lost
	VrcmPose headPose;            // VIEW space origin in STAGE
	VrcmPose eyePose[2];          // in STAGE
	VrcmFov eyeFov[2];
	VrcmHand hands[2];            // 0 = left, 1 = right
} VrcmTick;

enum VrcmPixelFormat {
	VRCM_PIXEL_RGBA8_SRGB = 0, // 8-bit RGBA, sRGB-encoded (bytes go straight into an *_sRGB texture)
};

// VRCM_MSG_FRAME payload: VrcmFrame, then eyeCount x (VrcmEyeImage + height*stride pixel bytes)
typedef struct VrcmFrame {
	int64_t frameId;      // tick this frame was rendered for
	int64_t displayTime;  // displayTime the guest passed to xrEndFrame
	uint32_t eyeCount;    // 0 = "submitted, but nothing to show"
	uint32_t reserved;
} VrcmFrame;

typedef struct VrcmEyeImage {
	VrcmPose pose; // pose the guest rendered with (STAGE)
	VrcmFov fov;
	uint32_t width, height, stride;
	uint32_t format; // VrcmPixelFormat
} VrcmEyeImage;

typedef struct VrcmHaptic {
	uint32_t hand;      // 0 left, 1 right
	float amplitude;    // 0 stops
	int64_t durationNs; // -1 = minimum
	float frequency;    // 0 = runtime default
} VrcmHaptic;

typedef struct VrcmSessionState {
	uint32_t running;
} VrcmSessionState;

#pragma pack(pop)
