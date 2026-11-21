// spi_probe: can this D3D11 implementation do what Unity's single-pass instanced VR rendering needs?
//   1. reports D3D11_OPTIONS3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer
//   2. draws an instanced fullscreen triangle into a 2-slice texture array, picking the slice with
//      SV_RenderTargetArrayIndex from the vertex shader (instance 0 -> red, instance 1 -> green)
//   3. draws plainly into slice 1 via a single-slice RTV (control: no array index in the shader)
// and reads every slice back.
//
// Shaders are precompiled DXBC (spi_shaders.h, made with Microsoft's d3dcompiler_47 from the HLSL
// below), so no shader compiler is needed at runtime.
// Build: x86_64-w64-mingw32-g++ -std=c++17 -O1 spi_probe.cpp -o spi_probe.exe -ld3d11 -static
// Run:   wine spi_probe.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>

static const char* kVS = R"(
struct O { float4 pos : SV_Position; float4 col : COLOR; uint rt : SV_RenderTargetArrayIndex; };
O main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
	O o;
	float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.col = iid == 0 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1);
	o.rt = iid;
	return o;
})";
static const char* kVSPlain = R"(
struct O { float4 pos : SV_Position; float4 col : COLOR; };
O main(uint vid : SV_VertexID) {
	O o;
	float2 uv = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.col = float4(0, 0, 1, 1);
	return o;
})";
static const char* kPS = R"(
float4 main(float4 pos : SV_Position, float4 col : COLOR) : SV_Target { return col; })";

#include "spi_shaders.h"

static void ReadSlices(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const char* label)
{
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);
	D3D11_TEXTURE2D_DESC sd = d;
	sd.ArraySize = 1;
	sd.BindFlags = 0;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	for (UINT s = 0; s < d.ArraySize; s++) {
		ID3D11Texture2D* st = nullptr;
		dev->CreateTexture2D(&sd, nullptr, &st);
		ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, D3D11CalcSubresource(0, s, 1), nullptr);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
			const unsigned char* p = (const unsigned char*)m.pData + (d.Height / 2) * m.RowPitch + (d.Width / 2) * 4;
			printf("  %s slice %u centre = (%u, %u, %u, %u)\n", label, s, p[0], p[1], p[2], p[3]);
			ctx->Unmap(st, 0);
		}
		st->Release();
	}
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, got;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want + 1, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
		if (FAILED(hr)) {
			printf("D3D11CreateDevice failed 0x%08lx\n", hr);
			return 1;
		}
	}
	printf("device feature level 0x%x\n", got);

	D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
	hr = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
	printf("[1] VPAndRTArrayIndexFromAnyShaderFeedingRasterizer = %s\n",
	    FAILED(hr) ? "query failed" : (o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer ? "YES" : "NO"));

	ID3D11VertexShader *vs = nullptr, *vsPlain = nullptr;
	ID3D11PixelShader* ps = nullptr;
	HRESULT h1 = dev->CreateVertexShader(kVSBytes, sizeof(kVSBytes), nullptr, &vs);
	HRESULT h2 = dev->CreateVertexShader(kVSPlainBytes, sizeof(kVSPlainBytes), nullptr, &vsPlain);
	dev->CreatePixelShader(kPSBytes, sizeof(kPSBytes), nullptr, &ps);
	printf("    CreateVertexShader(with SV_RenderTargetArrayIndex) = 0x%08lx, plain = 0x%08lx\n", h1, h2);

	const UINT W = 256, H = 256;
	D3D11_TEXTURE2D_DESC td{};
	td.Width = W;
	td.Height = H;
	td.MipLevels = 1;
	td.ArraySize = 2;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ID3D11Texture2D* tex = nullptr;
	dev->CreateTexture2D(&td, nullptr, &tex);

	D3D11_RENDER_TARGET_VIEW_DESC rd{};
	rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
	rd.Texture2DArray.ArraySize = 2;
	ID3D11RenderTargetView* rtvAll = nullptr;
	dev->CreateRenderTargetView(tex, &rd, &rtvAll);
	rd.Texture2DArray.FirstArraySlice = 1;
	rd.Texture2DArray.ArraySize = 1;
	ID3D11RenderTargetView* rtv1 = nullptr;
	dev->CreateRenderTargetView(tex, &rd, &rtv1);

	D3D11_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
	ctx->RSSetViewports(1, &vp);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->PSSetShader(ps, nullptr, 0);
	float black[4] = { 0, 0, 0, 0 };

	// [2] single-pass instanced
	ctx->ClearRenderTargetView(rtvAll, black);
	ctx->OMSetRenderTargets(1, &rtvAll, nullptr);
	if (vs) {
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->DrawInstanced(3, 2, 0, 0);
	}
	ctx->OMSetRenderTargets(0, nullptr, nullptr);
	printf("[2] instanced draw, slice chosen by SV_RenderTargetArrayIndex (expect slice0 red, slice1 green):\n");
	ReadSlices(dev, ctx, tex, "SPI");

	// [3] control: plain draw into slice 1 through a single-slice RTV
	ctx->ClearRenderTargetView(rtvAll, black);
	ctx->OMSetRenderTargets(1, &rtv1, nullptr);
	ctx->VSSetShader(vsPlain, nullptr, 0);
	ctx->Draw(3, 0);
	ctx->OMSetRenderTargets(0, nullptr, nullptr);
	printf("[3] plain draw into slice 1 via single-slice RTV (expect slice0 black, slice1 blue):\n");
	ReadSlices(dev, ctx, tex, "plain");
	printf("\nVerdict: if [2] isn't red/green but [3] is blue, this D3D11 layer can't do Unity's\n"
	       "single-pass instanced rendering (SV_RenderTargetArrayIndex from the vertex shader).\n");
	return 0;
}
