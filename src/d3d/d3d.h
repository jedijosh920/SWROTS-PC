#pragma once
// Native Direct3D layer: implements the game's Xbox Direct3D 8 API on Direct3D 9.
//
// Boundary with the game's statically linked XDK D3D library:
//  - Pure CPU parts of the XDK (resource creation, locks, headers, surface
//    descriptions, shader object creation, mode enumeration) run as-is.
//  - GPU synchronisation (fences, push-buffer space) is stubbed; XDK code that
//    still writes push-buffer commands writes into a scratch buffer.
//  - Everything that defines what gets drawn (state, draws, render targets,
//    presentation) is implemented here on D3D9, reading the Xbox state the
//    engine keeps in D3D's own global arrays.

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

#include "d3d/xd3d.h"

namespace swrots::d3d {

// Addresses of XDK D3D globals in the retail XBE.
namespace xbox_globals {
inline constexpr uint32_t kDevicePtr = 0x0050FE68;          // D3D__pDevice
inline constexpr uint32_t kDeviceObject = 0x0050FE70;       // the static CDevice instance
inline constexpr uint32_t kDeviceObjectSize = 0x24A0;
inline constexpr uint32_t kVertexConstants = 0x0050EDB0;    // shadow of the 192 vertex shader constants
inline constexpr uint32_t kRenderState = 0x0050FBD0;        // D3D__RenderState[RS_COUNT]
inline constexpr uint32_t kTextureState = 0x0050F9D0;       // D3D__TextureState[4][32] (deferred part)
inline constexpr uint32_t kDirtyFlags = 0x0050F9C8;         // D3D__DirtyFlags
inline constexpr uint32_t kIndexData = 0x0050F9C4;          // D3D__IndexData (current index buffer data)
} // namespace xbox_globals

inline DWORD* XboxRenderStates() { return reinterpret_cast<DWORD*>(uintptr_t(xbox_globals::kRenderState)); }
inline DWORD* XboxTextureStates(DWORD stage)
{
    return reinterpret_cast<DWORD*>(uintptr_t(xbox_globals::kTextureState)) + stage * xd3d::TSS_COUNT;
}

// CPU address of a resource's data (resources hold physical addresses).
inline uint8_t* ResourceData(const xd3d::Resource* r)
{
    return reinterpret_cast<uint8_t*>(uintptr_t(r->Data | 0x80000000));
}

struct Viewport {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};

// Shadow of the Xbox device state set through replaced API functions.
struct XboxState {
    xd3d::PresentParameters pp = {};
    xd3d::Surface* backBuffer = nullptr;
    xd3d::Surface* depthBuffer = nullptr;
    xd3d::Surface* renderTarget = nullptr;
    xd3d::Surface* depthStencil = nullptr;
    Viewport viewport = {};
    xd3d::BaseTexture* textures[4] = {};
    xd3d::Palette* palettes[4] = {};
    DWORD textureStates[4][xd3d::TSS_COUNT] = {}; // non-deferred texture states (22+)
    struct Stream {
        xd3d::VertexBuffer* buffer;
        UINT stride;
    } streams[16] = {};
    xd3d::IndexBuffer* indexBuffer = nullptr;
    UINT baseVertexIndex = 0;
    DWORD vertexShader = 0; // FVF or (X_D3DVertexShader* | 1)
    xd3d::PixelShader* pixelShader = nullptr;
    D3DMATRIX transforms[xd3d::TS_COUNT] = {};
    float vertexConstants[192][4] = {};
    DWORD shaderConstantMode = 0;
    float screenSpaceOffset[2] = {};
    DWORD swapCount = 0;
};

XboxState& State();
IDirect3DDevice9* Device();

// Internal resolution scale: host render targets are this many times the
// size of their Xbox surfaces. Xbox pixel coordinates (viewports, scissors,
// pre-transformed vertices) are multiplied by it. Fixed while a device exists.
UINT RenderScale();
// Width and height of an Xbox surface or texture level 0, in Xbox pixels.
void XboxSurfaceSize(const xd3d::PixelContainer* surface, UINT& width, UINT& height);

// Host render target / depth surface backing an Xbox surface (RenderScale() x its size, wider for
// true widescreen: see SurfaceMap).
IDirect3DSurface9* HostSurfaceFor(xd3d::Surface* surface, bool depth);

// --- True widescreen (Widescreen=2) ---------------------------------------------------------------
// The game draws its usual 640x480 4:3 frame. Render targets shaped like the back buffer get host
// surfaces as wide as the picture: the game's frame sits in the middle at RenderScale(), undistorted,
// and the 3D picture continues at the sides (Hor+). Whole-width screen effects (bloom, fades, letterbox
// bars) are spread over the whole width; pictures and menus stay in the middle; HUD pieces near the
// sides follow the screen's edges (Settings::hudAnchor).
struct SurfaceMap {
    UINT xboxWidth = 0, xboxHeight = 0; // the surface's Xbox size
    UINT hostWidth = 0, hostHeight = 0; // its host surface's size
    bool wide = false;                  // hostWidth is more than xboxWidth x RenderScale()
    float centre = 0;                   // host x of the Xbox frame's left edge, centred
    float left = 0, right = 0;          // the same for HUD pieces kept at the left or the right edge
};
// Cover: a picture filling the whole width, its height grown as much and cut at the top and bottom (the front
// end's background movies, like CSS object-fit: cover).
enum class Placement { Centre, Stretch, Left, Right, Cover };
// How an Xbox surface's pixels map to its host surface.
SurfaceMap MapFor(const xd3d::PixelContainer* surface);
// Host width of a render target of this Xbox size.
UINT HostWidthFor(UINT xboxWidth, UINT xboxHeight);
// Host x of an Xbox pixel coordinate (pre-transformed vertex) on a surface, placed as given.
float MapScreenX(const SurfaceMap& map, float x, Placement placement);
// Host y of an Xbox pixel coordinate on a surface: RenderScale() times, or grown about the middle for Cover.
float MapScreenY(const SurfaceMap& map, float y, Placement placement);
// The source rectangle (Xbox pixels) of a whole-frame picture shown covering a wide surface: the middle band
// of its height that fills the width at the same scale (the whole picture when not wide).
RECT CoverSource(const SurfaceMap& map);
// Host x range of an Xbox pixel range (viewport, scissor, rectangle) on a surface: the whole Xbox
// width spans the whole host width, anything narrower stays in the centred frame.
void MapRangeX(const SurfaceMap& map, LONG x1, LONG x2, LONG& hostX1, LONG& hostX2);
// The back buffer's margin at each side, in Xbox pixels (0 without true widescreen): the game's view is
// widened by this much at each side (game/widescreen.cpp), its viewport reaching from -margin to 640 + margin.
int WideMargin();
// Whether a texture's data is one of the render targets the game drew into (resources.cpp).
bool IsRenderTargetData(DWORD data);
// Whether a texture's data is a render target with a wide host surface (or the back buffer).
bool IsWideData(DWORD data);
// Host texture for an Xbox texture (uploads and converts guest data as needed).
IDirect3DBaseTexture9* HostTextureFor(xd3d::BaseTexture* texture, xd3d::Palette* palette);
// Frees host resources not used for a while (called once per frame).
void CollectResources(DWORD frame);

// Applies the Xbox render/texture state to the host device before a draw.
void ApplyState();
// Z scale for the current depth buffer format (Xbox depth is not normalized).
float DepthScale();

// The game rebooted in-process: releases the host device and everything created for the
// old instance (its Xbox-side surfaces are in memory that is about to be freed).
void ResetForReboot();

} // namespace swrots::d3d
