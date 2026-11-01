// Online play in lockstep (see session.h).

#include "net/session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "game/characters.h"
#include "game/coop.h"
#include "game/hvv.h"
#include "game/hvvmenu.h"
#include "game/menus.h"
#include "game/onlinemenu.h"
#include "input/controls.h"
#include "kernel/clock.h"
#include "kernel/kernel.h"
#include "kernel/sched.h"
#include "net/duelstate.h"
#include "net/transport.h"
#include "net/upnp.h"

#include "version.h"

namespace swrots::net {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x4E525753; // 'SWRN'
constexpr uint16_t kProtocol = 3; // 2: the players' names; 3: client/server matches
constexpr uint16_t kDefaultPort = 27960;
constexpr uint32_t kHistory = 512;      // frames of inputs kept (a power of two)
constexpr uint32_t kMaxSend = 48;       // inputs in one packet at most
constexpr uint32_t kChunk = 1024;       // bytes of the package in one packet
constexpr uint32_t kHashEveryDefault = 60; // frames between two checks that the games have not parted

// SWROTS_ONLINE_CHECK_EVERY=<frames> (diagnostics): checks more often, to find the first frame that parts.
// [Online] Mode: "server" (the default: the host's game is the match, the guest's follows its state) or "lockstep"
// (both games computed alike, the deterministic scheduler). SWROTS_ONLINE_MODE overrides it.
bool ServerModeWanted()
{
    char v[32] = "";
    if (!GetEnvironmentVariableA("SWROTS_ONLINE_MODE", v, sizeof(v))) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring ini = path;
        ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\settings.ini";
        wchar_t w[32] = L"";
        GetPrivateProfileStringW(L"Online", L"Mode", L"server", w, 32, ini.c_str());
        for (int i = 0; i < 31 && w[i]; ++i)
            v[i] = char(w[i]);
    }
    return _stricmp(v, "lockstep") != 0;
}

uint32_t HashEvery()
{
    static const uint32_t every = [] {
        char v[16] = "";
        const uint32_t n = GetEnvironmentVariableA("SWROTS_ONLINE_CHECK_EVERY", v, sizeof(v)) ? uint32_t(std::atoi(v)) : 0;
        return n ? n : kHashEveryDefault;
    }();
    return every;
}
constexpr int kSilenceMs = 20000;       // the other PC is gone after that long without a word
constexpr int kConnectMs = 20000;       // the guest gives up calling after that long

enum class Msg : uint8_t {
    Hello = 1, Welcome, Reject, Chunk, ChunkAck, Ready, Start, Started, Input, Ping, Pong, Bye, Pages,
    Pad, Launch, State, Room, // client/server
};

constexpr uint32_t kPage = 1024;      // a desync's report: the data checked, hashed by 1 KiB pages
constexpr uint32_t kPagesPerMsg = 256;
#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t protocol;
    uint8_t type;
    uint8_t pad;
    uint32_t token; // the session's, chosen by the host (0 in the guest's first Hello)
};
// A desync's report: the hashes of the data a PC checked at a frame, page by page, so that each PC logs which pages
// (which addresses of the game's data) differ.
struct PagesMsg {
    Header h;
    uint32_t frame;
    uint64_t trace;     // the scheduler's course hash at that frame
    uint16_t first;     // the first page in this message
    uint16_t count;
    uint32_t hashes[kPagesPerMsg];
};
struct HelloMsg {
    Header h;
    uint64_t build;   // the port's binaries
    uint64_t content; // the game's files and the mods
    uint32_t nonce;
    char version[40];
    char name[24];    // the guest's player name ([Online] Name)
};
struct WelcomeMsg {
    Header h;
    uint32_t nonce;
    uint8_t inputDelay;
    uint8_t fps;
    uint16_t reserved;
    uint32_t packageSize;
    uint64_t packageHash;
    char name[24];    // the host's player name
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
// Client/server: a player's pad now (both ways, every frame).
struct PadMsg {
    Header h;
    uint32_t seq;
    input::GamePad pad;
    uint8_t inDuel;   // the sender's game is in a duel (the host waits for the guest's at a duel's start)
    uint32_t screen;  // the front-end screen the sender's game shows (the guest's follows the host's), 0 none
};
// Client/server: the host's room (its settings), a few times a second.
struct RoomMsg {
    Header h;
    OnlineRoomSettings room;
};
// Client/server: the host's game starts a level (or the menus): the guest's follows.
struct LaunchMsg {
    Header h;
    uint32_t id;
    char args[1024];
};
// Client/server: the host's duel, every frame: both fighters' gameplay state.
struct StateMsg {
    Header h;
    uint32_t seq;
    uint32_t hostFrame;
    FighterState fighters[2];
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

// The game's data checked between the PCs, less what the port writes there for this PC alone (not the match's
// course): the button prompts' tip table (game/prompts.cpp: the keyboard's caps on a PC playing with the keyboard).
uint64_t HashGameData(const uint8_t* data, size_t size, uint64_t h)
{
    static const struct { uint32_t address, size; } kLocal[] = { { 0x007F0608, 18 * 16 } };
    size_t at = 0;
    for (const auto& r : kLocal) {
        const size_t begin = r.address - 0x00612BE0, end = begin + r.size;
        if (begin < at || end > size)
            continue;
        h = HashBlock(data + at, (begin - at) & ~size_t(7), h);
        at = (end + 7) & ~size_t(7);
    }
    return HashBlock(data + at, size - at, h);
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

uint64_t HashFileQuick(const std::wstring& path, uint64_t h, uint64_t wholeUpTo = 4 * 1024 * 1024)
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
    if (uint64_t(size.QuadPart) <= wholeUpTo) {
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

// The game's files by their names and sizes (and the small ones whole: the settings files); the mods' by their
// contents. Computed once (a few seconds at most), on the network thread.
// The game's files and the mods, as both PCs must have them: GameData by name and size (its small files whole), the
// mods whole (not their notes: .txt, .md). Each file's own hash goes to netplay\\content.txt, to compare two PCs'
// when one refuses the other.
uint64_t ContentHash()
{
    static uint64_t cached = 0;
    if (cached)
        return cached;
    const std::wstring exe = ExeDirectory();
    uint64_t h = 0xCBF29CE484222325ull;
    std::string manifest;
    for (const wchar_t* folder : { L"\\GameData" }) { // matches are played without mods (SetSessionFolders)
        const bool mods = folder[1] == L'm';
        std::vector<std::wstring> files;
        ListFiles(exe + folder, L"", files);
        for (const std::wstring& f : files) {
            std::wstring lower = f;
            for (wchar_t& c : lower)
                c = wchar_t(towlower(c));
            if (mods && ((lower.size() > 4 && (lower.compare(lower.size() - 4, 4, L".txt") == 0 ||
                                                  lower.compare(lower.size() - 3, 3, L".md") == 0)) ||
                            lower.rfind(L"\\_", 0) == 0 || lower.find(L"\\__pycache__\\") != std::wstring::npos))
                continue; // a mod's notes, and work folders (mods\\_work...) the game never reads
            uint64_t one = 0xCBF29CE484222325ull;
            WIN32_FILE_ATTRIBUTE_DATA a = {};
            GetFileAttributesExW((exe + folder + f).c_str(), GetFileExInfoStandard, &a);
            const uint64_t size = uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow;
            if (mods || size <= 64 * 1024)
                one = HashFileQuick(exe + folder + f, one);
            else
                one = Fnv(&size, 8, one);
            h = Fnv(lower.data(), lower.size() * sizeof(wchar_t), h);
            h = Fnv(&one, 8, h);
            char line[64];
            std::snprintf(line, sizeof(line), " %llu %016llX\n", (unsigned long long)size, (unsigned long long)one);
            for (wchar_t c : std::wstring(folder + 1) + lower)
                manifest += c < 128 ? char(c) : '?';
            manifest += line;
        }
    }
    CreateDirectoryW((exe + L"\\netplay").c_str(), nullptr);
    WriteWholeFile(exe + L"\\netplay\\content.txt", reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size());
    cached = h;
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
    std::string localName, remoteName; // the players' names ([Online] Name: the Windows user name by default)
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
    uint64_t paceClock = 0; // the fixed clock at paceStart: the match runs at the game's own time
    struct Check {
        uint32_t frame;
        uint64_t hash;
    } checks[16] = {};
    Check lastLocal = {}, lastRemote = {};
    bool desync = false;
    uint32_t desyncFrame = 0;
    Clock::time_point endAt = {};      // a desync's end: after the reports were swapped
    std::string endWhy;
    std::vector<bool> pageLogged;      // the pages already reported as different
    // The game's data at the last check (for a desync's report: written next to the executable).
    struct Checked {
        std::vector<uint8_t> data;
        uint32_t frame = 0;
        uint64_t trace = 0;
    } checked[8];

    Settings saved;          // the guest's own settings, given back after the match
    bool settingsSaved = false;

    // Client/server ([Online] Mode=server, the default): the host's game is the match; the guest's follows it.
    bool server = true;
    uint32_t padSeqOut = 0, padSeqIn = 0;
    input::GamePad remotePad = {};
    uint32_t remotePacket = 0;
    uint32_t launchIdOut = 0, launchIdIn = 0;
    std::string pendingLaunch;         // the guest: the host's launch to follow
    bool pendingLaunchSet = false, followingLaunch = false, followStarted = false;
    uint32_t stateSeqOut = 0, stateSeqIn = 0, statesIn = 0;
    StateMsg latestState = {};
    bool haveState = false;
    bool guestInDuel = false;
    input::GamePad ownPad = {};
    uint32_t ownPacket = 0;
    uint32_t remoteReads = 0, remoteChanges = 0;
    bool remoteInDuel = false, duelStartWaited = false;
    OnlineRoomSettings room;            // the host's (the guest: as it last came)
    uint32_t remoteScreen = 0;          // the other's front-end screen (the guest follows the host's)
    uint32_t screen = 0, screenSince = 0; // this game's, and the frame it came
    bool toRoom = false;                // to the room's screen once the front end is up
    uint32_t toRoomAt = 0;              // the frame it was asked
    bool lostPopup = false;             // after the session's end: the game's popup at the Online screen
    Clock::time_point endedAt = {};
    uint32_t followTried = 0;
    std::string ownMenuArgs;            // this PC's own start at its main menu (its profile), for the session's end
    uint32_t joinAt = ~0u, arenaAt = ~0u; // the frames the port's own presses start (InjectLocked)
    bool lost = false;                  // the session ended by a lost connection (the popup says so)
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

// A player's name as received: printable characters only, at most `size` (none: `fallback`).
std::string PlayerName(const char* text, size_t size, const char* fallback)
{
    std::string name;
    for (size_t i = 0; i < size && text[i]; ++i)
        if (text[i] >= 32 && text[i] < 127)
            name += text[i];
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name.empty() ? fallback : name;
}

// This PC's player name: [Online] Name, else the Windows user name (letters without accents kept as they are,
// others dropped), at most 20 characters.
// The name of the profile last saved on this PC (the newest UDATA\<title>\<save>\jedi: a u32, then its name in
// wide characters), or "".
std::string ProfileName()
{
    const std::wstring root = kernel::SaveDrivePath() + L"\\UDATA\\4c410017\\";
    WIN32_FIND_DATAW f;
    HANDLE find = FindFirstFileW((root + L"*").c_str(), &f);
    if (find == INVALID_HANDLE_VALUE)
        return {};
    FILETIME newest = {};
    std::wstring best;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == L'.')
            continue;
        WIN32_FILE_ATTRIBUTE_DATA a;
        const std::wstring file = root + f.cFileName + L"\\jedi";
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &newest) > 0) {
            newest = a.ftLastWriteTime;
            best = file;
        }
    } while (FindNextFileW(find, &f));
    FindClose(find);
    std::vector<uint8_t> data;
    if (best.empty() || !ReadWholeFile(best, data, 1 << 16) || data.size() < 8)
        return {};
    std::string name;
    for (size_t i = 4; i + 1 < data.size() && name.size() < 15; i += 2) {
        const wchar_t w = wchar_t(data[i] | data[i + 1] << 8);
        if (!w)
            break;
        if (w >= 32 && w < 127)
            name += char(w);
    }
    return name;
}

// A name as the menus show it: letters, digits and spaces, upper case, at most 15.
std::string CleanName(const std::wstring& text)
{
    std::string name;
    for (wchar_t w : text) {
        static const wchar_t* const kFrom = L"\u00e0\u00e2\u00e4\u00e9\u00e8\u00ea\u00eb\u00ee\u00ef\u00f4\u00f6\u00f9\u00fb\u00fc\u00e7\u00c0\u00c9\u00c8\u00ca\u00c7";
        static const char kTo[] = "aaaeeeeiioouuucAEEEC";
        if (const wchar_t* at = wcschr(kFrom, w); at && w)
            w = wchar_t(kTo[at - kFrom]);
        if (w >= 'a' && w <= 'z')
            w = wchar_t(w - 'a' + 'A');
        if ((w >= 'A' && w <= 'Z') || (w >= '0' && w <= '9') || (w == ' ' && !name.empty()))
            name += char(w);
        if (name.size() >= 15)
            break;
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name;
}

// The player's name online: [Online] Name (Online Settings), else the profile's, else "NONAME". Never the PC's user.
std::string LocalPlayerNameNow();
// Cached: the menus ask every frame (the profile's file is read at most once a second).
std::string LocalPlayerName()
{
    static std::string name;
    static ULONGLONG at = 0;
    if (!at || GetTickCount64() - at > 1000) {
        name = LocalPlayerNameNow();
        at = GetTickCount64();
    }
    return name;
}

std::string LocalPlayerNameNow()
{
    wchar_t text[64] = {};
    const std::wstring ini = ExeDirectory() + L"\\settings.ini";
    GetPrivateProfileStringW(L"Online", L"Name", L"", text, 64, ini.c_str());
    std::string name = CleanName(text);
    if (name.empty()) {
        const std::string profile = ProfileName();
        name = CleanName(std::wstring(profile.begin(), profile.end()));
    }
    return name.empty() ? "NONAME" : name;
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
    kernel::SetLaunchObserver(nullptr);
    if (g_S.server && !g_S.host)
        game::SetDamageFilter(nullptr);
    kernel::SetDeterministicBoot(false);
    kernel::SetDeterministicAddresses(false);
    kernel::SetSessionDrives(std::wstring(), std::wstring());
    kernel::SetSessionFolders(std::wstring(), std::wstring());
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
    g_S.lost = !tellOther;
    g_S.lostPopup = !tellOther && matchOn;
    g_S.endedAt = Clock::now();
    g_S.armed = false;
    g_S.bootPending = false;
    RestoreLocked();
    g_ThreadStop = true;
    g_S.inputs.notify_all();
    if (matchOn) { // the game back to its main menu, as a PC alone (its own profile)
        LOG_INFO("Online: the match is over; back to the main menu");
        if (g_S.ownMenuArgs.empty())
            kernel::RestartFromDashboard();
        else
            kernel::LaunchGame(g_S.ownMenuArgs);
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

uint32_t PageHash(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

void SendPagesLocked(const Session::Checked& k)
{
    const uint32_t pages = uint32_t((k.data.size() + kPage - 1) / kPage);
    for (int round = 0; round < 2; ++round) // twice: a lost datagram loses a part of the report
        for (uint32_t first = 0; first < pages; first += kPagesPerMsg) {
            PagesMsg m = {};
            m.h = MakeHeader(Msg::Pages, g_S.token);
            m.frame = k.frame;
            m.trace = k.trace;
            m.first = uint16_t(first);
            m.count = uint16_t(std::min(kPagesPerMsg, pages - first));
            for (uint32_t i = 0; i < m.count; ++i) {
                const size_t at = size_t(first + i) * kPage;
                m.hashes[i] = PageHash(k.data.data() + at, std::min<size_t>(kPage, k.data.size() - at));
            }
            SendLocked(m);
        }
}

// The other PC's report: the pages of the game's data that differ from this PC's at that frame, logged.
void OnPagesLocked(const PagesMsg& m)
{
    const Session::Checked& k = g_S.checked[(m.frame / HashEvery()) % std::size(g_S.checked)];
    if (k.frame != m.frame || k.data.empty())
        return;
    const uint32_t pages = uint32_t((k.data.size() + kPage - 1) / kPage);
    if (g_S.pageLogged.size() != pages) {
        g_S.pageLogged.assign(pages, false);
        LOG_INFO("Online: desync report from the other PC (frame %u): scheduler trace %016llX here, %016llX there",
            m.frame, k.trace, m.trace);
    }
    for (uint32_t i = 0; i < m.count && m.first + i < pages; ++i) {
        const uint32_t page = m.first + i;
        const size_t at = size_t(page) * kPage;
        if (g_S.pageLogged[page] ||
            PageHash(k.data.data() + at, std::min<size_t>(kPage, k.data.size() - at)) == m.hashes[i])
            continue;
        g_S.pageLogged[page] = true;
        LOG_INFO("Online: desync: the game's data differs in %08X-%08X", uint32_t(0x00612BE0 + at),
            uint32_t(0x00612BE0 + at + kPage - 1));
    }
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
            const Session::Checked& k = g_S.checked[(r.frame / HashEvery()) % std::size(g_S.checked)];
            if (k.frame == r.frame && !k.data.empty()) { // the data as checked, for a comparison
                wchar_t instance[64] = {};
                GetEnvironmentVariableW(L"SWROTS_INSTANCE", instance, 64);
                const std::wstring path = ExeDirectory() + L"\\netplay\\desync-" + (instance[0] ? instance : (g_S.host ? L"host" : L"guest")) + L".bin";
                std::vector<uint8_t> out(8);
                std::memcpy(out.data(), &k.trace, 8);
                out.insert(out.end(), k.data.begin(), k.data.end());
                WriteWholeFile(path, out.data(), out.size());
                LOG_INFO("Online: the game's data at frame %u written to %ls (scheduler trace %016llX)", r.frame,
                    path.c_str(), k.trace);
            }
            // the two games no longer play the same match: it ends on both (each back to its title), once both
            // reports were swapped
            if (k.frame == r.frame && !k.data.empty())
                SendPagesLocked(k);
            g_S.endWhy = "the two games no longer match (desync at frame " + std::to_string(r.frame) + ")";
            g_S.endAt = Clock::now() + std::chrono::milliseconds(2500);
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

// --- Client/server ---------------------------------------------------------------------------------------------------

// The host: player 1 is this PC's own (its devices), player 2 the guest's pad as it last came. The guest: its game
// only shows the host's duel (no input in it); out of a duel its player 1 has this PC's devices (its own menus, to
// leave).
// Where both players play this game's player 1 and 2 (the select screens, a duel): the host's player 2 is the
// guest's pad, the guest's player 1 the host's and its player 2 its own. Elsewhere each PC's player 1 is its own
// devices and player 2 nothing (the guest's pad does not move the host's menus).
constexpr uint32_t kVersusSelectKey = 0x8DC51DC9u; // the fighters' select as the Versus row opens it

bool IsSelectScreen(uint32_t screen)
{
    return screen == kVersusSelectKey || screen == game::MenuKey("select_jedi");
}

bool BothPlayLocked()
{
    const uint32_t screen = g_S.screen;
    return g_S.guestInDuel || IsSelectScreen(screen) || screen == game::MenuKey("select_arena");
}

// Presses of the port's own: player 2 joins the fighters' select (Start), player 1 takes the arena (A).
void InjectLocked(int port, input::GamePad& pad)
{
    const uint32_t f = g_S.frame;
    if (port == 1 && f >= g_S.joinAt && f < g_S.joinAt + 9)
        pad.buttons |= 0x0010; // Start
    if (port == 0 && f >= g_S.arenaAt && f < g_S.arenaAt + 6)
        pad.analog[0] = 0xFF;  // A
}

bool PadFromServer(int port, input::GamePad& pad, uint32_t& packet)
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    if (!g_S.armed)
        return false;
    const bool both = BothPlayLocked();
    if (g_S.host) {
        if (port == 0) {
            if (g_S.frame < g_S.arenaAt || g_S.frame >= g_S.arenaAt + 6)
                return false; // this PC's own devices
            input::ReadLivePad(0, pad);
            InjectLocked(0, pad);
            packet = ++g_S.ownPacket;
            return true;
        }
        if (port == 1 && both) { // (player 2's join comes in the guest's pad: as a Start of its own)
            pad = g_S.remotePad;
            packet = g_S.remotePacket;
            ++g_S.remoteReads;
            return true;
        }
    } else if (both) {
        if (port == 0) {
            pad = g_S.remotePad;
            if (g_S.screen == game::MenuKey("select_arena")) { // the arena is the host's to take (its game starts
                pad.analog[0] = 0;                              // the duel; this one follows that)
                pad.buttons &= ~0x0010;
            }
            packet = g_S.remotePacket;
            return true;
        }
        if (port == 1) {
            input::ReadLivePad(0, pad);
            InjectLocked(1, pad);
            if (std::memcmp(&pad, &g_S.ownPad, sizeof(pad)) != 0) {
                g_S.ownPad = pad;
                ++g_S.ownPacket;
            }
            packet = g_S.ownPacket;
            return true;
        }
    } else if (port == 0) {
        return false; // this PC's own menus
    }
    pad = {};
    packet = 0;
    return true;
}

// The guest's fighters in a duel: no health change of their own (the host's come through ApplyFighter).
std::atomic<uint8_t*> g_GuestFighters[2] = {};
float GuestDamage(uint8_t* character, float change)
{
    return character && (character == g_GuestFighters[0] || character == g_GuestFighters[1]) ? 0.0f : change;
}

// The game starts a level (or the menus) on its own (XLaunchNewImage). The host's goes to the guest, whose game
// follows it; the guest's own waits for the host's and takes it instead.
void LaunchObserved(std::string& arguments)
{
    // Never with the session's lock held by this thread (the port's own starts are made outside it).
    std::unique_lock<std::mutex> lock(g_S.lock);
    if (!g_S.armed || !g_S.server)
        return;
    if (g_S.host) {
        // the room's rounds to win
        const size_t at = arguments.find("v_roundcount:");
        if (at != std::string::npos) {
            const size_t end = arguments.find(' ', at);
            arguments.replace(at, (end == std::string::npos ? arguments.size() : end) - at,
                "v_roundcount:" + std::to_string(g_S.room.rounds));
        }
        // back to the menus after a match: to the room, unless it plays again at once
        if (arguments.find("v_roundsp1:") != std::string::npos && !g_S.room.rematch) {
            g_S.toRoom = true;
            g_S.toRoomAt = g_S.frame;
        }
        LaunchMsg m = {};
        m.h = MakeHeader(Msg::Launch, g_S.token);
        m.id = ++g_S.launchIdOut;
        std::snprintf(m.args, sizeof(m.args), "%s", arguments.c_str());
        for (int i = 0; i < 5; ++i)
            SendLocked(m);
        g_S.haveState = false;
        g_S.duelStartWaited = false; // the next duel starts when the guest's game is in it too
        g_S.remoteInDuel = false;
        LOG_INFO("Online: the host starts '%s' (launch %u)", arguments.c_str(), m.id);
        return;
    }
    if (g_S.followingLaunch) { // the guest's start of the host's launch itself
        g_S.followingLaunch = false;
        return;
    }
    if (g_S.followStarted) { // the host's launch is being started already (this game's own comes too late)
        LOG_INFO("Online: the guest's game would start '%s'; the host's start is on its way", arguments.c_str());
        return;
    }
    // The guest: the host's launch (it may have come already).
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (g_S.armed && !g_S.pendingLaunchSet && Clock::now() < until)
        g_S.inputs.wait_for(lock, std::chrono::milliseconds(100));
    if (g_S.pendingLaunchSet) {
        LOG_INFO("Online: the guest's game would start '%s'; it starts the host's '%s'", arguments.c_str(),
            g_S.pendingLaunch.c_str());
        arguments = g_S.pendingLaunch;
        g_S.pendingLaunchSet = false;
    } else {
        LOG_WARN("Online: no launch from the host; the guest's game starts its own '%s'", arguments.c_str());
    }
    g_S.haveState = false;
}

void OnServerMsgLocked(Msg type, const uint8_t* data, size_t size)
{
    switch (type) {
    case Msg::Pad:
        if (size >= sizeof(PadMsg)) {
            PadMsg m;
            std::memcpy(&m, data, sizeof(m));
            if (m.seq <= g_S.padSeqIn)
                return;
            g_S.padSeqIn = m.seq;
            g_S.remoteInDuel = m.inDuel != 0;
            g_S.remoteScreen = m.screen;
            if (g_S.remoteInDuel)
                g_S.inputs.notify_all();
            if (std::memcmp(&m.pad, &g_S.remotePad, sizeof(m.pad)) != 0) {
                g_S.remotePad = m.pad;
                ++g_S.remotePacket;
                if (++g_S.remoteChanges == 1)
                    LOG_INFO("Online: the other player's pad moved (buttons %04X)", m.pad.buttons);
            }
        }
        break;
    case Msg::Launch:
        if (!g_S.host && size >= sizeof(LaunchMsg)) {
            LaunchMsg m;
            std::memcpy(&m, data, sizeof(m));
            m.args[sizeof(m.args) - 1] = 0;
            if (m.id <= g_S.launchIdIn)
                return;
            g_S.launchIdIn = m.id;
            g_S.pendingLaunch = m.args;
            g_S.pendingLaunchSet = true;
            g_S.haveState = false;
            g_S.inputs.notify_all();
        }
        break;
    case Msg::Room:
        if (!g_S.host && size >= sizeof(RoomMsg)) {
            RoomMsg m;
            std::memcpy(&m, data, sizeof(m));
            g_S.room = m.room;
        }
        break;
    case Msg::State:
        if (!g_S.host && size >= sizeof(StateMsg)) {
            StateMsg m;
            std::memcpy(&m, data, sizeof(m));
            if (m.seq <= g_S.stateSeqIn)
                return;
            g_S.stateSeqIn = m.seq;
            g_S.latestState = m;
            g_S.haveState = true;
            ++g_S.statesIn;
        }
        break;
    default:
        break;
    }
}

// A frame of a client/server match (OnlineFrame, the lock held; it may unlock).
void ServerFrameLocked(std::unique_lock<std::mutex>& lock)
{
    ++g_S.frame;
    PadMsg pm = {};
    pm.h = MakeHeader(Msg::Pad, g_S.token);
    pm.seq = ++g_S.padSeqOut;
    input::ReadLivePad(0, pm.pad);
    if (!g_S.host && g_S.frame >= g_S.joinAt && g_S.frame < g_S.joinAt + 9) {
        pm.pad.buttons |= 0x0010; // player 2's join on the Versus select: a Start of the guest's, for both games
        if (g_S.frame == g_S.joinAt)
            LOG_INFO("Online: player 2 joins the select");
    }
    pm.inDuel = g_S.guestInDuel ? 1 : 0;
    pm.screen = g_S.screen;
    SendLocked(pm);

    // The front-end screen (none in a level), the port's presses, the room, the guest following the host's screens.
    {
        uint8_t* p1 = nullptr;
        uint8_t* p2 = nullptr;
        const bool level = DuelFighters(p1, p2) || game::PlayerObject();
        const uint32_t screen = level ? 0 : game::LastShellScreen();
        if (screen != g_S.screen) {
            LOG_INFO("Online: screen 0x%08X (was 0x%08X)", screen, g_S.screen);
            g_S.screen = screen;
            g_S.screenSince = g_S.frame;
            if (IsSelectScreen(screen) && !g_S.host)
                g_S.joinAt = g_S.frame + 75; // player 2 joins (as its Start would), once the screen is in
            if (screen == game::MenuKey("select_arena") && g_S.host && g_S.room.arena == 1)
                g_S.arenaAt = g_S.frame + 20; // the arena: Random's row, taken
        }
    }
    const uint32_t screen = g_S.screen, since = g_S.frame - g_S.screenSince;
    uint8_t* q1 = nullptr;
    uint8_t* q2 = nullptr;
    const bool inShell = !DuelFighters(q1, q2) && !game::PlayerObject();
    if (g_S.host && g_S.frame + 2 == g_S.arenaAt) {
        const bool ok = game::ArenaCursorToRandom();
        LOG_INFO("Online: a random arena %s", ok ? "picked, taken" : "could not be picked: the host picks");
        if (!ok)
            g_S.arenaAt = ~0u;
    }
    if (g_S.host && g_S.toRoom && inShell && g_S.frame - g_S.toRoomAt > 90 && (g_S.frame - g_S.toRoomAt) % 30 == 0) {
        lock.unlock();
        const bool ok = game::GoToShellScreen("online_room");
        LOG_INFO("Online: to the room%s", ok ? "" : " (not yet)");
        std::lock_guard<std::mutex> relock(g_S.lock);
        if (ok)
            g_S.toRoom = false;
        return;
    }
    if (!g_S.host && inShell && g_S.remoteScreen && g_S.remoteScreen != screen && since > 20 &&
        g_S.frame - g_S.followTried > 30 && game::OnlineFollowsScreen(g_S.remoteScreen)) {
        g_S.followTried = g_S.frame;
        const uint32_t to = g_S.remoteScreen;
        lock.unlock();
        game::OnlineGoToScreen(to);
        return;
    }
    if (!g_S.host && g_S.pendingLaunchSet) { // the host's game went on to a level (or the menus): this one follows
        const std::string args = g_S.pendingLaunch;
        g_S.pendingLaunchSet = false;
        g_S.haveState = false;
        g_S.guestInDuel = false;
        g_S.followingLaunch = true;
        g_S.followStarted = true;
        lock.unlock();
        LOG_INFO("Online: following the host's launch '%s'", args.c_str());
        if (args.empty())
            kernel::RestartFromDashboard();
        else
            kernel::LaunchGame(args);
        return;
    }
    const bool host = g_S.host;
    const int pingMs = g_S.pingMs;
    if (host && g_S.frame % 600 == 0)
        LOG_INFO("Online: the guest's pad: %u change(s) received, read %u time(s) as player 2", g_S.remoteChanges,
            g_S.remoteReads);
    StateMsg state = g_S.latestState;
    const bool haveState = g_S.haveState;
    const uint32_t frame = g_S.frame;
    lock.unlock();

    uint8_t* fighters[2] = {};
    const bool duel = DuelFighters(fighters[0], fighters[1]);
    if (host) {
        if (!duel)
            return;
        { // a duel's start: the host's game waits for the guest's to be in it too (its intro at the same time)
            std::unique_lock<std::mutex> relock(g_S.lock);
            if (!g_S.duelStartWaited) {
                g_S.duelStartWaited = true;
                const auto start = Clock::now();
                while (g_S.armed && !g_S.remoteInDuel && Clock::now() - start < std::chrono::seconds(20))
                    g_S.inputs.wait_for(relock, std::chrono::milliseconds(20));
                LOG_INFO("Online: the duel starts (waited %d ms for the guest's game)",
                    int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()));
            }
        }
        StateMsg m = {};
        bool ok = true;
        for (int i = 0; i < 2; ++i)
            ok = CaptureFighter(fighters[i], m.fighters[i]) && ok;
        if (!ok)
            return;
        std::lock_guard<std::mutex> relock(g_S.lock);
        m.h = MakeHeader(Msg::State, g_S.token);
        m.seq = ++g_S.stateSeqOut;
        m.hostFrame = frame;
        SendLocked(m);
        if (m.seq % 600 == 1)
            LOG_INFO("Online: host frame %u, state %u sent (%u bytes), ping %d ms", frame, m.seq, unsigned(sizeof(m)),
                pingMs);
        return;
    }
    {
        std::lock_guard<std::mutex> relock(g_S.lock);
        g_S.guestInDuel = duel;
    }
    g_GuestFighters[0] = duel ? fighters[0] : nullptr;
    g_GuestFighters[1] = duel ? fighters[1] : nullptr;
    game::SetDamageFilter(duel ? &GuestDamage : nullptr);
    if (!duel || !haveState)
        return;
    const uint32_t lead = pingMs > 0 ? uint32_t(pingMs * 60 / 2000) : 0; // half the round trip, in game ticks
    for (int i = 0; i < 2; ++i)
        ApplyFighter(i, fighters[i], state.fighters[i], lead, i == 1);
    if (frame % 600 == 0)
        LOG_INFO("Online: guest frame %u, host state %u applied (%s), ping %d ms", frame, state.seq,
            ApplyStats().c_str(), pingMs);
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
    // With the host's profile: the main menu, as the game's own return from a level. Without (the host at the
    // title): a start as from the dashboard (empty arguments), its intro and title.
    std::string args;
    std::vector<std::pair<std::wstring, std::vector<uint8_t>>> files;
    int controller = 0, memoryUnit = 0;
    if (game::SaveProfileForStart(controller, memoryUnit)) {
        std::vector<uint8_t> persist;
        if (ReadWholeFile(kernel::UtilityDrivePath() + L"\\persist.tmp", persist)) {
            files.push_back({ L"utility\\persist.tmp", std::move(persist) });
            args = "map: v_shellBetweenLevels:0 v_versusMode:0 v_bonusMissionMode:0 v_challengeMissionMode:0 "
                   "v_checkpointIndex:-1 playlicensemovie:0 v_sessionCreated:1 title:0 controller:0 memoryunit:" +
                std::to_string(memoryUnit);
        }
    }
    if (args.empty())
        LOG_WARN("Online: no profile to bring along (the match starts as the game does, at its title)");
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
    GameplaySettings g = CurrentGameplay();
    // The match's pace: [Online] Fps (30 by default, the console's own: both PCs must keep it, the slower sets it)
    const std::wstring ini = ExeDirectory() + L"\\settings.ini";
    const int onlineFps = std::clamp(int(GetPrivateProfileIntW(L"Online", L"Fps", 30, ini.c_str())), 20, 60);
    if (!g_S.server) // lockstep: the pace both PCs keep
        g.fpsLimit = std::min(g.fpsLimit > 0 ? g.fpsLimit : 60, onlineFps);
    g_S.inputDelay = g.fpsLimit >= 50 ? 3 : 2; // about 60 ms either way
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
    g_S.saved = GetSettings(); // both PCs play with the host's gameplay settings and the match's pace
    g_S.settingsSaved = true;
    ApplyGameplay(g);
    g_S.fps = uint32_t(std::clamp(g.fpsLimit, 10, 120));
    kernel::SetSessionDrives(root + L"\\saves", root + L"\\utility");
    kernel::SetSessionFolders(root + L"\\nomods", root + L"\\cache"); // no mods in a match: the same game on both PCs
    input::SetOnlinePorts(true);
    if (g_S.server) { // client/server: each game at its own pace; the host's is the match
        input::SetPadSource(&PadFromServer);
        g_S.bootPending = true;
        g_S.phase = OnlinePhase::Starting;
        SetMessage("starting the match");
        LOG_INFO("Online: client/server match (%s)", g_S.host ? "the host's game is the match" : "following the host");
        if (args.empty())
            kernel::RestartFromDashboard();
        else
            kernel::LaunchGame(args);
        // Only now: the observer takes the session's lock, which this start holds (a std::mutex taken twice throws).
        kernel::SetLaunchObserver(&LaunchObserved);
        return;
    }
    input::SetPadSource(&PadFromSession);
    if (!kernel::SetDeterministicAddresses(true)) {
        EndLocked("this PC's memory has no room at the match's fixed addresses", true);
        return;
    }
    kernel::SetDeterministicBoot(true);
    g_S.bootPending = true;
    g_S.phase = OnlinePhase::Starting;
    SetMessage("starting the match");
    if (args.empty())
        kernel::RestartFromDashboard();
    else
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
                reject("the host's game files differ from yours (compare netplay\\content.txt on both PCs)");
                return;
            }
            if (!g_S.peer.Valid()) {
                g_S.peer = from;
                g_S.nonce = m.nonce;
                g_S.remoteName = PlayerName(m.name, sizeof(m.name), "PLAYER 2");
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
                w.reserved = g_S.server ? 1 : 0;
                w.packageSize = uint32_t(g_S.package.size());
                w.packageHash = g_S.packageHash;
                std::snprintf(w.name, sizeof(w.name), "%s", g_S.localName.c_str());
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
            g_S.server = (w.reserved & 1) != 0;
            g_S.package.assign(w.packageSize, 0);
            g_S.packageHash = w.packageHash;
            g_S.remoteName = PlayerName(w.name, sizeof(w.name), "PLAYER 1");
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
    if (type == Msg::Pad || type == Msg::Launch || type == Msg::State || type == Msg::Room) {
        if (g_S.server && (g_S.armed || g_S.bootPending || type == Msg::Room))
            OnServerMsgLocked(type, data, size);
        return;
    }
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
    case Msg::Pages:
        if (size >= offsetof(PagesMsg, hashes))
            OnPagesLocked(*reinterpret_cast<const PagesMsg*>(data));
        break;
    case Msg::Bye:
        if (size >= sizeof(RejectMsg)) {
            RejectMsg r;
            std::memcpy(&r, data, sizeof(r));
            r.text[sizeof(r.text) - 1] = 0;
            EndLocked("the other player left", false);
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
    if (g_S.endAt != Clock::time_point{} && now >= g_S.endAt) {
        g_S.endAt = {};
        EndLocked(g_S.endWhy, true);
        return;
    }
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
        std::snprintf(m.name, sizeof(m.name), "%s", g_S.localName.c_str());
        for (const Endpoint& e : g_S.candidates)
            g_S.socket.SendTo(e, &m, sizeof(m));
        return;
    }
    if (since(g_S.lastHeard) > kSilenceMs) {
        EndLocked("connection lost", false);
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
    if (g_S.host && g_S.server && g_S.peer.Valid()) {
        RoomMsg r = {};
        r.h = MakeHeader(Msg::Room, g_S.token);
        r.room = g_S.room;
        SendLocked(r);
    }
    if (g_S.peer.Valid()) {
        PingMsg p = {};
        p.h = MakeHeader(Msg::Ping, g_S.token);
        p.time = NowMs();
        SendLocked(p);
    }
    if ((g_S.armed || g_S.bootPending) && !g_S.server)
        SendInputsLocked(); // the inputs again (one could have been lost), and the check
}

void NetThread(bool host, uint16_t port)
{
    UpnpMapping mapping; // the host's port on its router, taken back as the thread ends
    uint8_t buffer[2048];
    const auto hashStart = Clock::now();
    const uint64_t build = BuildHash(), content = ContentHash();
    {
        std::lock_guard<std::mutex> lock(g_S.lock);
        g_S.build = build;
        g_S.content = content;
        g_S.started = g_S.lastHeard = Clock::now();
        LOG_INFO("Online: build %016llX, game files %016llX (%lld ms)", build, content,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - hashStart).count());
    }
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
        // UPnP first (on by default: [Online] Upnp=0 off): the port opened on the router, the same port outside
        bool upnp = GetPrivateProfileIntW(L"Online", L"Upnp", 1, iniPath.c_str()) != 0;
        if (GetEnvironmentVariableA("SWROTS_ONLINE_UPNP", env, sizeof(env)) && env[0] == '0')
            upnp = false;
        if (upnp)
            UpnpMapPort(port, localAddress.ip, 2500, mapping);
        if (stun && Resolve(std::string(serverName.begin(), serverName.end()), 3478, server)) {
            if (!StunPublicAddress(g_S.socket, server, publicAddress, 3000))
                LOG_WARN("Online: the STUN server did not answer");
        }
        if (mapping.Active()) { // the mapped port, at the address STUN saw (or the router's own)
            if (!publicAddress.Valid())
                publicAddress.ip = mapping.externalIp;
            publicAddress.port = mapping.port;
        }
        std::lock_guard<std::mutex> lock(g_S.lock);
        if (g_S.phase != OnlinePhase::Opening) {
            UpnpUnmap(mapping);
            return;
        }
        if (!publicAddress.Valid()) {
            LOG_WARN("Online: no public address (STUN and UPnP off or not answering); the room code holds this PC's "
                     "local address only");
            publicAddress = localAddress;
        }
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
    UpnpUnmap(mapping);
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
    g_S.endAt = {};
    g_S.pageLogged.clear();
    g_S.padSeqOut = g_S.padSeqIn = 0;
    g_S.remotePad = {};
    g_S.remotePacket = 0;
    g_S.launchIdOut = g_S.launchIdIn = 0;
    g_S.pendingLaunch.clear();
    g_S.pendingLaunchSet = g_S.followingLaunch = g_S.followStarted = false;
    g_S.stateSeqOut = g_S.stateSeqIn = g_S.statesIn = 0;
    g_S.haveState = false;
    g_S.guestInDuel = false;
    g_S.ownPad = {};
    g_S.ownPacket = g_S.remoteReads = g_S.remoteChanges = 0;
    g_S.remoteInDuel = g_S.duelStartWaited = false;
    g_S.remoteScreen = g_S.screen = g_S.screenSince = g_S.followTried = 0;
    g_S.toRoom = g_S.lost = false;
    g_S.joinAt = g_S.arenaAt = ~0u;
    g_S.started = g_S.lastHeard = Clock::now();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    g_S.nonce = uint32_t(counter.QuadPart) ^ GetCurrentProcessId() * 2654435761u;
    g_S.token = host ? (g_S.nonce * 2246822519u) | 1 : 0;
}

bool Prepare(bool host, std::string& error)
{
    StopThread();
    // This PC's own main menu (its profile), for the session's end; made first (it saves the profile).
    const std::string session = game::ProfileSessionArguments();
    const std::string ownMenu = session.empty() ? std::string() :
        "map: v_shellBetweenLevels:0 v_versusMode:0 v_bonusMissionMode:0 v_challengeMissionMode:0 v_checkpointIndex:-1" +
        session;
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
    g_S.ownMenuArgs = ownMenu;
    g_S.localName = LocalPlayerName();
    g_S.remoteName.clear();
    g_S.inputDelay = 3;
    g_S.fps = uint32_t(std::clamp(GetSettings().fpsLimit, 10, 120));
    g_S.server = host ? ServerModeWanted() : true; // the guest takes the host's (its Welcome)
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
    const std::wstring ini = ExeDirectory() + L"\\settings.ini";
    const uint16_t port = uint16_t(GetPrivateProfileIntW(L"Online", L"Port", kDefaultPort, ini.c_str()));
    if (ReadRoomCode(codeOrAddress, port, publicAddress, localAddress)) {
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
    i.localName = g_S.localName;
    i.remoteName = g_S.remoteName;
    i.matchOn = g_S.armed || g_S.bootPending;
    i.guestHere = g_S.host && g_S.peer.Valid();
    i.lost = g_S.lost;
    i.room = g_S.room;
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

std::string OnlinePlayerName()
{
    return LocalPlayerName();
}

void SetOnlinePlayerName(const std::string& name)
{
    const std::string clean = CleanName(std::wstring(name.begin(), name.end()));
    const std::wstring ini = ExeDirectory() + L"\\settings.ini";
    WritePrivateProfileStringW(L"Online", L"Name", std::wstring(clean.begin(), clean.end()).c_str(), ini.c_str());
    LOG_INFO("Online: the player's name is now '%s'", LocalPlayerName().c_str());
}

void SetOnlineRoom(const OnlineRoomSettings& room)
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    if (!g_S.host && g_S.phase != OnlinePhase::Off && g_S.phase != OnlinePhase::Ended)
        return; // the host's to set
    g_S.room = room;
}

bool OnlineActive()
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    return g_S.armed;
}

void OnlineReboot()
{
    std::lock_guard<std::mutex> lock(g_S.lock);
    g_S.followStarted = false; // the host's launch is in
    if (!g_S.bootPending)
        return; // a level change in a match: the frames go on
    g_S.bootPending = false;
    g_S.armed = true;
    g_S.frame = 0;
    g_S.localLatest = g_S.inputDelay; // frames up to the delay are neutral on both PCs
    g_S.remoteContiguous = std::max(g_S.remoteContiguous, g_S.inputDelay); // the other may have started first
    g_S.paceFrame = 0;
    g_S.paceStart = Clock::now();
    g_S.paceClock = 0; // set at the first frame
    g_S.phase = OnlinePhase::Playing;
    g_S.lastHeard = Clock::now();
    g_S.toRoom = g_S.server && g_S.host; // the match's first screen: the room (the guest's follows)
    g_S.toRoomAt = g_S.frame;
    SetMessage(std::string("the match starts: player ") + (g_S.host ? "1 (host)" : "2 (guest)") + ", input delay " +
        std::to_string(g_S.inputDelay) + " frames");
}

void OnlineFrame()
{
    std::unique_lock<std::mutex> lock(g_S.lock);
    if (g_S.lostPopup && g_S.phase == OnlinePhase::Ended && Clock::now() - g_S.endedAt > std::chrono::seconds(4)) {
        g_S.lostPopup = false; // the game is back at its main menu: the Online screen, the game's popup over it
        const std::string why = g_S.message;
        lock.unlock();
        game::OnlineShowLost(why);
        return;
    }
    if (g_S.packageWanted && g_S.host) { // the host's profile, saved by the game's code
        g_S.packageWanted = false;
        BuildPackageLocked();
        g_S.welcomed = true; // the next Hello is answered with the Welcome
        WelcomeMsg w = {};
        w.h = MakeHeader(Msg::Welcome, g_S.token);
        w.nonce = g_S.nonce;
        w.inputDelay = uint8_t(g_S.inputDelay);
        w.fps = uint8_t(g_S.fps);
        w.reserved = g_S.server ? 1 : 0;
        w.packageSize = uint32_t(g_S.package.size());
        w.packageHash = g_S.packageHash;
        std::snprintf(w.name, sizeof(w.name), "%s", g_S.localName.c_str());
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
    if (g_S.server) {
        ServerFrameLocked(lock);
        return;
    }

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
    if (next % HashEvery() == 0) {
        const auto* data = reinterpret_cast<const uint8_t*>(uintptr_t(0x00612BE0));
        const size_t dataSize = 0x00962BF8 - 0x00612BE0;
        uint64_t h = kernel::SchedTraceHash();
        if (!g_S.desync) { // kept until the other's check for this frame came (a desync's report)
            Session::Checked& k = g_S.checked[(next / HashEvery()) % std::size(g_S.checked)];
            k.data.assign(data, data + dataSize);
            k.frame = next;
            k.trace = h;
        }
        h = HashGameData(data, dataSize, h);
        g_S.checks[(next / HashEvery()) % std::size(g_S.checks)] = { next, h };
        g_S.lastLocal = { next, h };
        if (next % 600 == 0)
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
        g_S.paceClock = kernel::FixedClockPeek();
    }
    // The packet numbers (they move on when a pad changes), then the frame.
    for (int p = 0; p < 2; ++p)
        if (std::memcmp(&InputOf(p, next), &InputOf(p, next - 1), sizeof(input::GamePad)) != 0)
            ++g_S.packets[p];
    g_S.frame = next;

    // The pace: the game's own time (the fixed clock, which its vertical blanks and frame limiter move on) shown at
    // the PC's; a little slower while this PC is ahead of the other.
    const auto period = std::chrono::microseconds(1000000 / g_S.fps);
    const uint64_t gameNow = kernel::FixedClockPeek();
    if (!g_S.paceClock)
        g_S.paceClock = gameNow;
    const uint32_t remoteFrame = g_S.remoteLatest > g_S.inputDelay ? g_S.remoteLatest - g_S.inputDelay : 0;
    if (next > remoteFrame + 1)
        g_S.paceStart += period / 4; // this PC is ahead: it slows down until the other catches up
    const auto target = g_S.paceStart + std::chrono::microseconds((gameNow - g_S.paceClock) / 10);
    lock.unlock();
    const auto now = Clock::now();
    if (target > now + std::chrono::milliseconds(150) || now > target + std::chrono::milliseconds(150)) {
        std::lock_guard<std::mutex> relock(g_S.lock); // far off (a load): the pace starts again here
        g_S.paceStart = now;
        g_S.paceFrame = next;
        g_S.paceClock = gameNow;
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
