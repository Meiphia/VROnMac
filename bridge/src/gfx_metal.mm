// Metal glue for the bridge (XR_KHR_metal_enable). Uploads guest RGBA8 frames into the runtime's
// swapchain textures via a shared-storage MTLBuffer + blit.

#import <Metal/Metal.h>

#define XR_USE_GRAPHICS_API_METAL
#include "xr_gfx.h"

#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

class MetalGfx : public XrGfx {
public:
	const char* ExtensionName() const override { return XR_KHR_METAL_ENABLE_EXTENSION_NAME; }

	bool Init(XrApi& xr, XrInstance inst, XrSystemId sys, std::string& err) override
	{
		auto getReq = (PFN_xrGetMetalGraphicsRequirementsKHR)xr.Get(inst, "xrGetMetalGraphicsRequirementsKHR");
		if (!getReq) {
			err = "runtime lacks xrGetMetalGraphicsRequirementsKHR";
			return false;
		}
		XrGraphicsRequirementsMetalKHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
		XrResult r = getReq(inst, sys, &req);
		if (XR_FAILED(r)) {
			err = "xrGetMetalGraphicsRequirementsKHR failed: " + xr.ResultString(inst, r);
			return false;
		}
		device_ = req.metalDevice ? (__bridge id<MTLDevice>)req.metalDevice : MTLCreateSystemDefaultDevice();
		if (!device_) {
			err = "no Metal device";
			return false;
		}
		queue_ = [device_ newCommandQueue];
		binding_ = { XR_TYPE_GRAPHICS_BINDING_METAL_KHR };
		binding_.commandQueue = (__bridge void*)queue_;
		printf("[metal] device: %s\n", [[device_ name] UTF8String]);
		return true;
	}

	const void* SessionBinding() const override { return &binding_; }

	int64_t ChooseFormat(const std::vector<int64_t>& formats, bool& swapRB) const override
	{
		// Guest pixels are sRGB-encoded RGBA8: store them in an *_sRGB texture as-is.
		for (int64_t f : formats)
			if (f == MTLPixelFormatRGBA8Unorm_sRGB) {
				swapRB = false;
				return f;
			}
		for (int64_t f : formats)
			if (f == MTLPixelFormatBGRA8Unorm_sRGB) {
				swapRB = true;
				return f;
			}
		return 0;
	}

	bool BindImages(XrApi& xr, XrSwapchain sc, int eye, std::string& err) override
	{
		uint32_t n = 0;
		xr.xrEnumerateSwapchainImages(sc, 0, &n, nullptr);
		std::vector<XrSwapchainImageMetalKHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR });
		XrResult r = xr.xrEnumerateSwapchainImages(sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
		if (XR_FAILED(r)) {
			err = "xrEnumerateSwapchainImages failed";
			return false;
		}
		textures_[eye] = [NSMutableArray arrayWithCapacity:n];
		for (auto& im : imgs)
			[textures_[eye] addObject:(__bridge id<MTLTexture>)im.texture];
		return true;
	}

	bool Upload(int eye, uint32_t index, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t stride, bool swapRB) override
	{
		if (!textures_[eye] || index >= textures_[eye].count)
			return false;
		id<MTLTexture> tex = textures_[eye][index];
		size_t rowBytes = (size_t)w * 4;
		size_t bytes = rowBytes * h;
		if (!staging_[eye] || staging_[eye].length < bytes)
			staging_[eye] = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];

		uint8_t* dst = (uint8_t*)staging_[eye].contents;
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t* s = rgba + (size_t)y * stride;
			uint8_t* d = dst + (size_t)y * rowBytes;
			if (!swapRB) {
				memcpy(d, s, rowBytes);
			} else {
				for (uint32_t x = 0; x < w; x++) {
					d[x * 4 + 0] = s[x * 4 + 2];
					d[x * 4 + 1] = s[x * 4 + 1];
					d[x * 4 + 2] = s[x * 4 + 0];
					d[x * 4 + 3] = s[x * 4 + 3];
				}
			}
		}

		uint32_t cw = std::min<uint32_t>(w, (uint32_t)tex.width), ch = std::min<uint32_t>(h, (uint32_t)tex.height);
		id<MTLCommandBuffer> cb = [queue_ commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromBuffer:staging_[eye]
		           sourceOffset:0
		      sourceBytesPerRow:rowBytes
		    sourceBytesPerImage:bytes
		             sourceSize:MTLSizeMake(cw, ch, 1)
		              toTexture:tex
		       destinationSlice:0
		       destinationLevel:0
		      destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		[cb commit];
		// Simple and safe for a proof of concept: the staging buffer is reused next frame.
		[cb waitUntilCompleted];
		return cb.status == MTLCommandBufferStatusCompleted;
	}

	void ReleaseImages(int eye) override { textures_[eye] = nil; }

private:
	id<MTLDevice> device_ = nil;
	id<MTLCommandQueue> queue_ = nil;
	XrGraphicsBindingMetalKHR binding_{ XR_TYPE_GRAPHICS_BINDING_METAL_KHR };
	NSMutableArray<id<MTLTexture>>* textures_[2] = { nil, nil };
	id<MTLBuffer> staging_[2] = { nil, nil };
};

} // namespace

std::unique_ptr<XrGfx> CreateMetalGfx() { return std::make_unique<MetalGfx>(); }
