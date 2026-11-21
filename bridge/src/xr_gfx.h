// Graphics glue between the bridge's OpenXR session and the host GPU API.
#pragma once
#include "xr_loader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class XrGfx {
public:
	virtual ~XrGfx() = default;
	virtual const char* ExtensionName() const = 0;
	// Called after xrGetSystem; must call the runtime's graphics-requirements function.
	virtual bool Init(XrApi& xr, XrInstance inst, XrSystemId sys, std::string& err) = 0;
	// Graphics binding struct to chain into XrSessionCreateInfo::next.
	virtual const void* SessionBinding() const = 0;
	// Pick a swapchain format; swapRB=true means the bytes we upload must be BGRA.
	virtual int64_t ChooseFormat(const std::vector<int64_t>& formats, bool& swapRB) const = 0;
	// Enumerate a freshly created swapchain's images for `eye`.
	virtual bool BindImages(XrApi& xr, XrSwapchain sc, int eye, std::string& err) = 0;
	// Copy RGBA8 pixels into swapchain image `index` of `eye`. Must be complete (or ordered on the
	// runtime's queue) before xrReleaseSwapchainImage.
	virtual bool Upload(int eye, uint32_t index, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t stride, bool swapRB) = 0;
	virtual void ReleaseImages(int eye) = 0;
};

std::unique_ptr<XrGfx> CreateMetalGfx(); // gfx_metal.mm (Apple only)
