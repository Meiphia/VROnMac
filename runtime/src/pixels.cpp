#include "pixels.h"

#include <cmath>
#include <vector>

namespace vrcm {

bool IsReadbackFormat(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return true;
	default:
		return false;
	}
}

bool IsDepthFormat(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D16_UNORM ||
	    f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

static uint8_t LinearToSrgb8(float v)
{
	if (!(v > 0.0f)) return 0; // also catches NaN
	if (v >= 1.0f) return 255;
	float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
	return (uint8_t)std::lround(s * 255.0f);
}

static float HalfToFloat(uint16_t h)
{
	uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
	float v;
	if (exp == 0) v = std::ldexp((float)mant, -24);
	else if (exp == 31) v = mant ? NAN : INFINITY;
	else v = std::ldexp((float)(mant | 0x400), (int)exp - 25);
	return sign ? -v : v;
}

static const uint8_t* Lut8() // linear 8-bit -> sRGB 8-bit
{
	static std::vector<uint8_t> t = [] {
		std::vector<uint8_t> v(256);
		for (int i = 0; i < 256; i++) v[i] = LinearToSrgb8(i / 255.0f);
		return v;
	}();
	return t.data();
}

static const uint8_t* Lut10() // linear 10-bit -> sRGB 8-bit
{
	static std::vector<uint8_t> t = [] {
		std::vector<uint8_t> v(1024);
		for (int i = 0; i < 1024; i++) v[i] = LinearToSrgb8(i / 1023.0f);
		return v;
	}();
	return t.data();
}

static const uint8_t* LutHalfColor() // half bits -> sRGB 8-bit
{
	static std::vector<uint8_t> t = [] {
		std::vector<uint8_t> v(65536);
		for (int i = 0; i < 65536; i++) v[i] = LinearToSrgb8(HalfToFloat((uint16_t)i));
		return v;
	}();
	return t.data();
}

static const uint8_t* LutHalfAlpha() // half bits -> linear 8-bit
{
	static std::vector<uint8_t> t = [] {
		std::vector<uint8_t> v(65536);
		for (int i = 0; i < 65536; i++) {
			float f = HalfToFloat((uint16_t)i);
			v[i] = !(f > 0) ? 0 : f >= 1 ? 255 : (uint8_t)std::lround(f * 255.0f);
		}
		return v;
	}();
	return t.data();
}

void ConvertToRGBA8sRGB(DXGI_FORMAT f, const uint8_t* src, uint32_t srcPitch, uint32_t srcX, uint32_t srcY,
    uint32_t w, uint32_t h, uint8_t* dst)
{
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t* row = src + (size_t)(srcY + y) * srcPitch;
		uint8_t* out = dst + (size_t)y * w * 4;
		switch (f) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			memcpy(out, row + srcX * 4, (size_t)w * 4);
			break;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
			const uint8_t* p = row + srcX * 4;
			for (uint32_t x = 0; x < w; x++, p += 4, out += 4) {
				out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
			}
			break;
		}
		case DXGI_FORMAT_R8G8B8A8_UNORM: {
			const uint8_t* l = Lut8();
			const uint8_t* p = row + srcX * 4;
			for (uint32_t x = 0; x < w; x++, p += 4, out += 4) {
				out[0] = l[p[0]]; out[1] = l[p[1]]; out[2] = l[p[2]]; out[3] = p[3];
			}
			break;
		}
		case DXGI_FORMAT_B8G8R8A8_UNORM: {
			const uint8_t* l = Lut8();
			const uint8_t* p = row + srcX * 4;
			for (uint32_t x = 0; x < w; x++, p += 4, out += 4) {
				out[0] = l[p[2]]; out[1] = l[p[1]]; out[2] = l[p[0]]; out[3] = p[3];
			}
			break;
		}
		case DXGI_FORMAT_R16G16B16A16_FLOAT: {
			const uint8_t* lc = LutHalfColor();
			const uint8_t* la = LutHalfAlpha();
			const uint16_t* p = (const uint16_t*)(row + srcX * 8);
			for (uint32_t x = 0; x < w; x++, p += 4, out += 4) {
				out[0] = lc[p[0]]; out[1] = lc[p[1]]; out[2] = lc[p[2]]; out[3] = la[p[3]];
			}
			break;
		}
		case DXGI_FORMAT_R10G10B10A2_UNORM: {
			const uint8_t* l = Lut10();
			const uint32_t* p = (const uint32_t*)(row + srcX * 4);
			for (uint32_t x = 0; x < w; x++, p++, out += 4) {
				uint32_t v = *p;
				out[0] = l[v & 0x3ff]; out[1] = l[(v >> 10) & 0x3ff]; out[2] = l[(v >> 20) & 0x3ff];
				out[3] = (uint8_t)((v >> 30) * 85);
			}
			break;
		}
		default:
			memset(out, 0, (size_t)w * 4);
			break;
		}
	}
}

} // namespace vrcm
