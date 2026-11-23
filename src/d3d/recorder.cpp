// Flight recorder: the last few seconds of frames, saved on Ctrl+Shift+F10.
//
// Every frame keeps a small screenshot (a GPU render target, so recording costs
// one StretchRect) and a compact record of each draw: which game code issued it,
// the shaders, textures, vertex data and the render states that decide whether
// and how it shows. The dump writes the screenshots as PNGs plus a report that
// lists every draw per frame and flags objects that were drawn for only a few
// frames (pop-in / flashes).

#include "d3d/recorder.h"

#include <wincodec.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "core/window.h"
#include "d3d/d3d.h"
#include "game/release.h"

namespace swrots::d3d {

using namespace xd3d;

static const double kSeconds = 3.0;
static const int kMaxNotes = 256;
static GAME_VA(kGameCodeBegin, 0x00011000);
static GAME_VA(kGameCodeEnd, 0x004EF000);

static const DWORD kRecordedStates[] = { RS_ZENABLE, RS_ZWRITEENABLE, RS_ZFUNC, RS_ALPHABLENDENABLE, RS_SRCBLEND,
    RS_DESTBLEND, RS_BLENDOP, RS_ALPHATESTENABLE, RS_ALPHAREF, RS_ALPHAFUNC, RS_COLORWRITEENABLE, RS_CULLMODE,
    RS_STENCILENABLE, RS_STENCILFUNC, RS_STENCILREF, RS_FOGENABLE, RS_TEXTUREFACTOR, RS_PSCONSTANT0_0,
    RS_PSCONSTANT1_0 };
static const int kStateCount = int(std::size(kRecordedStates));

struct DrawRecord {
    uint32_t callers[4];
    uint32_t vs, ps, rt, ds;
    uint32_t tex[4];
    uint32_t vertexData, constants; // stream 0 data; hash of the transform constants
    uint32_t primitive, count, first;
    uint32_t states[kStateCount];
    float position[3]; // first vertex, raw stream 0 floats
    float c[12][4];    // vertex constants c0-c11 (programs) or the world matrix (fixed function)
    uint32_t vertexHash, indexHash;  // contents of the referenced vertices and the index list
    uint32_t constantGroups[12];     // hash of each group of 16 vertex constants (c-96..c95)
    uint8_t mode;
    uint8_t skipped;
};

struct FrameSlot {
    DWORD number = 0;
    LONGLONG time = 0;
    std::vector<DrawRecord> draws;
    std::vector<std::string> notes;
    float constants[192][4] = {}; // all vertex constants at the frame's first vertex program draw
    bool hasConstants = false;
    IDirect3DSurface9* thumbnail = nullptr;
    bool captured = false;
};

static bool g_Enabled = false;
static std::wstring g_OutputDir;
static std::vector<FrameSlot> g_Frames;
static size_t g_Head = 0;     // slot being recorded
static size_t g_Recorded = 0; // finished slots
static DWORD g_FrameNumber = 0;
static uint32_t g_Callers[4];
static UINT g_ThumbWidth = 0, g_ThumbHeight = 0;

void ConfigureFlightRecorder(bool enabled, const std::wstring& outputDir)
{
    g_Enabled = enabled;
    g_OutputDir = outputDir;
    if (enabled)
        LOG_INFO("Flight recorder on: Ctrl+Shift+F10 saves the last %.0f seconds to %ls", kSeconds, outputDir.c_str());
}

static size_t Capacity()
{
    int fps = GetSettings().fpsLimit > 0 ? GetSettings().fpsLimit : 60;
    return size_t(std::clamp(int(fps * kSeconds + 0.5), 90, 240));
}

static FrameSlot& Current()
{
    if (g_Frames.empty())
        g_Frames.resize(Capacity());
    return g_Frames[g_Head];
}

// True if the address follows a call instruction in game code (a return address).
static bool IsReturnAddress(uint32_t a)
{
    if (a < kGameCodeBegin + 8 || a >= kGameCodeEnd)
        return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(uintptr_t(a));
    return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
           (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
}

void FlightNoteCaller(void* addressOfReturnAddress)
{
    if (!g_Enabled)
        return;
    const uint32_t* sp = static_cast<const uint32_t*>(addressOfReturnAddress);
    int n = 0;
    g_Callers[n++] = sp[0];
    for (int i = 1; i < 96 && n < 4; ++i)
        if (IsReturnAddress(sp[i]))
            g_Callers[n++] = sp[i];
    while (n < 4)
        g_Callers[n++] = 0;
}

static uint32_t HashDwords(const void* data, size_t count)
{
    const uint32_t* d = static_cast<const uint32_t*>(data);
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < count; ++i)
        h = (h ^ d[i]) * 0x01000193u;
    return h;
}

static uint32_t HashBytes(const void* data, size_t bytes)
{
    const uint32_t* d = static_cast<const uint32_t*>(data);
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < bytes / 4; ++i)
        h = ((h << 5) | (h >> 27)) ^ d[i];
    return h * 0x01000193u;
}

void FlightRecordDraw(DWORD primitive, UINT vertexCount, UINT firstVertex, int vertexMode, const void* vertexData,
    UINT stride, const uint32_t* indices, UINT lastVertex)
{
    if (!g_Enabled)
        return;
    const XboxState& st = State();
    const DWORD* rs = XboxRenderStates();
    DrawRecord r = {};
    std::copy(std::begin(g_Callers), std::end(g_Callers), r.callers);
    r.vs = st.vertexShader;
    r.ps = uint32_t(uintptr_t(st.pixelShader));
    r.rt = st.renderTarget ? st.renderTarget->Data : 0;
    r.ds = st.depthStencil ? st.depthStencil->Data : 0;
    for (int i = 0; i < 4; ++i)
        r.tex[i] = st.textures[i] ? st.textures[i]->Data : 0;
    r.vertexData = uint32_t(uintptr_t(vertexData));
    r.constants = vertexMode >= 2 ? HashDwords(st.vertexConstants, sizeof(st.vertexConstants) / 4)
                                  : HashDwords(st.transforms, sizeof(st.transforms) / 4);
    r.primitive = primitive;
    r.count = vertexCount;
    r.first = firstVertex;
    for (int i = 0; i < kStateCount; ++i)
        r.states[i] = rs[kRecordedStates[i]];
    if (vertexData && stride >= 12) {
        const float* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vertexData) + size_t(firstVertex) * stride);
        r.position[0] = p[0];
        r.position[1] = p[1];
        r.position[2] = p[2];
    }
    r.mode = uint8_t(vertexMode);
    if (vertexMode >= 2)
        std::memcpy(r.c, &st.vertexConstants[96][0], sizeof(r.c));
    else
        std::memcpy(r.c, &st.transforms[TS_WORLD], sizeof(D3DMATRIX));
    if (vertexData && stride && lastVertex >= firstVertex)
        r.vertexHash = HashBytes(static_cast<const uint8_t*>(vertexData) + size_t(firstVertex) * stride,
            std::min<size_t>(size_t(lastVertex - firstVertex + 1) * stride, 1u << 18));
    if (indices)
        r.indexHash = HashBytes(indices, size_t(vertexCount) * 4);
    for (int g = 0; g < 12; ++g)
        r.constantGroups[g] = HashBytes(st.vertexConstants[g * 16], 16 * 16);
    FrameSlot& fs = Current();
    if (vertexMode == 3 && !fs.hasConstants && st.renderTarget == st.backBuffer) {
        std::memcpy(fs.constants, st.vertexConstants, sizeof(fs.constants));
        fs.hasConstants = true;
    }
    fs.draws.push_back(r);
    std::fill(std::begin(g_Callers), std::end(g_Callers), 0u);
}

void FlightMarkSkipped()
{
    if (g_Enabled && !Current().draws.empty())
        Current().draws.back().skipped = 1;
}

void FlightNote(const char* fmt, ...)
{
    if (!g_Enabled || Current().notes.size() >= kMaxNotes)
        return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Current().notes.emplace_back(buf);
}

void FlightReleaseResources()
{
    for (FrameSlot& f : g_Frames) {
        if (f.thumbnail) f.thumbnail->Release();
        f.thumbnail = nullptr;
        f.captured = false;
    }
}

// --- Dump -------------------------------------------------------------------------------------

static bool SavePng(IWICImagingFactory* wic, const std::wstring& path, const D3DLOCKED_RECT& lr, UINT w, UINT h)
{
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* s = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
        uint8_t* d = rgb.data() + size_t(y) * w * 3;
        for (UINT x = 0; x < w; ++x, s += 4, d += 3) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props)) &&
              SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
              IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
              SUCCEEDED(frame->WritePixels(h, w * 3, UINT(rgb.size()), rgb.data())) && SUCCEEDED(frame->Commit()) &&
              SUCCEEDED(encoder->Commit());
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    return ok;
}

// --- Screenshots ------------------------------------------------------------------------------

static std::mutex g_ShotLock;
static std::wstring g_ShotDir;
static std::vector<std::string> g_ShotRequests;

void SetScreenshotDirectory(const std::wstring& directory)
{
    std::lock_guard<std::mutex> lock(g_ShotLock);
    g_ShotDir = directory;
}

void RequestScreenshot(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_ShotLock);
    g_ShotRequests.push_back(name);
}

static FrameTap g_FrameTap = nullptr;

void SetFrameTap(FrameTap tap)
{
    g_FrameTap = tap;
}

void TapFrame(IDirect3DSurface9* windowBuffer)
{
    if (g_FrameTap && windowBuffer)
        g_FrameTap(windowBuffer);
}

void SavePendingScreenshot(IDirect3DSurface9* windowBuffer)
{
    std::vector<std::string> requests;
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(g_ShotLock);
        if (g_ShotRequests.empty())
            return;
        requests.swap(g_ShotRequests);
        dir = g_ShotDir;
    }
    D3DSURFACE_DESC desc;
    IDirect3DSurface9* staging = nullptr;
    D3DLOCKED_RECT lr;
    if (dir.empty() || !windowBuffer || FAILED(windowBuffer->GetDesc(&desc)) ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) ||
        FAILED(Device()->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
            &staging, nullptr)) ||
        FAILED(Device()->GetRenderTargetData(windowBuffer, staging)) ||
        FAILED(staging->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (staging) staging->Release();
        LOG_ERROR("Screenshot: could not read the frame");
        return;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const std::string& name : requests) {
        std::wstring file;
        if (name.empty()) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t stamp[64];
            swprintf_s(stamp, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                t.wSecond, t.wMilliseconds);
            file = stamp;
        } else {
            for (char c : name) // a plain file name: anything else (a path) becomes _
                file += (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? wchar_t(c) : L'_';
        }
        const std::wstring path = dir + L"\\" + file + L".png";
        if (wic && SavePng(wic, path, lr, desc.Width, desc.Height))
            LOG_INFO("Screenshot: %ls (%ux%u)", path.c_str(), desc.Width, desc.Height);
        else
            LOG_ERROR("Screenshot: could not write %ls", path.c_str());
    }
    staging->UnlockRect();
    staging->Release();
    if (wic) wic->Release();
}

static const char* ModeName(uint8_t m)
{
    static const char* kNames[] = { "fvf", "ff", "pass", "prog" };
    return m < 4 ? kNames[m] : "?";
}

static void WriteDraw(FILE* f, size_t index, const DrawRecord& r)
{
    const uint32_t* s = r.states;
    fprintf(f,
        "  d%-4zu %s%-4s prim %lu n %lu first %lu vs %08lX ps %08lX rt %08lX ds %08lX tex %08lX %08lX %08lX %08lX "
        "vb %08lX k %08lX pos %.2f %.2f %.2f | z %lu/%lu/%lX blend %lu %lX/%lX op %lX atest %lu ref %lX func %lX "
        "cw %lX cull %lX stencil %lu %lX/%lX fog %lu tf %08lX psc %08lX %08lX | from %06lX %06lX %06lX %06lX\n",
        index, r.skipped ? "SKIP " : "", ModeName(r.mode), r.primitive, r.count, r.first, r.vs, r.ps, r.rt, r.ds,
        r.tex[0], r.tex[1], r.tex[2], r.tex[3], r.vertexData, r.constants, r.position[0], r.position[1], r.position[2],
        s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16],
        s[17], s[18], r.callers[0], r.callers[1], r.callers[2], r.callers[3]);
    fprintf(f, "        vh %08lX ih %08lX groups", r.vertexHash, r.indexHash);
    for (uint32_t g : r.constantGroups)
        fprintf(f, " %08lX", g);
    fprintf(f, "\n        %s", r.mode >= 2 ? "c0-11" : "world");
    for (int row = 0; row < (r.mode >= 2 ? 12 : 4); ++row)
        fprintf(f, " | %.4g %.4g %.4g %.4g", r.c[row][0], r.c[row][1], r.c[row][2], r.c[row][3]);
    fprintf(f, "\n");
}

// Identity of an object across frames: the code that draws it, its shaders,
// main texture, vertex data, size and target.
static uint64_t DrawKey(const DrawRecord& r)
{
    const uint32_t k[] = { r.callers[0], r.vs, r.ps, r.tex[0], r.vertexData, r.count, r.rt };
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t v : k)
        h = (h ^ v) * 0x100000001b3ull;
    return h;
}

static void Dump()
{
    // Finished frames, oldest first, limited to the last kSeconds.
    std::vector<FrameSlot*> frames;
    for (size_t i = g_Recorded; i > 0; --i)
        frames.push_back(&g_Frames[(g_Head + g_Frames.size() - i) % g_Frames.size()]);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (frames.empty())
        return;
    const LONGLONG newest = frames.back()->time;
    while (frames.size() > 1 && double(newest - frames.front()->time) / freq.QuadPart > kSeconds)
        frames.erase(frames.begin());

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring dir = g_OutputDir + L"\\" + stamp;
    CreateDirectoryW(g_OutputDir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\frames").c_str(), nullptr);
    LOG_INFO("Flight recorder: saving %zu frames to %ls", frames.size(), dir.c_str());

    // Screenshots.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    IDirect3DSurface9* staging = nullptr;
    if (wic && g_ThumbWidth)
        Device()->CreateOffscreenPlainSurface(g_ThumbWidth, g_ThumbHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr);
    int saved = 0;
    for (size_t i = 0; i < frames.size() && staging; ++i) {
        FrameSlot& fs = *frames[i];
        D3DLOCKED_RECT lr;
        if (!fs.captured || FAILED(Device()->GetRenderTargetData(fs.thumbnail, staging)) ||
            FAILED(staging->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            continue;
        wchar_t name[32];
        swprintf_s(name, L"\\frames\\f%03zu.png", i);
        saved += SavePng(wic, dir + name, lr, g_ThumbWidth, g_ThumbHeight);
        staging->UnlockRect();
    }
    if (staging) staging->Release();
    if (wic) wic->Release();

    // Objects drawn in only a few frames, with no sign of them for a while before and after.
    const int kMaxRun = 6, kGap = 10;
    std::unordered_map<uint64_t, std::vector<int>> seen; // key -> frames it was drawn in
    for (size_t i = 0; i < frames.size(); ++i)
        for (const DrawRecord& r : frames[i]->draws) {
            std::vector<int>& v = seen[DrawKey(r)];
            if (v.empty() || v.back() != int(i))
                v.push_back(int(i));
        }
    struct Transient {
        int first, last;
        uint64_t key;
    };
    std::vector<Transient> transients;
    const int total = int(frames.size());
    for (auto& [key, v] : seen) {
        size_t a = 0;
        while (a < v.size()) {
            size_t b = a;
            while (b + 1 < v.size() && v[b + 1] == v[b] + 1)
                ++b;
            const int prev = a ? v[a - 1] : -1000, next = b + 1 < v.size() ? v[b + 1] : 1000000;
            if (v[b] - v[a] + 1 <= kMaxRun && v[a] >= kGap && v[b] < total - kGap && v[a] - prev > kGap &&
                next - v[b] > kGap)
                transients.push_back({ v[a], v[b], key });
            a = b + 1;
        }
    }
    std::sort(transients.begin(), transients.end(), [](const Transient& x, const Transient& y) { return x.first < y.first; });

    FILE* f = nullptr;
    _wfopen_s(&f, (dir + L"\\report.txt").c_str(), L"w");
    if (f) {
        fprintf(f, "SWROTS flight recording: %d frames (%.2f s), oldest first. Screenshots: frames\\fNNN.png\n",
            total, double(newest - frames.front()->time) / freq.QuadPart);
        fprintf(f, "Back buffer: %08lX\n\n", State().backBuffer ? State().backBuffer->Data : 0);
        fprintf(f, "== Objects drawn for at most %d frames, absent %d+ frames before and after (%zu) ==\n", kMaxRun,
            kGap, transients.size());
        for (const Transient& tr : transients) {
            fprintf(f, "f%03d-f%03d:\n", tr.first, tr.last);
            const auto& draws = frames[tr.first]->draws;
            for (size_t d = 0; d < draws.size(); ++d)
                if (DrawKey(draws[d]) == tr.key) {
                    WriteDraw(f, d, draws[d]);
                    break;
                }
        }
        fprintf(f, "\n== Frames ==\n");
        for (size_t i = 0; i < frames.size(); ++i) {
            const FrameSlot& fs = *frames[i];
            size_t skipped = std::count_if(fs.draws.begin(), fs.draws.end(), [](const DrawRecord& r) { return r.skipped; });
            const double at = double(fs.time - frames.front()->time) / freq.QuadPart * 1000.0;
            const double dt = i ? double(fs.time - frames[i - 1]->time) / freq.QuadPart * 1000.0 : 0.0;
            fprintf(f, "f%03zu  #%lu  %8.1f ms  (+%5.1f)  %zu draws, %zu skipped%s\n", i, fs.number, at, dt,
                fs.draws.size(), skipped, fs.notes.empty() ? "" : "  [notes]");
        }
        fclose(f);
    }
    _wfopen_s(&f, (dir + L"\\draws.txt").c_str(), L"w");
    if (f) {
        for (size_t i = 0; i < frames.size(); ++i) {
            const FrameSlot& fs = *frames[i];
            fprintf(f, "=== f%03zu #%lu: %zu draws ===\n", i, fs.number, fs.draws.size());
            for (const std::string& n : fs.notes)
                fprintf(f, "  note: %s\n", n.c_str());
            if (fs.hasConstants) {
                fprintf(f, "  constants at the first vertex program draw:\n");
                for (int row = 0; row < 192; ++row) {
                    const float* v = fs.constants[row];
                    if (v[0] || v[1] || v[2] || v[3])
                        fprintf(f, "    c%d = %.6g %.6g %.6g %.6g\n", row - 96, v[0], v[1], v[2], v[3]);
                }
            }
            for (size_t d = 0; d < fs.draws.size(); ++d)
                WriteDraw(f, d, fs.draws[d]);
        }
        fclose(f);
    }
    LOG_INFO("Flight recorder: saved %d screenshots, %zu flagged objects", saved, transients.size());
    MessageBeep(MB_ICONASTERISK);
}

// --- Frame end ----------------------------------------------------------------------------

static bool HotkeyPressed()
{
    static bool wasDown = false;
    const bool down = GameWindowActive() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                      (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F10) & 0x8000);
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

void FlightEndFrame(IDirect3DSurface9* backBuffer)
{
    if (!g_Enabled)
        return;
    FrameSlot& fs = Current();
    fs.number = ++g_FrameNumber;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    fs.time = now.QuadPart;
    if (!g_ThumbWidth) {
        g_ThumbHeight = 270;
        g_ThumbWidth = GetSettings().widescreen ? 480 : 360;
    }
    if (!fs.thumbnail)
        Device()->CreateRenderTarget(g_ThumbWidth, g_ThumbHeight, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
            &fs.thumbnail, nullptr);
    fs.captured = fs.thumbnail && backBuffer &&
                  SUCCEEDED(Device()->StretchRect(backBuffer, nullptr, fs.thumbnail, nullptr, D3DTEXF_LINEAR));

    g_Head = (g_Head + 1) % g_Frames.size();
    g_Recorded = std::min(g_Recorded + 1, g_Frames.size() - 1);
    if (HotkeyPressed())
        Dump();
    FrameSlot& next = g_Frames[g_Head];
    next.draws.clear();
    next.notes.clear();
    next.hasConstants = false;
}

} // namespace swrots::d3d
