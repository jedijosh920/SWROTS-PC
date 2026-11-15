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

// A picture held on screen across the game's restart (online play: the menus' last picture, with the game's popup,
// while both games restart for the match): 0 nothing, 1 the last picture kept (taken as the device goes), 2 shown
// in place of the game's frames once the device came back (the HUD sets it every frame).
void SetFrameHold(int mode);
// A held picture is shown now.
bool FrameHeld();
// The host device's count of creations (a restart makes a new one).
uint32_t DeviceGeneration();

// Internal resolution scale: host render targets are this many times the
// size of their Xbox surfaces. Xbox pixel coordinates (viewports, scissors,
// pre-transformed vertices) are multiplied by it. Fixed while a device exists.
UINT RenderScale();
// Width and height of an Xbox surface or texture level 0, in Xbox pixels.
void XboxSurfaceSize(const xd3d::PixelContainer* surface, UINT& width, UINT& height);

// Host render target / depth surface backing an Xbox surface (RenderScale() x its size).
IDirect3DSurface9* HostSurfaceFor(xd3d::Surface* surface, bool depth);
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
