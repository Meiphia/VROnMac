// Minimal OpenVR (SteamVR API) app, D3D11, Unity-style typeless sRGB eye textures. Used to test the
// full chain:  OpenVR app -> OpenComposite openvr_api.dll -> vrconmac_xr.dll -> bridge.
//
// Build: x86_64-w64-mingw32-g++ -std=c++17 -O1 -I<OpenComposite>/OpenVRHeaders vrapp.cpp -o vrapp.exe -ld3d11 -static
// Run:   XR_RUNTIME_JSON='C:\path\vrconmac_xr.json' wine vrapp.exe 'Z:\path\openvr_api.dll' 300

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include "openvr-1.16.8.h"

#include <cstdio>
#include <cstdlib>

using namespace vr;

typedef uint32_t (*PFN_VR_InitInternal2)(EVRInitError*, EVRApplicationType, const char*);
typedef void* (*PFN_VR_GetGenericInterface)(const char*, EVRInitError*);
typedef void (*PFN_VR_ShutdownInternal)();

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	const char* dll = argc > 1 ? argv[1] : "openvr_api.dll";
	int frames = argc > 2 ? atoi(argv[2]) : 300;

	HMODULE m = LoadLibraryA(dll);
	if (!m) {
		printf("FAIL LoadLibrary %s (%lu)\n", dll, GetLastError());
		return 1;
	}
	auto init = (PFN_VR_InitInternal2)GetProcAddress(m, "VR_InitInternal2");
	auto getIf = (PFN_VR_GetGenericInterface)GetProcAddress(m, "VR_GetGenericInterface");
	auto shut = (PFN_VR_ShutdownInternal)GetProcAddress(m, "VR_ShutdownInternal");
	if (!init || !getIf || !shut) {
		printf("FAIL missing OpenVR exports\n");
		return 1;
	}
	EVRInitError err = VRInitError_None;
	init(&err, VRApplication_Scene, nullptr);
	if (err != VRInitError_None) {
		printf("FAIL VR_Init error %d\n", (int)err);
		return 1;
	}
	auto* sys = (IVRSystem*)getIf(IVRSystem_Version, &err);
	auto* comp = (IVRCompositor*)getIf(IVRCompositor_Version, &err);
	if (!sys || !comp) {
		printf("FAIL interfaces (%d)\n", (int)err);
		return 1;
	}
	uint32_t w = 0, h = 0;
	sys->GetRecommendedRenderTargetSize(&w, &h);
	printf("ok   VR_Init, recommended %ux%u\n", w, h);

	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
		printf("FAIL D3D11CreateDevice\n");
		return 1;
	}
	ID3D11Texture2D* tex[2];
	ID3D11RenderTargetView* rtv[2];
	for (int i = 0; i < 2; i++) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; // what Unity hands to OpenVR
		td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		dev->CreateTexture2D(&td, nullptr, &tex[i]);
		D3D11_RENDER_TARGET_VIEW_DESC rd{};
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		dev->CreateRenderTargetView(tex[i], &rd, &rtv[i]);
	}

	TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
	int tracked = 0, ctrl = 0, submitOk = 0;
	for (int f = 0; f < frames; f++) {
		VREvent_t ev;
		while (sys->PollNextEvent(&ev, sizeof(ev))) {
		}
		comp->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
		if (poses[k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
			tracked++;
		TrackedDeviceIndex_t r = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_RightHand);
		VRControllerState_t cs{};
		if (r != k_unTrackedDeviceIndexInvalid && sys->GetControllerState(r, &cs, sizeof(cs)))
			ctrl++;
		for (int i = 0; i < 2; i++) {
			float col[4] = { i == 0 ? 0.f : 1.f, 0.f, i == 0 ? 1.f : 0.f, 1.f }; // left blue, right red
			ctx->ClearRenderTargetView(rtv[i], col);
			Texture_t t{ tex[i], TextureType_DirectX, ColorSpace_Auto };
			EVRCompositorError ce = comp->Submit(i == 0 ? Eye_Left : Eye_Right, &t, nullptr, Submit_Default);
			if (ce == VRCompositorError_None)
				submitOk++;
		}
		comp->PostPresentHandoff();
	}
	printf("ok   %d frames: HMD tracked %d, right controller %d, submits ok %d\n", frames, tracked, ctrl, submitOk);
	shut();
	printf("%s\n", (tracked > frames / 2 && submitOk > frames) ? "PASS" : "FAIL");
	return 0;
}
