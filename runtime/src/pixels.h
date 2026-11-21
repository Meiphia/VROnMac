// Converts mapped swapchain pixels into the wire format (RGBA8, sRGB-encoded).
#pragma once
#include "common.h"

namespace vrcm {

// True if we can read back + convert this swapchain format.
bool IsReadbackFormat(DXGI_FORMAT f);
bool IsDepthFormat(DXGI_FORMAT f);

// Converts `w` x `h` pixels at (srcX, srcY) of `src` (row pitch `srcPitch`) into tightly
// packed RGBA8 sRGB at `dst`.
void ConvertToRGBA8sRGB(DXGI_FORMAT f, const uint8_t* src, uint32_t srcPitch, uint32_t srcX, uint32_t srcY,
    uint32_t w, uint32_t h, uint8_t* dst);

} // namespace vrcm
