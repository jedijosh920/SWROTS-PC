// Host copies of Xbox resources.
//
// Xbox resources live in game memory (headers + contiguous data). Host D3D9
// resources are created from them on demand and keyed by the data address, so
// a texture and the surfaces of its levels share one host object. Render
// targets are host-owned: once the game renders into a surface, sampling it
// later uses the host render-target texture instead of the (stale) guest bytes.

#include "d3d/d3d.h"

#include <unordered_map>

#include "core/log.h"

namespace swrots::d3d {

using namespace xd3d;

IDirect3DSurface9* HostBackBuffer();
IDirect3DTexture9* HostBackBufferTexture();
IDirect3DSurface9* HostDepthBuffer();
void LogFrameStats(DWORD frame);

struct HostRenderTarget {
    IDirect3DTexture9* texture = nullptr; // color targets are textures so they can be sampled
    IDirect3DSurface9* surface = nullptr;
    UINT width = 0, height = 0;
    UINT hostWidth = 0;       // wider than width x RenderScale() for true widescreen (d3d.h)
    DWORD lastUsed = 0;
};

static std::unordered_map<DWORD, HostRenderTarget> g_RenderTargets; // key: Data | depth bit
static DWORD g_Frame = 0;

void XboxSurfaceSize(const PixelContainer* s, UINT& w, UINT& h)
{
    if (s->Size) {
        w = (s->Size & SIZE_WIDTH_MASK) + 1;
        h = ((s->Size & SIZE_HEIGHT_MASK) >> SIZE_HEIGHT_SHIFT) + 1;
    } else {
        w = 1u << ((s->Format >> FORMAT_USIZE_SHIFT) & 0xF);
        h = 1u << ((s->Format >> FORMAT_VSIZE_SHIFT) & 0xF);
    }
}

IDirect3DSurface9* HostSurfaceFor(Surface* surface, bool depth)
{
    if (surface == State().backBuffer)
        return HostBackBuffer();
    if (surface == State().depthBuffer)
        return HostDepthBuffer();

    DWORD key = surface->Data | (depth ? 0x80000000 : 0);
    UINT w, h;
    XboxSurfaceSize(surface, w, h);
    HostRenderTarget& rt = g_RenderTargets[key];
    if (rt.surface && (rt.width != w || rt.height != h)) {
        if (rt.texture) rt.texture->Release();
        rt.surface->Release();
        rt = {};
    }
    if (!rt.surface) {
        HRESULT hr;
        const UINT hw = HostWidthFor(w, h), hh = h * RenderScale();
        if (depth) {
            hr = Device()->CreateDepthStencilSurface(hw, hh, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt.surface, nullptr);
        } else {
            hr = Device()->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt.texture, nullptr);
            if (SUCCEEDED(hr))
                rt.texture->GetSurfaceLevel(0, &rt.surface);
        }
        if (FAILED(hr))
            Fatal("Could not create a %ux%u %s target (%08lX)", w, h, depth ? "depth" : "render", hr);
        rt.width = w;
        rt.height = h;
        rt.hostWidth = hw;
        LOG_DEBUG("Render target %08lX: %ux%u %s, host %ux%u", surface->Data, w, h, depth ? "depth" : "color", hw, hh);
    }
    rt.lastUsed = g_Frame;
    return rt.surface;
}

bool IsRenderTargetData(DWORD data) { return g_RenderTargets.count(data) != 0; }

bool IsWideData(DWORD data)
{
    if (State().backBuffer && data == State().backBuffer->Data)
        return MapFor(State().backBuffer).wide;
    auto it = g_RenderTargets.find(data);
    return it != g_RenderTargets.end() && it->second.hostWidth > it->second.width * RenderScale();
}

IDirect3DBaseTexture9* HostRenderTargetTexture(DWORD data)
{
    // The Xbox back buffer is sampled by post effects (e.g. the glow pass).
    if (State().backBuffer && data == State().backBuffer->Data)
        return HostBackBufferTexture();
    auto it = g_RenderTargets.find(data);
    if (it == g_RenderTargets.end() || !it->second.texture)
        return nullptr;
    it->second.lastUsed = g_Frame;
    return it->second.texture;
}

IDirect3DBaseTexture9* UploadTexture(BaseTexture* texture, Palette* palette);
void CollectTextures(DWORD frame);
void ReleaseTextures();

IDirect3DBaseTexture9* HostTextureFor(BaseTexture* texture, Palette* palette)
{
    return texture ? UploadTexture(texture, palette) : nullptr;
}

void ReleaseDrawResources();
void ReleaseShaders();
void InvalidateHostStateCache();

void ReleaseHostResources()
{
    ReleaseDrawResources();
    ReleaseShaders();
    InvalidateHostStateCache();
    for (auto& [key, rt] : g_RenderTargets) {
        if (rt.surface) rt.surface->Release();
        if (rt.texture) rt.texture->Release();
    }
    g_RenderTargets.clear();
    ReleaseTextures();
}

void CollectResources(DWORD frame)
{
    g_Frame = frame;
    LogFrameStats(frame);
    CollectTextures(frame);
}

} // namespace swrots::d3d
