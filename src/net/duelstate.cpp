// The duel recorder (see duelstate.h).
//
// File: "SWDR", u32 version (1), u32 object bytes; then per frame: u32 frame, u32 p1 address, u32 p2 address,
// GamePad p1, GamePad p2 (the PC's live pads, ports 0 and 1), then each fighter's first `object bytes` (zero when
// it is not there or could not be read).

#include "net/duelstate.h"

#include <windows.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "core/log.h"
#include "d3d/recorder.h"
#include "game/characters.h"
#include "game/coop.h"
#include "input/controls.h"

namespace swrots::net {

namespace {

constexpr uint32_t kObjectBytes = 0x1800;
FILE* g_Out = nullptr;
FILE* g_SeqOut = nullptr;       // each fighter's behaviour (CharState): its sequence and the state's first bytes
uint32_t g_ShotFrom = 0, g_ShotTo = 0, g_ShotEvery = 0; // the recorder's screenshots (rec-<frame>.png)
FILE* g_DataOut = nullptr;      // the game's .data, changed dwords every kDataEvery frames (the round's state)
std::vector<uint32_t> g_DataPrev;
constexpr uint32_t kDataBegin = 0x00612BE0, kDataEnd = 0x00962BF8, kDataEvery = 10;
uint32_t g_Left = 0, g_Frame = 0;
std::string g_Name;

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool ReadObject(const uint8_t* object, uint8_t* out)
{
    std::memset(out, 0, kObjectBytes);
    if (!object)
        return false;
    __try {
        std::memcpy(out, object, kObjectBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, kObjectBytes);
        return false;
    }
}

void Close(const char* why)
{
    if (!g_Out)
        return;
    std::fclose(g_Out);
    g_Out = nullptr;
    if (g_DataOut)
        std::fclose(g_DataOut);
    g_DataOut = nullptr;
    if (g_SeqOut)
        std::fclose(g_SeqOut);
    g_SeqOut = nullptr;
    g_DataPrev.clear();
    LOG_INFO("Duel recorder: '%s' closed after %u frames (%s)", g_Name.c_str(), g_Frame, why);
}

void Fighters(uint8_t*& p1, uint8_t*& p2)
{
    p1 = game::GamePlayerSlot(0);
    p2 = game::GamePlayerSlot(1);
    if (!p1)
        p1 = game::PlayerObject();
    if (!p2)
        p2 = game::GamePlayer2();
    if (!p2 && p1) { // the developer map's duel: its opponent is not the game's player 2; the other character
        static uint32_t scan = 0;
        static uint8_t* other = nullptr;
        if (++scan % 30 == 1 || !other) {
            other = nullptr;
            for (uint8_t* c : game::LevelCharacters())
                if (c != p1) {
                    other = c;
                    break;
                }
        }
        p2 = other;
    }
}


// A fighter's behaviour (GBehavior CharState, [character+0x434], vtable 0x58EFB8): +0x30 the running sequence (a
// record of its script class's list, [[state+4]+0xC], [state+4]+8 records, the name at +0xC), +0x38 the previous one,
// +0x78 counts the sequences started (0x49D2A0).
constexpr uint32_t kCharState = 0x434, kStateBytes = 0x140, kSeqNameBytes = 32;
constexpr uint32_t kCharStateVtable = 0x0058EFB8;

struct SeqInfo {
    int32_t index = -1;            // the running sequence's index in its class's list
    char name[kSeqNameBytes] = {};
    uint8_t state[kStateBytes] = {};
};

bool ReadSeqRaw(const uint8_t* character, SeqInfo& out)
{
    __try {
        const uint8_t* state = *reinterpret_cast<uint8_t* const*>(character + kCharState);
        if (!state || *reinterpret_cast<const uint32_t*>(state) != kCharStateVtable)
            return false;
        std::memcpy(out.state, state, kStateBytes);
        const uint8_t* running = *reinterpret_cast<uint8_t* const*>(state + 0x30);
        const uint8_t* info = *reinterpret_cast<uint8_t* const*>(state + 4);
        const int count = *reinterpret_cast<const int*>(info + 8);
        uint8_t* const* list = *reinterpret_cast<uint8_t* const* const*>(info + 0xC);
        for (int i = 0; i < count; ++i)
            if (list[i] == running) {
                out.index = i;
                break;
            }
        if (running) {
            const char* name = *reinterpret_cast<const char* const*>(running + 0xC);
            for (uint32_t i = 0; name && i + 1 < kSeqNameBytes && name[i]; ++i)
                out.name[i] = name[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ReadSeq(const uint8_t* character, SeqInfo& out)
{
    out = SeqInfo();
    if (character && !ReadSeqRaw(character, out))
        out = SeqInfo();
}

// Starts the sequence of this index of the character's class (as the duel's intro starts one: 0x150E30, thiscall
// (record, start time, 0, 0, speed)); false when it refuses.
bool StartSeqRaw(uint8_t* character, int32_t index)
{
    __try {
        const uint8_t* state = *reinterpret_cast<uint8_t* const*>(character + kCharState);
        const uint8_t* info = *reinterpret_cast<uint8_t* const*>(state + 4);
        if (index < 0 || index >= *reinterpret_cast<const int*>(info + 8))
            return false;
        uint8_t* record = (*reinterpret_cast<uint8_t* const* const*>(info + 0xC))[index];
        return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(0x00150E30)(
            character, nullptr, record, 0.0f, 0, 0, 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void StartDuelRecorder(const std::string& name, uint32_t frames)
{
    Close("a new recording");
    if (!frames)
        return;
    CreateDirectoryW((ExeDir() + L"\\netplay").c_str(), nullptr);
    const std::wstring path = ExeDir() + L"\\netplay\\duel-" + std::wstring(name.begin(), name.end()) + L".bin";
    g_Out = _wfopen(path.c_str(), L"wb");
    if (!g_Out) {
        LOG_WARN("Duel recorder: cannot write %ls", path.c_str());
        return;
    }
    const uint32_t header[3] = { 0x52445753, 1, kObjectBytes }; // "SWDR"
    std::fwrite(header, sizeof(header), 1, g_Out);
    // "SWDD": per record u32 frame, u32 count, then count (u32 offset from 0x612BE0, u32 value); the first has all
    g_DataOut = _wfopen((ExeDir() + L"\\netplay\\duel-" + std::wstring(name.begin(), name.end()) + L".data.bin").c_str(),
        L"wb");
    if (g_DataOut) {
        const uint32_t dataHeader[2] = { 0x44445753, 1 };
        std::fwrite(dataHeader, sizeof(dataHeader), 1, g_DataOut);
    }
    // "SWDS": per frame, for each fighter, i32 sequence index, char name[32], the behaviour's first 0x140 bytes
    g_SeqOut = _wfopen((ExeDir() + L"\\netplay\\duel-" + std::wstring(name.begin(), name.end()) + L".seq.bin").c_str(),
        L"wb");
    if (g_SeqOut) {
        const uint32_t seqHeader[3] = { 0x53445753, 1, sizeof(SeqInfo) };
        std::fwrite(seqHeader, sizeof(seqHeader), 1, g_SeqOut);
    }
    g_Name = name;
    g_Left = frames;
    g_Frame = 0;
    LOG_INFO("Duel recorder: recording %u frames as %ls", frames, path.c_str());
}

void SetDuelRecorderShots(uint32_t from, uint32_t to, uint32_t every)
{
    g_ShotFrom = from;
    g_ShotTo = to;
    g_ShotEvery = every;
}

std::string DuelRecorderStatus()
{
    char line[160];
    std::snprintf(line, sizeof(line), g_Out ? "duel recorder: '%s', %u frames written, %u to go" : "duel recorder: off",
        g_Name.c_str(), g_Frame, g_Left);
    return line;
}

namespace {

// The gameplay fields (offset, bytes), from the recorder's per-field changes (docs/research/netplay.md).
struct Range { uint16_t offset, bytes; };
constexpr Range kFields[] = {
    { 0x12C, 0x0C }, // dead, health, max health
    { 0x150, 0x0C }, // the transform's rows (their 4th dwords are not floats: +0x15C, +0x16C, +0x17C point into
    { 0x160, 0x0C }, // the game's memory) and the position (+0x180)
    { 0x170, 0x0C },
    { 0x180, 0x0C },
    { 0x1B0, 0x0C }, // velocity
    { 0x210, 0x0C }, // (+0x21C points)
    { 0x220, 0x0C },
    { 0x280, 0x1C },
    { 0x310, 0x08 }, // the action's frame, the fighter's tick
    { 0x398, 0x04 }, // the action's timers (+0x39C points)
    { 0x3A0, 0x04 },
    { 0x3B0, 0x04 }, // health (copy)
    { 0x3E4, 0x08 }, // state (+0x3EC: a pooled string, the state's name)
    { 0x3F0, 0x10 }, // flags
    { 0x410, 0x0C }, // velocity (copy)
    { 0x5A8, 0x04 },
    { 0x5D0, 0x04 },
    { 0x630, 0x0C }, // position (copies)
    { 0x750, 0x0C },
    { 0x918, 0x04 },
    { 0x920, 0x04 },
    { 0xA40, 0x08 }, // Force, max Force
    { 0xA60, 0x04 },
};
constexpr Range kPosition[] = { { 0x150, 0x0C }, { 0x160, 0x0C }, { 0x170, 0x0C }, { 0x180, 0x0C }, { 0x630, 0x0C },
    { 0x750, 0x0C } };

struct Snap {
    bool valid = false;
    uint8_t* object[2] = {};
    std::vector<uint8_t> bytes[2];
};
Snap g_Snap;
FILE* g_Play = nullptr;
FILE* g_PlaySeq = nullptr;        // the recording's sequences: started on the live fighters as they started
uint32_t g_PlaySeqStarts[2] = {}, g_PlaySeqOk = 0, g_PlaySeqRefused = 0;
bool g_PlaySeqFirst = true;
uint32_t g_PlayLeft = 0, g_PlayFrame = 0, g_PlayShots = 0, g_PlayMask = 3, g_PlayPad = 18;
int g_Watch = 0;           // frames still to log after a load
int g_TestSave = -1, g_TestLoad = -1;
std::string g_TestMode;

uint32_t Fields(const Range* ranges, size_t count)
{
    uint32_t n = 0;
    for (size_t i = 0; i < count; ++i)
        n += ranges[i].bytes;
    return n;
}

bool Write(uint8_t* object, const uint8_t* from, const Range* ranges, size_t count)
{
    __try {
        if (!ranges) {
            std::memcpy(object, from, kObjectBytes);
        } else {
            for (size_t i = 0; i < count; ++i)
                std::memcpy(object + ranges[i].offset, from + ranges[i].offset, ranges[i].bytes);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T> T At(const uint8_t* o, uint32_t offset)
{
    T v;
    std::memcpy(&v, o + offset, sizeof(T));
    return v;
}

// The fighter's state's name (+0x3EC, a pooled string), or "".
void ReadStateName(const uint8_t* o, char* text)
{
    __try {
        const char* name = *reinterpret_cast<const char* const*>(o + 0x3EC);
        if (name)
            for (int i = 0; i < 39 && name[i]; ++i)
                text[i] = (name[i] >= 32 && name[i] < 127) ? name[i] : '?';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        text[0] = 0;
    }
}

std::string StateName(const uint8_t* o)
{
    char text[40] = {};
    ReadStateName(o, text);
    return text;
}

void LogFighter(const char* what, int i, const uint8_t* o)
{
    LOG_INFO("Duel snap: %s P%d pos %.1f %.1f %.1f vel %.1f %.1f %.1f health %.0f force %.0f action 0x%x/%u "
             "state 0x%x 0x%x 0x%x '%s'",
        what, i + 1, At<float>(o, 0x180), At<float>(o, 0x184), At<float>(o, 0x188), At<float>(o, 0x1B0),
        At<float>(o, 0x1B4), At<float>(o, 0x1B8), At<float>(o, 0x130), At<float>(o, 0xA40), At<uint32_t>(o, 0x310),
        At<uint32_t>(o, 0x314), At<uint32_t>(o, 0x3E4), At<uint32_t>(o, 0x3F8), At<uint32_t>(o, 0x920),
        StateName(o).c_str());
    SeqInfo seq;
    ReadSeq(o, seq);
    LOG_INFO("Duel snap: %s P%d sequence '%s' (%d), started %u", what, i + 1, seq.name, seq.index,
        *reinterpret_cast<const uint32_t*>(seq.state + 0x78));
}

std::string Save()
{
    uint8_t* p[2] = {};
    Fighters(p[0], p[1]);
    if (!p[0] || !p[1])
        return "duel snap: no duel now";
    for (int i = 0; i < 2; ++i) {
        g_Snap.bytes[i].resize(kObjectBytes);
        if (!ReadObject(p[i], g_Snap.bytes[i].data()))
            return "duel snap: a fighter could not be read";
        g_Snap.object[i] = p[i];
        LogFighter("saved", i, g_Snap.bytes[i].data());
    }
    g_Snap.valid = true;
    return "duel snap: saved";
}

std::string Load(const std::string& mode)
{
    if (!g_Snap.valid)
        return "duel snap: nothing saved";
    uint8_t* p[2] = {};
    Fighters(p[0], p[1]);
    if (!p[0] || !p[1])
        return "duel snap: no duel now";
    const bool same = p[0] == g_Snap.object[0] && p[1] == g_Snap.object[1];
    const Range* ranges = kFields;
    size_t count = std::size(kFields);
    if (mode == "full") {
        if (!same)
            return "duel snap: full needs the same fighters (a new round re-creates them)";
        ranges = nullptr;
        count = 0;
    } else if (mode == "pos") {
        ranges = kPosition;
        count = std::size(kPosition);
    }
    for (int i = 0; i < 2; ++i) {
        LogFighter("before", i, p[i]);
        if (!Write(p[i], g_Snap.bytes[i].data(), ranges, count))
            return "duel snap: a fighter could not be written";
    }
    g_Watch = 8;
    char line[160];
    std::snprintf(line, sizeof(line), "duel snap: loaded (%s, %u bytes per fighter, %s fighters)",
        mode.empty() ? "fields" : mode.c_str(), ranges ? Fields(ranges, count) : kObjectBytes,
        same ? "the same" : "other");
    LOG_INFO("%s", line);
    return line;
}

} // namespace

std::string DuelSnapCommand(const std::string& verb, const std::string& arg1, const std::string& arg2)
{
    if (verb == "save")
        return Save();
    if (verb == "load")
        return Load(arg1);
    if (verb == "test") {
        g_TestSave = std::max(1, std::atoi(arg1.c_str()));
        g_TestLoad = std::max(1, std::atoi(arg2.c_str()));
        g_TestMode = "";
        return "duel snap: test armed";
    }
    if (verb == "stop") { // stops a playback (duelplay)
        if (g_Play)
            std::fclose(g_Play);
        g_Play = nullptr;
        return "duel play: stopped";
    }
    if (verb == "mode") {
        g_TestMode = arg1;
        return "duel snap: test mode " + arg1;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "duel snap: %s; fields %u bytes per fighter",
        g_Snap.valid ? "saved" : "nothing saved", Fields(kFields, std::size(kFields)));
    return line;
}

namespace {
// Playback: a recording's fighters written into the live duel, frame by frame (the guest's view in phase 2: a fighter
// driven only by the state it is sent).

void PlayFrame()
{
    if (!g_Play)
        return;
    uint8_t* p[2] = {};
    Fighters(p[0], p[1]);
    if (!p[0] || !p[1])
        return; // waits for the duel
    static std::vector<uint8_t> rec;
    rec.resize(12 + 2 * g_PlayPad + 2 * kObjectBytes);
    if (std::fread(rec.data(), rec.size(), 1, g_Play) != 1 || g_PlayLeft == 0) {
        std::fclose(g_Play);
        g_Play = nullptr;
        if (g_PlaySeq)
            std::fclose(g_PlaySeq);
        g_PlaySeq = nullptr;
        LOG_INFO("Duel play: done at the recording's frame %u (sequences started %u, refused %u)", g_PlayFrame,
            g_PlaySeqOk, g_PlaySeqRefused);
        return;
    }
    const uint8_t* fighters = rec.data() + 12 + 2 * g_PlayPad;
    for (int i = 0; i < 2; ++i)
        if (g_PlayMask & (1u << i))
            Write(p[i], fighters + i * kObjectBytes, kFields, std::size(kFields));
    if (g_PlaySeq) {
        SeqInfo seq[2];
        if (std::fread(seq, sizeof(seq), 1, g_PlaySeq) == 1) {
            for (int i = 0; i < 2; ++i) {
                if (!(g_PlayMask & (1u << i)) || seq[i].index < 0)
                    continue;
                uint32_t starts;
                std::memcpy(&starts, seq[i].state + 0x78, 4);
                if (!g_PlaySeqFirst && starts == g_PlaySeqStarts[i])
                    continue;
                g_PlaySeqStarts[i] = starts;
                SeqInfo live;
                ReadSeq(p[i], live);
                if (g_PlaySeqFirst && live.index == seq[i].index)
                    continue;
                const bool ok = StartSeqRaw(p[i], seq[i].index);
                ok ? ++g_PlaySeqOk : ++g_PlaySeqRefused;
                if (g_PlaySeqOk + g_PlaySeqRefused <= 300)
                    LOG_INFO("Duel play: frame %u P%d starts '%s' (%d)%s, was '%s'", g_PlayFrame, i + 1, seq[i].name,
                        seq[i].index, ok ? "" : ": refused", live.name);
            }
            g_PlaySeqFirst = false;
        }
    }
    if (g_PlayShots && g_PlayFrame % g_PlayShots == 0) {
        char name[32];
        std::snprintf(name, sizeof(name), "play-%05u", g_PlayFrame);
        d3d::RequestScreenshot(name);
    }
    if (g_PlayFrame % 20 == 0) {
        LogFighter("played", 0, p[0]);
        LogFighter("played", 1, p[1]);
    }
    ++g_PlayFrame;
    --g_PlayLeft;
}

} // namespace

void DuelSnapFrame()
{
    PlayFrame();
    if (g_Watch > 0) {
        uint8_t* p[2] = {};
        Fighters(p[0], p[1]);
        char what[24];
        std::snprintf(what, sizeof(what), "after+%d", 9 - g_Watch);
        for (int i = 0; i < 2; ++i)
            if (p[i])
                LogFighter(what, i, p[i]);
        --g_Watch;
    }
    if (g_TestSave > 0 && --g_TestSave == 0) {
        LOG_INFO("%s", Save().c_str());
        if (!g_Snap.valid)
            g_TestSave = 30; // not in a duel yet: try again
    } else if (g_TestSave <= 0 && g_TestLoad > 0 && --g_TestLoad == 0) {
        LOG_INFO("%s", Load(g_TestMode).c_str());
    }
}

std::string StartDuelPlay(const std::string& name, uint32_t from, uint32_t frames, uint32_t shotsEvery, uint32_t mask)
{
    if (g_Play)
        std::fclose(g_Play);
    g_Play = _wfopen((ExeDir() + L"\\netplay\\duel-" + std::wstring(name.begin(), name.end()) + L".bin").c_str(), L"rb");
    if (!g_Play)
        return "duel play: no such recording";
    uint32_t header[3] = {};
    std::fread(header, sizeof(header), 1, g_Play);
    if (header[0] != 0x52445753 || header[2] != kObjectBytes) {
        std::fclose(g_Play);
        g_Play = nullptr;
        return "duel play: not a duel recording";
    }
    const long long record = 12 + 2 * g_PlayPad + 2 * kObjectBytes;
    _fseeki64(g_Play, 12 + record * from, SEEK_SET);
    if (g_PlaySeq)
        std::fclose(g_PlaySeq);
    g_PlaySeq = _wfopen((ExeDir() + L"\\netplay\\duel-" + std::wstring(name.begin(), name.end()) + L".seq.bin").c_str(),
        L"rb");
    if (g_PlaySeq) {
        uint32_t seqHeader[3] = {};
        if (std::fread(seqHeader, sizeof(seqHeader), 1, g_PlaySeq) != 1 || seqHeader[0] != 0x53445753 ||
            seqHeader[2] != sizeof(SeqInfo)) {
            std::fclose(g_PlaySeq);
            g_PlaySeq = nullptr;
        } else {
            _fseeki64(g_PlaySeq, 12 + 2LL * sizeof(SeqInfo) * from, SEEK_SET);
        }
    }
    g_PlaySeqFirst = true;
    g_PlaySeqOk = g_PlaySeqRefused = 0;
    g_PlayFrame = from;
    g_PlayLeft = frames;
    g_PlayShots = shotsEvery;
    g_PlayMask = mask ? mask : 3;
    LOG_INFO("Duel play: '%s' from frame %u, %u frames, fighters %u, %s", name.c_str(), from, frames, g_PlayMask,
        g_PlaySeq ? "with its sequences" : "without sequences");
    return "duel play: started";
}

void DuelRecorderFrame()
{
    DuelSnapFrame();
    if (!g_Out)
        return;
    uint8_t* p1 = nullptr;
    uint8_t* p2 = nullptr;
    Fighters(p1, p2);
    if (!p1 || !p2)
        return; // not in a duel now (loading, the round's end): only frames with both fighters
    static std::vector<uint8_t> a(kObjectBytes), b(kObjectBytes);
    ReadObject(p1, a.data());
    ReadObject(p2, b.data());
    input::GamePad pads[2] = {};
    input::ReadLivePad(0, pads[0]);
    input::ReadLivePad(1, pads[1]);
    const uint32_t head[3] = { g_Frame, uint32_t(uintptr_t(p1)), uint32_t(uintptr_t(p2)) };
    std::fwrite(head, sizeof(head), 1, g_Out);
    std::fwrite(pads, sizeof(pads), 1, g_Out);
    std::fwrite(a.data(), kObjectBytes, 1, g_Out);
    std::fwrite(b.data(), kObjectBytes, 1, g_Out);
    if (g_SeqOut) {
        SeqInfo seq[2];
        ReadSeq(p1, seq[0]);
        ReadSeq(p2, seq[1]);
        std::fwrite(seq, sizeof(seq), 1, g_SeqOut);
    }
    if (g_ShotEvery && g_Frame >= g_ShotFrom && g_Frame <= g_ShotTo && (g_Frame - g_ShotFrom) % g_ShotEvery == 0) {
        char shot[32];
        std::snprintf(shot, sizeof(shot), "rec-%05u", g_Frame);
        d3d::RequestScreenshot(shot);
    }
    if (g_DataOut && g_Frame % kDataEvery == 0) {
        const auto* live = reinterpret_cast<const uint32_t*>(uintptr_t(kDataBegin));
        const size_t n = (kDataEnd - kDataBegin) / 4;
        static std::vector<uint32_t> changes;
        changes.clear();
        const bool first = g_DataPrev.empty();
        if (first)
            g_DataPrev.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (first || live[i] != g_DataPrev[i]) {
                changes.push_back(uint32_t(i * 4));
                changes.push_back(live[i]);
                g_DataPrev[i] = live[i];
            }
        const uint32_t head2[2] = { g_Frame, uint32_t(changes.size() / 2) };
        std::fwrite(head2, sizeof(head2), 1, g_DataOut);
        std::fwrite(changes.data(), 4, changes.size(), g_DataOut);
    }
    ++g_Frame;
    if (g_Frame % 300 == 0) {
        std::fflush(g_Out);
        if (g_DataOut)
            std::fflush(g_DataOut);
        if (g_SeqOut)
            std::fflush(g_SeqOut);
    }
    if (--g_Left == 0)
        Close("done");
}

} // namespace swrots::net
