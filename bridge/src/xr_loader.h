// Tiny OpenXR "loader": reads a runtime manifest, dlopens the runtime and negotiates with it
// directly. Avoids depending on the Khronos loader being installed on macOS.
#pragma once

#include <openxr/openxr.h>
#include <string>

#define VRCM_XR_FUNCTIONS(X)                \
	X(xrGetInstanceProcAddr)                \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance)                     \
	X(xrDestroyInstance)                    \
	X(xrGetInstanceProperties)              \
	X(xrPollEvent)                          \
	X(xrResultToString)                     \
	X(xrGetSystem)                          \
	X(xrGetSystemProperties)                \
	X(xrEnumerateViewConfigurationViews)    \
	X(xrEnumerateEnvironmentBlendModes)     \
	X(xrCreateSession)                      \
	X(xrDestroySession)                     \
	X(xrBeginSession)                       \
	X(xrEndSession)                         \
	X(xrRequestExitSession)                 \
	X(xrEnumerateReferenceSpaces)           \
	X(xrCreateReferenceSpace)               \
	X(xrGetReferenceSpaceBoundsRect)        \
	X(xrCreateActionSpace)                  \
	X(xrLocateSpace)                        \
	X(xrDestroySpace)                       \
	X(xrEnumerateSwapchainFormats)          \
	X(xrCreateSwapchain)                    \
	X(xrDestroySwapchain)                   \
	X(xrEnumerateSwapchainImages)           \
	X(xrAcquireSwapchainImage)              \
	X(xrWaitSwapchainImage)                 \
	X(xrReleaseSwapchainImage)              \
	X(xrWaitFrame)                          \
	X(xrBeginFrame)                         \
	X(xrEndFrame)                           \
	X(xrLocateViews)                        \
	X(xrStringToPath)                       \
	X(xrCreateActionSet)                    \
	X(xrCreateAction)                       \
	X(xrSuggestInteractionProfileBindings)  \
	X(xrAttachSessionActionSets)            \
	X(xrSyncActions)                        \
	X(xrGetActionStateBoolean)              \
	X(xrGetActionStateFloat)                \
	X(xrGetActionStateVector2f)             \
	X(xrGetActionStatePose)                 \
	X(xrApplyHapticFeedback)                \
	X(xrStopHapticFeedback)

struct XrApi {
#define VRCM_DECL(name) PFN_##name name = nullptr;
	VRCM_XR_FUNCTIONS(VRCM_DECL)
#undef VRCM_DECL

	void* library = nullptr;
	std::string runtimeName;

	// Loads + negotiates; fills the global (null-instance) functions.
	bool Load(const std::string& manifestPath, std::string& error);
	// After xrCreateInstance: resolve everything else.
	bool LoadInstanceFunctions(XrInstance instance, std::string& error);
	// Resolve an extension function by name.
	PFN_xrVoidFunction Get(XrInstance instance, const char* name);

	std::string ResultString(XrInstance inst, XrResult r);
};

// $XR_RUNTIME_JSON, then ~/.config/openxr/1/active_runtime.json (where OXRSys registers itself)
std::string DefaultRuntimeManifest();
