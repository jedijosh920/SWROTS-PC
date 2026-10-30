// Online play in lockstep (see session.h).

#include "net/session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "game/hvv.h"
#include "input/controls.h"
#include "kernel/clock.h"
#include "kernel/kernel.h"
#include "kernel/sched.h"
#include "net/transport.h"

#include "version.h"

namespace swrots::net {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x4E525753; // 'SWRN'
constexpr uint16_t kProtocol = 1;
constexpr uint16_t kDefaultPort = 27960;
constexpr uint32_t kHistory = 512;      // frames of inputs kept (a power of two)
constexpr uint32_t kMaxSend = 48;       // inputs in one packet at most
constexpr uint32_t kChunk = 1024;       // bytes of the package in one packet
constexpr uint32_t kHashEvery = 60;     // frames between two checks that the games have not parted
constexpr int kSilenceMs = 20000;       // the other PC is gone after that long without a word
constexpr int kConnectMs = 20000;       // the guest gives up calling after that long

enum class Msg : uint8_t {
    Hello = 1, Welcome, Reject, Chunk, ChunkAck, Ready, Start, Started, Input, Ping, Pong, Bye,
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t protocol;
    uint8_t type;
    uint8_t pad;
    uint32_t token; // the session's, chosen by the host (0 in the guest's first Hello)
};
struct HelloMsg {
    Header h;
    uint64_t build;   // the port's binaries
    uint64_t content; // the game's files and the mods
    uint32_t nonce;
    char version[40];
};
struct WelcomeMsg {
    Header h;
    uint32_t nonce;
    uint8_t inputDelay;
    uint8_t fps;
    uint16_t reserved;
    uint32_t packageSize;
    uint64_t packageHash;
};
struct RejectMsg {
    Header h;
    char text[96];
};
struct ChunkMsg {
    Header h;
    uint32_t offset;
    uint16_t size;
    uint8_t data[kChunk];
};
struct ChunkAckMsg {
    Header h;
    uint32_t offset;
};
struct InputMsg {
    Header h;
    uint32_t ack;       // the sender has every frame of the receiver's inputs up to this one
    uint32_t first;     // the first frame of the inputs below
    uint8_t count;
    uint8_t reserved[3];
    uint32_t hashFrame; // a check of the sender's game at the start of that frame (0: none yet)
    uint64_t hash;
    input::GamePad pads[kMaxSend];
};
struct PingMsg {
    Header h;
    uint64_t time;
};
// The host's gameplay settings, which the guest plays with for the match.
struct GameplaySettings {
    int32_t fpsLimit;
    int32_t coopDeath;
    int32_t coopCamera;
    int32_t hvvOverheadInfo;
    uint8_t widescreen, coop, coopStorySafety, coopBoss, coopFriendlyFire, hvvOthersSlowMotion, reserved[2];
    char coopPlayer2[48];
};
#pragma pack(pop)

uint64_t Fnv(const void* data, size_t size, uint64_t h = 0xCBF29CE484222325ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// A fast hash of a large block (the game's memory): 8 bytes at a time.
uint64_t HashBlock(const void* data, size_t size, uint64_t h)
{
    const auto* p = static_cast<const uint64_t*>(data);
    for (size_t i = 0; i < size / 8; ++i)
        h = (h ^ p[i]) * 0x9E3779B97F4A7C15ull + (h >> 29);
    return h;
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data, size_t limit = 64u << 20)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= LONGLONG(limit);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = data.empty() || (ReadFile(f, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size());
    }
    CloseHandle(f);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const uint8_t* data, size_t size)
{
    for (size_t i = 0; (i = path.find(L'\\', i + 1)) != std::wstring::npos;)
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = size == 0 || (WriteFile(f, data, DWORD(size), &written, nullptr) && written == size);
    CloseHandle(f);
    return ok;
}

void DeleteTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteTree(path);
            else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Every file under `dir` (relative paths, sorted), for the content hash and the saves' package.
void ListFiles(const std::wstring& dir, const std::wstring& relative, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + relative + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ListFiles(dir, relative + L"\\" + name, out);
        else
            out.push_back(relative + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
}

uint64_t HashFileQuick(const std::wstring& path, uint64_t h)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return Fnv("missing", 7, h);
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    h = Fnv(&size.QuadPart, 8, h);
    // Small files whole; large ones (the PAKs) by their size, start and end.
    std::vector<uint8_t> buffer(64 * 1024);
    auto readAt = [&](LONGLONG at) {
        LARGE_INTEGER to;
        to.QuadPart = at;
        SetFilePointerEx(f, to, nullptr, FILE_BEGIN);
        DWORD read = 0;
        ReadFile(f, buffer.data(), DWORD(buffer.size()), &read, nullptr);
        h = Fnv(buffer.data(), read, h);
    };
    if (size.QuadPart <= 4 * 1024 * 1024) {
        for (LONGLONG at = 0; at < size.QuadPart; at += LONGLONG(buffer.size()))
            readAt(at);
    } else {
        readAt(0);
        readAt(size.QuadPart / 2);
        readAt(size.QuadPart - LONGLONG(buffer.size()));
    }
    CloseHandle(f);
    return h;
}

// The port's binaries, and the files a match reads (the game's, the mods).
uint64_t BuildHash()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(L"core.dll"), path, MAX_PATH);
    return HashFileQuick(path, Fnv(SWROTS_VERSION, std::strlen(SWROTS_VERSION)));
}

uint64_t ContentHash()
{
    const std::wstring exe = ExeDirectory();
    uint64_t h = 0xCBF29CE484222325ull;
    for (const wchar_t* folder : { L"\\GameData", L"\\mods" }) {
        std::vector<std::wstring> files;
        ListFiles(exe + folder, L"", files);
        for (const std::wstring& f : files) {
            std::wstring lower = f;
            for (wchar_t& c : lower)
                c = wchar_t(towlower(c));
            h = Fnv(lower.data(), lower.size() * sizeof(wchar_t), h);
            h = HashFileQuick(exe + folder + f, h);
        }
    }
    return h;
}

GameplaySettings CurrentGameplay()
{
    const Settings& s = GetSettings();
    GameplaySettings g = {};
    g.fpsLimit = s.fpsLimit;
    g.coopDeath = s.coopDeath;
    g.coopCamera = s.coopCamera;
    g.hvvOverheadInfo = s.hvvOverheadInfo;
    g.widescreen = s.widescreen;
    g.coop = s.coop;
    g.coopStorySafety = s.coopStorySafety;
    g.coopBoss = s.coopBoss;
    g.coopFriendlyFire = s.coopFriendlyFire;
    g.hvvOthersSlowMotion = s.hvvOthersSlowMotion;
    std::snprintf(g.coopPlayer2, sizeof(g.coopPlayer2), "%s", s.coopPlayer2.c_str());
    return g;
}

void ApplyGameplay(const GameplaySettings& g)
{
    Settings& s = EditSettings();
    s.fpsLimit = g.fpsLimit;
    s.coopDeath = g.coopDeath;
    s.coopCamera = g.coopCamera;
    s.hvvOverheadInfo = g.hvvOverheadInfo;
    s.widescreen = g.widescreen != 0;
    s.coop = g.coop != 0;
    s.coopStorySafety = g.coopStorySafety != 0;
    s.coopBoss = g.coopBoss != 0;
    s.coopFriendlyFire = g.coopFriendlyFire != 0;
    s.hvvOthersSlowMotion = g.hvvOthersSlowMotion != 0;
    s.coopPlayer2 = std::string(g.coopPlayer2, strnlen(g.coopPlayer2, sizeof(g.coopPlayer2)));
}

// --- The session's state --------------------------------------------------------------------------------------------

struct Session {
    std::mutex lock;
    std::condition_variable inputs; // a frame's remote input came, or the session ended
    OnlinePhase phase = OnlinePhase::Off;
    bool host = false;
    std::string roomCode, message;

    UdpSocket socket;
    Endpoint peer;                    // the other PC (the guest: the host, once it answered)
    std::vector<Endpoint> candidates; // the guest: the host's addresses to call
    uint32_t token = 0, nonce = 0;
    uint64_t build = 0, content = 0;
    Clock::time_point started, lastHeard, lastSent;

    // The package: the gameplay settings, the start's arguments, the host's profile and saves.
    std::vector<uint8_t> package;
    uint64_t packageHash = 0;
    std::vector<bool> chunksDone;
    bool packageWanted = false; // the host builds it on a game thread (OnlineFrame)
    bool welcomed = false, ready = false, startSeen = false, bootRequested = false;

    // The match.
    uint32_t inputDelay = 3;
    uint32_t fps = 30;
    int local = 0;                        // 0 the host (player 1), 1 the guest (player 2)
    input::GamePad pads[2][kHistory] = {};
    uint32_t padFrame[2][kHistory] = {};  // the frame each slot holds (+1; 0: none)
    uint32_t localLatest = 0;             // the last frame of this PC's input made
    uint32_t remoteContiguous = 0;        // every frame of the other's input up to this one is here
    uint32_t remoteAck = 0;               // the other has every frame of ours up to this one
    uint32_t remoteLatest = 0;
    bool bootPending = false, armed = false;
    uint32_t frame = 0;                   // the frame being played
    uint32_t packets[2] = {};
    uint32_t stalls = 0;
    int pingMs = -1;
    Clock::time_point paceStart;
    uint32_t paceFrame = 0;
    struct Check {
        uint32_t frame;
        uint64_t hash;
    } checks[16] = {};
    Check lastLocal = {}, lastRemote = {};
    bool desync = false;
    uint32_t desyncFrame = 0;

    Settings saved;          // the guest's own settings, given back after the match
    bool settingsSaved = false;
};

Session g_S;
std::thread g_Thread;
std::atomic<bool> g_ThreadStop{ false };

uint64_t NowMs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

Header MakeHeader(Msg type, uint32_t token)
{
    return { kMagic, kProtocol, uint8_t(type), 0, token };
}

template <typename T>
void SendLocked(const T& msg, size_t size = sizeof(T))
{
    g_S.socket.SendTo(g_S.peer, &msg, size);
    g_S.lastSent = Clock::now();
}

void SetMessage(const std::string& text)
{
    if (g_S.message != text) {
        g_S.message = text;
        LOG_INFO("Online: %s", text.c_str());
    }
}

// --- The end ------------------------------------------------------------------------------------------------------------

// The game as before the match: the PC's own inputs, drives and settings, its normal starts.
void RestoreLocked()
{
    input::SetPadSource(nullptr);
    input::SetOnlinePorts(false);
    kernel::SetDeterministicBoot(false);
    kernel::SetDeterministicAddresses(false);
    kernel::SetSessionDrives(std::wstring(), std::wstring());
    if (g_S.settingsSaved) {
        EditSettings() = g_S.saved;
        g_S.settingsSaved = false;
    }
}

void EndLocked(const std::string& why, bool tellOther)
{
    if (g_S.phase == OnlinePhase::Off || g_S.phase == OnlinePhase::Ended)
        return;
    const bool matchOn = g_S.armed || g_S.bootPending;
    if (tellOther && g_S.peer.Valid()) {
        RejectMsg bye = {};
        bye.h = MakeHeader(Msg::Bye, g_S.token);
        std::snprintf(bye.text, sizeof(bye.text), "%s", why.c_str());
        for (int i = 0; i < 3; ++i)
            SendLocked(bye);
    }
    g_S.phase = OnlinePhase::Ended;
    SetMessage(why);
    g_S.armed = false;
    g_S.bootPending = false;
    RestoreLocked();
    g_ThreadStop = true;
    g_S.inputs.notify_all();
    if (matchOn) { // the game back to its title, as a PC alone
        LOG_INFO("Online: the match is over; back to the title");
        kernel::LaunchGame("map: playlicensemovie:0");
    }
}

// --- Inputs -------------------------------------------------------------------------------------------------------------

uint32_t Slot(uint32_t frame) { return frame & (kHistory - 1); }

bool HaveInput(int player, uint32_t frame)
{
    return frame <= g_S.inputDelay || g_S.padFrame[player][Slot(frame)] == frame + 1;
}

const input::GamePad& InputOf(int player, uint32_t frame)
{
    static const input::GamePad neutral = {};
    if (frame <= g_S.inputDelay || g_S.padFrame[player][Slot(frame)] != frame + 1)
        return neutral;
    return g_S.pads[player][Slot(frame)];
}

void StoreInput(int player, uint32_t frame, const input::GamePad& pad)
{
    g_S.pads[player][Slot(frame)] = pad;
    g_S.padFrame[player][Slot(frame)] = frame + 1;
}

void SendInputsLocked()
{
    InputMsg m = {};
    m.h = MakeHeader(Msg::Input, g_S.token);
    m.ack = g_S.remoteContiguous;
    uint32_t first = std::max(g_S.remoteAck + 1, g_S.inputDelay + 1);
    if (g_S.localLatest >= kMaxSend && first + kMaxSend <= g_S.localLatest)
        first = g_S.localLatest - kMaxSend + 1;
    m.first = first;
    uint32_t count = 0;
    for (uint32_t f = first; f <= g_S.localLatest && count < kMaxSend; ++f)
        m.pads[count++] = InputOf(g_S.local, f);
    m.count = uint8_t(count);
    m.hashFrame = g_S.lastLocal.frame;
    m.hash = g_S.lastLocal.hash;
    SendLocked(m, offsetof(InputMsg, pads) + count * sizeof(input::GamePad));
}

void CompareChecksLocked()
{
    const Session::Check& r = g_S.lastRemote;
    if (!r.frame || g_S.desync)
        return;
    for (const Session::Check& c : g_S.checks) {
        if (c.frame != r.frame)
            continue;
        if (c.hash != r.hash) {
            g_S.desync = true;
            g_S.desyncFrame = r.frame;
            LOG_ERROR("Online: the two games parted (desync) at frame %u: %016llX here, %016llX there", r.frame,
                c.hash, r.hash);
            SetMessage("the two games parted (desync) at frame " + std::to_string(r.frame));
        }
        return;
    }
}

void OnInputLocked(const InputMsg& m, size_t size)
{
    const int remote = 1 - g_S.local;
    g_S.remoteAck = std::max(g_S.remoteAck, m.ack);
    const uint32_t count = std::min<uint32_t>(m.count, uint32_t((size - offsetof(InputMsg, pads)) / sizeof(input::GamePad)));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t f = m.first + i;
        if (f + kHistory / 2 < g_S.frame || f > g_S.frame + kHistory / 2)
            continue;
        if (!HaveInput(remote, f))
            StoreInput(remote, f, m.pads[i]);
        g_S.remoteLatest = std::max(g_S.remoteLatest, f);
    }
    while (HaveInput(remote, g_S.remoteContiguous + 1) && g_S.remoteContiguous + 1 <= g_S.remoteLatest)
        ++g_S.remoteContiguous;
    if (m.hashFrame > g_S.lastRemote.frame) {
        g_S.lastRemote = { m.hashFrame, m.hash };
        CompareChecksLocked();
    }
    g_S.inputs.notify_all();
}

bool PadFromSession(int port, input::GamePad& pad, uint32_t& packet)
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    if (!g_S.armed)
        return false;
    if (port == 0 || port == 1) {
        pad = InputOf(port, g_S.frame);
        packet = g_S.packets[port];
    } else {
        pad = {};
        packet = 0;
    }
    return true;
}

// --- The package (host -> guest) ---------------------------------------------------------------------------------------

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(uint8_t(x >> (8 * i)));
}

uint32_t GetU32(const uint8_t*& p) { const uint32_t x = p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; p += 4; return x; }

// On a game thread (the profile is saved by the game's own code).
void BuildPackageLocked()
{
    std::string args = "map: v_shellBetweenLevels:0 v_versusMode:0 v_bonusMissionMode:0 v_challengeMissionMode:0 "
                       "v_checkpointIndex:-1 playlicensemovie:0";
    std::vector<std::pair<std::wstring, std::vector<uint8_t>>> files;
    int controller = 0, memoryUnit = 0;
    if (game::SaveProfileForStart(controller, memoryUnit)) {
        std::vector<uint8_t> persist;
        if (ReadWholeFile(kernel::UtilityDrivePath() + L"\\persist.tmp", persist)) {
            files.push_back({ L"utility\\persist.tmp", std::move(persist) });
            args += " v_sessionCreated:1 title:0 controller:0 memoryunit:" + std::to_string(memoryUnit);
        }
    } else {
        LOG_WARN("Online: no profile to bring along (the match starts at the title)");
    }
    const std::wstring saves = kernel::SaveDrivePath();
    std::vector<std::wstring> list;
    ListFiles(saves, L"", list);
    size_t total = 0;
    for (const std::wstring& f : list) {
        std::vector<uint8_t> data;
        if (!ReadWholeFile(saves + f, data, 8u << 20))
            continue;
        total += data.size();
        if (total > (32u << 20)) {
            LOG_WARN("Online: the saves are too large to bring along whole");
            break;
        }
        files.push_back({ L"saves" + f, std::move(data) });
    }
    std::vector<uint8_t>& p = g_S.package;
    p.clear();
    const GameplaySettings g = CurrentGameplay();
    PutU32(p, 1); // the package's format
    p.insert(p.end(), reinterpret_cast<const uint8_t*>(&g), reinterpret_cast<const uint8_t*>(&g) + sizeof(g));
    PutU32(p, uint32_t(args.size()));
    p.insert(p.end(), args.begin(), args.end());
    PutU32(p, uint32_t(files.size()));
    for (const auto& [name, data] : files) {
        const std::string narrow(name.begin(), name.end());
        PutU32(p, uint32_t(narrow.size()));
        p.insert(p.end(), narrow.begin(), narrow.end());
        PutU32(p, uint32_t(data.size()));
        p.insert(p.end(), data.begin(), data.end());
    }
    g_S.packageHash = Fnv(p.data(), p.size());
    g_S.chunksDone.assign((p.size() + kChunk - 1) / kChunk, false);
    LOG_INFO("Online: the match's package: %zu files, %zu bytes; start with '%s'", files.size(), p.size(), args.c_str());
}

// Writes the package's files into the match's own drives; its settings and arguments out.
bool UnpackLocked(GameplaySettings& g, std::string& args, const std::wstring& root)
{
    const std::vector<uint8_t>& v = g_S.package;
    const uint8_t* p = v.data();
    const uint8_t* end = p + v.size();
    if (v.size() < 4 + sizeof(g) + 8 || GetU32(p) != 1)
        return false;
    std::memcpy(&g, p, sizeof(g));
    p += sizeof(g);
    const uint32_t argsSize = GetU32(p);
    if (p + argsSize > end)
        return false;
    args.assign(reinterpret_cast<const char*>(p), argsSize);
    p += argsSize;
    const uint32_t count = GetU32(p);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 4 > end)
            return false;
        const uint32_t nameSize = GetU32(p);
        if (p + nameSize + 4 > end)
            return false;
        std::string name(reinterpret_cast<const char*>(p), nameSize);
        p += nameSize;
        const uint32_t size = GetU32(p);
        if (p + size > end || name.find("..") != std::string::npos)
            return false;
        if (!WriteWholeFile(root + L"\\" + std::wstring(name.begin(), name.end()), p, size))
            return false;
        p += size;
    }
    return true;
}

// --- The start of the match (both PCs, on a game thread) -------------------------------------------------------------

void BeginBootLocked()
{
    wchar_t instance[64] = {}; // SWROTS_INSTANCE: a second copy of the game on this PC (a test) has its own folder
    GetEnvironmentVariableW(L"SWROTS_INSTANCE", instance, 64);
    const std::wstring root = ExeDirectory() + L"\\netplay\\online" + (instance[0] ? std::wstring(L".") + instance : L"");
    CreateDirectoryW((ExeDirectory() + L"\\netplay").c_str(), nullptr);
    DeleteTree(root);
    CreateDirectoryW(root.c_str(), nullptr);
    GameplaySettings g = {};
    std::string args;
    if (!UnpackLocked(g, args, root)) {
        EndLocked("the match's package could not be unpacked", true);
        return;
    }
    if (!g_S.host) {
        g_S.saved = GetSettings();
        g_S.settingsSaved = true;
        ApplyGameplay(g);
    }
    g_S.fps = uint32_t(std::clamp(g.fpsLimit, 10, 120));
    kernel::SetSessionDrives(root + L"\\saves", root + L"\\utility");
    input::SetOnlinePorts(true);
    input::SetPadSource(&PadFromSession);
    if (!kernel::SetDeterministicAddresses(true)) {
        EndLocked("this PC's memory has no room at the match's fixed addresses", true);
        return;
    }
    kernel::SetDeterministicBoot(true);
    g_S.bootPending = true;
    g_S.phase = OnlinePhase::Starting;
    SetMessage("starting the match");
    kernel::LaunchGame(args);
}

// --- The network thread -------------------------------------------------------------------------------------------------

void HandleLocked(const uint8_t* data, size_t size, const Endpoint& from)
{
    if (size < sizeof(Header))
        return;
    Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic)
        return;
    const Msg type = Msg(h.type);
    if (h.protocol != kProtocol) {
        if (g_S.host && type == Msg::Hello) {
            RejectMsg r = {};
            r.h = MakeHeader(Msg::Reject, 0);
            std::snprintf(r.text, sizeof(r.text), "the host runs another version of the port (%s)", SWROTS_VERSION);
            g_S.socket.SendTo(from, &r, sizeof(r));
        }
        return;
    }
    if (g_S.host) {
        if (type == Msg::Hello && size >= sizeof(HelloMsg)) {
            HelloMsg m;
            std::memcpy(&m, data, sizeof(m));
            if (g_S.peer.Valid() && from != g_S.peer)
                return; // a second guest: the room is full (it hears nothing and gives up)
            auto reject = [&](const std::string& text) {
                RejectMsg r = {};
                r.h = MakeHeader(Msg::Reject, 0);
                std::snprintf(r.text, sizeof(r.text), "%s", text.c_str());
                g_S.socket.SendTo(from, &r, sizeof(r));
            };
            if (m.build != g_S.build) {
                reject(std::string("the host runs another build of the port (") + SWROTS_VERSION + ")");
                return;
            }
            if (m.content != g_S.content) {
                reject("the host's game files or mods differ from yours");
                return;
            }
            if (!g_S.peer.Valid()) {
                g_S.peer = from;
                g_S.nonce = m.nonce;
                g_S.lastHeard = Clock::now();
                g_S.phase = OnlinePhase::Preparing;
                g_S.packageWanted = true;
                SetMessage("a player joined from " + from.ToString() + "; preparing the match");
            }
            if (g_S.welcomed) {
                WelcomeMsg w = {};
                w.h = MakeHeader(Msg::Welcome, g_S.token);
                w.nonce = g_S.nonce;
                w.inputDelay = uint8_t(g_S.inputDelay);
                w.fps = uint8_t(g_S.fps);
                w.packageSize = uint32_t(g_S.package.size());
                w.packageHash = g_S.packageHash;
                SendLocked(w);
            }
            return;
        }
        if (from != g_S.peer || h.token != g_S.token)
            return;
        g_S.lastHeard = Clock::now();
        switch (type) {
        case Msg::ChunkAck:
            if (size >= sizeof(ChunkAckMsg)) {
                ChunkAckMsg a;
                std::memcpy(&a, data, sizeof(a));
                const size_t i = a.offset / kChunk;
                if (i < g_S.chunksDone.size())
                    g_S.chunksDone[i] = true;
            }
            break;
        case Msg::Ready:
            if (!g_S.ready) {
                g_S.ready = true;
                g_S.bootRequested = true; // at the next frame, on the game thread
                SetMessage("the guest is ready");
            }
            break;
        case Msg::Started:
            g_S.startSeen = true;
            break;
        default:
            break;
        }
    } else {
        bool fromHost = g_S.peer.Valid() ? from == g_S.peer
                                         : std::find(g_S.candidates.begin(), g_S.candidates.end(), from) != g_S.candidates.end();
        if (!fromHost)
            return;
        if (type == Msg::Reject && size >= sizeof(RejectMsg)) {
            RejectMsg r;
            std::memcpy(&r, data, sizeof(r));
            r.text[sizeof(r.text) - 1] = 0;
            EndLocked(std::string("the host refused: ") + r.text, false);
            return;
        }
        if (type == Msg::Welcome && size >= sizeof(WelcomeMsg)) {
            WelcomeMsg w;
            std::memcpy(&w, data, sizeof(w));
            if (w.nonce != g_S.nonce || g_S.welcomed)
                return;
            g_S.peer = from;
            g_S.token = w.h.token;
            g_S.inputDelay = std::clamp<uint32_t>(w.inputDelay, 1, 10);
            g_S.fps = w.fps ? w.fps : 30;
            g_S.package.assign(w.packageSize, 0);
            g_S.packageHash = w.packageHash;
            g_S.chunksDone.assign((w.packageSize + kChunk - 1) / kChunk, false);
            g_S.welcomed = true;
            g_S.lastHeard = Clock::now();
            g_S.phase = OnlinePhase::Preparing;
            SetMessage("connected to " + from.ToString() + "; receiving the host's profile");
            return;
        }
        if (!g_S.welcomed || h.token != g_S.token)
            return;
        g_S.lastHeard = Clock::now();
        switch (type) {
        case Msg::Chunk:
            if (size >= offsetof(ChunkMsg, data)) {
                ChunkMsg c;
                std::memcpy(&c, data, std::min(size, sizeof(c)));
                const size_t i = c.offset / kChunk;
                if (i < g_S.chunksDone.size() && c.offset + c.size <= g_S.package.size() &&
                    offsetof(ChunkMsg, data) + c.size <= size) {
                    std::memcpy(g_S.package.data() + c.offset, c.data, c.size);
                    g_S.chunksDone[i] = true;
                }
                ChunkAckMsg a = {};
                a.h = MakeHeader(Msg::ChunkAck, g_S.token);
                a.offset = c.offset;
                SendLocked(a);
                if (!g_S.ready && std::all_of(g_S.chunksDone.begin(), g_S.chunksDone.end(), [](bool b) { return b; })) {
                    if (Fnv(g_S.package.data(), g_S.package.size()) != g_S.packageHash) {
                        EndLocked("the host's profile came damaged", true);
                        return;
                    }
                    g_S.ready = true;
                    SetMessage("the host's profile is here; waiting for the start");
                }
            }
            break;
        case Msg::Start:
            if (g_S.ready && !g_S.startSeen) {
                g_S.startSeen = true;
                g_S.bootRequested = true;
            }
            if (g_S.startSeen) {
                Header s = MakeHeader(Msg::Started, g_S.token);
                SendLocked(s);
            }
            break;
        default:
            break;
        }
    }
    // Both ways.
    switch (type) {
    case Msg::Input:
        if (size >= offsetof(InputMsg, pads))
            OnInputLocked(*reinterpret_cast<const InputMsg*>(data), size);
        break;
    case Msg::Ping:
        if (size >= sizeof(PingMsg)) {
            PingMsg p;
            std::memcpy(&p, data, sizeof(p));
            p.h.type = uint8_t(Msg::Pong);
            SendLocked(p);
        }
        break;
    case Msg::Pong:
        if (size >= sizeof(PingMsg)) {
            PingMsg p;
            std::memcpy(&p, data, sizeof(p));
            const int rtt = int(NowMs() - p.time);
            g_S.pingMs = g_S.pingMs < 0 ? rtt : (g_S.pingMs * 7 + rtt) / 8;
        }
        break;
    case Msg::Bye:
        if (size >= sizeof(RejectMsg)) {
            RejectMsg r;
            std::memcpy(&r, data, sizeof(r));
            r.text[sizeof(r.text) - 1] = 0;
            EndLocked(std::string("the other player left (") + r.text + ")", false);
        }
        break;
    default:
        break;
    }
}

// What the network thread sends on its own, a few times a second.
void TickLocked()
{
    const auto now = Clock::now();
    const auto since = [&](Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count(); };
    if (g_S.phase == OnlinePhase::Opening || g_S.phase == OnlinePhase::Waiting || g_S.phase == OnlinePhase::Off ||
        g_S.phase == OnlinePhase::Ended)
        return;
    if (!g_S.host && !g_S.welcomed) { // the guest calls the host on each of its addresses
        if (since(g_S.started) > kConnectMs) {
            EndLocked("no answer from the host (a wrong code, or its router lets nothing in: see the port's online "
                      "help about the port to open)", false);
            return;
        }
        HelloMsg m = {};
        m.h = MakeHeader(Msg::Hello, 0);
        m.build = g_S.build;
        m.content = g_S.content;
        m.nonce = g_S.nonce;
        std::snprintf(m.version, sizeof(m.version), "%s", SWROTS_VERSION);
        for (const Endpoint& e : g_S.candidates)
            g_S.socket.SendTo(e, &m, sizeof(m));
        return;
    }
    if (since(g_S.lastHeard) > kSilenceMs) {
        EndLocked("the other player stopped answering", false);
        return;
    }
    if (g_S.host && g_S.welcomed && !g_S.ready) { // the package, the chunks not acknowledged yet
        int sent = 0;
        for (size_t i = 0; i < g_S.chunksDone.size() && sent < 64; ++i) {
            if (g_S.chunksDone[i])
                continue;
            ChunkMsg c = {};
            c.h = MakeHeader(Msg::Chunk, g_S.token);
            c.offset = uint32_t(i * kChunk);
            c.size = uint16_t(std::min<size_t>(kChunk, g_S.package.size() - c.offset));
            std::memcpy(c.data, g_S.package.data() + c.offset, c.size);
            SendLocked(c, offsetof(ChunkMsg, data) + c.size);
            ++sent;
        }
    }
    if (!g_S.host && g_S.ready && !g_S.startSeen) {
        Header r = MakeHeader(Msg::Ready, g_S.token);
        SendLocked(r);
    }
    if (g_S.host && g_S.ready && !g_S.startSeen) {
        Header s = MakeHeader(Msg::Start, g_S.token);
        SendLocked(s);
    }
    if (g_S.peer.Valid()) {
        PingMsg p = {};
        p.h = MakeHeader(Msg::Ping, g_S.token);
        p.time = NowMs();
        SendLocked(p);
    }
    if (g_S.armed || g_S.bootPending)
        SendInputsLocked(); // the inputs again (one could have been lost), and the check
}

void NetThread(bool host, uint16_t port)
{
    uint8_t buffer[2048];
    if (host) {
        // The public address: STUN (on by default: [Online] Stun=0 in settings.ini, or SWROTS_ONLINE_STUN=0, off).
        Endpoint publicAddress, localAddress{ LocalAddress(), port };
        wchar_t ini[MAX_PATH];
        std::wstring iniPath = ExeDirectory() + L"\\settings.ini";
        bool stun = GetPrivateProfileIntW(L"Online", L"Stun", 1, iniPath.c_str()) != 0;
        char env[16] = "";
        if (GetEnvironmentVariableA("SWROTS_ONLINE_STUN", env, sizeof(env)) && env[0] == '0')
            stun = false;
        GetPrivateProfileStringW(L"Online", L"StunServer", L"stun.l.google.com:19302", ini, MAX_PATH, iniPath.c_str());
        Endpoint server;
        const std::wstring serverName = ini;
        if (stun && Resolve(std::string(serverName.begin(), serverName.end()), 3478, server)) {
            if (!StunPublicAddress(g_S.socket, server, publicAddress, 3000))
                LOG_WARN("Online: the STUN server did not answer; the room code holds this PC's local address only");
        }
        std::lock_guard<std::mutex> lock(g_S.lock);
        if (g_S.phase != OnlinePhase::Opening)
            return;
        if (!publicAddress.Valid())
            publicAddress = localAddress;
        g_S.roomCode = MakeRoomCode(publicAddress, localAddress);
        g_S.phase = OnlinePhase::Waiting;
        SetMessage("room open: code " + g_S.roomCode + " (public " + publicAddress.ToString() + ", local " +
            localAddress.ToString() + ")");
    }
    auto lastTick = Clock::now() - std::chrono::seconds(1);
    while (!g_ThreadStop) {
        Endpoint from;
        const size_t n = g_S.socket.Receive(buffer, sizeof(buffer), from, 20);
        std::lock_guard<std::mutex> lock(g_S.lock);
        if (g_ThreadStop)
            break;
        if (n)
            HandleLocked(buffer, n, from);
        const auto now = Clock::now();
        if (now - lastTick >= std::chrono::milliseconds(g_S.armed ? 100 : 250)) {
            lastTick = now;
            TickLocked();
        }
    }
}

void StopThread()
{
    g_ThreadStop = true;
    if (g_Thread.joinable() && g_Thread.get_id() != std::this_thread::get_id())
        g_Thread.join();
    else if (g_Thread.joinable())
        g_Thread.detach();
    g_ThreadStop = false;
}

void ResetSessionLocked(bool host)
{
    g_S.host = host;
    g_S.roomCode.clear();
    g_S.message.clear();
    g_S.peer = {};
    g_S.candidates.clear();
    g_S.package.clear();
    g_S.chunksDone.clear();
    g_S.packageWanted = g_S.welcomed = g_S.ready = g_S.startSeen = g_S.bootRequested = false;
    g_S.bootPending = g_S.armed = false;
    g_S.local = host ? 0 : 1;
    std::memset(g_S.padFrame, 0, sizeof(g_S.padFrame));
    g_S.localLatest = g_S.remoteContiguous = g_S.remoteAck = g_S.remoteLatest = 0;
    g_S.frame = 0;
    g_S.packets[0] = g_S.packets[1] = 0;
    g_S.stalls = 0;
    g_S.pingMs = -1;
    std::memset(g_S.checks, 0, sizeof(g_S.checks));
    g_S.lastLocal = g_S.lastRemote = {};
    g_S.desync = false;
    g_S.started = g_S.lastHeard = Clock::now();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    g_S.nonce = uint32_t(counter.QuadPart) ^ GetCurrentProcessId() * 2654435761u;
    g_S.token = host ? (g_S.nonce * 2246822519u) | 1 : 0;
}

bool Prepare(bool host, std::string& error)
{
    StopThread();
    {
        std::lock_guard<std::mutex> lock(g_S.lock);
        if (g_S.phase != OnlinePhase::Off && g_S.phase != OnlinePhase::Ended) {
            error = "a session is already on";
            return false;
        }
    }
    if (!NetworkInit()) {
        error = "the network could not start";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_S.lock);
    ResetSessionLocked(host);
    g_S.build = BuildHash();
    g_S.content = ContentHash();
    g_S.inputDelay = 3;
    g_S.fps = uint32_t(std::clamp(GetSettings().fpsLimit, 10, 120));
    return true;
}

} // namespace

bool OnlineHost()
{
    std::string error;
    if (!Prepare(true, error)) {
        LOG_WARN("Online: %s", error.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lock(g_S.lock);
    const std::wstring ini = ExeDirectory() + L"\\settings.ini";
    const uint16_t wanted = uint16_t(GetPrivateProfileIntW(L"Online", L"Port", kDefaultPort, ini.c_str()));
    bool open = false;
    for (uint16_t p = wanted; p < wanted + 10 && !open; ++p)
        open = g_S.socket.Open(p);
    if (!open) {
        g_S.phase = OnlinePhase::Ended;
        SetMessage("no UDP port free to host on");
        return false;
    }
    g_S.phase = OnlinePhase::Opening;
    SetMessage("opening a room on UDP port " + std::to_string(g_S.socket.LocalPort()));
    g_Thread = std::thread(NetThread, true, g_S.socket.LocalPort());
    return true;
}

bool OnlineJoin(const std::string& codeOrAddress)
{
    std::string error;
    if (!Prepare(false, error)) {
        LOG_WARN("Online: %s", error.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lock(g_S.lock);
    Endpoint publicAddress, localAddress, direct;
    if (ReadRoomCode(codeOrAddress, publicAddress, localAddress)) {
        if (localAddress.Valid())
            g_S.candidates.push_back(localAddress);
        if (publicAddress.Valid() && publicAddress != localAddress)
            g_S.candidates.push_back(publicAddress);
    } else if (Resolve(codeOrAddress, kDefaultPort, direct)) {
        g_S.candidates.push_back(direct);
    } else {
        g_S.phase = OnlinePhase::Ended;
        SetMessage("that room code is not right (mistyped?)");
        return false;
    }
    if (!g_S.socket.Open(0)) {
        g_S.phase = OnlinePhase::Ended;
        SetMessage("no UDP port free");
        return false;
    }
    g_S.phase = OnlinePhase::Connecting;
    std::string list;
    for (const Endpoint& e : g_S.candidates)
        list += " " + e.ToString();
    SetMessage("calling the host at" + list);
    g_Thread = std::thread(NetThread, false, uint16_t(0));
    return true;
}

void OnlineLeave()
{
    {
        std::lock_guard<std::mutex> lock(g_S.lock);
        EndLocked("left", true);
        g_S.phase = OnlinePhase::Off;
    }
    StopThread();
    g_S.socket.Close();
}

OnlineStatusInfo OnlineStatus()
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    OnlineStatusInfo i;
    i.phase = g_S.phase;
    i.host = g_S.host;
    i.roomCode = g_S.roomCode;
    i.message = g_S.message;
    i.frame = g_S.frame;
    i.inputDelay = g_S.inputDelay;
    i.pingMs = g_S.pingMs;
    i.stalls = g_S.stalls;
    i.desync = g_S.desync;
    return i;
}

std::string OnlineStatusLine()
{
    const OnlineStatusInfo i = OnlineStatus();
    static const char* const names[] = { "off", "opening", "waiting", "connecting", "preparing", "starting", "playing", "ended" };
    char line[512];
    std::snprintf(line, sizeof(line), "online: %s (%s)%s%s, frame %u, delay %u, ping %d ms, %u stall(s)%s: %s",
        names[int(i.phase)], i.host ? "host" : "guest", i.roomCode.empty() ? "" : ", code ", i.roomCode.c_str(),
        i.frame, i.inputDelay, i.pingMs, i.stalls, i.desync ? ", DESYNC" : "", i.message.c_str());
    return line;
}

bool OnlineActive()
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    return g_S.armed;
}

void OnlineReboot()
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    if (!g_S.bootPending)
        return; // a level change in a match: the frames go on
    g_S.bootPending = false;
    g_S.armed = true;
    g_S.frame = 0;
    g_S.localLatest = g_S.inputDelay; // frames up to the delay are neutral on both PCs
    g_S.remoteContiguous = std::max(g_S.remoteContiguous, g_S.inputDelay); // the other may have started first
    g_S.paceFrame = 0;
    g_S.paceStart = Clock::now();
    g_S.phase = OnlinePhase::Playing;
    g_S.lastHeard = Clock::now();
    SetMessage(std::string("the match starts: player ") + (g_S.host ? "1 (host)" : "2 (guest)") + ", input delay " +
        std::to_string(g_S.inputDelay) + " frames");
}

void OnlineFrame()
{
    std::unique_lock<std::mutex> lock(g_S.lock);
    if (g_S.packageWanted && g_S.host) { // the host's profile, saved by the game's code
        g_S.packageWanted = false;
        BuildPackageLocked();
        g_S.welcomed = true; // the next Hello is answered with the Welcome
        WelcomeMsg w = {};
        w.h = MakeHeader(Msg::Welcome, g_S.token);
        w.nonce = g_S.nonce;
        w.inputDelay = uint8_t(g_S.inputDelay);
        w.fps = uint8_t(g_S.fps);
        w.packageSize = uint32_t(g_S.package.size());
        w.packageHash = g_S.packageHash;
        SendLocked(w);
        SetMessage("sending the profile to the guest");
    }
    if (g_S.bootRequested) {
        g_S.bootRequested = false;
        BeginBootLocked();
        return;
    }
    if (!g_S.armed)
        return;

    // The frame that ends here was g_S.frame; the next one is played with both players' inputs for it.
    const uint32_t next = g_S.frame + 1;
    // This PC's input, `inputDelay` frames ahead.
    const uint32_t ahead = next + g_S.inputDelay;
    if (ahead > g_S.localLatest) {
        input::GamePad pad;
        input::ReadLivePad(0, pad);
        for (uint32_t f = g_S.localLatest + 1; f <= ahead; ++f)
            StoreInput(g_S.local, f, pad);
        g_S.localLatest = ahead;
    }
    // A check of the game's course at the start of every kHashEvery-th frame (the same point on both PCs).
    if (next % kHashEvery == 0) {
        uint64_t h = kernel::SchedTraceHash();
        h = HashBlock(reinterpret_cast<const void*>(uintptr_t(0x00612BE0)), 0x00962BF8 - 0x00612BE0, h);
        g_S.checks[(next / kHashEvery) % std::size(g_S.checks)] = { next, h };
        g_S.lastLocal = { next, h };
        if (next % (kHashEvery * 10) == 0)
            LOG_INFO("Online: frame %u, check %016llX, %u stall(s), ping %d ms", next, h, g_S.stalls, g_S.pingMs);
        CompareChecksLocked();
    }
    SendInputsLocked();

    // The other's input for the next frame.
    const int remote = 1 - g_S.local;
    if (!HaveInput(remote, next)) {
        ++g_S.stalls;
        const auto waitStart = Clock::now();
        bool told = false;
        while (g_S.armed && !HaveInput(remote, next)) {
            g_S.inputs.wait_for(lock, std::chrono::milliseconds(50));
            if (!told && Clock::now() - waitStart > std::chrono::seconds(1)) {
                told = true;
                SetMessage("waiting for the other player");
            }
            if (g_S.armed && !HaveInput(remote, next) && Clock::now() - waitStart > std::chrono::milliseconds(100))
                SendInputsLocked();
        }
        if (!g_S.armed)
            return; // the session ended meanwhile
        if (told)
            SetMessage("playing");
        g_S.paceStart = Clock::now(); // the wait shifted the pace
        g_S.paceFrame = next;
    }
    // The packet numbers (they move on when a pad changes), then the frame.
    for (int p = 0; p < 2; ++p)
        if (std::memcmp(&InputOf(p, next), &InputOf(p, next - 1), sizeof(input::GamePad)) != 0)
            ++g_S.packets[p];
    g_S.frame = next;

    // The pace: frames at the match's rate; a little slower while this PC is ahead of the other.
    const auto period = std::chrono::microseconds(1000000 / g_S.fps);
    const uint32_t remoteFrame = g_S.remoteLatest > g_S.inputDelay ? g_S.remoteLatest - g_S.inputDelay : 0;
    if (next > remoteFrame + 1)
        g_S.paceStart += period / 4; // this PC is ahead: it slows down until the other catches up
    const auto target = g_S.paceStart + period * (next - g_S.paceFrame);
    lock.unlock();
    const auto now = Clock::now();
    if (target > now + period * 4 || now > target + period * 4) { // far off (a load): the pace starts again here
        std::lock_guard<std::mutex> relock(g_S.lock);
        g_S.paceStart = now;
        g_S.paceFrame = next;
        return;
    }
    while (Clock::now() < target) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(target - Clock::now()).count();
        if (left > 2)
            Sleep(DWORD(left - 1));
        else
            SwitchToThread();
    }
}

} // namespace swrots::net
