// Applies the Xbox render state (from the game's D3D state arrays and our
// shadow state) to the host device before each draw. The NV2A takes OpenGL-style
// enum values for many states; everything is translated to D3D9 here.

#include "d3d/d3d.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/settings.h"
#include "d3d/shaders.h"

namespace swrots::d3d {

using namespace xd3d;

// --- Cached host state setters ---------------------------------------------------------

static DWORD g_HostRS[256];
static bool g_HostRSValid[256];
static DWORD g_HostSS[8][16];
static bool g_HostSSValid[8][16];
static DWORD g_HostTSS[8][33];
static bool g_HostTSSValid[8][33];

void InvalidateHostStateCache()
{
    std::memset(g_HostRSValid, 0, sizeof(g_HostRSValid));
    std::memset(g_HostSSValid, 0, sizeof(g_HostSSValid));
    std::memset(g_HostTSSValid, 0, sizeof(g_HostTSSValid));
}

static void RS(D3DRENDERSTATETYPE s, DWORD v)
{
    if (g_HostRSValid[s] && g_HostRS[s] == v)
        return;
    g_HostRS[s] = v;
    g_HostRSValid[s] = true;
    Device()->SetRenderState(s, v);
}

static void SS(DWORD sampler, D3DSAMPLERSTATETYPE s, DWORD v)
{
    if (g_HostSSValid[sampler][s] && g_HostSS[sampler][s] == v)
        return;
    g_HostSS[sampler][s] = v;
    g_HostSSValid[sampler][s] = true;
    Device()->SetSamplerState(sampler, s, v);
}

static void TSS(DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
    if (g_HostTSSValid[stage][s] && g_HostTSS[stage][s] == v)
        return;
    g_HostTSS[stage][s] = v;
    g_HostTSSValid[stage][s] = true;
    Device()->SetTextureStageState(stage, s, v);
}

static float AsFloat(DWORD v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

static DWORD AsDword(float f)
{
    DWORD v;
    std::memcpy(&v, &f, 4);
    return v;
}

// --- Enum translation --------------------------------------------------------------------

static DWORD CmpFunc(DWORD x) { return (x & 0xF) + 1; } // 0x200.. -> D3DCMP_NEVER..

static DWORD Blend(DWORD x)
{
    switch (x) {
    case 0x000: return D3DBLEND_ZERO;
    case 0x001: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR;
    case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA;
    case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA;
    case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR;
    case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    case 0x8001: return D3DBLEND_BLENDFACTOR;
    case 0x8002: return D3DBLEND_INVBLENDFACTOR;
    case 0x8003: return D3DBLEND_BLENDFACTOR;    // constant alpha (approximated)
    case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    }
    return D3DBLEND_ONE;
}

static DWORD BlendOp(DWORD x)
{
    switch (x) {
    case 0x8006: return D3DBLENDOP_ADD;
    case 0x800A: return D3DBLENDOP_SUBTRACT;
    case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN;
    case 0x8008: return D3DBLENDOP_MAX;
    case 0xF006: return D3DBLENDOP_ADD;         // ADDSIGNED (Xbox only)
    case 0xF005: return D3DBLENDOP_REVSUBTRACT; // REVSUBTRACTSIGNED (Xbox only)
    }
    return D3DBLENDOP_ADD;
}

static DWORD StencilOp(DWORD x)
{
    switch (x) {
    case 0x1E00: return D3DSTENCILOP_KEEP;
    case 0x0000: return D3DSTENCILOP_ZERO;
    case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT;
    case 0x1E03: return D3DSTENCILOP_DECRSAT;
    case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR;
    case 0x8508: return D3DSTENCILOP_DECR;
    }
    return D3DSTENCILOP_KEEP;
}

static DWORD CullMode(DWORD x)
{
    switch (x) {
    case 0x900: return D3DCULL_CW;
    case 0x901: return D3DCULL_CCW;
    }
    return D3DCULL_NONE;
}

static DWORD FillMode(DWORD x)
{
    switch (x) {
    case 0x1B00: return D3DFILL_POINT;
    case 0x1B01: return D3DFILL_WIREFRAME;
    }
    return D3DFILL_SOLID;
}

static DWORD ColorWriteMask(DWORD x)
{
    DWORD m = 0;
    if (x & (1 << 16)) m |= D3DCOLORWRITEENABLE_RED;
    if (x & (1 << 8)) m |= D3DCOLORWRITEENABLE_GREEN;
    if (x & (1 << 0)) m |= D3DCOLORWRITEENABLE_BLUE;
    if (x & (1 << 24)) m |= D3DCOLORWRITEENABLE_ALPHA;
    return m;
}

static DWORD TextureAddress(DWORD x)
{
    switch (x) {
    case 1: return D3DTADDRESS_WRAP;
    case 2: return D3DTADDRESS_MIRROR;
    case 4: return D3DTADDRESS_BORDER;
    }
    return D3DTADDRESS_CLAMP; // CLAMP and CLAMPTOEDGE
}

static DWORD TextureFilter(DWORD x)
{
    switch (x) {
    case 0: return D3DTEXF_NONE;
    case 1: return D3DTEXF_POINT;
    case 3: return D3DTEXF_ANISOTROPIC;
    }
    return D3DTEXF_LINEAR; // LINEAR, QUINCUNX, GAUSSIANCUBIC
}

// Xbox and D3D9 order the texture operations differently from BLENDCURRENTALPHA on.
static DWORD TextureOp(DWORD x)
{
    switch (x) {
    case 13: return D3DTOP_BLENDCURRENTALPHA;
    case 14: return D3DTOP_BLENDTEXTUREALPHA;
    case 15: return D3DTOP_BLENDFACTORALPHA;
    case 16: return D3DTOP_BLENDTEXTUREALPHAPM;
    case 22: return D3DTOP_DOTPRODUCT3;
    case 23: return D3DTOP_MULTIPLYADD;
    case 24: return D3DTOP_LERP;
    case 25: return D3DTOP_BUMPENVMAP;
    case 26: return D3DTOP_BUMPENVMAPLUMINANCE;
    }
    return x ? x : D3DTOP_DISABLE;
}

static D3DCOLOR ToColor(const float c[4])
{
    auto b = [](float v) { return DWORD(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f); };
    return D3DCOLOR_ARGB(b(c[3]), b(c[0]), b(c[1]), b(c[2]));
}

static void ColorToFloat4(D3DCOLOR c, float* f)
{
    f[0] = ((c >> 16) & 0xFF) / 255.0f;
    f[1] = ((c >> 8) & 0xFF) / 255.0f;
    f[2] = (c & 0xFF) / 255.0f;
    f[3] = ((c >> 24) & 0xFF) / 255.0f;
}

// --- Render states -------------------------------------------------------------------------

static void ApplyRenderStates()
{
    const DWORD* rs = XboxRenderStates();
    bool depth = State().depthStencil != nullptr;

    RS(D3DRS_ZENABLE, depth ? rs[RS_ZENABLE] : D3DZB_FALSE);
    RS(D3DRS_ZWRITEENABLE, depth && rs[RS_ZWRITEENABLE]);
    RS(D3DRS_ZFUNC, CmpFunc(rs[RS_ZFUNC]));
    RS(D3DRS_ALPHATESTENABLE, rs[RS_ALPHATESTENABLE]);
    RS(D3DRS_ALPHAFUNC, CmpFunc(rs[RS_ALPHAFUNC]));
    RS(D3DRS_ALPHAREF, rs[RS_ALPHAREF] & 0xFF);
    RS(D3DRS_ALPHABLENDENABLE, rs[RS_ALPHABLENDENABLE]);
    RS(D3DRS_SRCBLEND, Blend(rs[RS_SRCBLEND]));
    RS(D3DRS_DESTBLEND, Blend(rs[RS_DESTBLEND]));
    RS(D3DRS_BLENDOP, BlendOp(rs[RS_BLENDOP]));
    RS(D3DRS_BLENDFACTOR, rs[RS_BLENDCOLOR]);
    RS(D3DRS_COLORWRITEENABLE, ColorWriteMask(rs[RS_COLORWRITEENABLE]));
    RS(D3DRS_DITHERENABLE, rs[RS_DITHERENABLE]);
    RS(D3DRS_SHADEMODE, rs[RS_SHADEMODE] == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    RS(D3DRS_CULLMODE, CullMode(rs[RS_CULLMODE]));
    RS(D3DRS_FILLMODE, FillMode(rs[RS_FILLMODE]));

    RS(D3DRS_STENCILENABLE, depth && rs[RS_STENCILENABLE]);
    RS(D3DRS_STENCILFAIL, StencilOp(rs[RS_STENCILFAIL]));
    RS(D3DRS_STENCILZFAIL, StencilOp(rs[RS_STENCILZFAIL]));
    RS(D3DRS_STENCILPASS, StencilOp(rs[RS_STENCILPASS]));
    RS(D3DRS_STENCILFUNC, CmpFunc(rs[RS_STENCILFUNC]));
    RS(D3DRS_STENCILREF, rs[RS_STENCILREF]);
    RS(D3DRS_STENCILMASK, rs[RS_STENCILMASK]);
    RS(D3DRS_STENCILWRITEMASK, rs[RS_STENCILWRITEMASK]);

    // Depth bias: Xbox polygon offset is in depth-buffer units (24-bit here).
    float bias = 0.0f, slope = 0.0f;
    if (rs[RS_SOLIDOFFSETENABLE]) {
        slope = AsFloat(rs[RS_POLYGONOFFSETZSLOPESCALE]);
        bias = AsFloat(rs[RS_POLYGONOFFSETZOFFSET]) / 16777215.0f;
    }
    if (rs[RS_ZBIAS])
        bias -= float(rs[RS_ZBIAS]) / 65536.0f;
    RS(D3DRS_DEPTHBIAS, AsDword(bias));
    RS(D3DRS_SLOPESCALEDEPTHBIAS, AsDword(slope));

    RS(D3DRS_TEXTUREFACTOR, rs[RS_TEXTUREFACTOR]);
    RS(D3DRS_FOGCOLOR, rs[RS_FOGCOLOR]);
    RS(D3DRS_POINTSIZE, rs[RS_POINTSIZE]);
    RS(D3DRS_POINTSIZE_MIN, rs[RS_POINTSIZE_MIN]);
    RS(D3DRS_POINTSIZE_MAX, rs[RS_POINTSIZE_MAX]);
    RS(D3DRS_POINTSPRITEENABLE, rs[RS_POINTSPRITEENABLE]);
    RS(D3DRS_POINTSCALEENABLE, rs[RS_POINTSCALEENABLE]);
    RS(D3DRS_POINTSCALE_A, rs[RS_POINTSCALE_A]);
    RS(D3DRS_POINTSCALE_B, rs[RS_POINTSCALE_B]);
    RS(D3DRS_POINTSCALE_C, rs[RS_POINTSCALE_C]);
    RS(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
}

// Linear (non power-of-two) Xbox textures are addressed in texels, not 0..1.
static float g_TextureScale[4][4];
// Set by the draw code: D3D9 does not transform texture coordinates of
// pre-transformed vertices, so those are scaled on the CPU instead.
bool g_PretransformedDraw = false;

const float* TextureScale(int stage) { return g_TextureScale[stage]; }

// True widescreen: the fixed-function projection's x is narrowed so that a viewport spread over a
// wide target shows the game's frame in the middle, undistorted, and more of the scene at the sides.
static float g_ProjectionSquash = 1.0f;

// Fixed-function vertex/pixel pipeline state (FVF and declaration-only draws).
static void ApplyFixedFunctionStates()
{
    const DWORD* rs = XboxRenderStates();
    RS(D3DRS_LIGHTING, rs[RS_LIGHTING]);
    RS(D3DRS_SPECULARENABLE, rs[RS_SPECULARENABLE]);
    RS(D3DRS_LOCALVIEWER, rs[RS_LOCALVIEWER]);
    RS(D3DRS_COLORVERTEX, rs[RS_COLORVERTEX]);
    RS(D3DRS_NORMALIZENORMALS, rs[RS_NORMALIZENORMALS]);
    RS(D3DRS_AMBIENT, rs[RS_AMBIENT]);
    RS(D3DRS_DIFFUSEMATERIALSOURCE, rs[RS_DIFFUSEMATERIALSOURCE]);
    RS(D3DRS_SPECULARMATERIALSOURCE, rs[RS_SPECULARMATERIALSOURCE]);
    RS(D3DRS_AMBIENTMATERIALSOURCE, rs[RS_AMBIENTMATERIALSOURCE]);
    RS(D3DRS_EMISSIVEMATERIALSOURCE, rs[RS_EMISSIVEMATERIALSOURCE]);
    RS(D3DRS_FOGENABLE, rs[RS_FOGENABLE]);
    RS(D3DRS_FOGTABLEMODE, rs[RS_FOGTABLEMODE]);
    RS(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    RS(D3DRS_FOGSTART, rs[RS_FOGSTART]);
    RS(D3DRS_FOGEND, rs[RS_FOGEND]);
    RS(D3DRS_FOGDENSITY, rs[RS_FOGDENSITY]);
    RS(D3DRS_RANGEFOGENABLE, rs[RS_RANGEFOGENABLE]);
    RS(D3DRS_VERTEXBLEND, rs[RS_VERTEXBLEND] == 0 ? D3DVBF_DISABLE : rs[RS_VERTEXBLEND] == 1 ? D3DVBF_1WEIGHTS
                           : rs[RS_VERTEXBLEND] == 3 ? D3DVBF_2WEIGHTS : D3DVBF_3WEIGHTS);

    const XboxState& st = State();
    Device()->SetTransform(D3DTS_VIEW, &st.transforms[TS_VIEW]);
    D3DMATRIX projection = st.transforms[TS_PROJECTION];
    if (g_ProjectionSquash != 1.0f)
        for (int r = 0; r < 4; ++r)
            projection.m[r][0] *= g_ProjectionSquash; // row vectors: column 0 makes clip x
    Device()->SetTransform(D3DTS_PROJECTION, &projection);
    for (int i = 0; i < 4; ++i) {
        Device()->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_WORLDMATRIX(i)), &st.transforms[TS_WORLD + i]);
    }

    for (DWORD i = 0; i < 4; ++i) {
        const DWORD* ts = XboxTextureStates(i);
        TSS(i, D3DTSS_COLOROP, TextureOp(ts[TSS_COLOROP]));
        TSS(i, D3DTSS_COLORARG0, ts[TSS_COLORARG0]);
        TSS(i, D3DTSS_COLORARG1, ts[TSS_COLORARG1]);
        TSS(i, D3DTSS_COLORARG2, ts[TSS_COLORARG2]);
        TSS(i, D3DTSS_ALPHAOP, TextureOp(ts[TSS_ALPHAOP]));
        TSS(i, D3DTSS_ALPHAARG0, ts[TSS_ALPHAARG0]);
        TSS(i, D3DTSS_ALPHAARG1, ts[TSS_ALPHAARG1]);
        TSS(i, D3DTSS_ALPHAARG2, ts[TSS_ALPHAARG2]);
        TSS(i, D3DTSS_RESULTARG, ts[TSS_RESULTARG]);
        // Texel-addressed (linear) textures: fold 1/size into the texture transform.
        D3DMATRIX m = st.transforms[TS_TEXTURE0 + i];
        DWORD ttf = ts[TSS_TEXTURETRANSFORMFLAGS];
        if (!g_PretransformedDraw && (g_TextureScale[i][0] != 1.0f || g_TextureScale[i][1] != 1.0f)) {
            if ((ttf & 0xFF) == D3DTTFF_DISABLE) {
                std::memset(&m, 0, sizeof(m));
                m._11 = m._22 = m._33 = m._44 = 1.0f;
                ttf = D3DTTFF_COUNT2;
            }
            for (int r = 0; r < 4; ++r) {
                m.m[r][0] /= g_TextureScale[i][0];
                m.m[r][1] /= g_TextureScale[i][1];
            }
        }
        Device()->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i), &m);
        TSS(i, D3DTSS_TEXTURETRANSFORMFLAGS, ttf);
        DWORD tci = State().textureStates[i][TSS_TEXCOORDINDEX];
        if ((tci & 0xFFFF0000) == 0x50000)
            tci = (tci & 0xFFFF) | D3DTSS_TCI_SPHEREMAP;
        TSS(i, D3DTSS_TEXCOORDINDEX, tci);
        const DWORD* nd = State().textureStates[i];
        TSS(i, D3DTSS_BUMPENVMAT00, nd[TSS_BUMPENVMAT00]);
        TSS(i, D3DTSS_BUMPENVMAT01, nd[TSS_BUMPENVMAT01]);
        TSS(i, D3DTSS_BUMPENVMAT10, nd[TSS_BUMPENVMAT10]);
        TSS(i, D3DTSS_BUMPENVMAT11, nd[TSS_BUMPENVMAT11]);
        TSS(i, D3DTSS_BUMPENVLSCALE, nd[TSS_BUMPENVLSCALE]);
        TSS(i, D3DTSS_BUMPENVLOFFSET, nd[TSS_BUMPENVLOFFSET]);
        if (TextureOp(ts[TSS_COLOROP]) == D3DTOP_DISABLE)
            break;
    }
}

// --- Textures and samplers ------------------------------------------------------------------

static void ApplyTextures()
{
    for (DWORD i = 0; i < 4; ++i) {
        BaseTexture* xt = State().textures[i];
        IDirect3DBaseTexture9* ht = xt ? HostTextureFor(xt, State().palettes[i]) : nullptr;
        Device()->SetTexture(i, ht);

        float* scale = g_TextureScale[i];
        scale[0] = scale[1] = scale[2] = scale[3] = 1.0f;
        if (xt && xt->Size) {
            scale[0] = float((xt->Size & SIZE_WIDTH_MASK) + 1);
            scale[1] = float(((xt->Size & SIZE_HEIGHT_MASK) >> SIZE_HEIGHT_SHIFT) + 1);
        }

        const DWORD* ts = XboxTextureStates(i);
        SS(i, D3DSAMP_ADDRESSU, TextureAddress(ts[TSS_ADDRESSU]));
        SS(i, D3DSAMP_ADDRESSV, TextureAddress(ts[TSS_ADDRESSV]));
        SS(i, D3DSAMP_ADDRESSW, TextureAddress(ts[TSS_ADDRESSW]));
        SS(i, D3DSAMP_MAGFILTER, TextureFilter(ts[TSS_MAGFILTER]));
        SS(i, D3DSAMP_MINFILTER, TextureFilter(ts[TSS_MINFILTER]));
        SS(i, D3DSAMP_MIPFILTER, TextureFilter(ts[TSS_MIPFILTER]));
        SS(i, D3DSAMP_MIPMAPLODBIAS, ts[TSS_MIPMAPLODBIAS]);
        SS(i, D3DSAMP_MAXMIPLEVEL, ts[TSS_MAXMIPLEVEL]);
        // Player setting: anisotropic filtering for linearly minified textures.
        const DWORD anisotropy = DWORD(GetSettings().anisotropy);
        if (anisotropy > 1 && TextureFilter(ts[TSS_MINFILTER]) == D3DTEXF_LINEAR) {
            SS(i, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            SS(i, D3DSAMP_MAXANISOTROPY, anisotropy);
        } else {
            SS(i, D3DSAMP_MAXANISOTROPY, ts[TSS_MAXANISOTROPY] ? ts[TSS_MAXANISOTROPY] : 1);
        }
        SS(i, D3DSAMP_BORDERCOLOR, State().textureStates[i][TSS_BORDERCOLOR]);
    }
}

// --- Shader constants -------------------------------------------------------------------------

static void ApplyVertexProgramConstants(const DWORD* registerPresent)
{
    IDirect3DDevice9* dev = Device();
    dev->SetVertexShaderConstantF(0, &State().vertexConstants[0][0], 192);
    dev->SetVertexShaderConstantF(kVsRegDefaults, VertexRegisterDefaults(), 16);
    float flags[16];
    for (int i = 0; i < 16; ++i)
        flags[i] = registerPresent[i] ? 0.0f : 1.0f;
    dev->SetVertexShaderConstantF(kVsRegDefaultFlags, flags, 4);

    // Undo the viewport transform built into Xbox vertex programs. Programs
    // output Xbox pixel coordinates; mapping them to clip space over the Xbox
    // surface size keeps Xbox pixel i on host pixels i*k .. i*k+k-1 at any
    // RenderScale() k (the 0.5 is D3D9's pixel-center convention).
    UINT width, height;
    XboxSurfaceSize(State().renderTarget, width, height);
    float scale[4] = { width * 0.5f, -(height * 0.5f), DepthScale(), 1.0f };
    // True widescreen: the target is wider than width x k; mapping its whole width keeps Xbox pixel i
    // on the same host pixels, shifted into the middle, and what the game's camera sees beyond the
    // frame's sides (clip x past +-1) lands at the sides (Hor+).
    const SurfaceMap map = MapFor(State().renderTarget);
    if (map.wide)
        scale[0] = float(map.hostWidth) / float(RenderScale()) * 0.5f;
    float offset[4] = { width * 0.5f - 0.5f, height * 0.5f - 0.5f, 0.0f, 0.0f };
    dev->SetVertexShaderConstantF(kVsRegScreenScale, scale, 1);
    dev->SetVertexShaderConstantF(kVsRegScreenOffset, offset, 1);
    dev->SetVertexShaderConstantF(kVsRegTextureScale, &g_TextureScale[0][0], 4);
}

static void ApplyPixelShaderConstants()
{
    const DWORD* rs = XboxRenderStates();
    float c[43][4] = {};
    for (int i = 0; i < 8; ++i) {
        ColorToFloat4(rs[RS_PSCONSTANT0_0 + i], c[kPsRegC0 + i]);
        ColorToFloat4(rs[RS_PSCONSTANT1_0 + i], c[kPsRegC1 + i]);
    }
    ColorToFloat4(rs[RS_PSFINALCOMBINERCONSTANT0], c[kPsRegFC0]);
    ColorToFloat4(rs[RS_PSFINALCOMBINERCONSTANT1], c[kPsRegFC1]);
    ColorToFloat4(rs[RS_FOGCOLOR], c[kPsRegFogColor]);
    for (int i = 0; i < 4; ++i) {
        const DWORD* ts = XboxTextureStates(i);
        const DWORD* nd = State().textureStates[i];
        DWORD sign = ts[TSS_COLORSIGN];
        c[kPsRegColorSign + i][0] = (sign & 0x20000000) ? 1.0f : 0.0f;
        c[kPsRegColorSign + i][1] = (sign & 0x40000000) ? 1.0f : 0.0f;
        c[kPsRegColorSign + i][2] = (sign & 0x80000000) ? 1.0f : 0.0f;
        c[kPsRegColorSign + i][3] = (sign & 0x10000000) ? 1.0f : 0.0f;
        c[kPsRegColorKeyOp + i][0] = float(ts[TSS_COLORKEYOP]);
        ColorToFloat4(nd[TSS_COLORKEYCOLOR], c[kPsRegColorKeyColor + i]);
        c[kPsRegBem + i][0] = AsFloat(nd[TSS_BUMPENVMAT00]);
        c[kPsRegBem + i][1] = AsFloat(nd[TSS_BUMPENVMAT01]);
        c[kPsRegBem + i][2] = AsFloat(nd[TSS_BUMPENVMAT10]);
        c[kPsRegBem + i][3] = AsFloat(nd[TSS_BUMPENVMAT11]);
        c[kPsRegLum + i][0] = AsFloat(nd[TSS_BUMPENVLSCALE]);
        c[kPsRegLum + i][1] = AsFloat(nd[TSS_BUMPENVLOFFSET]);
    }
    // Front/back diffuse selection (multiplied with VFACE). 0 = always the front
    // colours; only two-sided lighting uses the back colours. D3D9's VFACE is
    // positive for clockwise faces, so a counter-clockwise front face inverts it.
    c[kPsRegFrontFace][0] = rs[RS_TWOSIDEDLIGHTING] ? (rs[RS_FRONTFACE] == 0x900 ? 1.0f : -1.0f) : 0.0f;
    c[kPsRegFogInfo][0] = float(rs[RS_FOGTABLEMODE]);
    c[kPsRegFogInfo][1] = AsFloat(rs[RS_FOGDENSITY]);
    c[kPsRegFogInfo][2] = AsFloat(rs[RS_FOGSTART]);
    c[kPsRegFogInfo][3] = AsFloat(rs[RS_FOGEND]);
    c[kPsRegFogEnable][0] = rs[RS_FOGENABLE] ? 1.0f : 0.0f;
    ColorToFloat4(rs[RS_TEXTUREFACTOR], c[kPsRegTextureFactor]);
    Device()->SetPixelShaderConstantF(0, &c[0][0], 43);
}

// Builds the current pixel shader definition from render states 0..56 (which
// SetPixelShader and SetRenderState keep up to date, as on the Xbox).
static const PixelShaderDef* CurrentPixelShaderDef(PixelShaderDef& out)
{
    PixelShader* ps = State().pixelShader;
    if (!ps || !ps->pPSDef)
        return nullptr;
    out = *ps->pPSDef;
    std::memcpy(&out, XboxRenderStates(), (RS_PSINPUTTEXTURE + 1) * sizeof(DWORD));
    out.PSTextureModes = XboxRenderStates()[RS_PSTEXTUREMODES];
    return &out;
}

// --- Entry point --------------------------------------------------------------------------

// Sets the host pixel shader for the current Xbox pixel shader (or the
// fixed-function stage emulation when there is none) and its constants.
static bool ApplyTranslatedPixelShader()
{
    bool alphaKill[4];
    for (int i = 0; i < 4; ++i)
        alphaKill[i] = XboxTextureStates(i)[TSS_ALPHAKILL] != 0;
    PixelShaderDef def;
    const PixelShaderDef* pd = CurrentPixelShaderDef(def);
    IDirect3DPixelShader9* ps = pd ? HostPixelShaderFor(pd, alphaKill) : FixedFunctionPixelShader();
    if (!ps)
        return false;
    Device()->SetPixelShader(ps);
    ApplyPixelShaderConstants();
    return true;
}

// Returns false if the draw must be skipped (e.g. shader compile failure).
bool ApplyDrawState(const DWORD* registerPresent)
{
    IDirect3DDevice9* dev = Device();
    ApplyRenderStates();
    ApplyTextures();

    VertexMode mode = CurrentVertexMode();
    const Viewport& v = State().viewport;
    if (mode == VertexMode::Program) {
        IDirect3DVertexShader9* vs = CurrentHostVertexShader();
        if (!vs)
            return false;
        dev->SetVertexShader(vs);
        ApplyVertexProgramConstants(registerPresent);

        // The program already contains the viewport transform; cover the whole target.
        D3DSURFACE_DESC desc;
        HostSurfaceFor(State().renderTarget, false)->GetDesc(&desc);
        D3DVIEWPORT9 hv = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        dev->SetViewport(&hv);

        if (!ApplyTranslatedPixelShader())
            return false;
    } else if (State().pixelShader && mode == VertexMode::Fvf && g_PretransformedDraw) {
        // Screen-space draw with an Xbox pixel shader (e.g. the glow pass).
        IDirect3DVertexShader9* vs = ScreenSpaceVertexShader(State().vertexShader);
        IDirect3DVertexDeclaration9* decl = ScreenSpaceDeclaration(State().vertexShader);
        if (!vs || !decl)
            return false;
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        D3DSURFACE_DESC desc;
        HostSurfaceFor(State().renderTarget, false)->GetDesc(&desc);
        const float target[4] = { float(desc.Width), float(desc.Height), 0.0f, 0.0f };
        dev->SetVertexShaderConstantF(0, target, 1);
        D3DVIEWPORT9 hv = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        dev->SetViewport(&hv);
        if (!ApplyTranslatedPixelShader())
            return false;
    } else {
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        const UINT k = RenderScale();
        // On a wide target a whole-width viewport covers the whole host width: transformed geometry is
        // narrowed to keep its shape (more of the scene at the sides); pre-transformed vertices are
        // placed by the draw code (draw.cpp) and only need the room.
        const SurfaceMap map = MapFor(State().renderTarget);
        LONG x1, x2;
        MapRangeX(map, LONG(v.X), LONG(v.X + v.Width), x1, x2);
        // A viewport of the frame's width spread over the whole host width is narrowed; one the game already
        // widened by the margins (game/widescreen.cpp) covers it as it is.
        const bool spread = map.wide && x2 - x1 == LONG(map.hostWidth);
        g_ProjectionSquash = spread && !g_PretransformedDraw ? float(v.Width * k) / float(map.hostWidth) : 1.0f;
        ApplyFixedFunctionStates();
        D3DVIEWPORT9 hv = { DWORD(x1), v.Y * k, DWORD(x2 - x1), v.Height * k, v.MinZ, v.MaxZ };
        dev->SetViewport(&hv);
        static bool warned = false;
        if (State().pixelShader && !warned) {
            warned = true;
            LOG_WARN("Fixed-function vertex path with an Xbox pixel shader: pixel shader ignored");
        }
    }
    return true;
}

} // namespace swrots::d3d
