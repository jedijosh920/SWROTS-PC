// True widescreen: the renderer's view, widened at the sides.
//
// The renderer builds its view in one function (0x211140, thiscall (bool resetViewport, bool planes),
// ret 8), from the viewport rectangle in screen pixels (0x6193EC x, y, width, height) and the field of view:
//  - the corners of the view at depth 1: x = (px + inset) * 0x619410 + 0x619418 for the viewport's left
//    and right edges (inset 0x567134 pixels in), y likewise with 0x619414 / 0x61941C;
//  - the culling frustum's four side planes (0x619370, 4 x plane), each through the eye and two corners
//    (0x58190, thiscall (plane* out; eye, corner, corner, 0), ret 0x10), with the near and far planes
//    before them (0x619350): objects and sectors (through portals) outside them are not drawn;
//  - when the renderer draws (0x787728 set), the projection: D3DXMatrixPerspectiveOffCenterLH (0x512680)
//    over the same corners (scaled by a mode factor, renderer +0x1F8), into 0x78A3E0, set with 0x210030,
//    and the viewport set with 0x20FFF0; the combined view and projection (0x78B460, the shaded draws')
//    are rebuilt by 0x210FB0.
// The game's 4:3 frame is 640 pixels wide. With true widescreen the host frame has a margin of m Xbox pixels
// (d3d::WideMargin) at each side. After the renderer built its view, this builds the same view again over
// the viewport extended by m pixels at each side: the same pixel scale (nothing in the 4:3 frame moves),
// side planes that let through what the margins show, a projection that reaches them, and a viewport from
// -m to 640 + m. The game's own viewport rectangle (read by its 2D code) is left as it was.
// The game also draws parts of the screen on their own: the part of the level seen through a portal, over the
// rectangle the portal covers, clamped to the 4:3 frame (slot 0x64, 0x211A10, then 0x211140). When such a view
// has the whole view's mapping (the same camera) and reaches a side of the frame, it is widened at that side.
//
// Sectors seen through portals: the level's sectors are drawn through their portals (0x200A..: each portal's
// corners projected to the screen with the view's focal lengths and centre, 0x619430..0x61943C; the rectangle
// they cover is cut by the portal it is seen through, the first ones by the screen's rectangle 0x78B4E0 (x, y,
// width, height; push at 0x200DF6, cut by 0x58100): a portal outside it, and what is behind it, is not drawn).
// With true widescreen the first cut is by the screen's rectangle grown at the sides (g_PortalScreen, the
// push's address changed to it): a sector seen beside the 4:3 frame is drawn (the floor and walls there had
// holes). The side planes and this cut reach three margins out (kCullScale): with one margin, pieces of the
// level beside the 4:3 frame were still left out at times (seen in the first mission).
//
// The loading screen (an object of 0x48 bytes made as a level loads, 0x2156B0, its pictures set up by 0x42C40
// and let go by 0x42FB0, thiscall (), from its destructor 0x214F70 too) is noted while it is up.

#include "game/widescreen.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "d3d/d3d.h"

namespace swrots::game {

namespace {

constexpr uint32_t kBuildView = 0x00211140;
constexpr uint8_t kBuildViewPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x74 };
using BuildViewFn = char(__fastcall*)(void* renderer, void* edx, int resetViewport, int planes);
BuildViewFn g_OriginalBuildView = nullptr;

// The renderer's view state.
constexpr uint32_t kViewport = 0x006193EC;          // int x, y, width, height (screen pixels)
constexpr uint32_t kPixelToViewX = 0x00619410;      // float: view x per pixel, then its offset (0x619418)
constexpr uint32_t kPixelToViewY = 0x00619414;      // float: view y per pixel, then its offset (0x61941C)
constexpr uint32_t kViewOffsetX = 0x00619418;
constexpr uint32_t kViewOffsetY = 0x0061941C;
constexpr uint32_t kPixelInset = 0x00567134;        // float: the corners' inset from the viewport's edges
constexpr uint32_t kNear = 0x00619400, kFar = 0x00619404;
constexpr uint32_t kSidePlanes = 0x00619370;        // 4 planes (float4)
constexpr uint32_t kRendering = 0x00787728;         // the projection is built
constexpr uint32_t kProjection = 0x0078A3E0;        // the projection matrix
constexpr uint32_t kScreenSize = 0x0078B4E8;        // int width, height (640x480)
constexpr uint32_t kScreenOffset = 0x0078B4E0;      // int x, y added to the viewport set
constexpr uint32_t kRendererMode = 0x1F8;           // renderer field: 0x2A, 0x2C scale the projection's corners
constexpr uint32_t kModeScale2A = 0x00560BC8, kModeScale2C = 0x00562A6C;

constexpr uint32_t kPlaneThrough = 0x00058190;
using PlaneThroughFn = void(__fastcall*)(float* plane, void* edx, const float* a, const float* b, const float* c, int flag);
constexpr uint32_t kPerspectiveOffCenter = 0x00512680;
using PerspectiveOffCenterFn = void*(__stdcall*)(void* out, float l, float r, float b, float t, float zn, float zf);
constexpr uint32_t kSetProjection = 0x00210030;
using SetProjectionFn = void(__cdecl*)(const void* matrix);
constexpr uint32_t kSetViewport = 0x0020FFF0;
using SetViewportFn = void(__cdecl*)(const void* viewport);
constexpr uint32_t kCombineView = 0x00210FB0;
using CombineViewFn = void(__fastcall*)(void* renderer, void* edx);

constexpr uint32_t kPortalScreenPush = 0x00200DF6;  // push 0x78B4E0 (the first portals' cut)
constexpr uint8_t kPortalScreenPushBytes[] = { 0x68, 0xE0, 0xB4, 0x78, 0x00 };
int g_PortalScreen[4] = { 0, 0, 640, 480 };         // x, y, width, height: the screen's, grown at the sides
constexpr int kCullScale = 3;                       // the side planes and the portals' cut reach 3 margins out

constexpr uint32_t kLoadingSetUp = 0x00042C40, kLoadingRelease = 0x00042FB0;
constexpr uint8_t kLoadingSetUpPrologue[] = { 0x83, 0xEC, 0x74, 0x56, 0x57 };   // sub esp, 0x74; push esi; push edi
constexpr uint8_t kLoadingReleasePrologue[] = { 0x51, 0x53, 0x56, 0x8B, 0xF1 }; // push ecx, ebx, esi; mov esi, ecx
using LoadingFn = uint32_t(__fastcall*)(void* screen, void* edx);
LoadingFn g_OriginalLoadingSetUp = nullptr, g_OriginalLoadingRelease = nullptr;
volatile bool g_LoadingScreenUp = false;

template <typename T>
T& At(uint32_t address)
{
    return *reinterpret_cast<T*>(uintptr_t(address));
}

struct ViewportRect {
    DWORD x, y, width, height;
    float minZ, maxZ;
};

// The pixel-to-view mapping of the last whole-width view: a narrower view with the same mapping is a part of
// it (the camera's), seen through a portal.
float g_WholeSx = 0.0f, g_WholeOx = 0.0f;

bool SameMapping(float a, float b)
{
    const float d = a > b ? a - b : b - a;
    return d <= 1e-5f * (a > 0.0f ? a : -a) + 1e-7f;
}

void Widen(void* renderer, bool planes)
{
    const int margin = d3d::WideMargin();
    const int* viewport = &At<int>(kViewport);
    const int* screen = &At<int>(kScreenSize);
    const int* screenRect = &At<int>(kScreenOffset); // x, y, width, height
    std::memcpy(g_PortalScreen, screenRect, sizeof(g_PortalScreen));
    if (margin <= 0 || viewport[2] <= 0)
        return;
    const float inset = At<float>(kPixelInset), sx = At<float>(kPixelToViewX), ox = At<float>(kViewOffsetX);
    // The whole screen's view, or a part of it the game draws on its own through a portal: the rectangle a
    // portal covers is cut by the screen's grown one (g_PortalScreen) and then clamped to the 4:3 frame, so the
    // part of the level seen through a portal at a side of the frame was drawn, and chosen, only up to that side
    // (in the first mission, a view from x 329 to 640: floors, props and characters beside the frame came and
    // went as the player moved). Such a view is told apart from other narrower ones (a portrait, a picture in
    // the HUD) by its mapping, the camera's; it is widened at the sides it shares with the frame.
    bool atLeft, atRight;
    if (viewport[0] == 0 && viewport[2] == screen[0]) {
        atLeft = atRight = true;
        g_WholeSx = sx;
        g_WholeOx = ox;
    } else {
        if (!SameMapping(sx, g_WholeSx) || !SameMapping(ox, g_WholeOx))
            return;
        atLeft = viewport[0] <= 0;
        atRight = viewport[0] + viewport[2] >= screen[0];
        if (!atLeft && !atRight)
            return;
    }
    // What is drawn is chosen from a wider view than the one shown: with the same width, pieces of the level
    // beside the 4:3 frame were still left out at times (holes in the floor and walls at the sides).
    const int cullMargin = margin * kCullScale;
    g_PortalScreen[0] -= cullMargin;
    g_PortalScreen[2] += 2 * cullMargin;
    const int marginLeft = atLeft ? margin : 0, marginRight = atRight ? margin : 0;
    const int cullLeft = atLeft ? cullMargin : 0, cullRight = atRight ? cullMargin : 0;
    const float sy = At<float>(kPixelToViewY), oy = At<float>(kViewOffsetY);
    const float x0 = float(viewport[0] - marginLeft), x1 = float(viewport[0] + viewport[2] + marginRight);
    const float left = (x0 + inset) * sx + ox, right = (x1 - inset) * sx + ox;
    const float top = (float(viewport[1]) + inset) * sy + oy, bottom = (float(viewport[1] + viewport[3]) - inset) * sy + oy;

    if (planes) {
        const float c0 = float(viewport[0] - cullLeft), c1 = float(viewport[0] + viewport[2] + cullRight);
        const float cl = (c0 + inset) * sx + ox, cr = (c1 - inset) * sx + ox;
        alignas(16) float eye[4] = {};
        alignas(16) float corners[4][4] = {
            { cl, top, 1.0f, 0.0f }, { cl, bottom, 1.0f, 0.0f }, { cr, bottom, 1.0f, 0.0f }, { cr, top, 1.0f, 0.0f },
        };
        const auto through = reinterpret_cast<PlaneThroughFn>(uintptr_t(kPlaneThrough));
        for (int i = 0; i < 4; ++i)
            through(&At<float>(kSidePlanes + 16 * i), nullptr, eye, corners[(i + 1) & 3], corners[i], 0);
    }

    if (!At<int>(kRendering))
        return;
    float scale = 1.0f;
    const int mode = *reinterpret_cast<const int*>(static_cast<const uint8_t*>(renderer) + kRendererMode);
    if (mode == 0x2A)
        scale = At<float>(kModeScale2A);
    else if (mode == 0x2C)
        scale = At<float>(kModeScale2C);
    reinterpret_cast<PerspectiveOffCenterFn>(uintptr_t(kPerspectiveOffCenter))(&At<uint8_t>(kProjection), left * scale,
        right * scale, bottom * scale, top * scale, At<float>(kNear) * scale, At<float>(kFar));
    reinterpret_cast<SetProjectionFn>(uintptr_t(kSetProjection))(&At<uint8_t>(kProjection));
    const int* offset = &At<int>(kScreenOffset);
    // The viewport reaches past the 640-pixel frame (x is negative, as a signed value): d3d/device.cpp
    // places it on the wide frame.
    const ViewportRect wide = { DWORD(viewport[0] - marginLeft + offset[0]), DWORD(viewport[1] + offset[1]),
        DWORD(viewport[2] + marginLeft + marginRight), DWORD(viewport[3]), 0.0f, 1.0f };
    reinterpret_cast<SetViewportFn>(uintptr_t(kSetViewport))(&wide);
    reinterpret_cast<CombineViewFn>(uintptr_t(kCombineView))(renderer, nullptr);
}

uint32_t __fastcall LoadingSetUpHook(void* screen, void* edx)
{
    if (!g_LoadingScreenUp)
        LOG_INFO("Widescreen: loading screen up");
    g_LoadingScreenUp = true;
    return g_OriginalLoadingSetUp(screen, edx);
}

uint32_t __fastcall LoadingReleaseHook(void* screen, void* edx)
{
    if (g_LoadingScreenUp)
        LOG_INFO("Widescreen: loading screen gone");
    g_LoadingScreenUp = false;
    return g_OriginalLoadingRelease(screen, edx);
}

char __fastcall BuildViewHook(void* renderer, void* edx, int resetViewport, int planes)
{
    const char result = g_OriginalBuildView(renderer, edx, resetViewport, planes);
    if (TrueWidescreen())
        Widen(renderer, (planes & 0xFF) != 0);
    return result;
}

// Hooks a function whose first `length` bytes are position-independent instructions; returns the
// original (those bytes copied to a stub that continues after them).
void* Detour(uint32_t address, const uint8_t* prologue, uint32_t length, const void* hook)
{
    uint8_t* stub = AllocStub(length + 5);
    std::memcpy(stub, prologue, length);
    stub[length] = 0xE9;
    int32_t back = int32_t(address + length) - int32_t(uintptr_t(stub) + length + 5);
    std::memcpy(stub + length + 1, &back, 4);
    PatchJump(address, hook);
    return stub;
}

bool Matches(uint32_t address, const uint8_t* bytes, size_t length)
{
    return std::memcmp(reinterpret_cast<const void*>(uintptr_t(address)), bytes, length) == 0;
}

} // namespace

bool LoadingScreenUp()
{
    return g_LoadingScreenUp;
}

void InstallWidescreen()
{
    if (!TrueWidescreen())
        return;
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBuildView)), kBuildViewPrologue, sizeof(kBuildViewPrologue)) != 0) {
        LOG_WARN("Widescreen: the renderer's view code is not as expected; the view is not widened (objects beside "
                 "the 4:3 frame can be missing)");
        return;
    }
    // The hook stays across reboots: the image is reloaded, so it is installed again at each boot.
    g_OriginalBuildView = reinterpret_cast<BuildViewFn>(Detour(kBuildView, kBuildViewPrologue,
        sizeof(kBuildViewPrologue), reinterpret_cast<const void*>(&BuildViewHook)));
    LOG_INFO("Widescreen: the renderer's view is widened at the sides");

    if (Matches(kPortalScreenPush, kPortalScreenPushBytes, sizeof(kPortalScreenPushBytes))) {
        if (At<int>(kScreenSize) > 0) // else the 640x480 screen until the first view is built
            std::memcpy(g_PortalScreen, &At<int>(kScreenOffset), sizeof(g_PortalScreen));
        const uint32_t rect = uint32_t(uintptr_t(g_PortalScreen));
        PatchBytes(kPortalScreenPush + 1, &rect, sizeof(rect));
        LOG_INFO("Widescreen: sectors seen through portals beside the 4:3 frame are drawn");
    } else {
        LOG_WARN("Widescreen: the portals' code is not as expected; sectors beside the 4:3 frame can be missing");
    }

    g_LoadingScreenUp = false;
    if (Matches(kLoadingSetUp, kLoadingSetUpPrologue, sizeof(kLoadingSetUpPrologue)) &&
        Matches(kLoadingRelease, kLoadingReleasePrologue, sizeof(kLoadingReleasePrologue))) {
        g_OriginalLoadingSetUp = reinterpret_cast<LoadingFn>(Detour(kLoadingSetUp, kLoadingSetUpPrologue,
            sizeof(kLoadingSetUpPrologue), reinterpret_cast<const void*>(&LoadingSetUpHook)));
        g_OriginalLoadingRelease = reinterpret_cast<LoadingFn>(Detour(kLoadingRelease, kLoadingReleasePrologue,
            sizeof(kLoadingReleasePrologue), reinterpret_cast<const void*>(&LoadingReleaseHook)));
    } else {
        LOG_WARN("Widescreen: the loading screen's code is not as expected; it is found from its picture");
    }
}

} // namespace swrots::game
