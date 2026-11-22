// Draw calls: Xbox primitives, vertex streams and immediate mode -> D3D9.

#include "d3d/d3d.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "d3d/recorder.h"
#include "d3d/shaders.h"
#include "game/characters.h"
#include "game/hvv.h"
#include "game/menus.h"
#include "game/widescreen.h"
#include "xapi/xapi.h"

namespace swrots::d3d {

using namespace xd3d;

bool ApplyDrawState(const DWORD* registerPresent);
extern bool g_PretransformedDraw;
const float* TextureScale(int stage);

// Texture coordinate set used by a stage (low bits of TEXCOORDINDEX).
static int StageForTexcoord(int set)
{
    for (int stage = 0; stage < 4; ++stage)
        if (int(State().textureStates[stage][TSS_TEXCOORDINDEX] & 0xFFFF) == set)
            return stage;
    return set < 4 ? set : -1;
}

// Byte offsets of each texture coordinate set in an FVF vertex, and their sizes.
static int FvfTexcoords(DWORD fvf, WORD offsets[8], BYTE sizes[8])
{
    WORD o = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: o = 12; break;
    case D3DFVF_XYZRHW: o = 16; break;
    case D3DFVF_XYZB1: o = 16; break;
    case D3DFVF_XYZB2: o = 20; break;
    case D3DFVF_XYZB3: o = 24; break;
    case D3DFVF_XYZB4: o = 28; break;
    }
    if (fvf & D3DFVF_NORMAL) o += 12;
    if (fvf & D3DFVF_PSIZE) o += 4;
    if (fvf & D3DFVF_DIFFUSE) o += 4;
    if (fvf & D3DFVF_SPECULAR) o += 4;
    int count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (int t = 0; t < count && t < 8; ++t) {
        static const BYTE kSizes[4] = { 2, 3, 4, 1 };
        sizes[t] = kSizes[(fvf >> (16 + t * 2)) & 3];
        offsets[t] = o;
        o = WORD(o + sizes[t] * 4);
    }
    return count;
}

// --- Statistics ----------------------------------------------------------------------------

struct DrawStats {
    DWORD draws, programmable, fixed, pixelShader, skipped;
    DWORD world; // depth-tested vertex program draws into a wide target (the 3D scene), this frame
};
static DrawStats g_Stats;

static void Skipped()
{
    ++g_Stats.skipped;
    FlightMarkSkipped();
}

// Per-draw log of one frame (F11, or env SWROTS_DRAWLOG_FRAME=n).
static bool g_DrawLog = false;

static void GroupHudPieces(); // true widescreen, below
static void RowBandPieces();
static bool LoadingScreen();

void LogFrameStats(DWORD frame)
{
    static DWORD logFrame = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("SWROTS_DRAWLOG_FRAME", v, sizeof(v)) ? DWORD(atoi(v)) : 0;
    }();
    bool requested = (GetAsyncKeyState(VK_F11) & 1) != 0 || (logFrame && frame + 1 == logFrame);
    if (g_DrawLog)
        LOG_INFO("--- end of draw log (frame %lu): %lu 3D draws, shell %s, level %s, player %s, paused %s, loading screen %s ---",
            frame, g_Stats.world, game::InShell() ? "up" : "down", game::InLevel() ? "yes" : "no", game::PlayerObject() ? "in a level" : "none",
            game::GamePaused() ? "yes" : "no", LoadingScreen() ? "up" : "no");
    g_DrawLog = requested;
    if (g_DrawLog) LOG_INFO("--- draw log (frame %lu) ---", frame + 1);
    GroupHudPieces();
    RowBandPieces();
    if (frame <= 3 || frame % 600 == 0)
        LOG_INFO("Frame %lu: %lu draws (%lu vertex program, %lu fixed-function, %lu pixel shader, %lu skipped)", frame,
            g_Stats.draws, g_Stats.programmable, g_Stats.fixed, g_Stats.pixelShader, g_Stats.skipped);
    g_Stats = {};
}

// --- Resolution scaling ------------------------------------------------------------

// Pre-transformed vertices carry Xbox pixel coordinates. Map them to the host
// target, which is RenderScale() k times larger: Xbox pixel i covers host
// pixels i*k .. i*k+k-1, so a position x becomes x*k + (k-1)/2 (MapScreenX/Y,
// device.cpp, which also place them on a wide target).

// A pre-transformed position on the current target, placed for true widescreen (see Place). With `toEdges`
// (a band or panel of colour), a vertex on the frame's left or right edge goes to the screen's: the band reaches
// across the whole width and what it holds stays where it is.
static void PlaceScreenPosition(float* xy, const SurfaceMap& map, Placement placement, bool toEdges)
{
    const float x = xy[0];
    const float k = float(RenderScale());
    if (toEdges && x <= 1.0f)
        xy[0] = x * k + (k - 1.0f) * 0.5f;
    else if (toEdges && x >= float(map.xboxWidth) - 1.0f)
        xy[0] = x * k + (k - 1.0f) * 0.5f + 2.0f * map.centre;
    else
        xy[0] = MapScreenX(map, x, placement);
    xy[1] = MapScreenY(map, xy[1], placement);
}

static void ScaleScreenPositions(uint8_t* vertices, UINT count, UINT stride, UINT positionOffset,
    const SurfaceMap& map, Placement placement, bool toEdges = false)
{
    if (RenderScale() == 1 && !map.wide)
        return;
    for (UINT v = 0; v < count; ++v)
        PlaceScreenPosition(reinterpret_cast<float*>(vertices + size_t(v) * stride + positionOffset), map, placement,
            toEdges);
}

// --- True widescreen: where a screen-space draw goes ------------------------------------------------
// On a wide target (d3d.h) the game's 640x480 frame sits in the middle. A pre-transformed draw is:
//  - spread over the whole width (Stretch) when it covers the frame's whole width and is a screen
//    effect: untextured (fades, letterbox bars, darkening), reading a render target (bloom, glow, the
//    frame's own copies, which are as wide), or a see-through band (menu panels);
//  - in the front end, an opaque picture across the width and most of the height (the menus' background
//    movie) covers the whole width (Cover: grown to the width about the middle, cut at the top and bottom);
//  - kept in the middle (Centre) when it is any other picture covering the width (loading screens, movies),
//    when it is part of the 3D scene (a sprite the game projected itself: depth-tested, with depth and
//    perspective), in menus (the front end, the pause menu: laid out for the 4:3 frame as a whole), or before
//    the frame has drawn its 3D scene;
//  - otherwise a HUD piece: kept at the left or the right edge when its group starts near that side of the
//    frame (and does not reach across to the other), in the middle otherwise (Settings::hudAnchor).
// A band that is not spread (colour, or a wide see-through panel, ending at the frame's edge: ReachesEdges) has
// its vertices on the frame's edges taken to the screen's edges, so menu panels reach across the whole width;
// pictures placed at the edge (level shots, arrows, planets) keep their shape.
// HUD pieces are judged together with what they touch: the last frame's pieces are merged into groups
// wherever their boxes meet (a label and its band, a portrait and its bars), and a piece inside a group
// moves with the whole group, so the parts of one element are never torn apart.

// How many 3D draws a frame must have had before its screen-space draws count as a HUD over it.
constexpr DWORD kWorldDrawsForHud = 16;
// A HUD group belongs to a side when it starts within this fraction of the frame's width from that side...
constexpr float kHudEdge = 0.2f;
// ...and does not reach further than this fraction of the width from it (else it spans the screen: middle).
constexpr float kHudReach = 0.75f;
// HUD pieces this close (Xbox pixels) belong to one group.
constexpr float kHudGap = 6.0f;
// In the front end an opaque picture across the width and at least this much of the height (the background
// movie, letterboxed in the 4:3 frame or not) covers the width.
constexpr float kCoverHeight = 0.5f;

struct ScreenBounds {
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, maxZ = 0.0f;
    bool perspective = false; // some vertex has rhw other than 1 (projected by the game itself)
    bool any = false;
};

static void AddBounds(ScreenBounds& b, const uint8_t* data, UINT stride, UINT offset, int components,
    const uint32_t* indices, size_t count)
{
    if (!data)
        return;
    for (size_t i = 0; i < count; ++i) {
        const float* p = reinterpret_cast<const float*>(data + size_t(indices[i]) * stride + offset);
        b.minX = std::min(b.minX, p[0]);
        b.maxX = std::max(b.maxX, p[0]);
        b.minY = std::min(b.minY, p[1]);
        b.maxY = std::max(b.maxY, p[1]);
        if (components >= 3)
            b.maxZ = std::max(b.maxZ, p[2]);
        if (components >= 4 && std::fabs(p[3] - 1.0f) > 1e-3f)
            b.perspective = true;
        b.any = true;
    }
}

// What a screen-space draw shows: plain colour (no texture), a render target (a screen effect), or a picture.
enum class Look { Colour, Target, Picture };

static Look DrawLook()
{
    bool textured = false;
    for (int i = 0; i < 4; ++i) {
        const BaseTexture* t = State().textures[i];
        if (!t)
            continue;
        textured = true;
        if (IsRenderTargetData(t->Data) || IsWideData(t->Data))
            return Look::Target;
    }
    return textured ? Look::Picture : Look::Colour;
}

// A whole-width draw that is a screen effect rather than a picture: colour, a render target, or see-through.
static bool ScreenEffect()
{
    return DrawLook() != Look::Picture || XboxRenderStates()[RS_ALPHABLENDENABLE];
}

// A draw that stops at the frame's edge and is a band of the screen, not a picture placed there: colour or a
// render target, or a see-through picture that is a tile of a band. The game draws its wide pictures (menu
// panels, the loading screen's) in tiles of up to 256 pixels side by side, so a band is a row of see-through
// pictures of the same height, touching one another, that spans the frame from edge to edge; a picture placed at
// the edge (a level shot, an arrow, a planet, a character's full-body picture) has no such row. Rows are found
// from the last frame's see-through pictures (GroupHudPieces).
struct BandBox {
    float x0, x1, y0, y1;
};
static std::vector<BandBox> g_BandPieces; // this frame's see-through pictures
static std::vector<BandBox> g_BandRows;   // the last frame's rows spanning the frame
// The loading screen keeps its 4:3 shape with black at the sides: while it is up (game/widescreen.cpp), nothing
// reaches or continues to the edges. Found from its picture too (one 640x256 band in three tiles, 256, 256, 128:
// game/devmap.cpp), on a frame after one that drew it, should the game's own code not be as expected.
// A frame that draws the level is not the loading screen, whatever the game's state says: started from the
// missions menu, a level boots afresh and its loading screen is handed on by the game (0x215780) and not let go
// while the level runs, which kept the HUD in the middle for the whole mission.
static bool g_LoadingPicture = false;
static bool LoadingScreen()
{
    return (g_LoadingPicture || game::LoadingScreenUp()) && g_Stats.world < kWorldDrawsForHud;
}
constexpr float kLoadingPictureHeight = 256.0f;
// Tiles of one row: tops and bottoms this close, sides this close (Xbox pixels).
constexpr float kBandAlign = 1.0f;
constexpr float kBandJoin = 2.0f;

static void RowBandPieces()
{
    std::vector<BandBox> rows = std::move(g_BandPieces);
    g_BandPieces.clear();
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < rows.size() && !merged; ++i)
            for (size_t k = i + 1; k < rows.size(); ++k) {
                BandBox& a = rows[i];
                const BandBox& b = rows[k];
                if (std::fabs(a.y0 - b.y0) > kBandAlign || std::fabs(a.y1 - b.y1) > kBandAlign ||
                    a.x0 - kBandJoin > b.x1 || b.x0 - kBandJoin > a.x1)
                    continue;
                a = { std::min(a.x0, b.x0), std::max(a.x1, b.x1), std::min(a.y0, b.y0), std::max(a.y1, b.y1) };
                rows.erase(rows.begin() + ptrdiff_t(k));
                merged = true;
                break;
            }
    }
    g_BandRows.clear();
    g_LoadingPicture = false;
    for (const BandBox& r : rows)
        if (r.x0 <= 1.0f && r.x1 >= 639.0f) {
            g_BandRows.push_back(r);
            if (std::fabs(r.y1 - r.y0 - kLoadingPictureHeight) <= 2.0f)
                g_LoadingPicture = true;
        }
}

// Plain colour and render targets (fades, bars, panels of colour) ending at the frame's edge reach the screen's:
// their vertices on the edge are taken there (stretching colour changes nothing).
static bool ReachesEdges(const SurfaceMap& map, Placement placement, const ScreenBounds& b)
{
    if (!map.wide || !b.any || placement == Placement::Stretch || placement == Placement::Cover || LoadingScreen())
        return false;
    return DrawLook() != Look::Picture;
}

// A see-through picture is not stretched: it is continued to the screen's edge with its own edge, a copy of it
// from the frame's edge to the screen's where every vertex off the edge takes the texture coordinates and colours
// of the edge vertex at its height (the edge's column repeated; a cut-out with a clear edge adds nothing visible).
// Continued: the tiles at the ends of a band (a row spanning the frame: both sides), and in the front end a
// picture whose left side is the frame's left edge (a menu's panel). Sides as a mask: 1 left, 2 right.
constexpr int kLeftSide = 1, kRightSide = 2;

static int ContinuedSides(const SurfaceMap& map, Placement placement, bool toEdges, const ScreenBounds& b)
{
    if (!map.wide || !b.any || DrawLook() != Look::Picture || !XboxRenderStates()[RS_ALPHABLENDENABLE])
        return 0;
    const BandBox box = { b.minX, b.maxX, b.minY, b.maxY };
    if (map.xboxWidth == 640)
        g_BandPieces.push_back(box); // noted wherever placed: the rows (and the loading screen) are found from them
    if (placement != Placement::Centre || toEdges || LoadingScreen())
        return 0;
    const float width = float(map.xboxWidth);
    const int touching = (std::fabs(box.x0) <= 1.0f ? kLeftSide : 0) | (std::fabs(box.x1 - width) <= 1.0f ? kRightSide : 0);
    if (!touching)
        return 0;
    int sides = game::InShell() ? touching & kLeftSide : 0;
    for (const BandBox& r : g_BandRows)
        if (std::fabs(box.y0 - r.y0) <= kBandAlign && std::fabs(box.y1 - r.y1) <= kBandAlign &&
            box.x0 >= r.x0 - kBandJoin && box.x1 <= r.x1 + kBandJoin) {
            sides = touching;
            break;
        }
    if (g_DrawLog && sides)
        LOG_INFO("  widescreen: continued to the %s", sides == (kLeftSide | kRightSide) ? "left and right edges"
                                                     : sides == kLeftSide                 ? "left edge"
                                                                                          : "right edge");
    return sides;
}

// Where a draw's Xbox positions are: vertex v's x and y at base + v * stride + offset.
struct SourcePositions {
    const uint8_t* base;
    UINT stride, offset;
    const float* At(UINT v) const { return reinterpret_cast<const float*>(base + size_t(v) * stride + offset); }
};

// The continuation of a side from the draw's placed vertices (`placed`, `count` of `stride` bytes, host positions
// first) and its Xbox positions; `vertices` are the referenced ones (indices into both). False when a vertex off
// the edge has no edge vertex at its height (not a rectangle's side).
static bool BuildContinuation(int side, const SurfaceMap& map, const uint8_t* placed, UINT count, UINT stride,
    const SourcePositions& source, const std::vector<uint32_t>& vertices, UINT lo, std::vector<uint8_t>& out)
{
    const float width = float(map.xboxWidth);
    auto onEdge = [&](const float* xy) { return side == kLeftSide ? xy[0] <= 1.0f : xy[0] >= width - 1.0f; };
    const float screenEdge = side == kLeftSide ? -0.5f : float(map.hostWidth) - 0.5f;
    out.assign(placed, placed + size_t(count) * stride);
    for (uint32_t index : vertices) {
        const UINT v = index - lo;
        const float* xy = source.At(v);
        if (onEdge(xy))
            continue;
        const uint8_t* edge = nullptr;
        for (uint32_t other : vertices) {
            const float* oxy = source.At(other - lo);
            if (onEdge(oxy) && std::fabs(oxy[1] - xy[1]) < 0.5f) {
                edge = placed + size_t(other - lo) * stride;
                break;
            }
        }
        if (!edge)
            return false;
        uint8_t* dst = out.data() + size_t(v) * stride;
        std::memcpy(dst, edge, stride);
        reinterpret_cast<float*>(dst)[0] = screenEdge;
    }
    return true;
}

static const char* PlacementName(Placement p)
{
    switch (p) {
    case Placement::Stretch: return "spread over the width";
    case Placement::Left: return "at the left edge";
    case Placement::Right: return "at the right edge";
    case Placement::Cover: return "covering the width";
    case Placement::Centre: break;
    }
    return "in the middle";
}

struct HudBox {
    float x0, x1, y0, y1;
};
struct HudGroup {
    HudBox box;
    Placement placement;
};
static std::vector<HudBox> g_HudPieces;   // this frame's HUD pieces
static std::vector<HudGroup> g_HudGroups; // the last frame's, merged

static Placement SideOf(const HudBox& b, float width)
{
    if (b.x0 <= width * kHudEdge && b.x1 <= width * kHudReach)
        return Placement::Left;
    if (b.x1 >= width * (1.0f - kHudEdge) && b.x0 >= width * (1.0f - kHudReach))
        return Placement::Right;
    return Placement::Centre;
}

// End of a frame: merges its HUD pieces into groups for the next frame's placement.
static void GroupHudPieces()
{
    std::vector<HudBox> boxes = std::move(g_HudPieces);
    g_HudPieces.clear();
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < boxes.size() && !merged; ++i)
            for (size_t k = i + 1; k < boxes.size(); ++k) {
                HudBox& a = boxes[i];
                const HudBox& b = boxes[k];
                if (a.x0 - kHudGap > b.x1 || b.x0 - kHudGap > a.x1 || a.y0 - kHudGap > b.y1 || b.y0 - kHudGap > a.y1)
                    continue;
                a = { std::min(a.x0, b.x0), std::max(a.x1, b.x1), std::min(a.y0, b.y0), std::max(a.y1, b.y1) };
                boxes.erase(boxes.begin() + ptrdiff_t(k));
                merged = true;
                break;
            }
    }
    g_HudGroups.clear();
    for (const HudBox& b : boxes)
        g_HudGroups.push_back({ b, SideOf(b, 640.0f) });
}

static Placement Decide(const SurfaceMap& map, const ScreenBounds& b);

// Where a screen-space draw goes; in the per-draw log (F11) with its extent, for tuning.
static Placement Place(const SurfaceMap& map, const ScreenBounds& b)
{
    const Placement p = Decide(map, b);
    if (g_DrawLog && map.wide && b.any)
        LOG_INFO("  widescreen: x %.1f-%.1f y %.1f-%.1f z %.3f%s, %s%s, %s", b.minX, b.maxX, b.minY, b.maxY, b.maxZ,
            b.perspective ? " perspective" : "",
            DrawLook() == Look::Colour ? "colour" : DrawLook() == Look::Target ? "render target" : "picture",
            XboxRenderStates()[RS_ALPHABLENDENABLE] ? " see-through" : "", PlacementName(p));
    return p;
}

// Whether a stage reads a wide render target (the frame, or a copy as wide).
static bool ReadsWideTarget()
{
    for (int stage = 0; stage < 4; ++stage) {
        const BaseTexture* t = State().textures[stage];
        if (t && IsWideData(t->Data) && MapFor(t).wide)
            return true;
    }
    return false;
}

static Placement Decide(const SurfaceMap& map, const ScreenBounds& b)
{
    if (!b.any)
        return Placement::Centre;
    const float width = float(map.xboxWidth), height = float(map.xboxHeight);
    if (!map.wide) {
        // A smaller target filled from the wide frame across its whole width (the bloom's first step) takes the
        // whole wide frame: it is spread back over the whole width at the end (Stretch). Taking only the 4:3
        // middle there drew a blurred copy of the scene beside itself, pulled outwards (a ghost beside a
        // character right of the middle).
        return b.minX <= 1.0f && b.maxX >= width - 1.0f && ReadsWideTarget() ? Placement::Stretch : Placement::Centre;
    }
    // The menus: the front end (the shell up and not in a level: the shell stays up in a level, for the pause
    // menu) and the pause menu.
    const bool menus = (game::InShell() && !game::InLevel() && !game::PlayerObject()) || game::GamePaused();
    if (LoadingScreen())
        return Placement::Centre; // its band is still noted (ContinuedSides), to know the next frame is it too
    if (b.minX <= 1.0f && b.maxX >= width - 1.0f) {
        if (ScreenEffect())
            return Placement::Stretch;
        if (game::InShell() && b.maxY - b.minY >= height * kCoverHeight)
            return Placement::Cover;
        return Placement::Centre;
    }
    if (!GetSettings().hudAnchor || g_Stats.world < kWorldDrawsForHud || menus)
        return Placement::Centre;
    const DWORD* rs = XboxRenderStates();
    const bool depthTested = State().depthStencil && rs[RS_ZENABLE] && (rs[RS_ZFUNC] & 0xF) != 7; // not ALWAYS
    if (depthTested && b.maxZ > 1e-4f && b.perspective)
        return Placement::Centre; // a sprite of the 3D scene
    const HudBox box = { b.minX, b.maxX, b.minY, b.maxY };
    if (map.xboxWidth == 640)
        g_HudPieces.push_back(box);
    for (const HudGroup& g : g_HudGroups)
        if (box.x0 >= g.box.x0 - 1.0f && box.x1 <= g.box.x1 + 1.0f && box.y0 >= g.box.y0 - 1.0f &&
            box.y1 <= g.box.y1 + 1.0f)
            return g.placement;
    return SideOf(box, width);
}

// A pre-transformed draw reading a wide render target at screen positions (texel coordinates of the
// game's frame) reads the same place of the host surface: the frame is in its middle there too.
// Returns false when the stage's texture is not wide or the draw is spread over the width anyway.
static bool WideTexcoordMap(int stage, Placement placement, float& scale, float& offset)
{
    const BaseTexture* t = stage >= 0 && stage < 4 ? State().textures[stage] : nullptr;
    if (!t || placement == Placement::Stretch || placement == Placement::Cover || !IsWideData(t->Data))
        return false;
    const SurfaceMap m = MapFor(t);
    if (!m.wide)
        return false;
    scale = float(m.xboxWidth * RenderScale()) / float(m.hostWidth);
    offset = m.centre / float(m.hostWidth);
    return true;
}

// --- Dynamic ring buffers -------------------------------------------------------------------

static IDirect3DVertexBuffer9* g_RingVB = nullptr;
// Vertex declarations of immediate-mode draws (EndFixedFunction), by layout.
static std::unordered_map<uint64_t, IDirect3DVertexDeclaration9*> g_ImmediateDecls;
static IDirect3DIndexBuffer9* g_RingIB = nullptr;
static UINT g_RingVBPos = 0, g_RingIBPos = 0;
static const UINT kRingVBSize = 32 << 20, kRingIBSize = 4 << 20;

static void ReleaseLayouts();

void ReleaseDrawResources()
{
    ReleaseLayouts();
    // Device objects must go with their device: one kept would keep the old device alive and be
    // handed to the next one, which strict drivers (e.g. Parallels') answer with a fatal error.
    for (auto& [key, decl] : g_ImmediateDecls)
        if (decl) decl->Release();
    g_ImmediateDecls.clear();
    ReleaseScreenSpaceDeclarations();
    if (g_RingVB) g_RingVB->Release();
    if (g_RingIB) g_RingIB->Release();
    g_RingVB = nullptr;
    g_RingIB = nullptr;
    g_RingVBPos = g_RingIBPos = 0;
}

static uint8_t* AllocVertices(UINT bytes, UINT alignment, UINT& offset)
{
    if (!g_RingVB)
        Device()->CreateVertexBuffer(kRingVBSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_RingVB, nullptr);
    if (!g_RingVB || bytes > kRingVBSize)
        return nullptr;
    UINT pos = (g_RingVBPos + alignment - 1) / alignment * alignment;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (pos + bytes > kRingVBSize) {
        pos = 0;
        flags = D3DLOCK_DISCARD;
        FlightNote("vertex ring wrapped");
    }
    void* p = nullptr;
    if (FAILED(g_RingVB->Lock(pos, bytes, &p, flags)))
        return nullptr;
    offset = pos;
    g_RingVBPos = pos + bytes;
    return static_cast<uint8_t*>(p);
}

// Makes sure the next `bytes` (allocated in several pieces) fit without the
// ring wrapping in between. A wrap discards the buffer, so vertex data already
// written for the same draw -- e.g. an earlier stream -- would be lost and the
// draw would read garbage (a model vanishing or stretching for one frame).
static void ReserveVertices(UINT bytes)
{
    if (g_RingVBPos + bytes > kRingVBSize)
        g_RingVBPos = kRingVBSize; // the next allocation starts a new ring
}

static bool UploadIndices(const std::vector<uint16_t>& indices, UINT& startIndex)
{
    if (!g_RingIB)
        Device()->CreateIndexBuffer(kRingIBSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
            &g_RingIB, nullptr);
    UINT bytes = UINT(indices.size() * 2);
    if (!g_RingIB || bytes > kRingIBSize)
        return false;
    UINT pos = g_RingIBPos;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (pos + bytes > kRingIBSize) {
        pos = 0;
        flags = D3DLOCK_DISCARD;
    }
    void* p = nullptr;
    if (FAILED(g_RingIB->Lock(pos, bytes, &p, flags)))
        return false;
    std::memcpy(p, indices.data(), bytes);
    g_RingIB->Unlock();
    g_RingIBPos = pos + bytes;
    startIndex = pos / 2;
    return true;
}

// --- Vertex layouts ---------------------------------------------------------------------------

struct Element {
    BYTE reg;        // Xbox vertex register
    DWORD xboxType;  // X_D3DVSDT_*
    DWORD xboxOffset;
    BYTE hostType;   // D3DDECLTYPE_*
    WORD hostOffset;
    bool convert;
};

struct StreamLayout {
    DWORD xboxStream;
    std::vector<Element> elements;
    WORD hostStride; // only used when converting
    bool convert;
};

struct Layout {
    IDirect3DVertexDeclaration9* decl = nullptr;
    std::vector<StreamLayout> streams;
    DWORD present[16] = {};
};

static void HostType(DWORD xt, BYTE& type, WORD& size, bool& convert)
{
    convert = false;
    switch (xt) {
    case VSDT_FLOAT1: type = D3DDECLTYPE_FLOAT1; size = 4; return;
    case VSDT_FLOAT2: type = D3DDECLTYPE_FLOAT2; size = 8; return;
    case VSDT_FLOAT3: type = D3DDECLTYPE_FLOAT3; size = 12; return;
    case VSDT_FLOAT4: type = D3DDECLTYPE_FLOAT4; size = 16; return;
    case VSDT_D3DCOLOR: type = D3DDECLTYPE_D3DCOLOR; size = 4; return;
    case VSDT_SHORT2: type = D3DDECLTYPE_SHORT2; size = 4; return;
    case VSDT_SHORT4: type = D3DDECLTYPE_SHORT4; size = 8; return;
    case VSDT_NORMSHORT2: type = D3DDECLTYPE_SHORT2N; size = 4; return;
    case VSDT_NORMSHORT4: type = D3DDECLTYPE_SHORT4N; size = 8; return;
    case VSDT_PBYTE4: type = D3DDECLTYPE_UBYTE4N; size = 4; return;
    }
    type = D3DDECLTYPE_FLOAT4;
    size = 16;
    convert = true;
}

static UINT XboxTypeSize(DWORD xt)
{
    switch (xt) {
    case VSDT_NORMSHORT1: case VSDT_SHORT1: return 2;
    case VSDT_NORMSHORT3: case VSDT_SHORT3: return 6;
    case VSDT_PBYTE1: return 1;
    case VSDT_PBYTE2: return 2;
    case VSDT_PBYTE3: return 3;
    case VSDT_NORMPACKED3: return 4;
    case VSDT_FLOAT2H: return 12;
    }
    return 0;
}

// Converts one Xbox-only element to float4.
static void ConvertElement(DWORD xt, const uint8_t* src, float* d)
{
    d[0] = d[1] = d[2] = 0.0f;
    d[3] = 1.0f;
    auto s16 = [&](int i) { return float(reinterpret_cast<const int16_t*>(src)[i]); };
    switch (xt) {
    case VSDT_NORMSHORT1: d[0] = s16(0) / 32767.0f; break;
    case VSDT_NORMSHORT3: for (int i = 0; i < 3; ++i) d[i] = s16(i) / 32767.0f; break;
    case VSDT_SHORT1: d[0] = s16(0); break;
    case VSDT_SHORT3: for (int i = 0; i < 3; ++i) d[i] = s16(i); break;
    case VSDT_PBYTE1: d[0] = src[0] / 255.0f; break;
    case VSDT_PBYTE2: d[0] = src[0] / 255.0f; d[1] = src[1] / 255.0f; break;
    case VSDT_PBYTE3: for (int i = 0; i < 3; ++i) d[i] = src[i] / 255.0f; break;
    case VSDT_NORMPACKED3: {
        uint32_t v = *reinterpret_cast<const uint32_t*>(src);
        int x = int(v << 21) >> 21, y = int((v >> 11) << 21) >> 21, z = int(v) >> 22;
        d[0] = x / 1023.0f;
        d[1] = y / 1023.0f;
        d[2] = z / 511.0f;
        break;
    }
    case VSDT_FLOAT2H: {
        const float* f = reinterpret_cast<const float*>(src);
        d[0] = f[0];
        d[1] = f[1];
        d[2] = 0.0f;
        d[3] = f[2];
        break;
    }
    }
}

static D3DDECLUSAGE FixedFunctionUsage(int reg, BYTE& index, bool passthrough)
{
    index = 0;
    switch (reg) {
    case VSDE_POSITION: return passthrough ? D3DDECLUSAGE_POSITIONT : D3DDECLUSAGE_POSITION;
    case VSDE_BLENDWEIGHT: return D3DDECLUSAGE_BLENDWEIGHT;
    case VSDE_NORMAL: return D3DDECLUSAGE_NORMAL;
    case VSDE_DIFFUSE: return D3DDECLUSAGE_COLOR;
    case VSDE_SPECULAR: index = 1; return D3DDECLUSAGE_COLOR;
    case VSDE_FOG: return D3DDECLUSAGE_FOG;
    case VSDE_POINTSIZE: return D3DDECLUSAGE_PSIZE;
    case VSDE_BACKDIFFUSE: index = 2; return D3DDECLUSAGE_COLOR;
    case VSDE_BACKSPECULAR: index = 3; return D3DDECLUSAGE_COLOR;
    default: index = BYTE(reg - VSDE_TEXCOORD0); return D3DDECLUSAGE_TEXCOORD;
    }
}

static std::unordered_map<uint64_t, Layout> s_Layouts;

static const Layout* LayoutFor(const VertexAttributeFormat& attrs, VertexMode mode)
{
    struct Key {
        VertexAttributeFormat attrs;
        int mode;
    } key = { attrs, int(mode) };
    uint64_t hash = Hash64(&key, sizeof(key));
    auto it = s_Layouts.find(hash);
    if (it != s_Layouts.end())
        return &it->second;

    Layout& L = s_Layouts[hash];
    for (int r = 0; r < 16; ++r) {
        const VertexShaderInput& s = attrs.Slots[r];
        if ((s.Format & 0xFF) == VSDT_NONE || s.Format == 0)
            continue;
        StreamLayout* sl = nullptr;
        for (StreamLayout& x : L.streams)
            if (x.xboxStream == s.StreamIndex)
                sl = &x;
        if (!sl) {
            L.streams.push_back({ s.StreamIndex, {}, 0, false });
            sl = &L.streams.back();
        }
        Element e;
        e.reg = BYTE(r);
        e.xboxType = s.Format & 0xFF;
        e.xboxOffset = s.Offset;
        WORD size;
        HostType(e.xboxType, e.hostType, size, e.convert);
        sl->elements.push_back(e);
        sl->convert |= e.convert;
        L.present[r] = 1;
    }

    std::vector<D3DVERTEXELEMENT9> decl;
    for (StreamLayout& sl : L.streams) {
        std::sort(sl.elements.begin(), sl.elements.end(), [](const Element& a, const Element& b) { return a.xboxOffset < b.xboxOffset; });
        WORD hostOffset = 0;
        for (Element& e : sl.elements) {
            BYTE t;
            WORD size;
            bool c;
            HostType(e.xboxType, t, size, c);
            if (sl.convert) {
                e.hostOffset = hostOffset;
                hostOffset = WORD(hostOffset + size);
            } else {
                e.hostOffset = WORD(e.xboxOffset);
            }
            D3DVERTEXELEMENT9 ve = { WORD(sl.xboxStream), e.hostOffset, e.hostType, D3DDECLMETHOD_DEFAULT, 0, 0 };
            if (mode == VertexMode::Program) {
                ve.Usage = D3DDECLUSAGE_TEXCOORD;
                ve.UsageIndex = e.reg;
            } else {
                ve.Usage = BYTE(FixedFunctionUsage(e.reg, ve.UsageIndex, mode == VertexMode::Passthrough));
            }
            decl.push_back(ve);
        }
        sl.hostStride = hostOffset;
    }
    std::sort(decl.begin(), decl.end(), [](const D3DVERTEXELEMENT9& a, const D3DVERTEXELEMENT9& b) {
        return a.Stream != b.Stream ? a.Stream < b.Stream : a.Offset < b.Offset;
    });
    decl.push_back(D3DDECL_END());
    if (FAILED(Device()->CreateVertexDeclaration(decl.data(), &L.decl)))
        LOG_WARN("CreateVertexDeclaration failed (%zu elements)", decl.size() - 1);
    return &L;
}

static void ReleaseLayouts()
{
    for (auto& [k, l] : s_Layouts)
        if (l.decl)
            l.decl->Release();
    s_Layouts.clear();
}

// --- Primitives ---------------------------------------------------------------------------------

static bool Convert(DWORD xprim, const std::vector<uint32_t>& in, D3DPRIMITIVETYPE& type, std::vector<uint32_t>& out, UINT& prims)
{
    size_t n = in.size();
    out.clear();
    switch (xprim) {
    case PT_POINTLIST: type = D3DPT_POINTLIST; out = in; prims = UINT(n); break;
    case PT_LINELIST: type = D3DPT_LINELIST; out = in; prims = UINT(n / 2); break;
    case PT_LINESTRIP: type = D3DPT_LINESTRIP; out = in; prims = n ? UINT(n - 1) : 0; break;
    case PT_LINELOOP:
        type = D3DPT_LINESTRIP;
        out = in;
        if (n) out.push_back(in[0]);
        prims = UINT(n);
        break;
    case PT_TRIANGLELIST: type = D3DPT_TRIANGLELIST; out = in; prims = UINT(n / 3); break;
    case PT_TRIANGLESTRIP:
    case PT_QUADSTRIP: type = D3DPT_TRIANGLESTRIP; out = in; prims = n >= 3 ? UINT(n - 2) : 0; break;
    case PT_TRIANGLEFAN:
    case PT_POLYGON: type = D3DPT_TRIANGLEFAN; out = in; prims = n >= 3 ? UINT(n - 2) : 0; break;
    case PT_QUADLIST:
        type = D3DPT_TRIANGLELIST;
        for (size_t q = 0; q + 3 < n; q += 4) {
            uint32_t a = in[q], b = in[q + 1], c = in[q + 2], d = in[q + 3];
            out.insert(out.end(), { a, b, c, a, c, d });
        }
        prims = UINT(out.size() / 3);
        break;
    default:
        return false;
    }
    return prims > 0;
}

// --- Draw ---------------------------------------------------------------------------------------

struct StreamSource {
    const uint8_t* data;
    UINT stride;
};

bool IsRenderTargetData(DWORD data);

// The game's full-screen bloom ends with a screen-space quad that samples one
// of its render targets and adds it onto the back buffer (ONE/ONE blending).
static bool IsBloomComposite(VertexMode mode)
{
    const XboxState& st = State();
    const DWORD* rs = XboxRenderStates();
    return mode == VertexMode::Fvf && (st.vertexShader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW &&
           st.renderTarget == st.backBuffer && st.textures[0] && IsRenderTargetData(st.textures[0]->Data) &&
           rs[RS_ALPHABLENDENABLE] && rs[RS_SRCBLEND] == 1 && rs[RS_DESTBLEND] == 1;
}

// Draws the continuations built for a draw, with its indices (already set) and vertex stride.
// A continued picture's own edge columns, with a wrapping texture, blend in the opposite side's (a line of a pixel
// between the picture and its continuation): when its coordinates stay within the texture (u 0 to 1, no tiling),
// it is drawn clamped across too. `offsets` are its texture coordinates' places in a vertex.
static bool WithinTextureAcross(const uint8_t* placed, UINT stride, const std::vector<WORD>& offsets,
    const std::vector<uint32_t>& vertices, UINT lo)
{
    for (uint32_t index : vertices)
        for (WORD offset : offsets) {
            const float u = *reinterpret_cast<const float*>(placed + size_t(index - lo) * stride + offset);
            if (u < -0.001f || u > 1.001f)
                return false;
        }
    return true;
}

// Clamps the texture across on every stage while it lives (the game's addressing put back after).
class ClampAcross {
public:
    explicit ClampAcross(bool on) : m_on(on)
    {
        if (!m_on)
            return;
        for (DWORD stage = 0; stage < 4; ++stage) {
            Device()->GetSamplerState(stage, D3DSAMP_ADDRESSU, &m_address[stage]);
            Device()->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        }
    }
    ~ClampAcross()
    {
        if (m_on)
            for (DWORD stage = 0; stage < 4; ++stage)
                Device()->SetSamplerState(stage, D3DSAMP_ADDRESSU, m_address[stage]);
    }
    ClampAcross(const ClampAcross&) = delete;
    ClampAcross& operator=(const ClampAcross&) = delete;

private:
    bool m_on;
    DWORD m_address[4] = {};
};

// A continuation's triangles are the picture's folded over its edge, so they face the other way: drawn without
// culling, and with the texture clamped across (below).
static void DrawContinuations(const std::vector<std::vector<uint8_t>>& continuations, UINT stride, D3DPRIMITIVETYPE type,
    UINT count, UINT startIndex, UINT prims)
{
    if (continuations.empty())
        return;
    IDirect3DDevice9* dev = Device();
    DWORD cull = D3DCULL_NONE;
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    // The edge's own column, exactly: sampled at the picture's edge (u 0 or 1), a wrapping texture would blend in
    // the opposite side's column (a gradient's other end); clamped, the edge column is repeated as it is.
    const ClampAcross clamp(true);
    for (const std::vector<uint8_t>& c : continuations) {
        UINT offset;
        uint8_t* dst = AllocVertices(UINT(c.size()), stride, offset);
        if (!dst)
            break;
        std::memcpy(dst, c.data(), c.size());
        g_RingVB->Unlock();
        dev->SetStreamSource(0, g_RingVB, offset, stride);
        dev->DrawIndexedPrimitive(type, 0, 0, count, startIndex, prims);
    }
    dev->SetRenderState(D3DRS_CULLMODE, cull);
}

static void DrawVertices(DWORD xprim, const std::vector<uint32_t>& vertices, const StreamSource* sources)
{
    ++g_Stats.draws;
    VertexMode mode = CurrentVertexMode();
    {
        uint32_t first = 0, last = 0;
        if (!vertices.empty()) {
            auto [lo, hi] = std::minmax_element(vertices.begin(), vertices.end());
            first = *lo;
            last = *hi;
        }
        FlightRecordDraw(xprim, UINT(vertices.size()), first, int(mode), sources[0].data, sources[0].stride,
            vertices.data(), last);
    }
    if (!GetSettings().bloom && IsBloomComposite(mode))
        return Skipped(); // player setting: bloom off
    if (mode == VertexMode::Program) ++g_Stats.programmable; else ++g_Stats.fixed;
    const SurfaceMap targetMap = MapFor(State().renderTarget);
    if (mode == VertexMode::Program && targetMap.wide && State().depthStencil && XboxRenderStates()[RS_ZENABLE])
        ++g_Stats.world;
    if (State().pixelShader) ++g_Stats.pixelShader;

    D3DPRIMITIVETYPE type;
    std::vector<uint32_t> seq;
    UINT prims;
    if (vertices.empty() || !Convert(xprim, vertices, type, seq, prims)) {
        Skipped();
        return;
    }
    uint32_t lo = *std::min_element(seq.begin(), seq.end()), hi = *std::max_element(seq.begin(), seq.end());
    UINT count = hi - lo + 1;
    if (count > 0xFFFF) {
        Skipped();
        return;
    }

    IDirect3DDevice9* dev = Device();
    const Layout* layout = nullptr;
    DWORD fvfPresent[16] = {};
    bool pretransformed = false;
    if (mode == VertexMode::Fvf) {
        dev->SetFVF(State().vertexShader);
        for (int i = 0; i < 16; ++i) fvfPresent[i] = 1;
        pretransformed = (State().vertexShader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    } else {
        const VertexShader* vs = CurrentXboxVertexShader();
        if (!vs) {
            Skipped();
            return;
        }
        layout = LayoutFor(vs->VertexAttribute, mode);
        if (!layout->decl) {
            Skipped();
            return;
        }
    }

    if (g_DrawLog) {
        const DWORD* rs = XboxRenderStates();
        const Viewport& vp = State().viewport;
        const float* c = &State().vertexConstants[0][0];
        const float* p0 = sources[0].data ? reinterpret_cast<const float*>(sources[0].data + size_t(lo) * sources[0].stride) : nullptr;
        LOG_INFO("draw %lu: prim %lu n %zu mode %d vs %08lX ps %p rt %08lX ds %08lX vp %lu,%lu %lux%lu z %.2f-%.2f "
                 "zen %lu zw %lu zf %lX ab %lu %lX/%lX at %lu cw %lX cull %lX st %lu fog %lu tex %08lX %08lX %08lX %08lX "
                 "c58 %g %g %g %g c59 %g %g %g %g v0 %g %g %g stride %u",
            g_Stats.draws, xprim, vertices.size(), int(mode), State().vertexShader, State().pixelShader,
            State().renderTarget ? State().renderTarget->Data : 0, State().depthStencil ? State().depthStencil->Data : 0,
            vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, rs[RS_ZENABLE], rs[RS_ZWRITEENABLE], rs[RS_ZFUNC],
            rs[RS_ALPHABLENDENABLE], rs[RS_SRCBLEND], rs[RS_DESTBLEND], rs[RS_ALPHATESTENABLE], rs[RS_COLORWRITEENABLE],
            rs[RS_CULLMODE], rs[RS_STENCILENABLE], rs[RS_FOGENABLE],
            State().textures[0] ? State().textures[0]->Data : 0, State().textures[1] ? State().textures[1]->Data : 0,
            State().textures[2] ? State().textures[2]->Data : 0, State().textures[3] ? State().textures[3]->Data : 0,
            c[58 * 4], c[58 * 4 + 1], c[58 * 4 + 2], c[58 * 4 + 3], c[59 * 4], c[59 * 4 + 1], c[59 * 4 + 2], c[59 * 4 + 3],
            p0 ? p0[0] : 0.0f, p0 ? p0[1] : 0.0f, p0 ? p0[2] : 0.0f, sources[0].stride);
        if (mode == VertexMode::Fvf) {
            const DWORD* t0 = XboxTextureStates(0);
            LOG_INFO("  stage0 colorop %lu args %lX %lX %lX alphaop %lu args %lX %lX %lX | cw %08lX srcblend %lu destblend %lu",
                t0[TSS_COLOROP], t0[TSS_COLORARG0], t0[TSS_COLORARG1], t0[TSS_COLORARG2], t0[TSS_ALPHAOP],
                t0[TSS_ALPHAARG0], t0[TSS_ALPHAARG1], t0[TSS_ALPHAARG2], rs[RS_COLORWRITEENABLE], rs[RS_SRCBLEND],
                rs[RS_DESTBLEND]);
            if (BaseTexture* t = State().textures[0]) {
                UINT tw, th, rw, rh;
                XboxSurfaceSize(t, tw, th);
                XboxSurfaceSize(State().renderTarget, rw, rh);
                LOG_INFO("  tex0 %08lX fmt %08lX size %08lX -> %ux%u | rt %ux%u", t->Data, t->Format, t->Size, tw, th, rw, rh);
            }
            LOG_INFO("  tfactor %08lX psc0 %08lX %08lX %08lX %08lX psc1 %08lX %08lX %08lX %08lX", rs[RS_TEXTUREFACTOR],
                rs[RS_PSCONSTANT0_0], rs[RS_PSCONSTANT0_0 + 1], rs[RS_PSCONSTANT0_0 + 2], rs[RS_PSCONSTANT0_0 + 3],
                rs[RS_PSCONSTANT1_0], rs[RS_PSCONSTANT1_0 + 1], rs[RS_PSCONSTANT1_0 + 2], rs[RS_PSCONSTANT1_0 + 3]);
            if (vertices.size() == 4 && sources[0].data && sources[0].stride <= 64) {
                for (uint32_t vi : vertices) {
                    const float* f = reinterpret_cast<const float*>(sources[0].data + size_t(vi) * sources[0].stride);
                    std::string line;
                    char num[24];
                    for (UINT k = 0; k < sources[0].stride / 4; ++k) {
                        snprintf(num, sizeof(num), " %.3f", f[k]);
                        line += num;
                    }
                    LOG_INFO("   v%u:%s", vi, line.c_str());
                }
            }
        }
        if (const VertexShader* vs = mode != VertexMode::Fvf ? CurrentXboxVertexShader() : nullptr) {
            std::string attrs;
            char buf[48];
            for (int i = 0; i < 16; ++i) {
                const VertexShaderInput& in = vs->VertexAttribute.Slots[i];
                if ((in.Format & 0xFF) == 0x02) continue;
                snprintf(buf, sizeof(buf), " v%d:s%lu+%lu/%02lX", i, in.StreamIndex, in.Offset, in.Format);
                attrs += buf;
            }
            auto k = [&](int r) { return &State().vertexConstants[r + 96][0]; };
            LOG_INFO("  attrs%s | c0 %g %g %g c75 %g %g %g c77 %g %g %g c81 %g %g %g c92 %g", attrs.c_str(),
                k(0)[0], k(0)[1], k(0)[2], k(75)[0], k(75)[1], k(75)[2], k(77)[0], k(77)[1], k(77)[2],
                k(81)[0], k(81)[1], k(81)[2], k(92)[0]);
        }
        if (mode == VertexMode::Program && g_Stats.draws > 9 && g_Stats.draws < 11) {
            for (int r = 0; r < 192; ++r) {
                const float* v = &State().vertexConstants[r][0];
                if (v[0] || v[1] || v[2] || v[3])
                    LOG_INFO("  c%d = %g %g %g %g", r - 96, v[0], v[1], v[2], v[3]);
            }
        }
    }

    g_PretransformedDraw = pretransformed || mode == VertexMode::Passthrough;
    if (!ApplyDrawState(layout ? layout->present : fvfPresent)) {
        Skipped();
        return;
    }

    // Upload the referenced vertex range of each stream.
    std::vector<std::vector<uint8_t>> continuations; // true widescreen: pictures continued to the screen's edges
    bool clampMain = false;                          // ...and the picture itself clamped across (WithinTextureAcross)
    if (!layout) {
        const StreamSource& s = sources[0];
        if (!s.data || !s.stride) { Skipped(); return; }
        UINT offset;
        uint8_t* dst = AllocVertices(count * s.stride, s.stride, offset);
        if (!dst) { Skipped(); return; }
        std::memcpy(dst, s.data + size_t(lo) * s.stride, size_t(count) * s.stride);
        if (pretransformed) {
            ScreenBounds bounds;
            AddBounds(bounds, s.data, s.stride, 0, 4, vertices.data(), vertices.size());
            const Placement placement = Place(targetMap, bounds);
            const bool toEdges = ReachesEdges(targetMap, placement, bounds);
            const int sides = ContinuedSides(targetMap, placement, toEdges, bounds);
            ScaleScreenPositions(dst, count, s.stride, 0, targetMap, placement, toEdges);
            WORD offs[8];
            BYTE sizes[8];
            int sets = FvfTexcoords(State().vertexShader, offs, sizes);
            for (int t = 0; t < sets; ++t) {
                int stage = StageForTexcoord(t);
                if (stage < 0) continue;
                const float* sc = TextureScale(stage);
                float wideScale = 1.0f, wideOffset = 0.0f;
                const bool wide = WideTexcoordMap(stage, placement, wideScale, wideOffset);
                if (sc[0] == 1.0f && sc[1] == 1.0f && !wide) continue;
                for (UINT v = 0; v < count; ++v) {
                    float* tc = reinterpret_cast<float*>(dst + size_t(v) * s.stride + offs[t]);
                    tc[0] = tc[0] / sc[0] * wideScale + wideOffset;
                    if (sizes[t] > 1) tc[1] /= sc[1];
                }
            }
            for (int side : { kLeftSide, kRightSide }) {
                std::vector<uint8_t> c;
                if ((sides & side) && BuildContinuation(side, targetMap, dst, count, s.stride,
                                          { s.data + size_t(lo) * s.stride, s.stride, 0 }, vertices, lo, c))
                    continuations.push_back(std::move(c));
            }
            if (!continuations.empty()) {
                std::vector<WORD> texcoords(offs, offs + sets);
                clampMain = WithinTextureAcross(dst, s.stride, texcoords, vertices, lo);
            }
        }
        g_RingVB->Unlock();
        dev->SetStreamSource(0, g_RingVB, offset, s.stride);
    } else {
        dev->SetVertexDeclaration(layout->decl);
        // Pass-through programs take screen positions: placed as pre-transformed vertices are.
        Placement placement = Placement::Centre;
        if (mode == VertexMode::Passthrough && targetMap.wide) {
            ScreenBounds bounds;
            for (const StreamLayout& sl : layout->streams)
                for (const Element& e : sl.elements)
                    if (e.reg == VSDE_POSITION && (e.xboxType == VSDT_FLOAT2 || e.xboxType == VSDT_FLOAT3 ||
                                                   e.xboxType == VSDT_FLOAT4))
                        AddBounds(bounds, sources[sl.xboxStream].data, sources[sl.xboxStream].stride, e.xboxOffset,
                            e.xboxType == VSDT_FLOAT2 ? 2 : e.xboxType == VSDT_FLOAT3 ? 3 : 4, vertices.data(),
                            vertices.size());
            placement = Place(targetMap, bounds);
        }
        UINT needed = 0;
        for (const StreamLayout& sl : layout->streams) {
            UINT stride = sl.convert ? sl.hostStride : sources[sl.xboxStream].stride;
            needed += count * stride + stride; // + alignment padding
        }
        ReserveVertices(needed);
        for (const StreamLayout& sl : layout->streams) {
            const StreamSource& s = sources[sl.xboxStream];
            if (!s.data) { Skipped(); return; }
            UINT stride = sl.convert ? sl.hostStride : s.stride;
            if (!stride) { Skipped(); return; }
            UINT offset;
            uint8_t* dst = AllocVertices(count * stride, stride, offset);
            if (!dst) { Skipped(); return; }
            const uint8_t* src = s.data + size_t(lo) * s.stride;
            uint8_t* const first = dst;
            if (!sl.convert) {
                std::memcpy(dst, src, size_t(count) * s.stride);
            } else {
                for (UINT v = 0; v < count; ++v, src += s.stride, dst += stride) {
                    for (const Element& e : sl.elements) {
                        if (e.convert) {
                            ConvertElement(e.xboxType, src + e.xboxOffset, reinterpret_cast<float*>(dst + e.hostOffset));
                        } else {
                            BYTE t;
                            WORD size;
                            bool c;
                            HostType(e.xboxType, t, size, c);
                            std::memcpy(dst + e.hostOffset, src + e.xboxOffset, size);
                        }
                    }
                }
            }
            if (mode == VertexMode::Passthrough) {
                for (const Element& e : sl.elements) {
                    BYTE t = e.hostType;
                    if (e.reg == VSDE_POSITION && (t == D3DDECLTYPE_FLOAT2 || t == D3DDECLTYPE_FLOAT3 ||
                                                   t == D3DDECLTYPE_FLOAT4))
                        ScaleScreenPositions(first, count, stride, e.hostOffset, targetMap, placement);
                }
            }
            g_RingVB->Unlock();
            dev->SetStreamSource(sl.xboxStream, g_RingVB, offset, stride);
        }
    }

    std::vector<uint16_t> indices(seq.size());
    for (size_t i = 0; i < seq.size(); ++i)
        indices[i] = uint16_t(seq[i] - lo);
    UINT startIndex;
    if (!UploadIndices(indices, startIndex)) {
        Skipped();
        return;
    }
    dev->SetIndices(g_RingIB);
    const ClampAcross clamp(clampMain);
    dev->DrawIndexedPrimitive(type, 0, 0, count, startIndex, prims);
    DrawContinuations(continuations, sources[0].stride, type, count, startIndex, prims);
}

static void StreamsFromState(StreamSource* sources)
{
    for (int i = 0; i < 16; ++i) {
        const auto& s = State().streams[i];
        sources[i] = { s.buffer ? ResourceData(s.buffer) : nullptr, s.stride };
    }
}

static void __stdcall XbDrawVertices(DWORD primitiveType, UINT startVertex, UINT vertexCount)
{
    FlightNoteCaller(_AddressOfReturnAddress());
    std::vector<uint32_t> v(vertexCount);
    for (UINT i = 0; i < vertexCount; ++i)
        v[i] = startVertex + i;
    StreamSource sources[16];
    StreamsFromState(sources);
    DrawVertices(primitiveType, v, sources);
}

static void __stdcall XbDrawIndexedVertices(DWORD primitiveType, UINT vertexCount, const WORD* indexData)
{
    FlightNoteCaller(_AddressOfReturnAddress());

    std::vector<uint32_t> v(vertexCount);
    UINT base = State().baseVertexIndex;
    for (UINT i = 0; i < vertexCount; ++i)
        v[i] = indexData[i] + base;
    StreamSource sources[16];
    StreamsFromState(sources);
    DrawVertices(primitiveType, v, sources);
}

static void __stdcall XbDrawVerticesUP(DWORD primitiveType, UINT vertexCount, const void* vertexData, UINT stride)
{
    FlightNoteCaller(_AddressOfReturnAddress());
    std::vector<uint32_t> v(vertexCount);
    for (UINT i = 0; i < vertexCount; ++i)
        v[i] = i;
    StreamSource sources[16] = {};
    sources[0] = { static_cast<const uint8_t*>(vertexData), stride };
    DrawVertices(primitiveType, v, sources);
}

static void __stdcall XbDrawIndexedVerticesUP(DWORD primitiveType, UINT vertexCount, const void* indexData,
    const void* vertexData, UINT stride)
{
    FlightNoteCaller(_AddressOfReturnAddress());
    std::vector<uint32_t> v(vertexCount);
    for (UINT i = 0; i < vertexCount; ++i)
        v[i] = static_cast<const WORD*>(indexData)[i];
    StreamSource sources[16] = {};
    sources[0] = { static_cast<const uint8_t*>(vertexData), stride };
    DrawVertices(primitiveType, v, sources);
}

// --- Immediate mode -----------------------------------------------------------------------------
// Between Begin and End, SetVertexData* writes vertex registers; writing the
// position register emits a vertex. Outside Begin/End it sets the defaults for
// registers the vertex streams do not supply.

static bool g_InBegin = false;
static DWORD g_BeginPrimitive = 0;
static float g_ImmRegs[16][4];
static DWORD g_ImmUsed = 0;
static std::vector<float> g_ImmVertices; // 16 float4 per vertex

static void SetRegister(int reg, float a, float b, float c, float d)
{
    if (reg < 0)
        reg = VSDE_POSITION;
    if (reg >= 16)
        return;
    if (!g_InBegin) {
        float* v = VertexRegisterDefaults() + reg * 4;
        v[0] = a; v[1] = b; v[2] = c; v[3] = d;
        return;
    }
    g_ImmRegs[reg][0] = a;
    g_ImmRegs[reg][1] = b;
    g_ImmRegs[reg][2] = c;
    g_ImmRegs[reg][3] = d;
    g_ImmUsed |= 1u << reg;
    if (reg == VSDE_POSITION)
        g_ImmVertices.insert(g_ImmVertices.end(), &g_ImmRegs[0][0], &g_ImmRegs[0][0] + 64);
}

// Immediate-mode draw while an FVF is active (used e.g. by the movie player):
// feed the fixed-function pipeline, with colors packed as D3DCOLOR.
static void EndFixedFunction(UINT n)
{
    DWORD fvf = State().vertexShader;
    bool pretransformed = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    std::vector<D3DVERTEXELEMENT9> decl;
    struct Field {
        int reg;
        bool color;
        WORD offset;
    };
    std::vector<Field> fields;
    WORD stride = 0;
    for (int r = 0; r < 16; ++r) {
        if (!(g_ImmUsed & (1u << r)) && r != VSDE_POSITION)
            continue;
        BYTE index;
        D3DDECLUSAGE usage = FixedFunctionUsage(r, index, pretransformed);
        bool color = usage == D3DDECLUSAGE_COLOR;
        D3DVERTEXELEMENT9 e = { 0, stride, BYTE(color ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_FLOAT4), D3DDECLMETHOD_DEFAULT,
            BYTE(usage), index };
        if (usage == D3DDECLUSAGE_POSITION)
            e.Type = D3DDECLTYPE_FLOAT3;
        decl.push_back(e);
        fields.push_back({ r, color, stride });
        stride = WORD(stride + (color ? 4 : e.Type == D3DDECLTYPE_FLOAT3 ? 12 : 16));
    }
    decl.push_back(D3DDECL_END());

    uint64_t key = Hash64(decl.data(), decl.size() * sizeof(D3DVERTEXELEMENT9));
    IDirect3DVertexDeclaration9*& vd = g_ImmediateDecls[key];
    if (!vd && FAILED(Device()->CreateVertexDeclaration(decl.data(), &vd)))
        return;

    std::vector<uint32_t> v(n), seq;
    for (UINT i = 0; i < n; ++i)
        v[i] = i;
    D3DPRIMITIVETYPE type;
    UINT prims;
    DWORD present[16] = {};
    g_PretransformedDraw = pretransformed;
    if (!Convert(g_BeginPrimitive, v, type, seq, prims) || !ApplyDrawState(present)) {
        Skipped();
        return;
    }
    const SurfaceMap map = MapFor(State().renderTarget);
    Placement placement = Placement::Centre;
    bool toEdges = false;
    int sides = 0;
    float wideScale[4] = { 1, 1, 1, 1 }, wideOffset[4] = {};
    if (pretransformed && map.wide) {
        ScreenBounds bounds;
        AddBounds(bounds, reinterpret_cast<const uint8_t*>(g_ImmVertices.data()), 256, VSDE_POSITION * 16, 4, v.data(),
            v.size());
        placement = Place(map, bounds);
        toEdges = ReachesEdges(map, placement, bounds);
        sides = ContinuedSides(map, placement, toEdges, bounds);
        for (int stage = 0; stage < 4; ++stage)
            WideTexcoordMap(stage, placement, wideScale[stage], wideOffset[stage]);
    }
    UINT vbOffset;
    uint8_t* dst = AllocVertices(n * stride, stride, vbOffset);
    if (!dst)
        return;
    for (UINT i = 0; i < n; ++i) {
        const float* regs = &g_ImmVertices[size_t(i) * 64];
        for (const Field& f : fields) {
            const float* c = regs + f.reg * 4;
            uint8_t* out = dst + size_t(i) * stride + f.offset;
            if (f.color) {
                auto b = [](float x) { return DWORD(x <= 0 ? 0 : x >= 1 ? 255 : x * 255 + 0.5f); };
                *reinterpret_cast<DWORD*>(out) = D3DCOLOR_ARGB(b(c[3]), b(c[0]), b(c[1]), b(c[2]));
            } else {
                std::memcpy(out, c, f.reg == VSDE_POSITION && !pretransformed ? 12 : 16);
                if (f.reg == VSDE_POSITION && pretransformed)
                    PlaceScreenPosition(reinterpret_cast<float*>(out), map, placement, toEdges);
                int stage = f.reg >= VSDE_TEXCOORD0 ? StageForTexcoord(f.reg - VSDE_TEXCOORD0) : -1;
                if (pretransformed && stage >= 0) {
                    const float* sc = TextureScale(stage);
                    float* tc = reinterpret_cast<float*>(out);
                    tc[0] = tc[0] / sc[0] * wideScale[stage] + wideOffset[stage];
                    tc[1] /= sc[1];
                }
            }
        }
    }
    // Continuations (ContinuedSides) need the position first in the vertex.
    std::vector<std::vector<uint8_t>> continuations;
    const bool positionFirst = !fields.empty() && fields.front().reg == VSDE_POSITION && fields.front().offset == 0;
    for (int side : { kLeftSide, kRightSide }) {
        std::vector<uint8_t> c;
        if ((sides & side) && positionFirst &&
            BuildContinuation(side, map, dst, n, stride,
                { reinterpret_cast<const uint8_t*>(g_ImmVertices.data()), 256, VSDE_POSITION * 16 }, v, 0, c))
            continuations.push_back(std::move(c));
    }
    bool clampMain = false;
    if (!continuations.empty()) {
        std::vector<WORD> texcoords;
        for (const Field& f : fields)
            if (!f.color && f.reg >= VSDE_TEXCOORD0)
                texcoords.push_back(f.offset);
        clampMain = WithinTextureAcross(dst, stride, texcoords, v, 0);
    }
    g_RingVB->Unlock();
    IDirect3DDevice9* dev = Device();
    dev->SetVertexDeclaration(vd);
    dev->SetStreamSource(0, g_RingVB, vbOffset, stride);
    std::vector<uint16_t> indices(seq.begin(), seq.end());
    UINT startIndex;
    if (!UploadIndices(indices, startIndex))
        return;
    dev->SetIndices(g_RingIB);
    const ClampAcross clamp(clampMain);
    dev->DrawIndexedPrimitive(type, 0, 0, n, startIndex, prims);
    DrawContinuations(continuations, stride, type, n, startIndex, prims);
}

static void __stdcall XbBegin(DWORD primitiveType)
{
    g_InBegin = true;
    g_BeginPrimitive = primitiveType;
    g_ImmVertices.clear();
    g_ImmUsed = 0;
    std::memcpy(g_ImmRegs, VertexRegisterDefaults(), sizeof(g_ImmRegs));
}

static void __stdcall XbEnd()
{
    FlightNoteCaller(_AddressOfReturnAddress());
    g_InBegin = false;
    UINT n = UINT(g_ImmVertices.size() / 64);
    if (!n)
        return;
    // Present every used register as FLOAT4 from one interleaved stream.
    VertexAttributeFormat attrs = {};
    DWORD offset = 0;
    for (int r = 0; r < 16; ++r) {
        attrs.Slots[r].Format = VSDT_NONE;
        if (g_ImmUsed & (1u << r)) {
            attrs.Slots[r] = { 0, DWORD(r * 16), VSDT_FLOAT4, 0, 0, 0, 0 };
            offset = r * 16 + 16;
        }
    }
    (void)offset;
    VertexMode mode = CurrentVertexMode();
    FlightRecordDraw(g_BeginPrimitive, n, 0, int(mode), g_ImmVertices.data(), 256, nullptr, n - 1);
    if (mode == VertexMode::Fvf)
        return EndFixedFunction(n);
    // Temporarily route through a declaration built for the immediate data.
    const Layout* layout = LayoutFor(attrs, mode);
    std::vector<uint32_t> v(n);
    for (UINT i = 0; i < n; ++i)
        v[i] = i;
    ++g_Stats.draws;
    D3DPRIMITIVETYPE type;
    std::vector<uint32_t> seq;
    UINT prims;
    g_PretransformedDraw = mode == VertexMode::Passthrough;
    if (!layout->decl || !Convert(g_BeginPrimitive, v, type, seq, prims) || !ApplyDrawState(layout->present)) {
        Skipped();
        return;
    }
    IDirect3DDevice9* dev = Device();
    UINT vbOffset;
    uint8_t* dst = AllocVertices(n * 256, 256, vbOffset);
    if (!dst)
        return;
    std::memcpy(dst, g_ImmVertices.data(), size_t(n) * 256);
    g_RingVB->Unlock();
    dev->SetVertexDeclaration(layout->decl);
    dev->SetStreamSource(0, g_RingVB, vbOffset, 256);
    std::vector<uint16_t> indices(seq.begin(), seq.end());
    UINT startIndex;
    if (!UploadIndices(indices, startIndex))
        return;
    dev->SetIndices(g_RingIB);
    dev->DrawIndexedPrimitive(type, 0, 0, n, startIndex, prims);
}

static void __stdcall XbSetVertexData2f(int reg, float a, float b) { SetRegister(reg, a, b, 0.0f, 1.0f); }
static void __stdcall XbSetVertexData4f(int reg, float a, float b, float c, float d) { SetRegister(reg, a, b, c, d); }

// --- Partial clears --------------------------------------------------------------------------

// Clears only the given color channels, which D3D9's Clear cannot do.
void DrawClearQuad(DWORD channels, D3DCOLOR color)
{
    IDirect3DDevice9* dev = Device();
    struct V {
        float x, y, z, rhw;
        D3DCOLOR c;
    };
    // The host viewport the clear set (device.cpp): the Xbox viewport, placed on a wide target too.
    D3DVIEWPORT9 hv;
    dev->GetViewport(&hv);
    const float x0 = float(hv.X) - 0.5f, y0 = float(hv.Y) - 0.5f;
    const float x1 = float(hv.X + hv.Width) - 0.5f, y1 = float(hv.Y + hv.Height) - 0.5f;
    V quad[4] = {
        { x0, y0, 0, 1, color },
        { x1, y0, 0, 1, color },
        { x0, y1, 0, 1, color },
        { x1, y1, 0, 1, color },
    };
    DWORD mask = 0;
    if (channels & CLEAR_TARGET_R) mask |= D3DCOLORWRITEENABLE_RED;
    if (channels & CLEAR_TARGET_G) mask |= D3DCOLORWRITEENABLE_GREEN;
    if (channels & CLEAR_TARGET_B) mask |= D3DCOLORWRITEENABLE_BLUE;
    if (channels & CLEAR_TARGET_A) mask |= D3DCOLORWRITEENABLE_ALPHA;

    IDirect3DStateBlock9* saved = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &saved);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetTexture(0, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    if (saved) {
        saved->Apply();
        saved->Release();
    }
}

SDK_REPLACE("D3DDevice_DrawVertices", XbDrawVertices);
SDK_REPLACE("D3DDevice_DrawIndexedVertices", XbDrawIndexedVertices);
SDK_REPLACE("D3DDevice_DrawVerticesUP", XbDrawVerticesUP);
SDK_REPLACE("D3DDevice_DrawIndexedVerticesUP", XbDrawIndexedVerticesUP);
SDK_REPLACE("D3DDevice_Begin", XbBegin);
SDK_REPLACE("D3DDevice_End", XbEnd);
SDK_REPLACE("D3DDevice_SetVertexData2f", XbSetVertexData2f);
SDK_REPLACE("D3DDevice_SetVertexData4f", XbSetVertexData4f);

} // namespace swrots::d3d
