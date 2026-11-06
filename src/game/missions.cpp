// Missions and modes of mods' own: a Lua script drives a level built on the developer map (or a custom map) with the
// port's functions. See missions.h and docs/modding/missions.md (the scripting reference).
//
// A mission starts like Heroes vs Villains: the game restarts into the dev map with the launch argument
// `mission:<id>` (and `custommap:<id>` for its map). At that start InstallMissions places the mission's cast in the
// level (characters placed in a level load with it, their classes' sound sets too) and sets the player's class and
// start. Once the level runs, MissionFrame wakes the cast and runs the script: its `main` (and any `thread`s) as Lua
// coroutines, resumed once a frame while the game is not paused. Waiting is a loop of yields (the prelude below).

#include "game/missions.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "lua.hpp"

#include "audio/audio.h"
#include "core/log.h"
#include "core/patch.h"
#include "debug/console.h"
#include "game/characters.h"
#include "game/classsetup.h"
#include "game/coop.h"
#include "game/customchars.h"
#include "game/custommaps.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/game.h"
#include "game/hvv.h"
#include "game/menus.h"
#include "game/resources.h"
#include "input/controls.h"
#include "kernel/kernel.h"

namespace swrots::game {
namespace {

// --- The game's character fields and calls (as hvv.cpp and characters.cpp use them) ------------------------------
constexpr uint32_t kCharacterDead = 0x12C;       // byte: its death played out
constexpr uint32_t kCharacterHealth = 0x130;
constexpr uint32_t kCharacterMaxHealth = 0x134;
constexpr uint32_t kCharacterTransform = 0x150;  // rows right, up, forward, position
constexpr uint32_t kCharacterScript = 0x434;     // its script instance: +4 its sequence list
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte, "Invincible?"
constexpr uint32_t kCharacterAI = 0x9FC;         // its AI controller
constexpr uint32_t kAIOff = 0x28;                // byte on the controller: its AI switched off
constexpr uint32_t kAISetTarget = 0x00193D90;    // thiscall on the controller (target)
constexpr uint32_t kActivate = 0xB4;             // object vfunc: a placed character joins the level
constexpr uint32_t kPlace = 0x1F4;               // object vfunc, thiscall (const float m[16])
constexpr uint32_t kInstantKillSlot = 0xC8;      // ICharacter vfunc, thiscall (0): an instant kill
constexpr uintptr_t kPlaySequence = 0x150E30;    // ICharacter, thiscall (sequence, 0, 0, 0, speed)
constexpr uint32_t kObjectById = 0x000A31F0;     // cdecl (id): a level object by its id
// Objectives ([Objectives]): the game's own triggers, placed in the level
constexpr uint32_t kObjectiveFirstId = kDevMapFirstId + 0x200;
constexpr uint32_t kTriggerObjectiveVtable = 0x0058BD10;
constexpr uint32_t kTriggerFire = 0xA4; // a trigger object's vfunc: it fires
// Tips ([Tips]): the game's own (IVaderTip, its text a string table id), placed in the level
constexpr uint32_t kTipFirstId = kDevMapFirstId + 0x280;
constexpr uint32_t kVaderTipVtable = 0x005CBBA0;
// Force fields ([Fields]): the game's own (IForceField, an IStatic: the Jedi Temple's door field), placed in the level;
// vfunc +0xA4 (0x2778B0) lights it, +0xA8 (0x277910) fades it away
constexpr uint32_t kFieldFirstId = kDevMapFirstId + 0x300;
constexpr uint32_t kForceFieldVtable = 0x005AF710;
constexpr uint32_t kFieldOn = 0xA4, kFieldOff = 0xA8;
// The game manager's players (docs/research/coop.md): their number and an instance id per slot; a story duel's
// boss is its player 2 (its health bar on the HUD)
constexpr uint32_t kGameManager = 0x007EB964;
constexpr uint32_t kManagerPlayerCount = 0x1E4;
constexpr uint32_t kManagerPlayerIds = 0x2A4;
// Walking: the game's own goto (as Battlefront's AI uses it) to the level's goto markers (ICharacterGoto)
constexpr uintptr_t kCharacterGoto = 0x00151400;  // ICharacter, thiscall (marker, may fight on the way)
constexpr uintptr_t kExecutingGoto = 0x001513E0;  // ICharacter, thiscall (marker): still on its way
constexpr uint32_t kGotoMarkerVtable = 0x00598970;
// A walk called off: the AI's goto controllers (type 8, TAI +0x18 count / +0x1C items, type at +0x10) let go of their
// marker (SetGoto(null), vfunc +0x208: goto +0x13C and TAI +0x2D4 cleared)
constexpr uint32_t kAIControllerCount = 0x18, kAIControllers = 0x1C, kControllerType = 0x10, kGotoController = 8;
constexpr uint32_t kSetGoto = 0x208;
// Sabers: the character's weapon slots, and what the cinematics' ToggleSaber event (0x24A490) calls on a saber
constexpr uint32_t kCharacterWeapons = 0x1080;   // 4 slots, 0x20 apart: the weapon object, or null
constexpr uint32_t kSaberVtables[] = { 0x005B1FF0, 0x005B2620, 0x005B2C90, 0x005B32F8, 0x005B3958 };
constexpr uintptr_t kSaberExtend = 0x286080;     // ILightSaber, thiscall (float seconds): lit, its sound if seconds > 0
constexpr uintptr_t kSaberRetract = 0x2860E0;    // ILightSaber, thiscall (float seconds): put away
constexpr uint32_t kSaberLit = 0x801;            // byte on the saber
constexpr uint32_t kScriptSaberOnCall = 0x2BC586; // the script native SaberOn's call of kSaberExtend (5 bytes)

std::vector<uint8_t*> g_SabersAway; // sabers the mission put away: the class scripts may not light them// The ids of the cast placed in the level (after the dev map's own and Heroes vs Villains')
constexpr uint32_t kCastFirstId = kDevMapFirstId + 0x100;

template <typename T> T& At(uint8_t* p, uint32_t offset) { return *reinterpret_cast<T*>(p + offset); }
template <typename F> F Virtual(uint8_t* object, uint32_t offset)
{
    return reinterpret_cast<F>((*reinterpret_cast<void***>(object))[offset / 4]);
}
uint8_t* ObjectById(uint32_t id) { return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id); }
// An object's class name (its vtable's slot 3, GetTypeName), e.g. "IObiwan"
const char* VirtualTypeName(uint8_t* object)
{
    return reinterpret_cast<const char*(__fastcall*)(uint8_t*, void*)>((*reinterpret_cast<void* const* const*>(object))[3])(
        object, nullptr);
}

// --- Missions on disk ---------------------------------------------------------------------------------------------
std::wstring g_Mods;
std::vector<MissionInfo> g_Missions;

std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w)
        s += char(c < 128 ? c : '?');
    return s;
}

std::string Profile(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"")
{
    wchar_t value[2048] = {};
    GetPrivateProfileStringW(section, key, fallback, value, DWORD(std::size(value)), file.c_str());
    std::string s = Narrow(value);
    if (const size_t semi = s.find(';'); semi != std::string::npos) // a comment after the value
        s.erase(semi);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

std::vector<std::string> Keys(const std::wstring& file, const wchar_t* section)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetPrivateProfileStringW(section, nullptr, L"", buffer.data(), DWORD(buffer.size()), file.c_str());
    std::vector<std::string> keys;
    for (DWORD i = 0; i < n;) {
        const std::wstring key(buffer.data() + i);
        if (key.empty())
            break;
        keys.push_back(Narrow(key));
        i += DWORD(key.size()) + 1;
    }
    return keys;
}

std::vector<float> Numbers(const std::string& text)
{
    std::vector<float> v;
    const char* p = text.c_str();
    while (*p) {
        char* end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        v.push_back(f);
        p = end;
    }
    return v;
}

void ReadMissions()
{
    g_Missions.clear();
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((g_Mods + L"\\missions\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.')
            continue;
        const std::wstring folder = g_Mods + L"\\missions\\" + found.cFileName;
        const std::wstring ini = folder + L"\\mission.ini";
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        MissionInfo m;
        m.id = Narrow(found.cFileName);
        std::transform(m.id.begin(), m.id.end(), m.id.begin(), [](unsigned char c) { return char(tolower(c)); });
        m.name = Profile(ini, L"Mission", L"Name", found.cFileName);
        m.description = Profile(ini, L"Mission", L"Description");
        m.map = Profile(ini, L"Mission", L"Map");
        m.folder = "missions\\" + Narrow(found.cFileName);
        g_Missions.push_back(m);
        LOG_INFO("Missions: %s (%s)%s%s", m.id.c_str(), m.name.c_str(), m.map.empty() ? "" : ", on ", m.map.c_str());
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

// --- This start's mission ------------------------------------------------------------------------------------------
struct CastMember {
    std::string name;      // the script's name for it
    std::string className;
    uint32_t placedId = 0;
    float x = 0, z = 0, yaw = 0; // the map's coordinates
    SpawnSide side = SpawnSide::Default;
    std::string costume;
    bool asleep = false;   // Asleep: placed, but joins the level only when the script wakes it (spawn_cast)
};

struct Line {
    std::string speaker, text, voice;
    float seconds = 0;     // 0: from the text's length (or its voice's)
    std::string lineId;    // its line in the game's dialogue archive (RegisterDialogue), e.g. "MSN003"
    double length = 0;     // how long it lasts
};

// A character the script holds: an object and its instance id (+4), to tell it is still the same one.
struct Handle {
    uint8_t* object = nullptr;
    uint32_t instance = 0;
};

enum class ShotKind { None, Fixed, Orbit, Move, Track };
struct Shot {
    ShotKind kind = ShotKind::None;
    int target = 0;        // handle
    float eye[3] = {}, look[3] = {};
    float from[3] = {}, to[3] = {};
    float radius = 0, height = 0, fromDeg = 0, toDeg = 0, lookHeight = 50;
    float seconds = 1;
    double startedAt = 0;
    float facing[2] = { 0, 1 }; // the target's facing (x, z) when the shot started
};

bool g_Active = false;
std::wstring g_Folder;
const MissionInfo* g_Info = nullptr;
std::string g_PlayerClass;
std::vector<CastMember> g_Cast;
std::map<std::string, Line> g_Lines;
// mission.ini's [Markers]: places characters walk to (the level's goto markers, kDevMapFirstMarkerId on, in order)
struct Marker {
    std::string name;
    float x = 0, z = 0; // the map's coordinates
};
std::vector<Marker> g_Markers;
std::vector<std::string> g_Objectives; // [Objectives]' names, in order (ids kObjectiveFirstId on)
std::vector<std::string> g_Tips;       // [Tips]' names, in order (ids kTipFirstId on)
std::vector<std::string> g_Fields;     // [Fields]' names, in order (ids kFieldFirstId on)
std::string g_PlayersSubtitles; // the subtitles setting before the mission turned them on
int g_LevelCharFade = -1;       // the level's "Allow char fade?" while a mission's shot has it off; -1: untouched
float g_PlayersMusicVolume = -1; // volumemusic before music_volume() changed it (put back at the end); -1: unchanged
std::string g_MusicServed; // the disc's music file the mission's own track is served for
std::vector<Handle> g_Handles; // index 0 unused
bool g_Woken = false;
uint64_t g_InLevelAt = 0;
lua_State* g_Lua = nullptr;
std::vector<int> g_Threads; // registry references of the running coroutines
std::string g_Script;       // the script's text, loaded at the start
MissionHud g_Hud;
Shot g_Shot;
bool g_Held = false;
bool g_Skip = false;        // a cutscene is being skipped: waits end at once
double g_SkipAskedAt = -10; // a first press in a cutscene: the second one skips it
bool g_MissionSlow = false;  // the mission's slow motion is on (the letterbox must not cancel it)
bool g_Advance = false;     // the developer's "next" (mission next, F8): the current wait ends
bool g_InCutscene = false;
double g_FadeFrom = 0, g_FadeTo = 0, g_FadeStart = 0, g_FadeLength = 0;
double g_BarsFrom = 0, g_BarsTo = 0, g_BarsStart = 0;
double g_ObjectiveAt = -10, g_TitleUntil = 0, g_TitleAt = 0, g_NoticeUntil = 0, g_NoticeAt = 0, g_LineUntil = 0;
double g_TimerEnd = 0;      // the timer's end (time), or its frozen value
bool g_TimerCountsDown = true, g_TimerRunning = false;
double g_TimerStart = 0;
double g_EndAt = 0;         // the end showing since (time), 0: none
bool g_EndOk = false, g_EndDone = false;
bool g_PlayerDefeatedHandled = false;

double Now() { return double(GameTime()) / 1000.0; }

uint8_t* GameOptions()
{
    auto* services = *reinterpret_cast<uint8_t**>(uintptr_t(kEngineServices));
    auto* core = services ? *reinterpret_cast<uint8_t**>(services + 0x38) : nullptr;
    return core ? *reinterpret_cast<uint8_t**>(core + 8) : nullptr;
}

void Hold(bool hold)
{
    g_Held = hold;
    if (uint8_t* options = GameOptions())
        options[kOptionsAiDisabled] = hold ? 1 : 0;
    input::HoldPlayerInput(hold);
}

// The map's coordinates (x z, y from its floor) to the world's, and back.
void ToWorld(float x, float z, float& wx, float& wz)
{
    if (const CustomMap* map = DevMapCustomMap()) {
        CustomMapToWorld(*map, x, z, wx, wz);
        return;
    }
    wx = kDevMapMiddleX + x;
    wz = kDevMapMiddleZ + z;
}

float MapYaw()
{
    const CustomMap* map = DevMapCustomMap();
    return map ? CustomMapYaw(*map) : 0.0f;
}

void FromWorld(float wx, float wz, float& x, float& z)
{
    if (const CustomMap* map = DevMapCustomMap()) {
        // the inverse of CustomMapToWorld: found from the map's origin and its axes
        float ox, oz, ax, az, bx, bz;
        CustomMapToWorld(*map, 0, 0, ox, oz);
        CustomMapToWorld(*map, 1, 0, ax, az);
        CustomMapToWorld(*map, 0, 1, bx, bz);
        const float ux = ax - ox, uz = az - oz, vx = bx - ox, vz = bz - oz;
        const float det = ux * vz - uz * vx;
        const float dx = wx - ox, dz = wz - oz;
        x = det != 0 ? (dx * vz - dz * vx) / det : 0;
        z = det != 0 ? (ux * dz - uz * dx) / det : 0;
        return;
    }
    x = wx - kDevMapMiddleX;
    z = wz - kDevMapMiddleZ;
}

void Transform(float x, float z, float yaw, float (&m)[16], float lift = 2.0f)
{
    float wx, wz;
    ToWorld(x, z, wx, wz);
    yaw += MapYaw();
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float rows[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, wx, kDevMapFloorY + lift, wz, 1 };
    std::copy(std::begin(rows), std::end(rows), m);
}

int AddHandle(uint8_t* object)
{
    if (!object)
        return 0;
    const uint32_t instance = At<uint32_t>(object, 4);
    for (size_t i = 1; i < g_Handles.size(); ++i)
        if (g_Handles[i].object == object && g_Handles[i].instance == instance)
            return int(i);
    g_Handles.push_back({ object, instance });
    return int(g_Handles.size() - 1);
}

uint8_t* Resolve(int handle)
{
    if (handle <= 0 || size_t(handle) >= g_Handles.size())
        return nullptr;
    Handle& h = g_Handles[size_t(handle)];
    if (!h.object || ObjectById(h.instance) != h.object)
        return nullptr;
    return h.object;
}

const float* Position(uint8_t* c) { return reinterpret_cast<const float*>(c + kCharacterTransform + 48); }

bool Alive(uint8_t* c) { return c && At<float>(c, kCharacterHealth) > 0 && !c[kCharacterDead]; }

// A WAV file's samples (16-bit PCM, mono or stereo, interleaved); false for anything else.
bool ReadWav(const std::wstring& path, std::vector<int16_t>& samples, unsigned& channels, unsigned& rate)
{
    std::vector<uint8_t> data;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        uint8_t buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
            data.insert(data.end(), buffer, buffer + n);
        fclose(f);
    }
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        return false;
    uint16_t format = 0, chans = 0, bits = 0;
    uint32_t hz = 0;
    const uint8_t* pcm = nullptr;
    uint32_t pcmBytes = 0;
    for (size_t at = 12; at + 8 <= data.size();) {
        uint32_t size;
        std::memcpy(&size, data.data() + at + 4, 4);
        const size_t body = at + 8;
        if (body + size > data.size())
            size = uint32_t(data.size() - body);
        if (std::memcmp(data.data() + at, "fmt ", 4) == 0 && size >= 16) {
            std::memcpy(&format, data.data() + body, 2);
            std::memcpy(&chans, data.data() + body + 2, 2);
            std::memcpy(&hz, data.data() + body + 4, 4);
            std::memcpy(&bits, data.data() + body + 14, 2);
        } else if (std::memcmp(data.data() + at, "data", 4) == 0) {
            pcm = data.data() + body;
            pcmBytes = size;
        }
        at = body + size + (size & 1);
    }
    if (format != 1 || bits != 16 || (chans != 1 && chans != 2) || !hz || !pcm)
        return false;
    samples.resize(pcmBytes / 2);
    std::memcpy(samples.data(), pcm, samples.size() * 2);
    channels = chans;
    rate = hz;
    return true;
}

// Mono samples as the game's dialogue lines are: headerless Xbox ADPCM (IMA), blocks of 36 bytes (the first sample and
// the step index, then 8 words of 8 four-bit codes: 65 samples), as tools/audio/hwx.py's encoder for one channel.
std::vector<uint8_t> XboxAdpcm(const std::vector<int16_t>& mono)
{
    static const int kStep[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
        66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
        658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
        4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767 };
    static const int kIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
    std::vector<uint8_t> out;
    int index = 0;
    const size_t blocks = (mono.size() + 64) / 65;
    for (size_t b = 0; b < blocks; ++b) {
        auto sample = [&](size_t i) { return i < mono.size() ? int(mono[i]) : 0; };
        int pred = sample(b * 65);
        const int16_t first = int16_t(pred);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&first), reinterpret_cast<const uint8_t*>(&first) + 2);
        out.push_back(uint8_t(index));
        out.push_back(0);
        for (int group = 0; group < 8; ++group) {
            uint32_t code = 0;
            for (int j = 0; j < 8; ++j) {
                const int target = sample(b * 65 + 1 + group * 8 + j);
                const int step = kStep[index];
                int delta = target - pred;
                int n = delta < 0 ? 8 : 0;
                delta = std::abs(delta);
                int diff = step >> 3;
                if (delta >= step) { n |= 4; delta -= step; diff += step; }
                if (delta >= step >> 1) { n |= 2; delta -= step >> 1; diff += step >> 1; }
                if (delta >= step >> 2) { n |= 1; diff += step >> 2; }
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                index = std::clamp(index + kIndex[n], 0, 88);
                code |= uint32_t(n) << (4 * j);
            }
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&code), reinterpret_cast<const uint8_t*>(&code) + 4);
        }
    }
    return out;
}

// A WAV (any rate) as a dialogue line: mono, 22050 Hz (linear resampling), Xbox ADPCM. Empty when it is no WAV.
std::vector<uint8_t> DialogueVoice(const std::wstring& wav)
{
    std::vector<int16_t> samples;
    unsigned channels = 0, rate = 0;
    if (!ReadWav(wav, samples, channels, rate))
        return {};
    const size_t frames = samples.size() / channels;
    const size_t outFrames = size_t(double(frames) * 22050.0 / rate);
    std::vector<int16_t> mono(outFrames);
    for (size_t i = 0; i < outFrames; ++i) {
        const double at = double(i) * rate / 22050.0;
        const size_t i0 = std::min(frames - 1, size_t(at));
        const size_t i1 = std::min(frames - 1, i0 + 1);
        const double f = at - double(i0);
        double v = 0;
        for (unsigned c = 0; c < channels; ++c)
            v += (1 - f) * samples[i0 * channels + c] + f * samples[i1 * channels + c];
        mono[i] = int16_t(std::clamp(v / channels, -32768.0, 32767.0));
    }
    return XboxAdpcm(mono);
}

void ReadLines()
{
    g_Lines.clear();
    const std::wstring file = g_Folder + L"\\lines.ini";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    for (const std::string& id : Keys(file, L"Lines")) {
        const std::wstring key(id.begin(), id.end());
        const std::string value = Profile(file, L"Lines", key.c_str());
        Line line;
        const size_t bar = value.find('|');
        line.speaker = bar == std::string::npos ? "" : value.substr(0, bar);
        line.text = bar == std::string::npos ? value : value.substr(bar + 1);
        line.voice = Profile(file, L"Voice", key.c_str());
        const std::string seconds = Profile(file, L"Seconds", key.c_str());
        line.seconds = seconds.empty() ? 0.0f : float(atof(seconds.c_str()));
        g_Lines[id] = line;
    }
    LOG_INFO("Missions: %zu dialogue lines", g_Lines.size());
}

double LineSeconds(const std::string& text);

// The lines as the game's own dialogue: each a line of the generic dialogue archive ("MSN001"..., customchars.h
// AddVoiceLines), its voice from [Voice] (a WAV, made mono 22050 Hz Xbox ADPCM in cache\voice\missions\) or silence
// as long as the line shows, its subtitle its text.
void RegisterDialogue()
{
    if (g_Lines.empty())
        return;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring folder = exe;
    folder = folder.substr(0, folder.find_last_of(L'\\')) + L"\\cache\\voice";
    CreateDirectoryW(folder.c_str(), nullptr);
    folder += L"\\missions";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::vector<VoiceLineSource> sources;
    int n = 0;
    for (auto& [id, line] : g_Lines) {
        char lineId[16];
        std::snprintf(lineId, sizeof lineId, "MSN%03d", ++n);
        std::vector<uint8_t> voice;
        if (!line.voice.empty())
            voice = DialogueVoice(g_Folder + L"\\" + std::wstring(line.voice.begin(), line.voice.end()));
        if (!line.voice.empty() && voice.empty())
            LOG_WARN("Missions: line %s's voice %s is not a 16-bit PCM WAV", id.c_str(), line.voice.c_str());
        const double voiceSeconds = double(voice.size() / 36 * 65) / 22050.0;
        line.length = line.seconds > 0 ? line.seconds : voice.empty() ? LineSeconds(line.text) : voiceSeconds + 0.3;
        if (voice.empty()) // silence as long as the line shows: the dialogue keeps its subtitle up for its line
            voice = XboxAdpcm(std::vector<int16_t>(size_t(line.length * 22050.0), 0));
        const std::wstring path = folder + L"\\" + std::wstring(lineId, lineId + strlen(lineId)) + L".xadpcm";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            fwrite(voice.data(), 1, voice.size(), f);
            fclose(f);
        }
        line.lineId = lineId;
        sources.push_back({ lineId, path, std::wstring(line.text.begin(), line.text.end()) });
    }
    AddVoiceLines(sources);
}

// --- The script's functions ----------------------------------------------------------------------------------------
int CheckHandle(lua_State* L, int index)
{
    return lua_isinteger(L, index) ? int(lua_tointeger(L, index)) : 0;
}

uint8_t* CheckCharacter(lua_State* L, int index)
{
    return Resolve(CheckHandle(L, index));
}

SpawnSide SideFrom(const std::string& s)
{
    if (_stricmp(s.c_str(), "ally") == 0)
        return SpawnSide::Ally;
    if (_stricmp(s.c_str(), "enemy") == 0)
        return SpawnSide::Enemy;
    if (_stricmp(s.c_str(), "neutral") == 0)
        return SpawnSide::Neutral;
    if (_stricmp(s.c_str(), "riot") == 0)
        return SpawnSide::Riot;
    return SpawnSide::Default;
}

int L_log(lua_State* L)
{
    LOG_INFO("Mission: %s", luaL_tolstring(L, 1, nullptr));
    return 0;
}

int L_time(lua_State* L)
{
    lua_pushnumber(L, Now());
    return 1;
}

int L_paused(lua_State* L)
{
    lua_pushboolean(L, GamePaused());
    return 1;
}

int L_player(lua_State* L)
{
    lua_pushinteger(L, AddHandle(PlayerObject()));
    return 1;
}

int L_cast(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    for (const CastMember& m : g_Cast) {
        if (_stricmp(m.name.c_str(), name) != 0)
            continue;
        for (uint8_t* c : LevelCharacters())
            if (At<uint32_t>(c, 4) == m.placedId) {
                lua_pushinteger(L, AddHandle(c));
                return 1;
            }
    }
    lua_pushnil(L);
    return 1;
}

int L_spawn(lua_State* L)
{
    const char* className = luaL_checkstring(L, 1);
    const float x = float(luaL_optnumber(L, 2, 0)), z = float(luaL_optnumber(L, 3, 0));
    const float yaw = float(luaL_optnumber(L, 4, 0)) * 3.14159265f / 180.0f;
    const SpawnSide side = SideFrom(luaL_optstring(L, 5, ""));
    const std::string costume = luaL_optstring(L, 6, "");
    float m[16];
    Transform(x, z, yaw, m);
    std::string error;
    uint8_t* c = SpawnCharacterAt(className, costume, "", "", side, m, error);
    if (!c) {
        LOG_WARN("Mission: spawn %s: %s", className, error.c_str());
        lua_pushnil(L);
        return 1;
    }
    InvalidateLevelCharacters();
    lua_pushinteger(L, AddHandle(c));
    return 1;
}

int L_remove(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    if (c && !RemoveSpawnedCharacter(c)) { // a placed one: out of the way, still
        float m[16];
        Transform(0, 0, 0, m, -20000.0f);
        At<uint8_t>(c, kCharacterInvincible) = 1;
        if (uint8_t* brain = At<uint8_t*>(c, kCharacterAI))
            brain[kAIOff] = 1;
        Virtual<void(__fastcall*)(uint8_t*, void*, const float*)>(c, kPlace)(c, nullptr, m);
    }
    InvalidateLevelCharacters();
    return 0;
}

int L_alive(lua_State* L)
{
    lua_pushboolean(L, Alive(CheckCharacter(L, 1)));
    return 1;
}

int L_exists(lua_State* L)
{
    lua_pushboolean(L, CheckCharacter(L, 1) != nullptr);
    return 1;
}

int L_health(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    const float most = c ? At<float>(c, kCharacterMaxHealth) : 0;
    lua_pushnumber(L, c && most > 0 ? std::max(0.0f, At<float>(c, kCharacterHealth)) / most : 0.0);
    return 1;
}

int L_set_health(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1)) {
        const float f = float(luaL_checknumber(L, 2));
        At<float>(c, kCharacterHealth) = std::max(1.0f, At<float>(c, kCharacterMaxHealth) * std::clamp(f, 0.0f, 1.0f));
    }
    return 0;
}

int L_max_health(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    if (c && lua_isnumber(L, 2)) {
        const float most = std::max(1.0f, float(lua_tonumber(L, 2)));
        const float f = At<float>(c, kCharacterMaxHealth) > 0 ? At<float>(c, kCharacterHealth) / At<float>(c, kCharacterMaxHealth) : 1;
        At<float>(c, kCharacterMaxHealth) = most;
        At<float>(c, kCharacterHealth) = most * f;
    }
    lua_pushnumber(L, c ? At<float>(c, kCharacterMaxHealth) : 0);
    return 1;
}

int L_invincible(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1))
        At<uint8_t>(c, kCharacterInvincible) = lua_toboolean(L, 2) ? 1 : 0;
    return 0;
}

int L_position(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    if (!c)
        return 0;
    const float* p = Position(c);
    float x, z;
    FromWorld(p[0], p[2], x, z);
    lua_pushnumber(L, x);
    lua_pushnumber(L, z);
    lua_pushnumber(L, p[1] - kDevMapFloorY);
    return 3;
}

int L_distance(lua_State* L)
{
    uint8_t* a = CheckCharacter(L, 1);
    uint8_t* b = CheckCharacter(L, 2);
    if (!a || !b) {
        lua_pushnumber(L, 1e9);
        return 1;
    }
    const float* p = Position(a);
    const float* q = Position(b);
    lua_pushnumber(L, std::hypot(p[0] - q[0], p[2] - q[2]));
    return 1;
}

void CancelWalk(uint8_t* c, bool stopGoing);

int L_place(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1)) {
        CancelWalk(c, true); // (moved by a cut: a walk it was on is over)
        float m[16];
        // place(c, x, z [, heading, height]): a height above the floor (it falls from there), or below it (a pit's
        // fall: nothing stops it)
        Transform(float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
            float(luaL_optnumber(L, 4, 0)) * 3.14159265f / 180.0f, m, float(luaL_optnumber(L, 5, 2.0)));
        Virtual<void(__fastcall*)(uint8_t*, void*, const float*)>(c, kPlace)(c, nullptr, m);
    }
    return 0;
}

int L_face(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    uint8_t* t = CheckCharacter(L, 2);
    if (!c || !t)
        return 0;
    const float* p = Position(c);
    const float* q = Position(t);
    const float yaw = std::atan2(q[0] - p[0], q[2] - p[2]);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    float m[16] = { cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, p[0], p[1], p[2], 1 };
    Virtual<void(__fastcall*)(uint8_t*, void*, const float*)>(c, kPlace)(c, nullptr, m);
    return 0;
}

int L_side(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1))
        SetCharacterSide(c, SideFrom(luaL_checkstring(L, 2)));
    return 0;
}

int L_ai(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1)) {
        CancelWalk(c, false); // (the AI is the script's now: a walk on its way does not switch it off on arrival)
        if (uint8_t* brain = At<uint8_t*>(c, kCharacterAI))
            brain[kAIOff] = lua_toboolean(L, 2) ? 0 : 1;
    }
    return 0;
}

// actor(c, on): a cutscene actor: it does only what the script says (moves, walks, sabers, lines): its AI is off, no
// hit reaches it (no damage, no reaction, no block), no one attacks or targets it (characters.h script actors). Off:
// as it was (its AI, invincibility). A body that stays down: actor(c, true), then play_anim(c, <death>, {hold = true}).
struct ActorWas {
    uint8_t* character;
    uint8_t invincible, aiOff, ignored;
};
std::vector<ActorWas> g_Actors;
constexpr uint32_t kAIDataIgnored = 0x52; // [char+0xA00] "Ignored By AI"
constexpr uint32_t kCharacterAIData = 0xA00;

int L_actor(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    if (!c)
        return 0;
    const bool on = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    uint8_t* brain = At<uint8_t*>(c, kCharacterAI);
    uint8_t* data = At<uint8_t*>(c, kCharacterAIData);
    auto was = std::find_if(g_Actors.begin(), g_Actors.end(), [&](const ActorWas& a) { return a.character == c; });
    if (on && was == g_Actors.end())
        g_Actors.push_back({ c, At<uint8_t>(c, kCharacterInvincible), brain ? brain[kAIOff] : uint8_t(0),
            data ? data[kAIDataIgnored] : uint8_t(0) });
    if (on) {
        At<uint8_t>(c, kCharacterInvincible) = 1;
        if (brain)
            brain[kAIOff] = 1;
        if (data)
            data[kAIDataIgnored] = 1;
    } else if (was != g_Actors.end()) {
        CancelWalk(c, true); // (a scene's walk ends with the scene)
        At<uint8_t>(c, kCharacterInvincible) = was->invincible;
        if (brain)
            brain[kAIOff] = was->aiOff;
        if (data)
            data[kAIDataIgnored] = was->ignored;
        g_Actors.erase(was);
    }
    SetScriptActor(c, on);
    return 0;
}

// hide(c [, on]): it is taken out of sight where it stands, as the game's cinematics hide an object (their GhostObject
// event's Hide, 0x1D2360: the object's deactivate, vfunc +0xB0; shown again with its activate, +0xB4, at once and in
// the pose it had), an actor meanwhile (no hits, no AI, no one's target), quiet, and no one bumps into it;
// hide(c, false): back as it was. (Only the deactivate hides: the "inactive" flag alone, +0xC 0x08000000, still draws
// it. A character never drawn yet appears a frame late: a stand-in is best shown for a moment before it is hidden.)
// For a cutscene's stand-ins (docs/modding/missions.md).
constexpr uint32_t kCharacterPushFlag = 0x7F4;
constexpr uint32_t kDeactivate = 0xB0; // object vfunc, thiscall (): the cinematics' "GhostObject" Hide (0x1D2360)
struct HiddenWas {
    uint8_t* character;
    uint8_t push;
    bool wasActor;
    bool wasQuiet;
};
std::vector<HiddenWas> g_Hidden;

int L_hide(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    if (!c)
        return 0;
    const bool on = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    auto was = std::find_if(g_Hidden.begin(), g_Hidden.end(), [&](const HiddenWas& h) { return h.character == c; });
    const auto actor = [&](bool to) {
        lua_pushcfunction(L, L_actor);
        lua_pushvalue(L, 1);
        lua_pushboolean(L, to);
        lua_call(L, 2, 0);
    };
    if (on && was == g_Hidden.end()) {
        const bool isActor = std::any_of(g_Actors.begin(), g_Actors.end(), [&](const ActorWas& a) { return a.character == c; });
        g_Hidden.push_back({ c, At<uint8_t>(c, kCharacterPushFlag), isActor, CharacterQuiet(c) });
        SetCharacterQuiet(c, true); // (no battle cry from out of sight)
        actor(true);
        At<uint8_t>(c, kCharacterPushFlag) = 0;
        Virtual<void(__fastcall*)(uint8_t*, void*)>(c, kDeactivate)(c, nullptr);
    } else if (!on && was != g_Hidden.end()) {
        Virtual<void(__fastcall*)(uint8_t*, void*)>(c, kActivate)(c, nullptr);
        At<uint8_t>(c, kCharacterPushFlag) = was->push;
        SetCharacterQuiet(c, was->wasQuiet);
        const bool wasActor = was->wasActor;
        g_Hidden.erase(was);
        if (!wasActor)
            actor(false);
    }
    return 0;
}

// keep_body(c [, on]): when it dies, its body stays where it fell (the game's "Cinematic Death?", +0x5D4: no fade, not
// taken away). A body that nothing may wake: actor(c) once it has fallen.
constexpr uint32_t kCinematicDeath = 0x5D4;

int L_keep_body(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1))
        At<uint8_t>(c, kCinematicDeath) = (lua_isnoneornil(L, 2) || lua_toboolean(L, 2)) ? 1 : 0;
    return 0;
}

// pushable(c, on): whether characters bumping into it push it aside (and it them): off, they pass through it and it
// stays where it is (a body lying in the way). The game's "character push" (+0x7F4, Phys_EnableCharacterPush: its
// HandleCharacterBumping 0x16AED0 skips a character without it).
constexpr uint32_t kCharacterPush = 0x7F4;

int L_pushable(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1))
        At<uint8_t>(c, kCharacterPush) = lua_toboolean(L, 2) ? 1 : 0;
    return 0;
}

// force_powers(c, "Push Grip SaberThrow Lightning Heal"): the Force powers a custom character with the port's Force
// (ForceFrom=) uses from now on, e.g. a Maul who does not heal; "" for none. False for any other character.
int L_force_powers(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    lua_pushboolean(L, c && SetForcePowers(c, luaL_checkstring(L, 2)));
    return 1;
}

// saber_color(c, colour): the player's saber colour from now on ("red", "green", "blue", "purple" or "r g b", 0-255),
// e.g. a Padawan who takes up his Master's saber. The player only (false for another character); the colour the
// player had (the debug menu's, or the game's) comes back when the mission ends or starts again.
struct SaberWas {
    bool changed = false, had = false;
    float rgb[3] = {};
} g_SaberWas;

void RestoreSaberColor()
{
    if (!g_SaberWas.changed)
        return;
    SetPlayerSaberColor(g_SaberWas.had ? g_SaberWas.rgb : nullptr);
    g_SaberWas = {};
}

int L_saber_color(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    float rgb[3];
    if (!c || !ParseSaberColor(luaL_checkstring(L, 2), rgb)) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (c != PlayerObject()) { // another's (a cutscene's stand-in): its own from now on
        lua_pushboolean(L, SetCharacterSaberColor(c, rgb));
        return 1;
    }
    if (!g_SaberWas.changed) {
        g_SaberWas.changed = true;
        g_SaberWas.had = PlayerSaberColor(g_SaberWas.rgb);
    }
    SetPlayerSaberColor(rgb);
    lua_pushboolean(L, true);
    return 1;
}

// slow_motion(scale [, ramp]): the game's slow motion (as its kills use): everything at `scale` of its speed (0.3: a
// third) from now on, easing in over `ramp` seconds (default 0.15); slow_motion(1) back to normal. Script waits run
// on real time. Back to normal when the mission ends.
constexpr uintptr_t kSetSlowMotion = 0x000A20E0; // cdecl (scale, ramp in, hold, ramp out): frames at 30 a second

int L_slow_motion(lua_State* L)
{
    const float scale = std::clamp(float(luaL_checknumber(L, 1)), 0.05f, 1.0f);
    const int ramp = int(std::clamp(luaL_optnumber(L, 2, 0.15), 0.0, 5.0) * 30.0 + 0.5);
    reinterpret_cast<void(__cdecl*)(float, int, int, int)>(kSetSlowMotion)(scale, ramp, scale < 1.0f ? 9999 : 0, 0);
    g_MissionSlow = scale < 1.0f;
    return 0;
}

// quiet(c [, on]): it says no line of its own (battle cries, taunts, grunts), only the script's (say); off again with
// quiet(c, false).
int L_quiet(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1))
        SetCharacterQuiet(c, lua_isnoneornil(L, 2) || lua_toboolean(L, 2));
    return 0;
}

int L_attack(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    uint8_t* t = CheckCharacter(L, 2);
    if (c && t)
        if (uint8_t* brain = At<uint8_t*>(c, kCharacterAI))
            reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(brain, nullptr, t);
    return 0;
}

int L_hold(lua_State* L)
{
    Hold(lua_toboolean(L, 1) != 0);
    return 0;
}

// A move by its sequence's name (the class script's list, as Versus intros are found), e.g. "Anakin_Intro",
// "Idle_Flavor_v1"; case does not matter. False when the character has none of that name.
int L_play(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const float speed = float(luaL_optnumber(L, 3, 1.0));
    bool played = false;
    uint8_t* script = c ? At<uint8_t*>(c, kCharacterScript) : nullptr;
    uint8_t* list = script ? At<uint8_t*>(script, 4) : nullptr;
    if (list) {
        const int count = At<int>(list, 8);
        uint8_t** items = At<uint8_t**>(list, 0xC);
        for (int i = 0; i < count && !played; ++i) {
            const char* n = items[i] ? At<const char*>(items[i], 0xC) : nullptr;
            if (n && _stricmp(n, name) == 0) {
                reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
                    c, nullptr, items[i], 0.0f, 0, 0, speed);
                played = true;
            }
        }
    }
    if (!played)
        LOG_WARN("Mission: play %s: no such move", name);
    lua_pushboolean(L, played);
    return 1;
}

// mission.ini's [Animations]: animations, any character's, a class plays (play_anim), each a sequence of the class's
// own ("Msn_<animation>", added to its class script as custom intros are; its animation declared in the class's folder,
// resources.cpp). Required by the class while a mission runs, so loaded with it.
struct MissionAnimation {
    std::string shortName; // the class script's short name, e.g. "JDBrute"
    std::string animation, sequence;
    uint16_t handle = 0;
};
constexpr int kMaxMissionAnimations = 32;
MissionAnimation g_Animations[kMaxMissionAnimations];
int g_AnimationCount = 0;
constexpr uint32_t kSequenceSetup = 0x006965D8;
constexpr uint32_t kScriptInstance = 0x006964C0;

bool SequenceEvent(uint32_t test)
{
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(test))(
        reinterpret_cast<uint8_t*>(uintptr_t(kSequenceSetup)), nullptr);
}

// play_anim's partner (a paired kill, as the game's attack table plays one): the attacker's sequence grabs the partner
// as the game's own kill sequences do (Anakin_Kill_Lunge_Human_GB, 0x318B60): the partner's root is then locked to the
// attacker's (the pair's two animations share one origin), and the partner plays its half by name (e.g. Kill_Lunge_GR,
// whose own script shows the hit, the saber's heavy impact at his root, and falls him: Fall_StandingF, R_OnBack).
// Slow motion through the stab, the blade's impale effect at frame code 6 (as Anakin's: 0x401F40).
// A mission's slow motion (slow_motion(), a paired kill's): the game's letterbox cancels any slow motion every frame
// while its bars are up (0x12E0B7 calls 0xA2110: its cinematics run at normal speed); a mission's cutscene keeps the
// one the mission asked for (g_MissionSlow).
constexpr uint32_t kLetterboxCancelSlowCall = 0x0012E0B7;
constexpr uintptr_t kCancelSlowMotion = 0x000A2110;

void __cdecl LetterboxCancelSlow()
{
    if (!g_MissionSlow)
        reinterpret_cast<void(__cdecl*)()>(kCancelSlowMotion)();
}

struct Grapple {
    uint8_t* attacker;
    uint32_t attackerId;
    uint8_t* partner;
    uint32_t partnerId;
    std::string partnerSequence;
    bool grabbed;
    float slow; // the slow motion through the blow (the game's kills: 0.5)
};
std::vector<Grapple> g_Grapples;
constexpr uint32_t kScriptContext = 0x006943CC;   // the running script's: +0x68 its character
constexpr uint32_t kGrapplePartnerId = 0x5B0;     // the attacker's partner (an instance id), read by the grab
constexpr uintptr_t kGrab = 0x00172E00;           // cdecl (x, y, z): the partner's root offset
constexpr uintptr_t kRelease = 0x00172E20;
constexpr uintptr_t kPartnerSequence = 0x00174B40; // cdecl (name, 0, 0, 0, 0): the partner plays that sequence
constexpr uintptr_t kWeaponKillMode = 0x002BC210;  // cdecl (1, 0, 0) at a kill's start; 0x2BC1B0 (0, 0) after
constexpr uintptr_t kWeaponKillModeOff = 0x002BC1B0;
constexpr uintptr_t kSlowMotion = 0x000A20E0;      // cdecl (scale, ramp in, hold, ramp out): the time manager's ramp
// (not the scripts' 0x176BF0, which skips while the script effects are off, byte 0x6943D0: always, in a mission)
constexpr uintptr_t kImpaleEffect = 0x00401F40;    // IDS_FX_LIGHTSABER_Body_Impale at the saber's middle
constexpr uintptr_t kFrameCode = 0x001728C0;       // the animation's frame code now

// A sequence's events, each asked once a call: while the class is set up, asking registers the event with the
// sequence (only events asked then ever come); while it plays, it is true at that event.
struct SequenceEvents {
    bool start, frameCode, left, ended;
};

void GrappleEvents(const SequenceEvents& e)
{
    uint8_t* context = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptContext));
    uint8_t* c = context ? At<uint8_t*>(context, 0x68) : nullptr;
    auto g = std::find_if(g_Grapples.begin(), g_Grapples.end(), [&](const Grapple& x) { return x.attacker == c; });
    if (!c || g == g_Grapples.end())
        return;
    // (the attacker's animation before it, another of the mission's, is left as the kill starts: its end is not the
    // kill's, which has not grabbed yet)
    if (!e.start && !g->grabbed)
        return;
    const auto release = [&] {
        if (!g->grabbed)
            return;
        g->grabbed = false;
        reinterpret_cast<void(__cdecl*)(int, int)>(kWeaponKillModeOff)(0, 0);
        reinterpret_cast<void(__cdecl*)()>(kRelease)();
        reinterpret_cast<void(__cdecl*)(float, int, int, int)>(kSlowMotion)(1.0f, 10, 0, 0);
        g_MissionSlow = false;
    };
    if (e.start) { // its start: the grab, the partner's half
        if (At<uint32_t>(c, 4) != g->attackerId || At<uint32_t>(g->partner, 4) != g->partnerId) {
            g_Grapples.erase(g);
            return;
        }
        At<uint32_t>(c, kGrapplePartnerId) = g->partnerId;
        reinterpret_cast<void(__cdecl*)(int, int, int)>(kWeaponKillMode)(1, 0, 0);
        reinterpret_cast<void(__cdecl*)(float, float, float)>(kGrab)(0, 0, 0);
        reinterpret_cast<void(__cdecl*)(float, int, int, int)>(kSlowMotion)(g->slow, 0, 9999, 0);
        g_MissionSlow = g->slow < 1.0f;
        reinterpret_cast<void(__cdecl*)(const char*, int, int, int, int)>(kPartnerSequence)(g->partnerSequence.c_str(), 0, 0, 0, 0);
        g->grabbed = true;
        LOG_INFO("Mission: %s grabs its partner, who plays %s", VirtualTypeName(c), g->partnerSequence.c_str());
    }
    if (e.frameCode) {
        const int code = reinterpret_cast<int(__cdecl*)()>(kFrameCode)();
        if (code == 6 && g->grabbed)
            reinterpret_cast<void(__cdecl*)()>(kImpaleEffect)();
        else if (code == 20 && g->slow == 0.5f) { // (the game's own: a slower one the script chose lasts the whole blow)
            reinterpret_cast<void(__cdecl*)(float, int, int, int)>(kSlowMotion)(1.0f, 10, 0, 0);
            g_MissionSlow = false;
        }
    }
    if (e.left || e.ended) {
        release();
        g_Grapples.erase(g);
    }
}

template <int K> bool __fastcall MissionAnimationFn(uint8_t* self, void*, uint32_t, uint32_t)
{
    auto* setup = reinterpret_cast<uint8_t*>(uintptr_t(kSequenceSetup));
    MissionAnimation& a = g_Animations[K];
    reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(0x001F5B20))(setup, nullptr, &a.handle, a.sequence.c_str(), a.animation.c_str(), 1.0f, 0x0055FB60);
    SequenceEvents e;
    e.start = SequenceEvent(0x001F6310);
    e.left = SequenceEvent(0x001F6350);
    e.ended = SequenceEvent(0x001F62D0);
    e.frameCode = SequenceEvent(0x001F6390);
    if (!g_Grapples.empty())
        GrappleEvents(e);
    if (e.ended) // it ended: back to the class's state machine
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int, int)>(uintptr_t(0x0038A140))(
            *reinterpret_cast<uint8_t**>(uintptr_t(kScriptInstance)), self, -1, -1);
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, const char*, uint32_t)>(uintptr_t(0x001F6C00))(
        setup, nullptr, reinterpret_cast<const char*>(uintptr_t(0x0059C07C)), 0x7D00 + K);
}

template <int... K>
constexpr std::array<ClassSequenceFn, sizeof...(K)> MissionAnimationFns(std::integer_sequence<int, K...>)
{
    return { &MissionAnimationFn<K>... };
}
constexpr auto kMissionAnimationFns = MissionAnimationFns(std::make_integer_sequence<int, kMaxMissionAnimations>());

int __cdecl MissionSequences(const char* shortName, ClassSequenceFn* fns, int max)
{
    if (!g_Active)
        return 0;
    int count = 0;
    for (int k = 0; k < g_AnimationCount && count < max; ++k)
        if (_stricmp(g_Animations[k].shortName.c_str(), shortName) == 0) {
            g_Animations[k].handle = 0;
            fns[count++] = kMissionAnimationFns[size_t(k)];
            LOG_INFO("Missions: %s gets %s", shortName, g_Animations[k].animation.c_str());
        }
    return count;
}

// A character's sequence for play_anim's name: a borrowed animation's ([Animations]), the class's own sequence of that
// name, or the class's own sequence playing that animation.
uint8_t* FindSequence(uint8_t* c, const char* name)
{
    uint8_t* script = c ? At<uint8_t*>(c, kCharacterScript) : nullptr;
    uint8_t* list = script ? At<uint8_t*>(script, 4) : nullptr;
    if (!list)
        return nullptr;
    const int count = At<int>(list, 8);
    uint8_t** items = At<uint8_t**>(list, 0xC);
    const std::string borrowed = std::string("Msn_") + name;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < count; ++i) {
            const char* n = items[i] ? At<const char*>(items[i], pass == 2 ? 0x10 : 0xC) : nullptr;
            if (n && _stricmp(n, pass == 0 ? borrowed.c_str() : name) == 0)
                return items[i];
        }
    return nullptr;
}

// A character's animation state (TCharState, [char+0x434], vtable 0x58EFB8): its current sequence +0x30, speeds +0x50
// (the sequence's own, the class scripts change it) and +0x54 (the play's, 0 freezes the pose: a sequence ends only
// past its last frame), transitions done +0x78, transitions waiting +0xD0; its range state [+0xB8]: the range's first
// and last frames +8 / +0xA (int16), the current frame +0x14 (float, the animation's own frame number), rate +0x24.
// The animation advances once per tick of the character, by +0x50 * +0x54 * rate frames (60 a second at speed 1).
constexpr uint32_t kStateSequence = 0x30, kStateSpeed = 0x50, kStatePlaySpeed = 0x54, kStateTransitions = 0x78,
                   kStateRange = 0xB8, kStateQueued = 0xD0;
constexpr uint32_t kRangeFirst = 0x08, kRangeLast = 0x0A, kRangeFrame = 0x14, kRangeRate = 0x24;
constexpr uint32_t kTickScriptSlot = 0x0058EFDC; // ICharacter TCharState vtable +0x24
constexpr uintptr_t kTickScript = 0x0049CD90;    // thiscall (cs, arg): one tick of its sequence (its end handled here)
constexpr uintptr_t kSeekFrame = 0x0049D0E0;     // thiscall (cs, int frame): into the range holding the frame

// play_anim's controls: a range of frames played, looped or held, kept to by the tick hook until the character's
// sequence changes (stop_anim, a hit, its death, its class's own choice).
struct AnimControl {
    uint8_t* character = nullptr;
    uint32_t id = 0;          // its instance id (+4), to tell it is still the same one
    uint8_t* state = nullptr; // the character's TCharState
    uint8_t* sequence = nullptr;
    float from = -1, to = -1; // frames; -1: the animation's first / last
    float speed = 1;
    bool loop = false, hold = false;
    bool bounce = false;      // looped forth and back (no jump from `to` to `from`)
    bool backwards = false;   // a bounce on its way back: the frames stepped here (the game's play speed 0)
    bool started = false;     // its sequence is on (a play from the script starts at the character's next tick)
    int waited = 0;           // ticks before it was
    int transitions = 0;      // the state's count when it started: another transition means the character moved on
    float lastFrame = -1;     // where it was at the last tick
    int lastOfRange = 0;
    bool frozen = false;      // held at `to`
    int replays = 0;
};
constexpr int kMaxReplays = 600; // an actor put back (10 s of ticks put away one by one: something else wants it)
std::vector<AnimControl> g_AnimControls;

void Replay(AnimControl& k, float frame)
{
    k.started = false;
    k.waited = 0;
    ++k.replays;
    reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
        k.character, nullptr, k.sequence, 0.0f, frame >= 0 ? int(frame) : 0, 0, k.speed > 0 ? k.speed : 1.0f);
}

uint32_t __fastcall TickScriptHook(uint8_t* cs, void*, uint32_t arg)
{
    for (size_t i = 0; i < g_AnimControls.size(); ++i) {
        AnimControl& k = g_AnimControls[i];
        if (k.state != cs)
            continue;
        uint8_t* rs = At<uint8_t*>(cs, kStateRange);
        uint8_t* now = At<uint8_t*>(cs, kStateSequence);
        const bool same = At<uint32_t>(k.character, 4) == k.id;
        const auto drop = [&](const char* why) {
            LOG_INFO("Mission: play_anim %s %s (%s)", At<const char*>(k.sequence, 0xC), why,
                now ? At<const char*>(now, 0xC) : "nothing");
            g_AnimControls.erase(g_AnimControls.begin() + ptrdiff_t(i));
        };
        if (!same || !rs) {
            g_AnimControls.erase(g_AnimControls.begin() + ptrdiff_t(i));
            break;
        }
        if (!k.started) {
            if (now != k.sequence || At<int>(cs, kStateQueued) != 0) {
                // a transition the class queued as the play came (seen: its idle, at times) put it away: again
                if (At<int>(cs, kStateQueued) == 0 && ++k.waited > 2) {
                    if (k.replays >= kMaxReplays)
                        drop("did not start");
                    else
                        Replay(k, k.lastFrame >= 0 ? k.lastFrame : k.from);
                }
                break;
            }
            k.started = true;
            k.transitions = At<int>(cs, kStateTransitions);
            if (k.to < 0 && (k.hold || k.loop))
                k.to = float(At<int16_t>(rs, kRangeLast));
            if (k.speed <= 0 || k.frozen)
                At<float>(cs, kStatePlaySpeed) = 0;
        } else if (now != k.sequence || At<int>(cs, kStateTransitions) != k.transitions) {
            // moved on: at its end (a play to its end), or put away. An actor's is put back where it was; anyone
            // else's ends there (a hit, its death).
            const bool ended = k.to < 0 && k.lastFrame + 2 >= k.lastOfRange;
            if (ended || !IsScriptActor(k.character) || k.replays >= kMaxReplays) {
                drop(ended ? "played" : "ended by");
            } else {
                Replay(k, k.lastFrame);
            }
            break;
        }
        k.lastFrame = At<float>(rs, kRangeFrame);
        k.lastOfRange = At<int16_t>(rs, kRangeLast);
        if (k.to >= 0) {
            const float frame = At<float>(rs, kRangeFrame);
            const float step = At<float>(cs, kStateSpeed) * At<float>(cs, kStatePlaySpeed) * At<float>(rs, kRangeRate);
            if (k.bounce) {
                const float from = std::max(k.from >= 0 ? k.from : 0.0f, float(At<int16_t>(rs, kRangeFirst)));
                const float back = At<float>(cs, kStateSpeed) * k.speed * At<float>(rs, kRangeRate);
                if (!k.backwards && frame + step > k.to) {
                    k.backwards = true;
                    At<float>(cs, kStatePlaySpeed) = 0;
                } else if (k.backwards && frame - back <= from) {
                    k.backwards = false;
                    At<float>(rs, kRangeFrame) = from;
                    At<float>(cs, kStatePlaySpeed) = k.speed;
                } else if (k.backwards) {
                    At<float>(rs, kRangeFrame) = frame - back;
                }
            } else if (frame + step > k.to) {
                if (k.loop) {
                    const float from = k.from >= 0 ? k.from : float(At<int16_t>(rs, kRangeFirst));
                    if (from >= At<int16_t>(rs, kRangeFirst) && from <= At<int16_t>(rs, kRangeLast))
                        At<float>(rs, kRangeFrame) = from;
                    else
                        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(kSeekFrame)(cs, nullptr, int(from));
                } else if (k.hold) {
                    At<float>(rs, kRangeFrame) = k.to;
                    At<float>(cs, kStatePlaySpeed) = 0;
                    k.frozen = true;
                }
            }
        }
        break;
    }
    return reinterpret_cast<uint32_t(__fastcall*)(uint8_t*, void*, uint32_t)>(kTickScript)(cs, nullptr, arg);
}

void DropAnimControl(uint8_t* cs)
{
    g_AnimControls.erase(std::remove_if(g_AnimControls.begin(), g_AnimControls.end(),
                             [&](const AnimControl& k) { return k.state == cs; }),
        g_AnimControls.end());
}

// play_anim(c, name[, {from=, to=, loop=, bounce=, hold=, speed=, blend=}]): an animation (the character's own, or one
// of mission.ini's [Animations]), from frame `from` to frame `to` (the animation's own frame numbers; anim_frames tells
// them), then: loop (from..to again until stop_anim; bounce: to..from back, and again, so no jump), hold (the pose at `to` kept: a body that stays down), or (neither)
// on to its end and back to what the character does. speed 0 holds the pose at `from`. False when there is no such
// animation.
int L_play_anim(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    const char* name = luaL_checkstring(L, 2);
    AnimControl k;
    float blend = 0;
    uint8_t* partner = nullptr;
    std::string partnerSequence;
    float slow = 0.5f;
    if (lua_istable(L, 3)) {
        const auto number = [&](const char* field, float fallback) {
            lua_getfield(L, 3, field);
            const float v = float(luaL_optnumber(L, -1, fallback));
            lua_pop(L, 1);
            return v;
        };
        const auto flag = [&](const char* field) {
            lua_getfield(L, 3, field);
            const bool v = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return v;
        };
        k.from = number("from", -1);
        k.to = number("to", -1);
        k.speed = number("speed", 1);
        blend = number("blend", 0);
        k.bounce = flag("bounce");
        k.loop = flag("loop") || k.bounce;
        k.hold = flag("hold");
        lua_getfield(L, 3, "partner");
        partner = CheckCharacter(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "partner_anim");
        partnerSequence = luaL_optstring(L, -1, "");
        lua_pop(L, 1);
        slow = std::clamp(number("slow", 0.5f), 0.05f, 1.0f);
    }
    g_Grapples.erase(std::remove_if(g_Grapples.begin(), g_Grapples.end(), [&](const Grapple& g) { return g.attacker == c; }),
        g_Grapples.end());
    if (c && partner && partner != c && !partnerSequence.empty())
        g_Grapples.push_back({ c, At<uint32_t>(c, 4), partner, At<uint32_t>(partner, 4), partnerSequence, false, slow });
    uint8_t* sequence = FindSequence(c, name);
    uint8_t* cs = c ? At<uint8_t*>(c, kCharacterScript) : nullptr;
    if (!sequence || !cs) {
        LOG_WARN("Mission: play_anim %s: no such animation for %s (mission.ini [Animations])", name,
            c ? VirtualTypeName(c) : "nobody");
        lua_pushboolean(L, false);
        return 1;
    }
    DropAnimControl(cs);
    const bool played = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
        c, nullptr, sequence, blend > 0 ? blend : 0.0f, k.from >= 0 ? int(k.from) : 0, 0, k.speed > 0 ? k.speed : 1.0f);
    if (!played)
        LOG_WARN("Mission: play_anim %s: the game refused it", name);
    k.character = c;
    k.id = At<uint32_t>(c, 4);
    k.state = cs;
    k.sequence = sequence;
    g_AnimControls.push_back(k); // (also a plain play: put back if the class puts it away as it starts)
    lua_pushboolean(L, true);
    return 1;
}

// Hands a character's animation back to its class (what it would do now: its idle), as a sequence's own end does.
void BackToClass(uint8_t* cs)
{
    if (At<int>(cs, 0x24) <= 0) // its active states (the call reads the first)
        return;
    uint32_t* const globals[] = { reinterpret_cast<uint32_t*>(uintptr_t(0x006943CC)),
        reinterpret_cast<uint32_t*>(uintptr_t(0x0069652C)), reinterpret_cast<uint32_t*>(uintptr_t(kScriptInstance)),
        reinterpret_cast<uint32_t*>(uintptr_t(0x00696528)) };
    uint32_t saved[4];
    for (int i = 0; i < 4; ++i)
        saved[i] = *globals[i];
    Virtual<void(__fastcall*)(uint8_t*, void*)>(cs, 0xC)(cs, nullptr); // its script context
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, int, int)>(uintptr_t(0x0038A140))(*globals[2], *globals[1], -1, -1);
    for (int i = 0; i < 4; ++i)
        *globals[i] = saved[i];
}

// stop_anim(c): ends play_anim's animation (a loop, a held pose); the character goes back to what it does.
int L_stop_anim(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    uint8_t* cs = c ? At<uint8_t*>(c, kCharacterScript) : nullptr;
    if (!cs)
        return 0;
    DropAnimControl(cs);
    At<float>(cs, kStatePlaySpeed) = 1.0f;
    BackToClass(cs);
    return 0;
}

// anim_frames(c): the current animation's frame, its first and last frames, and its name (for play_anim's from/to).
int L_anim_frames(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    uint8_t* cs = c ? At<uint8_t*>(c, kCharacterScript) : nullptr;
    uint8_t* rs = cs ? At<uint8_t*>(cs, kStateRange) : nullptr;
    uint8_t* sequence = cs ? At<uint8_t*>(cs, kStateSequence) : nullptr;
    if (!rs || !sequence)
        return 0;
    lua_pushnumber(L, At<float>(rs, kRangeFrame));
    lua_pushinteger(L, At<int16_t>(rs, kRangeFirst));
    lua_pushinteger(L, At<int16_t>(rs, kRangeLast));
    const char* animation = At<const char*>(sequence, 0x10);
    lua_pushstring(L, animation ? animation : "");
    return 4;
}

// The names of a character's moves (its sequences), for finding the right one.
int L_moves(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    lua_newtable(L);
    uint8_t* script = c ? At<uint8_t*>(c, kCharacterScript) : nullptr;
    uint8_t* list = script ? At<uint8_t*>(script, 4) : nullptr;
    if (!list)
        return 1;
    const int count = At<int>(list, 8);
    uint8_t** items = At<uint8_t**>(list, 0xC);
    for (int i = 0, k = 1; i < count; ++i)
        if (const char* n = items[i] ? At<const char*>(items[i], 0xC) : nullptr) {
            lua_pushstring(L, n);
            lua_rawseti(L, -2, k++);
        }
    return 1;
}

// The character's sabers (every hand's), for the script's saber(); none for a character without one.
std::vector<uint8_t*> Sabers(uint8_t* c)
{
    std::vector<uint8_t*> sabers;
    for (uint32_t slot = 0; c && slot < 4; ++slot)
        if (uint8_t* w = At<uint8_t*>(c, kCharacterWeapons + slot * 0x20))
            if (std::find(std::begin(kSaberVtables), std::end(kSaberVtables), At<uint32_t>(w, 0)) != std::end(kSaberVtables))
                sabers.push_back(w);
    return sabers;
}

// saber(c, on [, seconds]): its sabers lit or put away, as the game's cinematics do (the blade grows or shrinks over
// `seconds`, with the sound; 0: at once, silent); saber(c): whether one is lit
int L_saber(lua_State* L)
{
    const std::vector<uint8_t*> sabers = Sabers(CheckCharacter(L, 1));
    if (lua_isnoneornil(L, 2)) {
        bool lit = false;
        for (uint8_t* s : sabers)
            lit = lit || At<uint8_t>(s, kSaberLit) != 0;
        lua_pushboolean(L, lit);
        return 1;
    }
    const bool on = lua_toboolean(L, 2) != 0;
    const float seconds = float(std::max(0.0, luaL_optnumber(L, 3, 0.4)));
    for (uint8_t* s : sabers) {
        g_SabersAway.erase(std::remove(g_SabersAway.begin(), g_SabersAway.end(), s), g_SabersAway.end());
        if (!on)
            g_SabersAway.push_back(s);
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float)>(on ? kSaberExtend : kSaberRetract)(s, nullptr, seconds);
    }
    lua_pushboolean(L, !sabers.empty());
    return 1;
}

// A marker of mission.ini's [Markers] by name: the level's object, or null.
uint8_t* MarkerObject(const char* name)
{
    for (size_t k = 0; k < g_Markers.size(); ++k)
        if (_stricmp(g_Markers[k].name.c_str(), name) == 0) {
            uint8_t* m = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(kDevMapFirstMarkerId + uint32_t(k));
            return m && At<uint32_t>(m, 0) == kGotoMarkerVtable ? m : nullptr;
        }
    return nullptr;
}

// go(c, marker [, fight]): it walks (runs) to a marker of [Markers] its own way, round what is in the way; with fight,
// it fights what it meets on the way. False when there is no such marker.
// Characters on their way with their AI on for it (a goto is the AI's to walk: with it off, nobody moves): off again
// once there (MissionFrame).
struct Walker {
    uint8_t* character;
    uint32_t id;
    uint8_t* marker;
};
std::vector<Walker> g_Walkers;

void WalkersFrame()
{
    for (size_t i = 0; i < g_Walkers.size();) {
        Walker& w = g_Walkers[i];
        const bool same = At<uint32_t>(w.character, 4) == w.id;
        if (same && reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*)>(kExecutingGoto)(w.character, nullptr, w.marker)) {
            ++i;
            continue;
        }
        if (same)
            if (uint8_t* brain = At<uint8_t*>(w.character, kCharacterAI))
                brain[kAIOff] = 1;
        g_Walkers.erase(g_Walkers.begin() + ptrdiff_t(i));
    }
}

// A walk called off (a cut moved the character, the script gave it back its AI): it stops going there, and its AI is
// left as the script set it (else a walk cut short would go on to the marker once the AI is back on, and switch it off
// there).
void CancelWalk(uint8_t* c, bool stopGoing)
{
    g_Walkers.erase(std::remove_if(g_Walkers.begin(), g_Walkers.end(), [&](const Walker& w) { return w.character == c; }),
        g_Walkers.end());
    uint8_t* brain = stopGoing ? At<uint8_t*>(c, kCharacterAI) : nullptr;
    if (!brain)
        return;
    const int count = At<int>(brain, kAIControllerCount);
    uint8_t** items = At<uint8_t**>(brain, kAIControllers);
    for (int i = 0; items && i < count; ++i)
        if (items[i] && At<uint32_t>(items[i], kControllerType) == kGotoController)
            Virtual<void(__fastcall*)(uint8_t*, void*, uint8_t*, float)>(items[i], kSetGoto)(items[i], nullptr, nullptr, 0.0f);
}

int L_go(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    const char* name = luaL_checkstring(L, 2);
    uint8_t* marker = MarkerObject(name);
    if (!marker)
        LOG_WARN("Mission: go: no marker %s", name);
    if (c && marker) {
        g_Walkers.erase(std::remove_if(g_Walkers.begin(), g_Walkers.end(), [&](const Walker& w) { return w.character == c; }),
            g_Walkers.end());
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*, bool)>(kCharacterGoto)(c, nullptr, marker, lua_toboolean(L, 3) != 0);
        uint8_t* brain = At<uint8_t*>(c, kCharacterAI);
        if (brain && brain[kAIOff]) { // (an actor stays one: no one's target, no hits)
            brain[kAIOff] = 0;
            g_Walkers.push_back({ c, At<uint32_t>(c, 4), marker });
        }
    }
    lua_pushboolean(L, c && marker);
    return 1;
}

// going(c, marker): it is still on its way there
int L_going(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    uint8_t* marker = MarkerObject(luaL_checkstring(L, 2));
    lua_pushboolean(L, c && marker && reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*)>(kExecutingGoto)(c, nullptr, marker));
    return 1;
}

// marker(name): its x, z (nil when there is none)
int L_marker(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    for (const Marker& m : g_Markers)
        if (_stricmp(m.name.c_str(), name) == 0) {
            lua_pushnumber(L, m.x);
            lua_pushnumber(L, m.z);
            return 2;
        }
    return 0;
}

// The fields' colour (mission.ini FieldColour=r g b, e.g. Theed's red lasers): their two textures (the separatist
// door field's, DXT5 64x64 / 128x32, which the Jedi Temple field model names) recoloured as they load, the glow's
// brightness kept and its hot core whiter; none: the game's own blue.
bool g_FieldTinted = false;
float g_FieldTint[3] = {};

void TintFieldTexture(std::vector<uint8_t>& data)
{
    constexpr size_t kPixels = 0x184;
    if (!g_FieldTinted || data.size() < kPixels || std::memcmp(data.data(), "STX", 4) != 0 || data[0x3C] != 0x52)
        return;
    const auto unpack = [](uint16_t c, float rgb[3]) {
        rgb[0] = float((c >> 11) & 31) * 255.0f / 31.0f;
        rgb[1] = float((c >> 5) & 63) * 255.0f / 63.0f;
        rgb[2] = float(c & 31) * 255.0f / 31.0f;
    };
    const auto pack = [](const float rgb[3]) {
        const auto q = [](float v, int bits) { return std::clamp(int(v * float((1 << bits) - 1) / 255.0f + 0.5f), 0, (1 << bits) - 1); };
        return uint16_t(q(rgb[0], 5) << 11 | q(rgb[1], 6) << 5 | q(rgb[2], 5));
    };
    // DXT5 blocks: 8 bytes of alpha, then the colour block (always four colours: its two end colours can be recoloured
    // in place)
    for (size_t at = kPixels; at + 16 <= data.size(); at += 16)
        for (size_t end = 0; end < 2; ++end) {
            uint8_t* c = &data[at + 8 + end * 2];
            float rgb[3];
            unpack(uint16_t(c[0] | c[1] << 8), rgb);
            const float light = std::max({ rgb[0], rgb[1], rgb[2] }) / 255.0f;
            const float core = std::clamp((light - 0.92f) * 3.0f, 0.0f, 0.2f); // the brightest a little whiter
            float out[3];
            for (int k = 0; k < 3; ++k)
                out[k] = std::min(255.0f, g_FieldTint[k] * light * (1 - core) + 255.0f * light * core);
            const uint16_t v = pack(out);
            c[0] = uint8_t(v);
            c[1] = uint8_t(v >> 8);
        }
}

// field(name, on): a force field of [Fields] on (lit, blocking) or off (fading away, open)
int L_field(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    const bool on = lua_toboolean(L, 2) != 0;
    for (size_t k = 0; k < g_Fields.size(); ++k) {
        if (_stricmp(g_Fields[k].c_str(), name) != 0)
            continue;
        bool switched = false;
        for (uint32_t side = 0; side < 2; ++side) { // its two halves (back to back)
            uint8_t* f = ObjectById(kFieldFirstId + uint32_t(2 * k) + side);
            if (!f || At<uint32_t>(f, 0) != kForceFieldVtable)
                continue;
            Virtual<void(__fastcall*)(uint8_t*, void*)>(f, on ? kFieldOn : kFieldOff)(f, nullptr);
            switched = true;
        }
        lua_pushboolean(L, switched);
        return 1;
    }
    LOG_WARN("Mission: field: no force field %s", name);
    lua_pushboolean(L, 0);
    return 1;
}

// boss(c): the character is the mission's boss, as the story duels' (Dooku, Grievous): the game manager's player 2
// slot (its health bar on the HUD); boss(nil): none.
int L_boss(lua_State* L)
{
    uint8_t* manager = *reinterpret_cast<uint8_t**>(uintptr_t(kGameManager));
    uint8_t* c = lua_isnoneornil(L, 1) ? nullptr : CheckCharacter(L, 1);
    if (!manager)
        return 0;
    if (c) {
        At<uint32_t>(manager, kManagerPlayerIds + 4) = At<uint32_t>(c, 4);
        At<int>(manager, kManagerPlayerCount) = 2;
    } else {
        At<uint32_t>(manager, kManagerPlayerIds + 4) = 0;
        At<int>(manager, kManagerPlayerCount) = 1;
    }
    return 0;
}

int L_kill(lua_State* L)
{
    if (uint8_t* c = CheckCharacter(L, 1)) {
        At<uint8_t>(c, kCharacterInvincible) = 0;
        Virtual<void(__fastcall*)(uint8_t*, void*, uint32_t)>(c, kInstantKillSlot)(c, nullptr, 0);
    }
    return 0;
}

int L_class_of(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    const char* name = c ? VirtualTypeName(c) : nullptr;
    if (name)
        lua_pushstring(L, name);
    else
        lua_pushnil(L);
    return 1;
}

// The player becomes this character: the player's class changes to its class (the live swap) where it stands, with
// its health; the character itself leaves.
int L_set_player(lua_State* L)
{
    uint8_t* c = CheckCharacter(L, 1);
    uint8_t* player = PlayerObject();
    const char* name = c ? VirtualTypeName(c) : nullptr;
    if (!c || !player || !name || c == player) {
        lua_pushboolean(L, c == player);
        return 1;
    }
    float m[16], was[16];
    std::memcpy(m, c + kCharacterTransform, sizeof m);
    std::memcpy(was, player + kCharacterTransform, sizeof was);
    const float health = At<float>(c, kCharacterHealth), most = At<float>(c, kCharacterMaxHealth);
    const float wasHealth = At<float>(player, kCharacterHealth), wasMost = At<float>(player, kCharacterMaxHealth);
    const CustomCharacter* custom = CustomCharacterOf(c);
    const std::string className = custom ? custom->className : name;
    const char* wasName = VirtualTypeName(player);
    const CustomCharacter* wasCustom = CustomCharacterOf(player);
    const std::string wasClass = wasCustom ? wasCustom->className : (wasName ? wasName : "");
    // the character goes; the player stands where it stood, then changes into its class
    lua_settop(L, 1);
    L_remove(L);
    Virtual<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
    SetPlayerClass(className.c_str(), false);
    std::string error;
    const bool ok = ReplacePlayer(error);
    if (!ok)
        LOG_WARN("Mission: set_player %s: %s", className.c_str(), error.c_str());
    if (uint8_t* now = PlayerObject()) {
        At<float>(now, kCharacterMaxHealth) = most;
        At<float>(now, kCharacterHealth) = std::max(1.0f, health);
    }
    // the character the player was stays behind where it stood, on the player's side (returned)
    uint8_t* left = nullptr;
    if (ok && !wasClass.empty()) {
        std::string spawnError;
        left = SpawnCharacterAt(wasClass.c_str(), "", "", "", SpawnSide::Ally, was, spawnError);
        if (left) {
            At<float>(left, kCharacterMaxHealth) = wasMost;
            At<float>(left, kCharacterHealth) = std::max(1.0f, wasHealth);
        } else {
            LOG_WARN("Mission: set_player: the player's %s could not stay: %s", wasClass.c_str(), spawnError.c_str());
        }
    }
    InvalidateLevelCharacters();
    lua_pushboolean(L, ok);
    if (left)
        lua_pushinteger(L, AddHandle(left));
    else
        lua_pushnil(L);
    return 2;
}

// --- Cameras: shots the port places the game's camera at (freecam.h's director) -----------------------------------
void LookAt(const float eye[3], const float at[3], float rows[16])
{
    float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    const float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (n < 1e-3f)
        return;
    for (float& v : f)
        v /= n;
    float r[3] = { f[2], 0, -f[0] };
    const float rn = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rn < 1e-4f)
        return;
    r[0] /= rn;
    r[2] /= rn;
    const float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    const float* src[4] = { r, u, f, eye };
    for (int i = 0; i < 4; ++i) {
        rows[i * 4 + 0] = src[i][0];
        rows[i * 4 + 1] = src[i][1];
        rows[i * 4 + 2] = src[i][2];
        rows[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
    }
}

float Ease(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

void MapPoint(const float in[3], float out[3])
{
    ToWorld(in[0], in[2], out[0], out[2]);
    out[1] = kDevMapFloorY + in[1];
}

bool Director(float rows[16], float)
{
    const Shot& s = g_Shot;
    if (!g_Active || s.kind == ShotKind::None)
        return false;
    const float t = Ease(float((Now() - s.startedAt) / std::max(0.01f, s.seconds)));
    float eye[3], at[3];
    uint8_t* target = s.target ? Resolve(s.target) : nullptr;
    switch (s.kind) {
    case ShotKind::Fixed:
        MapPoint(s.eye, eye);
        MapPoint(s.look, at);
        break;
    case ShotKind::Track: {
        if (!target)
            return false;
        MapPoint(s.eye, eye);
        const float* p = Position(target);
        at[0] = p[0];
        at[1] = p[1] + s.lookHeight;
        at[2] = p[2];
        break;
    }
    case ShotKind::Orbit: {
        if (!target)
            return false;
        const float* p = Position(target);
        const float base = std::atan2(s.facing[0], s.facing[1]);
        const float angle = base + (s.fromDeg + (s.toDeg - s.fromDeg) * t) * 3.14159265f / 180.0f;
        eye[0] = p[0] + std::sin(angle) * s.radius;
        eye[1] = p[1] + s.height;
        eye[2] = p[2] + std::cos(angle) * s.radius;
        at[0] = p[0];
        at[1] = p[1] + s.lookHeight;
        at[2] = p[2];
        break;
    }
    case ShotKind::Move: {
        if (!target)
            return false;
        const float* p = Position(target);
        float d[3];
        for (int k = 0; k < 3; ++k)
            d[k] = s.from[k] + (s.to[k] - s.from[k]) * t;
        // the offset in the target's frame at the shot's start: right (x), up (y), forward (z)
        const float fx = s.facing[0], fz = s.facing[1];
        eye[0] = p[0] + fz * d[0] + fx * d[2];
        eye[1] = p[1] + d[1];
        eye[2] = p[2] - fx * d[0] + fz * d[2];
        at[0] = p[0];
        at[1] = p[1] + s.lookHeight;
        at[2] = p[2];
        break;
    }
    default:
        return false;
    }
    LookAt(eye, at, rows);
    return true;
}

void StartShot(Shot shot)
{
    shot.startedAt = Now();
    if (uint8_t* c = shot.target ? Resolve(shot.target) : nullptr) {
        const float* f = reinterpret_cast<const float*>(c + kCharacterTransform + 32);
        const float n = std::max(1e-4f, std::hypot(f[0], f[2]));
        shot.facing[0] = f[0] / n;
        shot.facing[1] = f[2] / n;
    }
    if (g_Skip) // skipping: the shot's end at once
        shot.startedAt -= shot.seconds;
    g_Shot = shot;
}

int L_camera_fixed(lua_State* L)
{
    Shot s;
    s.kind = ShotKind::Fixed;
    for (int k = 0; k < 3; ++k) {
        s.eye[k] = float(luaL_checknumber(L, 1 + k));
        s.look[k] = float(luaL_checknumber(L, 4 + k));
    }
    // (x, y, z) in the script: x and z on the map, y the height above its floor
    StartShot(s);
    return 0;
}

int L_camera_orbit(lua_State* L)
{
    Shot s;
    s.kind = ShotKind::Orbit;
    s.target = CheckHandle(L, 1);
    s.radius = float(luaL_optnumber(L, 2, 200));
    s.height = float(luaL_optnumber(L, 3, 55));
    s.fromDeg = float(luaL_optnumber(L, 4, 0));
    s.toDeg = float(luaL_optnumber(L, 5, 90));
    s.seconds = float(luaL_optnumber(L, 6, 5));
    s.lookHeight = float(luaL_optnumber(L, 7, 50));
    StartShot(s);
    return 0;
}

int L_camera_move(lua_State* L)
{
    Shot s;
    s.kind = ShotKind::Move;
    s.target = CheckHandle(L, 1);
    for (int k = 0; k < 3; ++k) {
        s.from[k] = float(luaL_checknumber(L, 2 + k));
        s.to[k] = float(luaL_optnumber(L, 5 + k, s.from[k]));
    }
    s.seconds = float(luaL_optnumber(L, 8, 4));
    s.lookHeight = float(luaL_optnumber(L, 9, 50));
    StartShot(s);
    return 0;
}

int L_camera_track(lua_State* L)
{
    Shot s;
    s.kind = ShotKind::Track;
    for (int k = 0; k < 3; ++k)
        s.eye[k] = float(luaL_checknumber(L, 1 + k));
    s.target = CheckHandle(L, 4);
    s.lookHeight = float(luaL_optnumber(L, 5, 50));
    StartShot(s);
    return 0;
}

// camera_fov(degrees): the lens (the field of view) until camera_game(); nil or 0: the game's (the levels' own is
// about 80 degrees)
int L_camera_fov(lua_State* L)
{
    const double degrees = std::clamp(luaL_optnumber(L, 1, 0), 0.0, 150.0);
    SetCameraFov(degrees > 0 ? float(degrees * 3.14159265 / 180.0) : 0.0f);
    return 0;
}

int L_camera_game(lua_State*)
{
    SetCameraFov(0);
    g_Shot = Shot{};
    RefocusCameraOnPlayer();
    return 0;
}

// --- The screen ------------------------------------------------------------------------------------------------------
// The game's own cutscene bars (ILetterBox, [0x69226C]; its "set", thiscall (on, seconds, size), as the game's
// cinematics call it: 0.5 s, 0.125 of the screen): they slide in and out; while they are in, the game counts it a
// cutscene (0xA3720: its HUD hidden).
constexpr uint32_t kLetterBox = 0x0069226C;
constexpr uintptr_t kLetterBoxSet = 0x0012DF30;

void GameLetterbox(bool on, float seconds)
{
    uint8_t* box = *reinterpret_cast<uint8_t**>(uintptr_t(kLetterBox));
    if (!box || !(At<uint32_t>(box, 0xC) & 0x40000))
        return;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, bool, float, float)>(kLetterBoxSet)(box, nullptr, on, seconds, 0.125f);
}

int L_letterbox(lua_State* L)
{
    GameLetterbox(lua_toboolean(L, 1) != 0, g_Skip ? 0.05f : float(luaL_optnumber(L, 2, 0.5)));
    return 0;
}

int L_fade(lua_State* L, double to)
{
    g_FadeFrom = g_Hud.fade;
    g_FadeTo = to;
    g_FadeLength = std::max(0.01, luaL_optnumber(L, 1, 1.0));
    g_FadeStart = g_Skip ? Now() - g_FadeLength : Now();
    return 0;
}
int L_fade_out(lua_State* L) { return L_fade(L, 1.0); }
int L_fade_in(lua_State* L) { return L_fade(L, 0.0); }

// The game's own objectives (ITriggerObjective, placed in the level for mission.ini's [Objectives], their texts the
// port's own ids): a trigger's first firing (vfunc +0xA4, 0x13FBD0) makes its objective the new one (the HUD's
// "NEW OBJECTIVE:", the pause screen's list), its second completes it.
uint8_t* ObjectiveObject(const char* name)
{
    for (size_t k = 0; k < g_Objectives.size(); ++k)
        if (_stricmp(g_Objectives[k].c_str(), name) == 0) {
            uint8_t* o = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(kObjectiveFirstId + uint32_t(k));
            if (o && At<uint32_t>(o, 0) != kTriggerObjectiveVtable)
                LOG_WARN("Mission: objective %s: object 0x%X is a %s", name, kObjectiveFirstId + uint32_t(k), VirtualTypeName(o));
            else if (!o)
                LOG_WARN("Mission: objective %s: no object 0x%X in the level", name, kObjectiveFirstId + uint32_t(k));
            return o && At<uint32_t>(o, 0) == kTriggerObjectiveVtable ? o : nullptr;
        }
    return nullptr;
}

// objective(name): [Objectives]' objective becomes the new one; objective_done(name): it is completed
int L_objective(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    uint8_t* o = ObjectiveObject(name);
    if (!o) {
        LOG_WARN("Mission: objective: no objective %s in [Objectives]", name);
        return 0;
    }
    Virtual<void(__fastcall*)(uint8_t*, void*)>(o, kTriggerFire)(o, nullptr);
    LOG_INFO("Mission: objective: %s", name);
    return 0;
}

// tip(name): a tip of [Tips] shows, as the game's own tips do
int L_tip(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    for (size_t k = 0; k < g_Tips.size(); ++k) {
        if (_stricmp(g_Tips[k].c_str(), name) != 0)
            continue;
        uint8_t* t = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(kTipFirstId + uint32_t(k));
        if (t && At<uint32_t>(t, 0) == kVaderTipVtable) {
            Virtual<void(__fastcall*)(uint8_t*, void*)>(t, kTriggerFire)(t, nullptr);
            LOG_INFO("Mission: tip: %s", name);
        } else {
            LOG_WARN("Mission: tip %s: its object is not in the level", name);
        }
        return 0;
    }
    LOG_WARN("Mission: tip: no tip %s in [Tips]", name);
    return 0;
}

int L_objective_done(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    if (uint8_t* o = ObjectiveObject(name)) {
        Virtual<void(__fastcall*)(uint8_t*, void*)>(o, kTriggerFire)(o, nullptr);
        LOG_INFO("Mission: objective done: %s", name);
    }
    return 0;
}

int L_title(lua_State* L)
{
    g_Hud.title = luaL_checkstring(L, 1);
    g_Hud.subtitle = luaL_optstring(L, 2, "");
    g_TitleAt = Now();
    g_TitleUntil = g_TitleAt + luaL_optnumber(L, 3, 4.0);
    return 0;
}

int L_notice(lua_State* L)
{
    g_Hud.notice = luaL_checkstring(L, 1);
    g_NoticeAt = Now();
    g_NoticeUntil = g_NoticeAt + luaL_optnumber(L, 2, 3.0);
    return 0;
}

double LineSeconds(const std::string& text) { return 1.4 + 0.06 * double(text.size()); }

// subtitle(speaker, text [, seconds]): shows a line; returns its length (seconds)
int L_subtitle(lua_State* L)
{
    g_Hud.speaker = luaL_optstring(L, 1, "");
    g_Hud.line = luaL_checkstring(L, 2);
    const double seconds = luaL_optnumber(L, 3, LineSeconds(g_Hud.line));
    g_LineUntil = Now() + seconds;
    LOG_INFO("Mission: %s: %s", g_Hud.speaker.c_str(), g_Hud.line.c_str());
    lua_pushnumber(L, seconds);
    return 1;
}

// A sound file of the mission's folder played (a WAV: 16-bit PCM, mono or stereo); its length in seconds, 0 when it
// cannot be played.
double PlayMissionSound(const std::string& file, float volume)
{
    std::vector<int16_t> samples;
    unsigned channels = 0, rate = 0;
    if (!ReadWav(g_Folder + L"\\" + std::wstring(file.begin(), file.end()), samples, channels, rate)) {
        LOG_WARN("Mission: sound %s: not a 16-bit PCM WAV (mono or stereo) in the mission's folder", file.c_str());
        return 0;
    }
    const double seconds = double(samples.size()) / channels / rate;
    if (!audio::PlayPortSound(std::move(samples), channels, rate, volume)) {
        LOG_WARN("Mission: sound %s could not be played (no audio)", file.c_str());
        return 0;
    }
    LOG_INFO("Mission: sound %s (%.1f s)", file.c_str(), seconds);
    return seconds;
}

// sound(file [, volume]): a WAV of the mission's folder played (stingers, effects); returns its length (seconds)
int L_sound(lua_State* L)
{
    lua_pushnumber(L, PlayMissionSound(luaL_checkstring(L, 1), float(luaL_optnumber(L, 2, 1.0))));
    return 1;
}

int L_stop_sounds(lua_State*)
{
    audio::StopPortSounds();
    return 0;
}

// A speaker name lent for a line (a character without lines of its own says it through another's entry).
constexpr uint32_t kSpeakerName = 0xA04; // the character's subtitle name (a text handle, ICharacter::Init 0x15BC56)
struct LentName {
    uint8_t* character = nullptr;
    uint32_t own = 0;
    double until = 0;
};
LentName g_LentName;

void RestoreLentName()
{
    if (g_LentName.character)
        for (uint8_t* c : LevelCharacters())
            if (c == g_LentName.character)
                At<uint32_t>(c, kSpeakerName) = g_LentName.own;
    g_LentName = {};
}

// A line's speaker: a character of the cast by its name, or "player"; null for anyone else (the line is the port's
// subtitle then).
uint8_t* SpeakerOf(const std::string& name)
{
    if (_stricmp(name.c_str(), "player") == 0)
        return PlayerObject();
    for (const CastMember& m : g_Cast) {
        if (_stricmp(m.name.c_str(), name.c_str()) != 0)
            continue;
        for (uint8_t* c : LevelCharacters())
            if (At<uint32_t>(c, 4) == m.placedId)
                return c;
    }
    return nullptr;
}

// line(id): a line of lines.ini shown (its voice played once voices are supported); returns its length
int L_line(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const auto it = g_Lines.find(id);
    if (it == g_Lines.end()) {
        LOG_WARN("Mission: no line %s in lines.ini", id);
        lua_pushnumber(L, 0);
        return 1;
    }
    const Line& line = it->second;
    // a cutscene being skipped says nothing more (it rushes on: its lines would pile up over each other)
    if (g_Skip) {
        lua_pushnumber(L, 0);
        return 1;
    }
    // the game's own: the speaker (a character of the cast, "player", or the one given) says it, its subtitle
    // "SPEAKER: text" and its voice the game's
    // line(id [, speaker, fallback]): -1 when the dialogue engine is busy (say() asks again on the next frames);
    // with fallback, the port's own subtitle then
    uint8_t* speaker = !lua_isnoneornil(L, 2) ? CheckCharacter(L, 2) : SpeakerOf(line.speaker);
    if (speaker && !line.lineId.empty()) {
        // (a held scene has the game's "AI disabled" on, which keeps characters quiet: off for the line's start)
        // and the game's letterbox makes it a cutscene (0xA3720), in which characters say nothing of their own: both
        // off for the line's start
        uint8_t* options = GameOptions();
        const uint8_t aiOff = options ? options[kOptionsAiDisabled] : 0;
        if (options)
            options[kOptionsAiDisabled] = 0;
        uint8_t* box = *reinterpret_cast<uint8_t**>(uintptr_t(kLetterBox));
        const bool boxed = box && (At<uint32_t>(box, 0xC) & 0x40000);
        const int boxState = boxed ? At<int>(box, 0x1E8) : 0;
        if (boxed)
            At<int>(box, 0x1E8) = 0;
        RestoreLentName(); // a name lent for the last line: back before anyone says the next
        int said = SayLine(speaker, line.lineId.c_str());
        if (said < 0) {
            // its speaker has no lines in this level (Palpatine's are his levels' own): another character's entry says
            // it under the speaker's name (the subtitle's name, +0xA04, lent for the line's time)
            for (uint8_t* proxy : LevelCharacters()) {
                if (proxy == speaker || !Alive(proxy))
                    continue;
                RestoreLentName();
                g_LentName = { proxy, At<uint32_t>(proxy, kSpeakerName), Now() + line.length + 0.5 };
                At<uint32_t>(proxy, kSpeakerName) = At<uint32_t>(speaker, kSpeakerName);
                said = SayLine(proxy, line.lineId.c_str());
                if (said > 0)
                    break;
                RestoreLentName();
                if (said == 0)
                    break; // busy: asked again later
            }
        }
        if (boxed)
            At<int>(box, 0x1E8) = boxState;
        if (options)
            options[kOptionsAiDisabled] = aiOff;
        if (said > 0) {
            LOG_INFO("Mission: %s %s %s: %s", line.speaker.c_str(), said == 2 ? "will say (queued)" : "says",
                line.lineId.c_str(), line.text.c_str());
            lua_pushnumber(L, line.length);
            lua_pushboolean(L, said == 2); // queued: it starts when the line before it ends
            return 2;
        }
        if (!lua_toboolean(L, 3)) {
            lua_pushnumber(L, -1);
            return 1;
        }
    }
    g_Hud.speaker = line.speaker;
    g_Hud.line = line.text;
    double seconds = line.seconds > 0 ? line.seconds : LineSeconds(line.text);
    if (!line.voice.empty()) { // its voice: the line lasts as long as it (at least)
        const double voice = PlayMissionSound(line.voice, 1.0f);
        if (voice > 0 && line.seconds <= 0)
            seconds = voice + 0.3;
    }
    g_LineUntil = Now() + seconds;
    LOG_INFO("Mission: %s: %s", line.speaker.c_str(), line.text.c_str());
    lua_pushnumber(L, seconds);
    return 1;
}

int L_timer(lua_State* L)
{
    // timer(label, seconds): counts down from seconds; timer(label): counts up from 0
    g_Hud.timerShown = true;
    g_Hud.timerLabel = luaL_optstring(L, 1, "");
    g_TimerCountsDown = lua_isnumber(L, 2) != 0;
    g_TimerStart = Now();
    g_TimerEnd = g_TimerStart + (g_TimerCountsDown ? lua_tonumber(L, 2) : 0);
    g_TimerRunning = true;
    return 0;
}

double TimerValue()
{
    if (!g_Hud.timerShown)
        return 0;
    if (!g_TimerRunning)
        return g_TimerEnd;
    return g_TimerCountsDown ? std::max(0.0, g_TimerEnd - Now()) : Now() - g_TimerStart;
}

int L_timer_value(lua_State* L)
{
    lua_pushnumber(L, TimerValue());
    return 1;
}

int L_timer_stop(lua_State* L)
{
    const double v = TimerValue();
    g_TimerRunning = false;
    g_TimerEnd = v;
    if (lua_toboolean(L, 1)) // timer_stop(true): hidden too
        g_Hud.timerShown = false;
    return 0;
}

int L_counter(lua_State* L)
{
    const std::string label = luaL_checkstring(L, 1);
    auto& list = g_Hud.counters;
    const auto it = std::find_if(list.begin(), list.end(), [&](const auto& c) { return c.first == label; });
    if (lua_isnoneornil(L, 2)) {
        if (it != list.end())
            list.erase(it);
        return 0;
    }
    const std::string value = luaL_tolstring(L, 2, nullptr);
    lua_pop(L, 1);
    if (it != list.end())
        it->second = value;
    else
        list.push_back({ label, value });
    return 0;
}

int L_hud(lua_State* L)
{
    debug::WriteGameVariable("hud", lua_toboolean(L, 1) ? "1" : "0");
    return 0;
}

int L_set_var(lua_State* L)
{
    lua_pushboolean(L, debug::WriteGameVariable(luaL_checkstring(L, 1), luaL_tolstring(L, 2, nullptr)));
    return 1;
}

int L_get_var(lua_State* L)
{
    std::string value;
    if (debug::ReadGameVariable(luaL_checkstring(L, 1), value))
        lua_pushstring(L, value.c_str());
    else
        lua_pushnil(L);
    return 1;
}

// music_volume(f): the music at f times the player's own music volume (the game variable volumemusic; 1: as set)
int L_music_volume(lua_State* L)
{
    if (g_PlayersMusicVolume < 0) {
        std::string value;
        g_PlayersMusicVolume = debug::ReadGameVariable("volumemusic", value) ? std::strtof(value.c_str(), nullptr) : 1.0f;
    }
    const float f = std::clamp(float(luaL_checknumber(L, 1)), 0.0f, 1.0f);
    char text[32];
    std::snprintf(text, sizeof text, "%.3f", g_PlayersMusicVolume * f);
    debug::WriteGameVariable("volumemusic", text);
    return 0;
}

int L_command(lua_State* L)
{
    debug::QueueConsoleCommand(luaL_checkstring(L, 1));
    return 0;
}

int L_skipping(lua_State* L)
{
    lua_pushboolean(L, g_Skip);
    return 1;
}

int L_forced_starts(lua_State* L)
{
    lua_pushinteger(L, ForcedLineStarts());
    return 1;
}

// stop_lines(): the lines being said stop (their voices fade out, their subtitles go).
int L_stop_lines(lua_State* L)
{
    lua_pushinteger(L, StopDialogueLines());
    return 1;
}

// The prelude's cutscene(): the skip state, so waits end at once while skipping.
int L_cutscene_state(lua_State* L)
{
    const bool on = lua_toboolean(L, 1) != 0;
    g_InCutscene = on;
    SetDialogueQuiet(on); // no barks in a cutscene: only its own lines
    if (on) // and none from before it runs on over it (a fighter's taunt as the scene starts)
        if (const int stopped = StopDialogueLines())
            LOG_INFO("Mission: the cutscene stops %d line(s) being said", stopped);
    g_Skip = false;
    g_SkipAskedAt = -10;
    if (on)
        input::TakeHeldPress(); // a press from before does not skip it
    return 0;
}

// A cutscene is skipped by a second press (A or Start) within a few seconds of the first, which asks; one press by
// mistake (Start to pause) does not skip it.
int L_skip_pressed(lua_State* L)
{
    if (g_InCutscene && g_Held && input::TakeHeldPress()) {
        if (Now() - g_SkipAskedAt < 3.0) {
            g_Skip = true;
            StopDialogueLines(); // (a skipped scene's lines end with it)
            g_NoticeUntil = Now();
        } else {
            g_SkipAskedAt = Now();
            g_Hud.notice = "Press again to skip";
            g_NoticeAt = Now();
            g_NoticeUntil = g_NoticeAt + 3.0;
        }
    }
    lua_pushboolean(L, g_Skip);
    return 1;
}

// The developer's "next" outside a cutscene: true once (the wait that sees it ends), for the story's own waits only
// (main's): a thread's wait (one watching for a death, say) is not the story's, and must not take it.
lua_State* g_MainThread = nullptr;

int L_advance(lua_State* L)
{
    const bool advance = g_Advance && !g_InCutscene && L == g_MainThread;
    if (advance)
        g_Advance = false;
    lua_pushboolean(L, advance);
    return 1;
}

void End(bool ok, const std::string& detail)
{
    if (g_EndAt > 0)
        return;
    g_EndOk = ok;
    g_EndAt = Now();
    g_Hud.ending = ok ? "MISSION COMPLETE" : "MISSION FAILED";
    g_Hud.endingDetail = detail;
    Hold(true); // everyone stops for the end
    RestoreSaberColor();
    reinterpret_cast<void(__cdecl*)(float, int, int, int)>(kSetSlowMotion)(1.0f, 0, 0, 0);
    g_MissionSlow = false;
    if (!g_PlayersSubtitles.empty()) // the player's own subtitles setting back
        debug::WriteGameVariable("subtitles", g_PlayersSubtitles);
    if (g_PlayersMusicVolume >= 0) { // the player's own music volume back
        char text[32];
        std::snprintf(text, sizeof text, "%.3f", g_PlayersMusicVolume);
        debug::WriteGameVariable("volumemusic", text);
    }
    LOG_INFO("Mission: %s%s%s", g_Hud.ending.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

int L_complete(lua_State* L)
{
    End(true, luaL_optstring(L, 1, ""));
    return 0;
}

int L_fail(lua_State* L)
{
    End(false, luaL_optstring(L, 1, ""));
    return 0;
}

// The script's own coroutines: main, and any thread(fn) it starts.
int L_thread(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* co = lua_newthread(L);
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    g_Threads.push_back(luaL_ref(L, LUA_REGISTRYINDEX)); // (pops the thread)
    return 0;
}

// The prelude: waiting is a loop of yields; a cutscene holds everyone, shows the bars, can be skipped.
constexpr const char* kPrelude = R"LUA(
function wait(seconds)
  local until_ = time() + (seconds or 0)
  while time() < until_ and not _skip_pressed() and not _advance() do coroutine.yield() end
end
function wait_until(condition, timeout)
  local until_ = timeout and (time() + timeout) or nil
  while not condition() do
    if _advance() then return true end
    if until_ and time() >= until_ then return false end
    coroutine.yield()
  end
  return true
end
function walk(c, where, timeout)
  if not go(c, where) then return false end
  wait(0.1)
  return wait_until(function() return not going(c, where) end, timeout or 20)
end
function say(id, who)
  local starts = _line_starts()
  local seconds, queued = line(id, who)
  local from = time()
  while seconds < 0 and time() - from < 4 do -- another line is being said: when it ends
    coroutine.yield()
    starts = _line_starts()
    seconds, queued = line(id, who)
  end
  if seconds < 0 then
    seconds = line(id, who, true)
  elseif queued then -- the engine starts it when the line before it ends: its time counts from then
    from = time()
    while _line_starts() == starts and time() - from < 6 and not _skip_pressed() and not _advance() do
      coroutine.yield()
    end
  end
  wait(seconds)
end
function speak(speaker, text, seconds) wait(subtitle(speaker, text, seconds)) end
-- the end stops the script where it is: nothing after it runs, so a cutscene's last shot stays up under the end's words
do
  local end_complete, end_fail = complete, fail
  local function stop() if coroutine.isyieldable() then while true do coroutine.yield() end end end
  function complete(text) end_complete(text) stop() end
  function fail(text) end_fail(text) stop() end
end
function cutscene(fn)
  _cutscene_state(true)
  hold(true)
  hud(false)
  letterbox(true)
  camera_fov(35) -- a film's lens (the levels' own is wide)
  local ok, err = pcall(fn)
  letterbox(false)
  camera_game()
  hud(true)
  hold(false)
  _cutscene_state(false)
  if not ok then error(err, 0) end
end
)LUA";

void Register(lua_State* L)
{
    static const luaL_Reg kFunctions[] = {
        { "log", L_log }, { "time", L_time }, { "paused", L_paused },
        { "player", L_player }, { "cast", L_cast }, { "spawn", L_spawn }, { "remove", L_remove },
        { "alive", L_alive }, { "exists", L_exists }, { "health", L_health }, { "set_health", L_set_health },
        { "max_health", L_max_health }, { "invincible", L_invincible }, { "position", L_position },
        { "distance", L_distance }, { "place", L_place }, { "face", L_face }, { "side", L_side }, { "ai", L_ai },
        { "attack", L_attack }, { "force_powers", L_force_powers }, { "hold", L_hold }, { "play", L_play }, { "play_anim", L_play_anim }, { "stop_anim", L_stop_anim }, { "actor", L_actor }, { "keep_body", L_keep_body }, { "pushable", L_pushable }, { "anim_frames", L_anim_frames }, { "moves", L_moves }, { "saber", L_saber }, { "saber_color", L_saber_color }, { "hide", L_hide }, { "slow_motion", L_slow_motion }, { "quiet", L_quiet }, { "kill", L_kill }, { "boss", L_boss }, { "objective_done", L_objective_done }, { "tip", L_tip },
        { "go", L_go }, { "going", L_going }, { "marker", L_marker }, { "field", L_field },
        { "sound", L_sound }, { "stop_sounds", L_stop_sounds }, { "get_var", L_get_var },
        { "music_volume", L_music_volume },
        { "class_of", L_class_of }, { "set_player", L_set_player },
        { "camera_fixed", L_camera_fixed }, { "camera_orbit", L_camera_orbit }, { "camera_move", L_camera_move },
        { "camera_track", L_camera_track }, { "camera_game", L_camera_game }, { "camera_fov", L_camera_fov },
        { "letterbox", L_letterbox }, { "fade_out", L_fade_out }, { "fade_in", L_fade_in },
        { "objective", L_objective }, { "title", L_title }, { "notice", L_notice }, { "subtitle", L_subtitle },
        { "line", L_line }, { "timer", L_timer }, { "timer_value", L_timer_value }, { "timer_stop", L_timer_stop },
        { "counter", L_counter }, { "hud", L_hud }, { "set_var", L_set_var }, { "command", L_command },
        { "skipping", L_skipping }, { "complete", L_complete }, { "fail", L_fail }, { "thread", L_thread },
        { "_cutscene_state", L_cutscene_state }, { "stop_lines", L_stop_lines }, { "_line_starts", L_forced_starts }, { "_skip_pressed", L_skip_pressed }, { "_advance", L_advance },
    };
    for (const luaL_Reg& f : kFunctions) {
        lua_pushcfunction(L, f.func);
        lua_setglobal(L, f.name);
    }
}

void ScriptError(const std::string& error)
{
    LOG_ERROR("Mission: script error: %s", error.c_str());
    g_Hud.error = error;
}

void CloseScript()
{
    if (g_Lua)
        lua_close(g_Lua);
    g_Lua = nullptr;
    g_Threads.clear();
    g_MainThread = nullptr;
}

// Loads the script and starts `main` as the first coroutine.
void StartScript()
{
    CloseScript();
    g_Lua = luaL_newstate();
    lua_State* L = g_Lua;
    // the safe libraries only: no files, processes or loading of native code
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);
    for (const char* unsafe : { "dofile", "loadfile", "load" }) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    Register(L);
    if (luaL_dostring(L, kPrelude) != LUA_OK) {
        ScriptError(lua_tostring(L, -1));
        return;
    }
    if (luaL_loadbuffer(L, g_Script.data(), g_Script.size(), "mission.lua") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ScriptError(lua_tostring(L, -1));
        return;
    }
    lua_getglobal(L, "main");
    if (!lua_isfunction(L, -1)) {
        ScriptError("mission.lua has no function main()");
        lua_pop(L, 1);
        return;
    }
    lua_State* co = lua_newthread(L);
    lua_insert(L, -2);
    lua_xmove(L, co, 1);
    g_Threads.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    g_MainThread = co;
    LOG_INFO("Missions: the script runs");
}

void RunThreads()
{
    if (!g_Lua)
        return;
    for (size_t i = 0; i < g_Threads.size();) {
        lua_rawgeti(g_Lua, LUA_REGISTRYINDEX, g_Threads[i]);
        lua_State* co = lua_tothread(g_Lua, -1);
        lua_pop(g_Lua, 1);
        int results = 0;
        const int status = co ? lua_resume(co, g_Lua, 0, &results) : LUA_ERRRUN;
        if (co && results > 0)
            lua_pop(co, results);
        if (status == LUA_YIELD) {
            ++i;
            continue;
        }
        if (status != LUA_OK)
            ScriptError(co ? luaL_tolstring(co, -1, nullptr) : "a thread is gone");
        luaL_unref(g_Lua, LUA_REGISTRYINDEX, g_Threads[i]);
        g_Threads.erase(g_Threads.begin() + std::ptrdiff_t(i));
    }
}

// The player's fatal hit: the script's on_player_defeated(), else the mission fails. The game never sees the player
// die (that would end the level with its own Game Over).
bool PlayerFatal(uint8_t* character)
{
    // The player's defeat is the game's own (its death, then its Game Over screen: Restart or Quit), unless the
    // script takes it (on_player_defeated: the player stays up with a little health and the script goes on).
    if (!g_Active || !g_Woken || character != PlayerObject())
        return false;
    if (g_EndAt > 0) {
        At<float>(character, kCharacterHealth) = 1;
        return true;
    }
    if (g_PlayerDefeatedHandled)
        return false;
    if (g_Lua) {
        lua_getglobal(g_Lua, "on_player_defeated");
        if (lua_isfunction(g_Lua, -1)) {
            g_PlayerDefeatedHandled = true;
            At<float>(character, kCharacterHealth) = 1;
            lua_State* co = lua_newthread(g_Lua);
            lua_insert(g_Lua, -2);
            lua_xmove(g_Lua, co, 1);
            g_Threads.push_back(luaL_ref(g_Lua, LUA_REGISTRYINDEX));
            return true;
        }
        lua_pop(g_Lua, 1);
    }
    LOG_INFO("Mission: the player fell (the game's own Game Over)");
    return false;
}

double Towards(double from, double to, double start, double length)
{
    const double t = std::clamp((Now() - start) / std::max(0.001, length), 0.0, 1.0);
    return from + (to - from) * t;
}

void UpdateHud()
{
    const double now = Now();
    g_Hud.active = g_Active && g_Woken;
    g_Hud.paused = GamePaused();
    // the bars are the game's own (GameLetterbox): the port's screen only notes them (nothing of its own shows under them)
    {
        uint8_t* box = *reinterpret_cast<uint8_t**>(uintptr_t(kLetterBox));
        g_Hud.letterbox = box && (At<uint32_t>(box, 0xC) & 0x40000) && At<int>(box, 0x1E8) != 0 ? 1.0f : 0.0f;
    }
    g_Hud.fade = float(Towards(g_FadeFrom, g_FadeTo, g_FadeStart, g_FadeLength));
    g_Hud.objectiveFlash = float(std::clamp(1.0 - (now - g_ObjectiveAt) / 3.0, 0.0, 1.0));
    const auto alpha = [&](double at, double until) {
        return float(std::clamp(std::min((now - at) / 0.4, (until - now) / 0.6), 0.0, 1.0));
    };
    g_Hud.titleAlpha = alpha(g_TitleAt, g_TitleUntil);
    g_Hud.noticeAlpha = alpha(g_NoticeAt, g_NoticeUntil);
    if (now > g_LineUntil)
        g_Hud.speaker.clear(), g_Hud.line.clear();
    g_Hud.timerSeconds = float(TimerValue());
    g_Hud.endingAlpha = g_EndAt > 0 ? float(std::clamp((now - g_EndAt) / 0.6, 0.0, 1.0)) : 0;
    g_Hud.endingFade = g_EndAt > 0 ? float(std::clamp((now - g_EndAt - 3.4) / 1.4, 0.0, 1.0)) : 0;
}

} // namespace

const std::vector<MissionInfo>& Missions() { return g_Missions; }

const MissionInfo* FindMission(const std::string& id)
{
    for (const MissionInfo& m : g_Missions)
        if (_stricmp(m.id.c_str(), id.c_str()) == 0)
            return &m;
    return nullptr;
}

bool MissionActive() { return g_Active; }

void AdvanceMission()
{
    if (!g_Active)
        return;
    if (g_InCutscene) {
        g_Skip = true;
        StopDialogueLines(); // (a skipped scene's lines end with it)
    } else
        g_Advance = true;
    LOG_INFO("Mission: next (%s)", g_InCutscene ? "the cutscene skipped" : "the current wait ends");
}

MissionHud GetMissionHud() { return g_Hud; }

bool StartMission(const std::string& id, std::string& error)
{
    const MissionInfo* m = FindMission(id);
    if (!m) {
        error = "no mission " + id + " in mods\\missions";
        return false;
    }
    if (!m->map.empty() && !FindCustomMap(m->map)) {
        error = "its map " + m->map + " is not installed (mods\\maps)";
        return false;
    }
    StartDevMap("mission:" + m->id + (m->map.empty() ? "" : " custommap:" + m->map) + ProfileSessionArguments());
    return true;
}

namespace {

// The class scripts light a saber that is out of their hands' way again at once (their state updates call the script
// native SaberOn, 0x2BC540, e.g. Cin Drallig's at 0x352F7C): one the mission put away stays put away until it lights it.
void __fastcall ScriptSaberOn(uint8_t* saber, void*, float seconds)
{
    if (std::find(g_SabersAway.begin(), g_SabersAway.end(), saber) == g_SabersAway.end())
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float)>(kSaberExtend)(saber, nullptr, seconds);
}

} // namespace

// The game's own restart of the mission (the pause screen's Restart Mission, the Game Over screen's Restart) launches
// the level it runs (Grievous Survival's, which the dev map is built from) with its own arguments: the mission's
// are added again, so the restart is the mission's, not the bonus mission's.
void MissionRestartLaunch(std::string& arguments)
{
    if (!g_Active || !g_Info)
        return;
    std::string lower = " " + arguments + " ";
    for (char& c : lower)
        c = char(tolower(static_cast<unsigned char>(c)));
    if (lower.find(" map:u107_uta_grievoussurvival ") == std::string::npos || lower.find(" mission:") != std::string::npos) {
        LOG_INFO("Missions: the game launches '%s' (left as it is)", arguments.substr(0, 160).c_str());
        return;
    }
    arguments += " devmap:1 mission:" + g_Info->id + (g_Info->map.empty() ? "" : " custommap:" + g_Info->map);
    LOG_INFO("Missions: the restart is %s's", g_Info->id.c_str());
}

void InstallMissions(const std::wstring& mods)
{
    CloseScript();
    g_SabersAway.clear();
    g_PlayersMusicVolume = -1;
    g_PlayersSubtitles.clear();
    if (!g_MusicServed.empty()) // the last start's own track: the disc's again
        kernel::SetDiscFileOverride(g_MusicServed, L"");
    g_MusicServed.clear();
    g_Mods = mods;
    g_Active = false;
    g_Info = nullptr;
    g_Cast.clear();
    g_Lines.clear();
    g_Handles.assign(1, Handle{});
    g_Woken = false;
    g_InLevelAt = 0;
    g_Hud = MissionHud{};
    g_Shot = Shot{};
    SetCameraFov(0);
    g_Held = g_Skip = g_InCutscene = g_Advance = false;
    g_SkipAskedAt = -10;
    RestoreSaberColor();
    g_MissionSlow = false;
    SetDialogueQuiet(false);
    g_LentName = {};
    g_LevelCharFade = -1;
    g_FadeFrom = g_FadeTo = g_FadeStart = g_FadeLength = 0;
    g_BarsFrom = g_BarsTo = g_BarsStart = 0;
    g_ObjectiveAt = -10;
    g_TitleUntil = g_TitleAt = g_NoticeUntil = g_NoticeAt = g_LineUntil = 0;
    g_TimerEnd = g_TimerStart = 0;
    g_TimerRunning = false;
    g_EndAt = 0;
    g_EndDone = false;
    g_PlayerDefeatedHandled = false;
    g_AnimationCount = 0;
    g_FieldTinted = false;
    g_AnimControls.clear();
    g_Grapples.clear();
    g_Actors.clear();
    g_Hidden.clear();
    ForgetCharacterSaberColors();
    g_Walkers.clear();
    SetBorrowedAnimations({});
    AddClassSequences(&MissionSequences);
    ReadMissions();
    std::string id;
    if (!DevMapActive() || !BootArgument("mission", id))
        return;
    g_Info = FindMission(id);
    if (!g_Info) {
        LOG_WARN("Missions: this start's mission %s is not installed", id.c_str());
        return;
    }
    PatchCall(kScriptSaberOnCall, reinterpret_cast<const void*>(&ScriptSaberOn), 5);
    if (const uint8_t* call = reinterpret_cast<const uint8_t*>(uintptr_t(kLetterboxCancelSlowCall));
        call[0] == 0xE8 && kLetterboxCancelSlowCall + 5 + *reinterpret_cast<const int32_t*>(call + 1) == kCancelSlowMotion)
        PatchCall(kLetterboxCancelSlowCall, reinterpret_cast<const void*>(&LetterboxCancelSlow), 5);
    else
        LOG_WARN("Missions: unexpected code at the letterbox's slow motion cancel; no slow motion in cutscenes");
    HookReactions(); // actor(): no hits
    if (*reinterpret_cast<const uint32_t*>(uintptr_t(kTickScriptSlot)) == kTickScript) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&TickScriptHook));
        PatchBytes(kTickScriptSlot, &hook, 4);
    } else {
        LOG_WARN("Missions: unexpected animation tick; play_anim plays whole animations only");
    }
    kernel::SetLaunchFilter(&MissionRestartLaunch);
    g_Folder = mods + L"\\" + std::wstring(g_Info->folder.begin(), g_Info->folder.end());
    const std::wstring ini = g_Folder + L"\\mission.ini";
    // the script
    g_Script.clear();
    if (FILE* f = _wfopen((g_Folder + L"\\mission.lua").c_str(), L"rb")) {
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
            g_Script.append(buffer, n);
        fclose(f);
    }
    // the player and the cast
    g_PlayerClass = Profile(ini, L"Mission", L"Player", L"IAnakin");
    const std::vector<float> at = Numbers(Profile(ini, L"Mission", L"PlayerAt", L"0 0 0"));
    DevMapLayout layout;
    layout.start = true;
    {
        float wx, wz;
        ToWorld(at.size() > 0 ? at[0] : 0, at.size() > 1 ? at[1] : 0, wx, wz);
        layout.startX = wx;
        layout.startZ = wz;
        layout.startYaw = (at.size() > 2 ? at[2] : 0) * 3.14159265f / 180.0f + MapYaw();
    }
    uint32_t next = kCastFirstId;
    for (const std::string& name : Keys(ini, L"Cast")) {
        // name=Class, x z yaw, side[, costume]
        const std::wstring key(name.begin(), name.end());
        const std::string value = Profile(ini, L"Cast", key.c_str());
        std::vector<std::string> parts;
        size_t from = 0;
        while (from <= value.size()) {
            size_t comma = value.find(',', from);
            if (comma == std::string::npos)
                comma = value.size();
            std::string p = value.substr(from, comma - from);
            while (!p.empty() && p.front() == ' ')
                p.erase(p.begin());
            while (!p.empty() && p.back() == ' ')
                p.pop_back();
            parts.push_back(p);
            from = comma + 1;
        }
        CastMember m;
        m.name = name;
        m.className = parts.empty() ? "" : parts[0];
        if (m.className.empty()) {
            LOG_WARN("Missions: cast %s has no class", name.c_str());
            continue;
        }
        if (const CustomCharacter* custom = FindCustomCharacter(m.className.c_str()))
            m.className = custom->className; // (the game's own classes are checked as the level loads)
        const std::vector<float> p = Numbers(parts.size() > 1 ? parts[1] : "");
        m.x = p.size() > 0 ? p[0] : 0;
        m.z = p.size() > 1 ? p[1] : 0;
        m.yaw = (p.size() > 2 ? p[2] : 0) * 3.14159265f / 180.0f;
        m.side = SideFrom(parts.size() > 2 ? parts[2] : "");
        m.costume = parts.size() > 3 ? parts[3] : "";
        m.placedId = next++;
        float wx, wz;
        ToWorld(m.x, m.z, wx, wz);
        layout.characters.push_back({ m.className, m.placedId, wx, wz, m.yaw + MapYaw(), m.costume });
        g_Cast.push_back(m);
    }
    // animations a class plays that are another's: <cast name, player or class>=<animation>[, <animation>...]
    std::vector<std::string> borrowed;
    for (const std::string& key : Keys(ini, L"Animations")) {
        std::string className = key;
        if (_stricmp(key.c_str(), "player") == 0)
            className = g_PlayerClass;
        for (const CastMember& m : g_Cast)
            if (_stricmp(m.name.c_str(), key.c_str()) == 0)
                className = m.className;
        if (const CustomCharacter* custom = FindCustomCharacter(className.c_str()))
            className = custom->base; // its moves are its base's
        if (className.size() < 2) {
            LOG_WARN("Missions: [Animations] %s is no class", key.c_str());
            continue;
        }
        std::string list = Profile(ini, L"Animations", std::wstring(key.begin(), key.end()).c_str(), L"");
        for (char& ch : list)
            if (ch == ',')
                ch = ' ';
        size_t from = 0;
        while ((from = list.find_first_not_of(' ', from)) != std::string::npos) {
            const size_t end = std::min(list.find(' ', from), list.size());
            std::string animation = list.substr(from, end - from);
            from = end;
            if (g_AnimationCount >= kMaxMissionAnimations) {
                LOG_WARN("Missions: more than %d [Animations]; %s left out", kMaxMissionAnimations, animation.c_str());
                continue;
            }
            for (char& ch : animation)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            MissionAnimation& a = g_Animations[g_AnimationCount++];
            a.shortName = className.substr(1);
            a.animation = animation;
            a.sequence = "Msn_" + animation;
            borrowed.push_back(animation + ".bnm");
        }
    }
    SetBorrowedAnimations(std::move(borrowed));
    // its music: Music=<a slot of the game's sound table>, the game's own track, or <slot>.hwx in the mission's folder
    // (in the slot's format, as a custom map's) played in its place for this start
    layout.music = Profile(ini, L"Mission", L"Music");
    for (char& ch : layout.music)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (!layout.music.empty()) {
        const std::wstring track = g_Folder + L"\\" + std::wstring(layout.music.begin(), layout.music.end()) + L".hwx";
        if (GetFileAttributesW(track.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_MusicServed = "\\audio\\xbox\\" + layout.music + ".hwx";
            kernel::SetDiscFileOverride(g_MusicServed, track);
        }
    }
    // the places characters can be sent to: the level's goto markers, one each (reached closer than Battlefront's posts)
    g_Markers.clear();
    layout.markerReach = 80.0f;
    for (const std::string& name : Keys(ini, L"Markers")) {
        const std::vector<float> p = Numbers(Profile(ini, L"Markers", std::wstring(name.begin(), name.end()).c_str(), L""));
        if (p.size() < 2) {
            LOG_WARN("Missions: marker %s has no x z", name.c_str());
            continue;
        }
        float wx, wz;
        ToWorld(p[0], p[1], wx, wz);
        g_Markers.push_back({ name, p[0], p[1] });
        layout.markers.push_back({ wx, wz });
    }
    // objectives: name=text, each the game's own objective trigger with a text id of the port's own
    g_Objectives.clear();
    for (const std::string& name : Keys(ini, L"Objectives")) {
        const std::string text = Profile(ini, L"Objectives", std::wstring(name.begin(), name.end()).c_str(), L"");
        // the objective manager takes a level's objectives from the string table at the level's start:
        // IDS_OBJECTIVES_<level>_00.._99 (0x13FF40; the dev map's level is Grievous Survival's), from 51 on here
        char textId[64];
        std::snprintf(textId, sizeof textId, "IDS_OBJECTIVES_U107_UTA_GrievousSurvival_%02zu", 51 + g_Objectives.size());
        SetMenuText(textId, std::wstring(text.begin(), text.end()));
        DevMapObject o;
        o.className = "ITriggerObjective";
        o.id = kObjectiveFirstId + uint32_t(g_Objectives.size());
        o.x = layout.startX;
        o.z = layout.startZ;
        o.y = 100;
        o.yaw = 0;
        std::vector<uint8_t> idString;
        const uint32_t n = uint32_t(strlen(textId));
        idString.insert(idString.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
        idString.insert(idString.end(), textId, textId + n);
        o.properties.push_back({ "ID string", idString });
        layout.objects.push_back(o);
        g_Objectives.push_back(name);
    }
    // tips: name=text [| seconds], each the game's own tip (IVaderTip: its text id, "Time to show tip")
    g_Tips.clear();
    for (const std::string& name : Keys(ini, L"Tips")) {
        std::string text = Profile(ini, L"Tips", std::wstring(name.begin(), name.end()).c_str(), L"");
        float seconds = 6.0f;
        if (const size_t bar = text.find('|'); bar != std::string::npos) {
            seconds = std::strtof(text.c_str() + bar + 1, nullptr);
            text.erase(bar);
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
        }
        char textId[48];
        std::snprintf(textId, sizeof textId, "IDS_TIPS_PORT_%zu", g_Tips.size() + 1);
        SetMenuText(textId, std::wstring(text.begin(), text.end()));
        DevMapObject o;
        o.className = "IVaderTip";
        o.id = kTipFirstId + uint32_t(g_Tips.size());
        o.x = layout.startX;
        o.z = layout.startZ;
        o.y = 100;
        o.yaw = 0;
        std::vector<uint8_t> idString, time(4);
        const uint32_t n = uint32_t(strlen(textId));
        idString.insert(idString.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
        idString.insert(idString.end(), textId, textId + n);
        std::memcpy(time.data(), &seconds, 4);
        o.properties.push_back({ "Text (string table ID)", idString });
        o.properties.push_back({ "Time to show tip", time });
        layout.objects.push_back(o);
        g_Tips.push_back(name);
    }
    // force fields: name=x z heading [scale] [off] (heading 0: the field across the z axis, closing the way along z;
    // 50 wide, 100 high at scale 1). Drawn as a static with its light record, its collision from its own bound.
    g_Fields.clear();
    {
        const std::vector<float> tint = Numbers(Profile(ini, L"Mission", L"FieldColour"));
        g_FieldTinted = tint.size() >= 3;
        for (int k = 0; k < 3 && g_FieldTinted; ++k)
            g_FieldTint[k] = std::clamp(tint[size_t(k)], 0.0f, 255.0f);
        for (const char* folder : { "s_m02b_forcefield_smdoor", "s_forcefield_lghallway" })
            for (const char* texture : { "shield_door_static1", "sh_shield01_white" })
                RegisterResourcePatch(std::string("meshes\\props\\interactiveitems\\forcefields\\separatist\\") + folder +
                    "\\" + texture + ".stx", &TintFieldTexture);
    }
    for (const std::string& name : Keys(ini, L"Fields")) {
        const std::string text = Profile(ini, L"Fields", std::wstring(name.begin(), name.end()).c_str(), L"");
        const std::vector<float> p = Numbers(text);
        if (p.size() < 2) {
            LOG_WARN("Missions: field %s has no x z", name.c_str());
            continue;
        }
        DevMapObject o;
        o.className = "IForceField";
        ToWorld(p[0], p[1], o.x, o.z);
        o.y = 0;
        auto bytes = [](const void* v, size_t n) {
            return std::vector<uint8_t>(static_cast<const uint8_t*>(v), static_cast<const uint8_t*>(v) + n);
        };
        std::vector<uint8_t> mesh;
        for (const char* part : { "j_forcefield_smdoor", "props\\interactiveitems\\forcefields\\jeditemple\\j_forcefield_smdoor\\" }) {
            const uint32_t n = uint32_t(strlen(part));
            mesh.insert(mesh.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
            mesh.insert(mesh.end(), part, part + n);
        }
        if (p.size() > 3) {
            const float scale = p[3];
            o.properties.push_back({ "Uniform scale", bytes(&scale, 4) });
        }
        const uint32_t one = 1, isOn = text.find("off") == std::string::npos ? 1 : 0;
        o.properties.push_back({ "MeshName", mesh });
        o.properties.push_back({ "Auto generate bound?", bytes(&one, 4) }); // its collision (none without)
        o.properties.push_back({ "Is On?", bytes(&isOn, 4) });
        o.lightMesh = "meshes\\props\\interactiveitems\\forcefields\\jeditemple\\j_forcefield_smdoor\\j_forcefield_smdoor";
        o.lightParts = { 19, 49, 58, 17 }; // the model's vertices per part (143)
        // the model is one-sided (seen from its front only, heading + 180 degrees): a field is two, back to back
        const float yaw = (p.size() > 2 ? p[2] : 0) * 3.14159265f / 180.0f + MapYaw();
        for (int side = 0; side < 2; ++side) {
            o.id = kFieldFirstId + uint32_t(2 * g_Fields.size() + side);
            o.yaw = yaw + float(side) * 3.14159265f;
            layout.objects.push_back(o);
        }
        g_Fields.push_back(name);
    }
    SetDevMapLayout(layout);
    SetPlayerClass(nullptr);
    SetPlayerClass(g_PlayerClass.c_str(), false);
    ReadLines();
    RegisterDialogue();
    SetFatalHitGuard(&PlayerFatal);
    SetCameraDirector(&Director);
    g_Active = true;
    SetDialogueQuiet(true); // (until the mission is under way: see MissionFrame)
    LOG_INFO("Missions: %s (%s): the player is %s, %zu in the cast", g_Info->id.c_str(), g_Info->name.c_str(),
        g_PlayerClass.c_str(), g_Cast.size());
}

// The level's "Allow char fade?" (ILevelProperties [0x68F720] +0xC8): the game fades characters that look near the
// camera (by their size on screen: a long lens makes far ones "near"), which empties a cutscene's close shots: off
// while a shot of the mission's is on, the level's own again for the game's camera.
constexpr uint32_t kLevelProperties = 0x0068F720;
constexpr uint32_t kAllowCharFade = 0xC8;

void KeepCharactersInShot(bool shot)
{
    uint8_t* properties = *reinterpret_cast<uint8_t**>(uintptr_t(kLevelProperties));
    if (!properties)
        return;
    if (shot && g_LevelCharFade < 0) {
        g_LevelCharFade = At<uint8_t>(properties, kAllowCharFade);
        At<uint8_t>(properties, kAllowCharFade) = 0;
    } else if (!shot && g_LevelCharFade >= 0) {
        At<uint8_t>(properties, kAllowCharFade) = uint8_t(g_LevelCharFade);
        g_LevelCharFade = -1;
    }
}

void MissionFrame()
{
    if (!g_Active)
        return;
    KeepCharactersInShot(g_Shot.kind != ShotKind::None);
    if (g_LentName.character && Now() > g_LentName.until) // a lent speaker name back
        RestoreLentName();
    UpdateHud();
    // The characters say no line of their own (a companion's first-sight "Surrender while you can!") until the mission
    // is under way, while the screen is black, in a cutscene or once it has ended: only the script's own lines. (A line
    // the level started as it loaded was otherwise cut off a moment later by the first cutscene.)
    SetDialogueQuiet(!g_Woken || g_InCutscene || g_Hud.fade > 0.5f || g_EndAt > 0);
    uint8_t* player = PlayerObject();
    if (!player)
        return;
    WalkersFrame();
    const uint64_t now = GameTime();
    if (!g_InLevelAt)
        g_InLevelAt = now;
    if (!g_Woken) {
        if (now - g_InLevelAt < 500)
            return;
        g_Woken = true;
        // the lines' subtitles: the game's own, on for the mission (the player's setting put back at its end)
        {
            std::string value;
            if (debug::ReadGameVariable("subtitles", value)) {
                g_PlayersSubtitles = value;
                debug::WriteGameVariable("subtitles", "true");
            }
        }
        // the tips join the level (a placed object is updated and drawn once active, vfunc +0xB4)
        for (size_t k = 0; k < g_Tips.size(); ++k) {
            uint8_t* t = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(kTipFirstId + uint32_t(k));
            if (t && At<uint32_t>(t, 0) == kVaderTipVtable)
                Virtual<void(__fastcall*)(uint8_t*, void*)>(t, kActivate)(t, nullptr);
        }
        // the cast joins the level, on its sides
        for (const CastMember& m : g_Cast) {
            uint8_t* placed = nullptr;
            for (uint8_t* c : LevelCharacters())
                if (At<uint32_t>(c, 4) == m.placedId)
                    placed = c;
            if (!placed) {
                LOG_WARN("Missions: cast %s (%s) is not in the level", m.name.c_str(), m.className.c_str());
                continue;
            }
            if (At<float>(placed, kCharacterHealth) <= 0) {
                Virtual<void(__fastcall*)(uint8_t*, void*)>(placed, kActivate)(placed, nullptr);
                SettleActivated(placed);
            }
            if (m.side != SpawnSide::Default)
                SetCharacterSide(placed, m.side);
        }
        InvalidateLevelCharacters();
        StartScript();
        UpdateHud();
        return;
    }
    if (g_EndAt > 0) {
        // the end shows a few seconds; then back to the menus (complete) or the mission again (failed)
        if (!g_EndDone && Now() - g_EndAt > 5.0) {
            g_EndDone = true;
            Hold(false);
            if (g_EndOk) {
                std::string value;
                const std::string session = ProfileSessionArguments();
                if (!session.empty() && BootArgument("v_sessionCreated", value) && value == "1")
                    kernel::LaunchGame("map: v_shellBetweenLevels:0 v_versusMode:0 v_bonusMissionMode:0 "
                        "v_challengeMissionMode:0 v_checkpointIndex:-1" + session);
                else
                    kernel::RestartMission();
            } else {
                kernel::RestartMission();
            }
        }
        return;
    }
    if (GamePaused())
        return;
    RunThreads();
}

} // namespace swrots::game
