// Device creation, presentation, render targets, viewport and clears.

#include "d3d/d3d.h"

#include <algorithm>
#include <cstring>

#include "core/crash.h"
#include "core/log.h"
#include "core/settings.h"
#include "debug/menu.h"
#include "game/devoptions.h"
#include "game/game.h"
#include "core/window.h"
#include "d3d/recorder.h"
#include "kernel/kernel.h"
#include "kernel/mm.h"
#include "xapi/xapi.h"

namespace swrots::d3d {

using namespace xd3d;

static void WriteViewportConstants();

void ReleaseHostResources();

static XboxState g_State;
static IDirect3D9Ex* g_D3D = nullptr;
static IDirect3DDevice9* g_Device = nullptr;
// The Xbox back buffer is rendered into g_HostBackBuffer, a texture at
// RenderScale() x the Xbox size; Swap scales it into the swap chain's back
// buffer (sized to the window) with the chosen aspect ratio.
static IDirect3DTexture9* g_HostBackTexture = nullptr;
static IDirect3DSurface9* g_HostBackBuffer = nullptr;
static IDirect3DSurface9* g_HostDepth = nullptr;
// Frames are shown through a swap chain sized to the window's client area,
// recreated when the window is resized (the device's own swap chain is unused).
static IDirect3DSwapChain9* g_WindowChain = nullptr;
static IDirect3DSurface9* g_SwapChainBuffer = nullptr;
static UINT g_ChainWidth = 0, g_ChainHeight = 0;
static UINT g_RenderScale = 1;
static IDirect3DSurface9* g_CpuFrame = nullptr; // staging for CPU-written frames

// Fake Xbox device object. XDK code that still runs natively only touches the
// push-buffer pointers at its start.
static uint8_t* g_XboxDevice = nullptr;
static uint8_t* g_PushBuffer = nullptr;
static const uint32_t kPushBufferSize = 0x40000;
// Offsets of X_D3DDevice fields used by natively running XDK code.
static const uint32_t kDeviceFrameBuffer = 0x1A14;
static const uint32_t kDeviceMultiSampleType = 0x196C;
static const uint32_t kDeviceRefCount = 0x938;

XboxState& State() { return g_State; }
UINT RenderScale() { return g_RenderScale; }
IDirect3DDevice9* Device() { return g_Device; }

static void ResetPushBuffer()
{
    reinterpret_cast<uint8_t**>(g_XboxDevice)[0] = g_PushBuffer;                              // m_pPush
    reinterpret_cast<uint8_t**>(g_XboxDevice)[1] = g_PushBuffer + kPushBufferSize - 0x10000; // m_pThreshold
}

// Allocates an Xbox surface header + contiguous memory, as the XDK would.
static Surface* CreateXboxSurface(UINT width, UINT height, DWORD format, UINT bytesPerPixel)
{
    auto* s = static_cast<Surface*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Surface)));
    UINT pitch = (width * bytesPerPixel + 63) & ~63u;
    void* mem = kernel::MmAllocateContiguousMemoryEx(pitch * height, 0, 0xFFFFFFFF, 0x1000, PAGE_READWRITE);
    s->Common = 1 | COMMON_TYPE_SURFACE | COMMON_D3DCREATED;
    s->Data = uint32_t(uintptr_t(mem)) & 0x0FFFFFFF;
    s->Format = (2 << FORMAT_DIMENSION_SHIFT) | (format << FORMAT_FORMAT_SHIFT) | (1 << FORMAT_MIPMAP_SHIFT);
    s->Size = (width - 1) | ((height - 1) << SIZE_HEIGHT_SHIFT) | ((pitch / 64 - 1) << SIZE_PITCH_SHIFT);
    return s;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
// Releases the host device and everything created on it.
static void ReleaseDevice()
{
    if (!g_Device)
        return;
    debug::MenuDeviceReleasing();
    ReleaseHostResources();
    FlightReleaseResources();
    if (g_CpuFrame) g_CpuFrame->Release();
    g_CpuFrame = nullptr;
    if (g_HostBackBuffer) g_HostBackBuffer->Release();
    if (g_HostBackTexture) g_HostBackTexture->Release();
    if (g_HostDepth) g_HostDepth->Release();
    if (g_SwapChainBuffer) g_SwapChainBuffer->Release();
    if (g_WindowChain) g_WindowChain->Release();
    g_HostBackBuffer = g_HostDepth = g_SwapChainBuffer = nullptr;
    g_WindowChain = nullptr;
    g_ChainWidth = g_ChainHeight = 0;
    g_HostBackTexture = nullptr;
    // Anything still holding the device keeps it, and its video memory, alive.
    if (ULONG remaining = g_Device->Release())
        LOG_WARN("Graphics device still referenced %lu time(s) after release: resources leaked", remaining);
    g_Device = nullptr;
}

void ResetForReboot()
{
    ReleaseDevice();
    g_State = XboxState();
    ResetPushBuffer();
    LOG_INFO("Reboot: graphics device released");
}

static HRESULT __stdcall XbCreateDevice(UINT adapter, DWORD deviceType, HWND focusWindow, DWORD behaviorFlags,
    PresentParameters* pp, void** device)
{
    (void)adapter;
    (void)deviceType;
    (void)focusWindow;
    (void)behaviorFlags;
    g_State.pp = *pp;
    LOG_INFO("CreateDevice: %ux%u fmt %02lX x%u, depth %d fmt %02lX, flags %lX, interval %u", pp->BackBufferWidth,
        pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->EnableAutoDepthStencil,
        pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_PresentationInterval);

    // The engine tears the device down and creates it again when it changes
    // video mode; start from a clean host device each time.
    ReleaseDevice();
    // Direct3D 9Ex: its devices are not lost when the window loses focus or the
    // display changes (plain D3D9 needs a full reset and resource rebuild).
    if (!g_D3D && FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_D3D)))
        g_D3D = nullptr;
    if (!g_D3D)
        Fatal("Direct3D 9 is not available on this system.");

    const Settings& settings = GetSettings();
    g_RenderScale = UINT(settings.resolutionScale);
    if (g_RenderScale == 0) {
        // Auto: the smallest scale whose height covers the monitor, so the
        // picture stays sharp when the window is maximized or fullscreen.
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(GameWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
        UINT screenHeight = UINT(mi.rcMonitor.bottom - mi.rcMonitor.top);
        UINT xboxHeight = std::max<UINT>(pp->BackBufferHeight, 1);
        g_RenderScale = std::clamp<UINT>((screenHeight + xboxHeight - 1) / xboxHeight, 1u, 8u);
    }
    D3DPRESENT_PARAMETERS hp = {};
    hp.BackBufferWidth = 64; // the device's own swap chain is not used for presenting
    hp.BackBufferHeight = 64;
    hp.BackBufferFormat = D3DFMT_X8R8G8B8;
    hp.BackBufferCount = 1;
    hp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    hp.hDeviceWindow = GameWindow();
    hp.Windowed = TRUE;
    hp.EnableAutoDepthStencil = FALSE;
    hp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9Ex* hostDevice = nullptr;
    HRESULT hr = g_D3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GameWindow(),
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &hp, nullptr, &hostDevice);
    g_Device = hostDevice;
    if (FAILED(hr))
        Fatal("Could not create the Direct3D 9 device (%08lX).", hr);
    static bool adapterLogged = false; // the device is made again at each video mode change
    D3DADAPTER_IDENTIFIER9 gpu = {};
    if (!adapterLogged && SUCCEEDED(g_D3D->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &gpu))) {
        adapterLogged = true;
        LOG_INFO("GPU: %s (vendor %04lX device %04lX), driver %s %u.%u.%u.%u", gpu.Description, gpu.VendorId,
            gpu.DeviceId, gpu.Driver, HIWORD(gpu.DriverVersion.HighPart),
            LOWORD(gpu.DriverVersion.HighPart), HIWORD(gpu.DriverVersion.LowPart),
            LOWORD(gpu.DriverVersion.LowPart));
    }

    const UINT hostWidth = pp->BackBufferWidth * g_RenderScale, hostHeight = pp->BackBufferHeight * g_RenderScale;
    if (FAILED(g_Device->CreateTexture(hostWidth, hostHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &g_HostBackTexture, nullptr)) ||
        FAILED(g_Device->CreateDepthStencilSurface(hostWidth, hostHeight, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
            &g_HostDepth, nullptr)))
        Fatal("Could not create the %ux%u frame buffer. Lower ResolutionScale in settings.ini.", hostWidth, hostHeight);
    g_HostBackTexture->GetSurfaceLevel(0, &g_HostBackBuffer);
    LOG_INFO("Rendering at %ux%u (%ux)", hostWidth, hostHeight, g_RenderScale);
    debug::MenuDeviceCreated(g_Device, GameWindow());

    // Xbox-side device and frame buffers.
    // The XDK's device is a static object inside the D3D library's data; XDK
    // code that runs natively addresses it directly as well as via D3D__pDevice.
    g_XboxDevice = reinterpret_cast<uint8_t*>(uintptr_t(xbox_globals::kDeviceObject));
    if (!g_PushBuffer)
        g_PushBuffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPushBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    std::memset(g_XboxDevice, 0, xbox_globals::kDeviceObjectSize);
    *reinterpret_cast<DWORD*>(g_XboxDevice + kDeviceRefCount) = 1;
    ResetPushBuffer();
    *reinterpret_cast<uint8_t**>(uintptr_t(xbox_globals::kDevicePtr)) = g_XboxDevice;

    g_State.backBuffer = CreateXboxSurface(pp->BackBufferWidth, pp->BackBufferHeight, FMT_LIN_X8R8G8B8, 4);
    g_State.depthBuffer = CreateXboxSurface(pp->BackBufferWidth, pp->BackBufferHeight, FMT_LIN_D24S8, 4);
    g_State.backBuffer->Common += 1; // held by the device
    kernel::ConsumeWrites(ResourceData(g_State.backBuffer), 0x1000000); // our own zero-fill is not game output
    g_State.depthBuffer->Common += 1;
    // Device fields read by XDK code that runs natively (Get2DSurfaceDesc).
    *reinterpret_cast<Surface**>(g_XboxDevice + kDeviceFrameBuffer) = g_State.backBuffer;
    *reinterpret_cast<DWORD*>(g_XboxDevice + kDeviceMultiSampleType) = 0x0011; // D3DMULTISAMPLE_NONE

    // Direct3D's initial texture stage states: each stage reads the texture
    // coordinate set with its own number (the glow blur relies on this).
    for (DWORD stage = 0; stage < 4; ++stage)
        g_State.textureStates[stage][TSS_TEXCOORDINDEX] = stage;

    g_State.renderTarget = g_State.backBuffer;
    g_State.depthStencil = pp->EnableAutoDepthStencil ? g_State.depthBuffer : nullptr;
    g_State.viewport = { 0, 0, pp->BackBufferWidth, pp->BackBufferHeight, 0.0f, 1.0f };
    WriteViewportConstants();

    g_Device->SetRenderTarget(0, g_HostBackBuffer);
    g_Device->SetDepthStencilSurface(g_State.depthStencil ? g_HostDepth : nullptr);
    g_Device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    g_Device->BeginScene();

    *device = g_XboxDevice;
    return D3D_OK;
}

// Mirrors the XDK: the reference count lives in the device object (game code
// may AddRef it inline) and the last Release destroys the device.
static ULONG __stdcall XbDeviceRelease()
{
    DWORD& refs = *reinterpret_cast<DWORD*>(g_XboxDevice + kDeviceRefCount);
    if (refs > 1)
        return --refs;
    refs = 0;
    *reinterpret_cast<uint8_t**>(uintptr_t(xbox_globals::kDevicePtr)) = nullptr;
    LOG_INFO("Device released by the game");
    return 0;
}

// ---------------------------------------------------------------------------
// Surfaces and render targets
// ---------------------------------------------------------------------------
IDirect3DSurface9* HostSurfaceFor(Surface* surface, bool depth);

static Surface* AddRef(Surface* s)
{
    if (s)
        s->Common++;
    return s;
}

static Surface* __stdcall XbGetBackBuffer2(INT index)
{
    (void)index;
    return AddRef(g_State.backBuffer);
}

static Surface* __stdcall XbGetRenderTarget2() { return AddRef(g_State.renderTarget); }
static Surface* __stdcall XbGetDepthStencilSurface2() { return AddRef(g_State.depthStencil); }

static void __stdcall XbSetRenderTarget(Surface* renderTarget, Surface* depthStencil)
{
    if (renderTarget)
        g_State.renderTarget = renderTarget;
    g_State.depthStencil = depthStencil;
    g_Device->SetRenderTarget(0, HostSurfaceFor(g_State.renderTarget, false));
    g_Device->SetDepthStencilSurface(depthStencil ? HostSurfaceFor(depthStencil, true) : nullptr);
    // Like D3D8, setting a render target resets the viewport to cover it.
    UINT width, height;
    XboxSurfaceSize(g_State.renderTarget, width, height);
    g_State.viewport = { 0, 0, width, height, 0.0f, 1.0f };
    WriteViewportConstants();
}

// Copies rectangles between surfaces on the GPU. The game uses it to grab the
// frame into an image surface (e.g. the saber-clash flash). Host surfaces are
// RenderScale() times the Xbox size, so rectangles are scaled.
static HRESULT __stdcall XbCopyRects(Surface* source, const RECT* rects, UINT count, Surface* dest, const POINT* points)
{
    if (!source || !dest)
        return D3DERR_INVALIDCALL;
    IDirect3DSurface9* from = HostSurfaceFor(source, false);
    IDirect3DSurface9* to = HostSurfaceFor(dest, false);
    UINT sw, sh;
    XboxSurfaceSize(source, sw, sh);
    const RECT whole = { 0, 0, LONG(sw), LONG(sh) };
    if (!rects) {
        rects = &whole;
        count = 1;
    }
    const LONG k = LONG(g_RenderScale);
    for (UINT i = 0; i < count; ++i) {
        const RECT& r = rects[i];
        POINT at = points ? points[i] : POINT{ r.left, r.top };
        RECT src = { r.left * k, r.top * k, r.right * k, r.bottom * k };
        RECT dst = { at.x * k, at.y * k, (at.x + r.right - r.left) * k, (at.y + r.bottom - r.top) * k };
        g_Device->StretchRect(from, &src, to, &dst, D3DTEXF_NONE);
    }
    return D3D_OK;
}

IDirect3DSurface9* HostBackBuffer() { return g_HostBackBuffer; }
IDirect3DTexture9* HostBackBufferTexture() { return g_HostBackTexture; }
IDirect3DSurface9* HostDepthBuffer() { return g_HostDepth; }

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------
float DepthScale()
{
    Surface* ds = g_State.depthStencil;
    if (!ds)
        return 1.0f;
    switch ((ds->Format & FORMAT_FORMAT_MASK) >> FORMAT_FORMAT_SHIFT) {
    case FMT_D16:
    case FMT_LIN_D16:
        return 65535.0f;
    case FMT_D24S8:
    case FMT_LIN_D24S8:
        return 16777215.0f;
    case FMT_F16:
    case FMT_LIN_F16:
        return 511.9375f;
    case FMT_F24S8:
    case FMT_LIN_F24S8:
        return 1.0e30f;
    }
    return 1.0f;
}

static void __stdcall XbGetViewportOffsetAndScale(float* offset, float* scale)
{
    const Viewport& v = g_State.viewport;
    float z = DepthScale();
    scale[0] = v.Width * 0.5f;
    scale[1] = -(v.Height * 0.5f);
    scale[2] = (v.MaxZ - v.MinZ) * z;
    scale[3] = 1.0f;
    offset[0] = v.Width * 0.5f + v.X + g_State.screenSpaceOffset[0];
    offset[1] = v.Height * 0.5f + v.Y + g_State.screenSpaceOffset[1];
    offset[2] = v.MinZ * z;
    offset[3] = 0.0f;
}

// Like the XDK, keep the viewport transform in the reserved vertex program
// constants c-38 (scale) and c-37 (offset); compiled programs end with it.
static void WriteViewportConstants()
{
    float c[2][4];
    XbGetViewportOffsetAndScale(c[1], c[0]);
    constexpr int kReg = 96 - 38;
    std::memcpy(g_State.vertexConstants[kReg], c, sizeof(c));
    std::memcpy(reinterpret_cast<uint8_t*>(uintptr_t(xbox_globals::kVertexConstants)) + kReg * 16, c, sizeof(c));
}

static void __stdcall XbSetViewport(const Viewport* vp)
{
    g_State.viewport = *vp;
    WriteViewportConstants();
}

static void __stdcall XbSetScissors(DWORD count, BOOL exclusive, const D3DRECT* rects)
{
    if (count == 0 || !rects) {
        g_Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        return;
    }
    if (exclusive || count > 1)
        LOG_DEBUG("SetScissors: count %lu exclusive %d (using first rect)", count, exclusive);
    const LONG k = LONG(g_RenderScale);
    RECT r = { rects[0].x1 * k, rects[0].y1 * k, rects[0].x2 * k, rects[0].y2 * k };
    g_Device->SetScissorRect(&r);
    g_Device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

static void __stdcall XbSetScreenSpaceOffset(float x, float y)
{
    g_State.screenSpaceOffset[0] = x;
    g_State.screenSpaceOffset[1] = y;
    WriteViewportConstants();
}

// ---------------------------------------------------------------------------
// Clear. Xbox can clear individual color channels; D3D9 Clear cannot, so
// partial clears are drawn as a masked full-viewport quad.
// ---------------------------------------------------------------------------
void DrawClearQuad(DWORD channels, D3DCOLOR color);

static void __stdcall XbClear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    const Viewport& v = g_State.viewport;
    const UINT k = g_RenderScale;
    D3DVIEWPORT9 hv = { v.X * k, v.Y * k, v.Width * k, v.Height * k, 0.0f, 1.0f };
    g_Device->SetViewport(&hv);
    D3DRECT scaled[16];
    if (rects && count <= 16) {
        for (DWORD i = 0; i < count; ++i)
            scaled[i] = { rects[i].x1 * LONG(k), rects[i].y1 * LONG(k), rects[i].x2 * LONG(k), rects[i].y2 * LONG(k) };
        rects = scaled;
    } else {
        count = 0; // no (or unexpectedly many) rectangles: clear the whole viewport
        rects = nullptr;
    }

    DWORD host = 0;
    if ((flags & CLEAR_TARGET) == CLEAR_TARGET)
        host |= D3DCLEAR_TARGET;
    if ((flags & CLEAR_ZBUFFER) && g_State.depthStencil)
        host |= D3DCLEAR_ZBUFFER;
    if ((flags & CLEAR_STENCIL) && g_State.depthStencil)
        host |= D3DCLEAR_STENCIL;
    if (host)
        g_Device->Clear(count, rects, host, color, z, stencil);
    if ((flags & CLEAR_TARGET) && (flags & CLEAR_TARGET) != CLEAR_TARGET)
        DrawClearQuad(flags & CLEAR_TARGET, color);
}

// ---------------------------------------------------------------------------
// Presentation and frame pacing
// ---------------------------------------------------------------------------
static const double kRefreshHz = 60.0;

// The emulated display's vertical blank: a fixed 60 Hz clock. Several threads wait on it
// (the swap, and the Sofdec movie player's vsync threads, which resume its decoder thread
// each vblank), so a wait only reads the clock: it sleeps until the next tick after now.
// The last moment is waited out by yielding, not spinning: game threads share one CPU core.
static void WaitForVBlank()
{
    static LARGE_INTEGER freq;
    static LONGLONG epoch = 0;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    const LONGLONG period = LONGLONG(freq.QuadPart / kRefreshHz);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!epoch)
        epoch = now.QuadPart;
    const LONGLONG next = epoch + ((now.QuadPart - epoch) / period + 1) * period;
    while (now.QuadPart < next) {
        const LONGLONG remainingMs = (next - now.QuadPart) * 1000 / freq.QuadPart;
        if (remainingMs >= 2)
            Sleep(DWORD(remainingMs - 1));
        else
            SwitchToThread();
        QueryPerformanceCounter(&now);
    }
}

static void __stdcall XbBlockUntilVerticalBlank() { WaitForVBlank(); }

// The frame rate in the log, every 30 seconds of play: the average, the slowest frame and how many took
// longer than two vblanks (visible stutter), so a report's log shows how the game ran.
static void NoteFrameTime()
{
    static LARGE_INTEGER freq;
    static LONGLONG start = 0, last = 0, slowest = 0;
    static int frames = 0, long_ = 0;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!start) {
        start = last = now.QuadPart;
        return;
    }
    const LONGLONG frame = now.QuadPart - last;
    last = now.QuadPart;
    ++frames;
    slowest = std::max(slowest, frame);
    long_ += frame * 1000 > freq.QuadPart * 34; // over two 60 Hz vblanks
    const double seconds = double(now.QuadPart - start) / double(freq.QuadPart);
    if (seconds < 30.0)
        return;
    LOG_INFO("Performance: %.1f fps over %.0f s, slowest frame %.0f ms, %d frame(s) over 34 ms", frames / seconds,
        seconds, double(slowest) * 1000.0 / double(freq.QuadPart), long_);
    start = now.QuadPart;
    frames = long_ = 0;
    slowest = 0;
}

// Software-rendered content (the Sofdec movie player) is written straight into
// the Xbox back buffer's memory; show it when the CPU has written there.
static void UploadCpuWrittenBackBuffer()
{
    Surface* bb = g_State.backBuffer;
    UINT w = (bb->Size & SIZE_WIDTH_MASK) + 1, h = ((bb->Size & SIZE_HEIGHT_MASK) >> SIZE_HEIGHT_SHIFT) + 1;
    UINT pitch = (((bb->Size & SIZE_PITCH_MASK) >> SIZE_PITCH_SHIFT) + 1) * 64;
    const uint8_t* src = ResourceData(bb);
    if (!kernel::ConsumeWrites(src, pitch * h))
        return;
    if (!g_CpuFrame && FAILED(g_Device->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_CpuFrame, nullptr)))
        return;
    D3DLOCKED_RECT lr;
    if (FAILED(g_CpuFrame->LockRect(&lr, nullptr, 0)))
        return;
    for (UINT y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, src + y * pitch, w * 4);
    g_CpuFrame->UnlockRect();
    g_Device->StretchRect(g_CpuFrame, nullptr, g_HostBackBuffer, nullptr, D3DTEXF_LINEAR);
}

// (Re)creates the window swap chain when the client area changes size.
// Returns false while the window has no area (minimized).
static bool EnsureWindowChain()
{
    RECT client;
    GetClientRect(GameWindow(), &client);
    UINT width = UINT(std::max<LONG>(client.right - client.left, 0));
    UINT height = UINT(std::max<LONG>(client.bottom - client.top, 0));
    if (!width || !height) {
        // Minimised in a background test run (core/window.h): keep presenting at the last size, or the set
        // one, so frames are finished and screenshots taken.
        if (!RunningInBackground() || !IsIconic(GetAncestor(GameWindow(), GA_ROOT)))
            return false;
        if (g_WindowChain)
            return true;
        width = GetSettings().width > 0 ? UINT(GetSettings().width) : 1280;
        height = GetSettings().height > 0 ? UINT(GetSettings().height) : 720;
    }
    if (g_WindowChain && width == g_ChainWidth && height == g_ChainHeight)
        return true;
    if (g_SwapChainBuffer) g_SwapChainBuffer->Release();
    if (g_WindowChain) g_WindowChain->Release();
    g_SwapChainBuffer = nullptr;
    g_WindowChain = nullptr;
    D3DPRESENT_PARAMETERS cp = {};
    cp.BackBufferWidth = width;
    cp.BackBufferHeight = height;
    cp.BackBufferFormat = D3DFMT_X8R8G8B8;
    cp.BackBufferCount = 1;
    cp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    cp.hDeviceWindow = GameWindow();
    cp.Windowed = TRUE;
    // Frame pacing is done by our vblank clock; VSync additionally waits for the display.
    cp.PresentationInterval = GetSettings().vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(g_Device->CreateAdditionalSwapChain(&cp, &g_WindowChain)) ||
        FAILED(g_WindowChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &g_SwapChainBuffer))) {
        LOG_ERROR("Could not create a %ux%u swap chain", width, height);
        if (g_WindowChain) g_WindowChain->Release();
        g_WindowChain = nullptr;
        return false;
    }
    g_ChainWidth = width;
    g_ChainHeight = height;
    LOG_INFO("Presenting at %ux%u", width, height);
    return true;
}

// Draws the debug menu over the window's back buffer (it saves and restores the
// device state it changes; the render target and depth buffer are ours to restore).
static void DrawMenu()
{
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* depth = nullptr;
    g_Device->GetRenderTarget(0, &target);
    g_Device->GetDepthStencilSurface(&depth);
    g_Device->SetRenderTarget(0, g_SwapChainBuffer);
    g_Device->SetDepthStencilSurface(nullptr);
    g_Device->BeginScene();
    debug::RenderMenu();
    g_Device->EndScene();
    g_Device->SetRenderTarget(0, target);
    g_Device->SetDepthStencilSurface(depth);
    if (target) target->Release();
    if (depth) depth->Release();
}

// Scales the rendered frame into the window at the chosen aspect ratio (the
// game renders 16:9 anamorphic when widescreen is on), with black bars.
static void PresentFrame()
{
    g_Device->EndScene();
    FlightEndFrame(g_HostBackBuffer);
    if (!EnsureWindowChain()) {
        g_Device->BeginScene();
        return;
    }
    D3DSURFACE_DESC out;
    g_SwapChainBuffer->GetDesc(&out);
    const float aspect = GetSettings().widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    LONG w = LONG(out.Width), h = LONG(out.Height);
    if (!GetSettings().stretch) {
        if (w > LONG(h * aspect + 0.5f))
            w = LONG(h * aspect + 0.5f);
        else
            h = LONG(w / aspect + 0.5f);
    }
    RECT dest = { (LONG(out.Width) - w) / 2, (LONG(out.Height) - h) / 2, 0, 0 };
    dest.right = dest.left + w;
    dest.bottom = dest.top + h;
    if (w != LONG(out.Width) || h != LONG(out.Height))
        g_Device->ColorFill(g_SwapChainBuffer, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    g_Device->StretchRect(g_HostBackBuffer, nullptr, g_SwapChainBuffer, &dest, D3DTEXF_LINEAR);
    SavePendingScreenshot(g_SwapChainBuffer);
    if (debug::MenuOpen())
        DrawMenu();
    HRESULT presented = g_WindowChain->Present(nullptr, nullptr, nullptr, nullptr, 0);
    if (SUCCEEDED(presented))
        NoteGameFramePresented();
    static HRESULT lastError = S_OK;
    if (FAILED(presented) && presented != lastError)
        LOG_ERROR("Present failed (%08lX)", presented);
    lastError = FAILED(presented) ? presented : S_OK;
    g_Device->BeginScene();
}

// The engine's frame limiter reads fpsLimit (vars_xbox.cfg) from its renderer
// object; keep it at the player's setting.
static void ApplyFpsLimit()
{
    auto* services = *reinterpret_cast<uint8_t**>(uintptr_t(game::kEngineServices));
    auto* core = services ? *reinterpret_cast<uint8_t**>(services + 0x38) : nullptr;
    auto* renderer = core ? *reinterpret_cast<uint8_t**>(core + 8) : nullptr;
    if (renderer)
        *reinterpret_cast<int*>(renderer + game::kRendererFpsLimit) = GetSettings().fpsLimit;
    game::ApplyDevOptions(renderer);
}

static DWORD __stdcall XbSwap(DWORD flags)
{
    (void)flags;
    UploadCpuWrittenBackBuffer();
    PresentFrame();
    ApplyFpsLimit();
    debug::MenuAfterFrame();
    NoteFrame();

    DWORD interval = XboxRenderStates()[RS_PRESENTATIONINTERVAL];
    if (interval == 0)
        interval = g_State.pp.FullScreen_PresentationInterval;
    // D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000) presents without waiting.
    if (!(interval & 0x80000000)) {
        DWORD vblanks = interval == 0 ? 1 : interval;
        for (DWORD i = 0; i < vblanks && i < 4; ++i)
            WaitForVBlank();
    }

    ++g_State.swapCount;
    NoteFrameTime();
    CollectResources(g_State.swapCount);
    return g_State.swapCount;
}

static void __stdcall XbGetDisplayMode(DisplayMode* mode)
{
    mode->Width = g_State.pp.BackBufferWidth;
    mode->Height = g_State.pp.BackBufferHeight;
    mode->RefreshRate = 60;
    mode->Flags = g_State.pp.Flags;
    mode->Format = g_State.pp.BackBufferFormat;
}

// ---------------------------------------------------------------------------
// GPU synchronisation: the host driver orders everything, so these are no-ops.
// ---------------------------------------------------------------------------
static void* __stdcall XbMakeSpace()
{
    ResetPushBuffer();
    return g_PushBuffer;
}

static void* __stdcall XbMakeRequestedSpace(DWORD minimum, DWORD requested)
{
    (void)minimum;
    (void)requested;
    ResetPushBuffer();
    return g_PushBuffer;
}

static void __stdcall XbBlockOnResource(Resource* resource) { (void)resource; }
static void __stdcall XbBlockOnTime(DWORD time, int makeSpace)
{
    (void)time;
    (void)makeSpace;
}
static void __stdcall XbKickOffAndWaitForIdle() {}
static void __stdcall XbKickOff() {}
static BOOL __stdcall XbIsBusy() { return FALSE; }
static DWORD g_Fence = 0;
static DWORD __stdcall XbSetFence(DWORD type)
{
    (void)type;
    return ++g_Fence;
}

// ---------------------------------------------------------------------------
// Display features with no PC equivalent.
// ---------------------------------------------------------------------------
static void __stdcall XbSetFlickerFilter(DWORD filter) { (void)filter; }
static void __stdcall XbSetSoftDisplayFilter(BOOL enable) { (void)enable; }
static HRESULT __stdcall XbPersistDisplay() { return D3D_OK; }
static void __stdcall XbSetGammaRamp(DWORD flags, const void* ramp)
{
    (void)flags;
    (void)ramp;
}

SDK_REPLACE("Direct3D_CreateDevice", XbCreateDevice);
SDK_REPLACE("D3DDevice_Release", XbDeviceRelease);
SDK_REPLACE("D3DDevice_GetBackBuffer2", XbGetBackBuffer2);
SDK_REPLACE("D3DDevice_GetRenderTarget2", XbGetRenderTarget2);
SDK_REPLACE("D3DDevice_GetDepthStencilSurface2", XbGetDepthStencilSurface2);
SDK_REPLACE("D3DDevice_SetRenderTarget", XbSetRenderTarget);
SDK_REPLACE("D3DDevice_SetViewport", XbSetViewport);
SDK_REPLACE("D3DDevice_GetViewportOffsetAndScale", XbGetViewportOffsetAndScale);
SDK_REPLACE("D3DDevice_SetScissors", XbSetScissors);
SDK_REPLACE("D3DDevice_SetScreenSpaceOffset", XbSetScreenSpaceOffset);
SDK_REPLACE("D3DDevice_Clear", XbClear);
SDK_REPLACE("D3DDevice_BlockUntilVerticalBlank", XbBlockUntilVerticalBlank);
SDK_REPLACE("D3DDevice_Swap", XbSwap);
SDK_REPLACE("D3DDevice_GetDisplayMode", XbGetDisplayMode);
SDK_REPLACE("D3DDevice_MakeSpace", XbMakeSpace);
SDK_REPLACE("D3D_MakeRequestedSpace_8", XbMakeRequestedSpace);
SDK_REPLACE("D3D_BlockOnResource", XbBlockOnResource);
SDK_REPLACE("D3D_BlockOnTime", XbBlockOnTime);
SDK_REPLACE("D3D_KickOffAndWaitForIdle", XbKickOffAndWaitForIdle);
SDK_REPLACE("CDevice_KickOff", XbKickOff);
SDK_REPLACE("D3DDevice_IsBusy", XbIsBusy);
SDK_REPLACE("D3D_SetFence", XbSetFence);
SDK_REPLACE("D3DDevice_SetFlickerFilter", XbSetFlickerFilter);
SDK_REPLACE("D3DDevice_SetSoftDisplayFilter", XbSetSoftDisplayFilter);
SDK_REPLACE("D3DDevice_PersistDisplay", XbPersistDisplay);
SDK_REPLACE("D3DDevice_SetGammaRamp", XbSetGammaRamp);

// Pure CPU parts of the XDK D3D library, run as-is (see d3d.h).
SDK_PASSTHROUGH("D3D_sub_504DB0"); // Direct3DCreate8
SDK_PASSTHROUGH("D3D_GetAdapterModeCount");
SDK_PASSTHROUGH("D3D_EnumAdapterModes");
SDK_PASSTHROUGH("CMiniport_GetDisplayCapabilities");
SDK_PASSTHROUGH("D3D_GetDeviceCaps");
SDK_PASSTHROUGH("D3DDevice_GetDeviceCaps");
SDK_PASSTHROUGH("D3D_CheckDeviceFormat");
SDK_PASSTHROUGH("D3D_SetPushBufferSize");
SDK_PASSTHROUGH("D3DDevice_CreateTexture2");
SDK_PASSTHROUGH("D3DDevice_CreateVertexBuffer2");
SDK_PASSTHROUGH("D3DDevice_CreateIndexBuffer2");
SDK_PASSTHROUGH("D3DDevice_CreatePalette2");
SDK_PASSTHROUGH("D3DDevice_CreateVertexShader");
SDK_PASSTHROUGH("D3DDevice_DeleteVertexShader");
SDK_PASSTHROUGH("D3DDevice_CreatePixelShader");
SDK_PASSTHROUGH("D3DDevice_DeletePixelShader");
SDK_PASSTHROUGH("D3DResource_Register");
SDK_PASSTHROUGH("D3DResource_GetType");
SDK_PASSTHROUGH("D3DResource_AddRef");
SDK_PASSTHROUGH("D3DResource_Release");
SDK_PASSTHROUGH("D3D_DestroyResource");
SDK_PASSTHROUGH("D3DBaseTexture_GetLevelCount");
SDK_PASSTHROUGH("D3DTexture_GetSurfaceLevel2");
SDK_PASSTHROUGH("D3DTexture_LockRect");
SDK_PASSTHROUGH("D3DSurface_LockRect");
SDK_PASSTHROUGH("D3DSurface_GetDesc");
SDK_PASSTHROUGH("Lock2DSurface");
SDK_PASSTHROUGH("Get2DSurfaceDesc");
// Unnamed XDK helpers the game calls directly: 503600 forwards to
// Get2DSurfaceDesc (GetLevelDesc); 503C60 creates an image surface with the
// engine's allocators. Both are CPU-only.
SDK_PASSTHROUGH("D3D_sub_503600");
SDK_PASSTHROUGH("D3D_sub_503C60");
SDK_REPLACE("D3DDevice_CopyRects", XbCopyRects);
SDK_PASSTHROUGH("D3DVertexBuffer_Lock2");
SDK_PASSTHROUGH("D3DPalette_Lock2");
SDK_PASSTHROUGH("XGIsSwizzledFormat");
SDK_PASSTHROUGH("XGSwizzleRect");
SDK_PASSTHROUGH("XGUnswizzleRect");
SDK_PASSTHROUGH("XGSetTextureHeader");
SDK_PASSTHROUGH("XGSetVertexBufferHeader");
SDK_PASSTHROUGH("XGSetIndexBufferHeader");
SDK_PASSTHROUGH("D3DX_sub_512680");
SDK_PASSTHROUGH("D3DX_sub_512721");
SDK_PASSTHROUGH("D3DX_sub_512A4B");

} // namespace swrots::d3d
