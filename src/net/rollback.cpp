// Netplay's rollback core and SyncTest (see rollback.h).

#include "net/rollback.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"
#include "core/settings.h"
#include "game/characters.h"
#include "input/controls.h"
#include "kernel/clock.h"
#include "kernel/kernel.h"
#include "net/state.h"
#include "net/watch.h"

namespace swrots::net {

namespace {

constexpr uint32_t kHistory = 128; // frames of inputs and checksums remembered (more than any rollback)
constexpr uint32_t kPorts = 4;

enum class Mode { Off, Arming, Running };

// The inputs a frame is played with: each port's pad and packet number.
struct FrameInputs {
    uint32_t frame = ~0u;
    input::GamePad pads[kPorts] = {};
    uint32_t packets[kPorts] = {};
};

struct Stats {
    uint32_t frames = 0, checks = 0, mismatches = 0, rearms = 0, resimFrames = 0;
    double saveMs = 0, saveMax = 0, loadMs = 0, loadMax = 0, resimMs = 0, resimMax = 0;
    uint64_t savedPages = 0;
    uint32_t loads = 0;
};

Mode g_Mode = Mode::Off;
bool g_Rebooting = false;
uint32_t g_Distance = 0, g_Every = 1;
DWORD g_MainThread = 0;
uint32_t g_Fps = 30;
uint32_t g_SwapBase = 0;

uint32_t g_Frame = 0;    // the frame being played
uint32_t g_ArmFrame = 0; // the first frame of the current snapshot session
bool g_Resim = false;
uint32_t g_ResimTarget = 0;
bool g_ResimMismatch = false;
uint32_t g_FirstBadFrame = 0;

uint64_t g_Checksums[kHistory] = {}; // each frame's checksum as first played
FrameInputs g_Inputs[kHistory];
std::vector<uint64_t> g_HashesAtTarget;

Stats g_Stats;
std::chrono::steady_clock::time_point g_LastBoundary;
bool g_HaveLastBoundary = false;

// --- the game's other threads ------------------------------------------------------------------------------------
// While a snapshot is saved or loaded nothing else may touch the game's memory: the dispatcher lock (IRQL) keeps
// DPCs and the game's own IRQL-raising code out, and the game's other threads are suspended.

HANDLE g_Suspended[64];
uint32_t g_SuspendedCount = 0;
UCHAR g_OldIrql = 0;

bool g_Opened[64]; // the handle was opened here (a thread the port adopted, e.g. the DPC thread): closed after

void StopTheWorld()
{
    g_OldIrql = kernel::RaiseIrql(kernel::kDispatchLevel);
    g_SuspendedCount = 0;
    kernel::ForEachThread(
        [](kernel::XboxThread* t, void*) {
            if (t->threadId == GetCurrentThreadId() || g_SuspendedCount == std::size(g_Suspended))
                return;
            HANDLE h = t->hostHandle;
            const bool opened = !h;
            if (opened)
                h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t->threadId);
            if (!h)
                return;
            if (SuspendThread(h) != DWORD(-1)) {
                // SuspendThread only asks; reading the context waits until the thread has stopped.
                CONTEXT context = {};
                context.ContextFlags = CONTEXT_CONTROL;
                GetThreadContext(h, &context);
                g_Opened[g_SuspendedCount] = opened;
                g_Suspended[g_SuspendedCount++] = h;
            } else if (opened) {
                CloseHandle(h);
            }
        },
        nullptr);
    WatchSuppress(true);
}

void StartTheWorld()
{
    WatchSuppress(false);
    for (uint32_t i = 0; i < g_SuspendedCount; ++i) {
        ResumeThread(g_Suspended[i]);
        if (g_Opened[i])
            CloseHandle(g_Suspended[i]);
    }
    g_SuspendedCount = 0;
    kernel::LowerIrql(g_OldIrql);
}

// Watching the words a mismatch shows (net/watch.h): a few rounds of four words each, each for a few seconds.
constexpr uint32_t kWatchRounds = 8;
constexpr uint32_t kWatchFrames = 240;
uint32_t g_WatchRound = 0;
uint32_t g_WatchUntil = 0;
std::vector<uint32_t> g_Watched;

void StartWatch(const std::vector<uint64_t>& hashesAtTarget)
{
    if (Watching() || g_WatchRound >= kWatchRounds)
        return;
    std::vector<uint32_t> words;
    StateDifferingWords(hashesAtTarget, words, 64);
    uint32_t pick[kWatchSlots];
    int count = 0;
    for (uint32_t w : words) {
        // a word not watched yet, and not next to one (the same structure, most likely)
        const bool seen = std::any_of(g_Watched.begin(), g_Watched.end(),
            [&](uint32_t v) { return (v > w ? v - w : w - v) < 64; });
        if (seen || count == kWatchSlots)
            continue;
        pick[count++] = w;
        g_Watched.push_back(w);
    }
    if (!count)
        return;
    ++g_WatchRound;
    if (WatchWords(pick, count))
        g_WatchUntil = g_Stats.frames + kWatchFrames;
}

void StopWatch(const char* why)
{
    if (Watching())
        WatchStopAndLog(why);
}

// --- inputs ------------------------------------------------------------------------------------------------------

FrameInputs& InputsOf(uint32_t frame) { return g_Inputs[frame % kHistory]; }

// The players' pads for `frame`, read now from the PC's devices; a port's packet number moves on when its pad
// changed from the previous frame's.
void SampleInputs(uint32_t frame)
{
    const FrameInputs& previous = InputsOf(frame - 1);
    const bool havePrevious = frame > 0 && previous.frame == frame - 1;
    FrameInputs& in = InputsOf(frame);
    in.frame = frame;
    for (uint32_t port = 0; port < kPorts; ++port) {
        input::ReadLivePad(int(port), in.pads[port]);
        const bool changed = !havePrevious || std::memcmp(&in.pads[port], &previous.pads[port], sizeof(input::GamePad)) != 0;
        in.packets[port] = (havePrevious ? previous.packets[port] : 0) + (changed ? 1 : 0);
    }
}

bool PadFromSession(int port, input::GamePad& pad, uint32_t& packet)
{
    if (g_Mode != Mode::Running || port < 0 || port >= int(kPorts))
        return false;
    const FrameInputs& in = InputsOf(g_Frame);
    if (in.frame != g_Frame)
        return false;
    pad = in.pads[port];
    packet = in.packets[port];
    return true;
}

// --- The cross-run determinism test (nettest) ---------------------------------------------------------------------
// Lockstep needs two PCs to start a match in the same state and to compute the same frames from the same inputs.
// `nettest record <name>` writes, when the session starts, all the memory it covers (netplay\<name>.mem next to the
// executable), then each frame's inputs and checksum, and every second every page's hash (<name>.frames); `nettest
// compare <name>`, in another run, compares its memory with the recorded one when its session starts, plays the
// recorded inputs, and reports the first frame whose checksum differs and which pages differ.

enum class Test { None, Record, Compare };

#pragma pack(push, 1)
struct TestFrame {
    uint32_t frame;
    uint64_t checksum;
    input::GamePad pads[kPorts];
    uint32_t packets[kPorts];
    uint32_t hashCount; // page hashes following (every second), else 0
};
#pragma pack(pop)

Test g_Test = Test::None;
std::string g_TestName;
uint32_t g_TestFrames = 0;    // frames to record
FILE* g_TestOut = nullptr;
std::vector<TestFrame> g_TestRecorded;
std::vector<std::vector<uint64_t>> g_TestHashes; // the recorded page hashes, by frame / hash interval
uint32_t g_TestFirstBad = ~0u;
uint32_t g_TestMismatches = 0;

std::wstring TestPath(const char* extension)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\netplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring name(g_TestName.begin(), g_TestName.end());
    return dir + L"\\" + name + L"." + std::wstring(extension, extension + std::strlen(extension));
}

struct DumpRegion {
    char name[40];
    uint32_t base, size, watched;
};

void TestDumpMemory()
{
    FILE* f = _wfopen(TestPath("mem").c_str(), L"wb");
    if (!f) {
        LOG_ERROR("nettest: cannot write the memory dump");
        return;
    }
    StateForEachRegion(
        [](const char* name, const uint8_t* base, uint32_t size, bool watched, void* ctx) {
            DumpRegion r = {};
            std::snprintf(r.name, sizeof(r.name), "%s", name);
            r.base = uint32_t(uintptr_t(base));
            r.size = size;
            r.watched = watched;
            std::fwrite(&r, sizeof(r), 1, static_cast<FILE*>(ctx));
            std::fwrite(base, 1, size, static_cast<FILE*>(ctx));
        },
        f);
    const long bytes = std::ftell(f);
    std::fclose(f);
    LOG_INFO("nettest: memory at the session's start written (%.1f MiB)", bytes / 1048576.0);
}

void TestCompareMemory()
{
    FILE* f = _wfopen(TestPath("mem").c_str(), L"rb");
    if (!f) {
        LOG_ERROR("nettest: no recorded memory to compare with (nettest record first)");
        return;
    }
    struct Ctx {
        FILE* f;
        uint32_t regions = 0, moved = 0, differingRegions = 0, words = 0, shown = 0;
        uint64_t differingBytes = 0;
        std::vector<uint8_t> buffer;
        std::string report;
    } ctx;
    ctx.f = f;
    StateForEachRegion(
        [](const char* name, const uint8_t* base, uint32_t size, bool, void* p) {
            Ctx& c = *static_cast<Ctx*>(p);
            ++c.regions;
            DumpRegion r = {};
            char line[200];
            if (std::fread(&r, sizeof(r), 1, c.f) != 1) {
                if (c.shown++ < 80) {
                    std::snprintf(line, sizeof(line), "\n  %-24s %p: not in the recording", name, base);
                    c.report += line;
                }
                return;
            }
            c.buffer.resize(r.size);
            std::fread(c.buffer.data(), 1, r.size, c.f);
            if (r.base != uint32_t(uintptr_t(base)) || r.size != size || std::strcmp(r.name, name) != 0) {
                ++c.moved;
                if (c.shown++ < 80) {
                    std::snprintf(line, sizeof(line), "\n  %-24s %p %u KiB, recorded: %s %08X %u KiB", name, base,
                        size / 1024, r.name, r.base, r.size / 1024);
                    c.report += line;
                }
                return;
            }
            uint32_t regionWords = 0;
            for (uint32_t at = 0; at + 4 <= size; at += 4) {
                uint32_t was, now;
                std::memcpy(&was, c.buffer.data() + at, 4);
                std::memcpy(&now, base + at, 4);
                if (was == now)
                    continue;
                ++regionWords;
                if (c.shown++ < 80) {
                    std::snprintf(line, sizeof(line), "\n  %-24s %08X: %08X recorded, %08X here", name,
                        uint32_t(uintptr_t(base + at)), was, now);
                    c.report += line;
                }
            }
            if (regionWords) {
                ++c.differingRegions;
                c.words += regionWords;
            }
        },
        &ctx);
    std::fclose(f);
    LOG_INFO("nettest: memory at the session's start: %u regions, %u placed differently, %u differing in %u word(s)%s",
        ctx.regions, ctx.moved, ctx.differingRegions, ctx.words, ctx.report.c_str());
}

void TestLoadFrames()
{
    g_TestRecorded.clear();
    g_TestHashes.clear();
    FILE* f = _wfopen(TestPath("frames").c_str(), L"rb");
    if (!f) {
        LOG_ERROR("nettest: no recorded frames");
        return;
    }
    TestFrame t;
    while (std::fread(&t, sizeof(t), 1, f) == 1) {
        g_TestRecorded.push_back(t);
        std::vector<uint64_t> hashes(t.hashCount);
        if (t.hashCount)
            std::fread(hashes.data(), sizeof(uint64_t), t.hashCount, f);
        g_TestHashes.push_back(std::move(hashes));
    }
    std::fclose(f);
    LOG_INFO("nettest: %zu recorded frames", g_TestRecorded.size());
}

void TestRecordFrame(uint32_t frame, uint64_t checksum)
{
    if (!g_TestOut)
        return;
    const FrameInputs& in = InputsOf(frame);
    TestFrame t = {};
    t.frame = frame;
    t.checksum = checksum;
    std::memcpy(t.pads, in.pads, sizeof(t.pads));
    std::memcpy(t.packets, in.packets, sizeof(t.packets));
    std::vector<uint64_t> hashes;
    if (frame % g_Fps == 0) {
        StateCopyPageHashes(hashes);
        t.hashCount = uint32_t(hashes.size());
    }
    std::fwrite(&t, sizeof(t), 1, g_TestOut);
    if (t.hashCount)
        std::fwrite(hashes.data(), sizeof(uint64_t), hashes.size(), g_TestOut);
    if (frame >= g_TestFrames) {
        std::fclose(g_TestOut);
        g_TestOut = nullptr;
        LOG_INFO("nettest: %u frames recorded", frame);
    }
}

// Compare: the recorded inputs of `frame` instead of the PC's.
bool TestInputs(uint32_t frame)
{
    if (frame >= g_TestRecorded.size())
        return false;
    const TestFrame& t = g_TestRecorded[frame];
    FrameInputs& in = InputsOf(frame);
    in.frame = frame;
    std::memcpy(in.pads, t.pads, sizeof(in.pads));
    std::memcpy(in.packets, t.packets, sizeof(in.packets));
    return true;
}

void TestCompareFrame(uint32_t frame, uint64_t checksum)
{
    if (frame >= g_TestRecorded.size()) {
        if (frame == g_TestRecorded.size())
            LOG_INFO("nettest: end of the recording at frame %u: %u frame(s) differed, the first at %d", frame,
                g_TestMismatches, g_TestFirstBad == ~0u ? -1 : int(g_TestFirstBad));
        return;
    }
    const TestFrame& t = g_TestRecorded[frame];
    if (t.checksum != checksum) {
        ++g_TestMismatches;
        if (g_TestFirstBad == ~0u) {
            g_TestFirstBad = frame;
            LOG_WARN("nettest: frame %u differs from the recording (the first)", frame);
        }
    }
    // Every second: which pages differ (the first times).
    const std::vector<uint64_t>& recorded = g_TestHashes[frame];
    static uint32_t reports = 0;
    if (!recorded.empty() && t.checksum != checksum && reports < 6) {
        ++reports;
        std::vector<uint64_t> now;
        StateCopyPageHashes(now);
        std::string text;
        uint32_t count = 0;
        for (uint32_t i = 0; i < now.size() && i < recorded.size(); ++i) {
            if (now[i] == recorded[i])
                continue;
            if (count++ < 24)
                text += "\n  " + StatePageName(i);
        }
        LOG_WARN("nettest: at frame %u, %u page(s) differ from the recording:%s", frame, count, text.c_str());
    }
}

// --- the session -------------------------------------------------------------------------------------------------

StateConfig Config()
{
    StateConfig config;
    config.window = std::max<uint32_t>(8, g_Distance);
    return config;
}

void SetClock() { kernel::SetFixedClockFrame(g_Frame, g_Fps); }

// The start of the frame about to be played (or played again): its clock, then what the sound model completes by
// then, the same in every play of the frame.
void EnterFrame()
{
    SetClock();
    audio::SetReplaying(g_Resim);
    audio::OnFrame();
    WatchSetFrame(g_Frame, g_Resim);
}

void LogStats(const char* when)
{
    const Stats& s = g_Stats;
    const StateCosts& c = StateCurrentCosts();
    LOG_INFO("SyncTest (%s): %u frames, %u checks, %u mismatch(es), %u re-arm(s); save %.2f ms avg / %.2f max, "
             "%.1f pages/frame; load %.2f ms avg / %.2f max; resimulated frame %.2f ms avg / %.2f max; journal %u "
             "pages; %u clock reads/frame at most",
        when, s.frames, s.checks, s.mismatches, s.rearms, s.frames ? s.saveMs / s.frames : 0.0, s.saveMax,
        s.frames ? double(s.savedPages) / s.frames : 0.0, s.loads ? s.loadMs / s.loads : 0.0, s.loadMax,
        s.resimFrames ? s.resimMs / s.resimFrames : 0.0, s.resimMax, c.journalPages, kernel::FixedClockReadsMax());
}

// Starts a snapshot session at the current frame boundary, the frame about to be played being `frame`.
bool Arm(const void* frameStack, uint32_t frame)
{
    static bool clockRegistered = false;
    if (!clockRegistered) { // the fixed clock's state is the game's: saved and restored with it
        uint32_t size = 0;
        void* clock = kernel::FixedClockState(size);
        RegisterHostState("fixed clock", clock, size);
        void* sound = audio::DeterministicState(size);
        RegisterHostState("sound model", sound, size);
        clockRegistered = true;
    }
    if (!StatePrepare(Config(), frameStack))
        return false;
    uint64_t checksum = 0;
    StopTheWorld();
    const bool armed = StateArm(frameStack, frame, checksum);
    StartTheWorld();
    if (!armed) {
        LOG_WARN("Netplay: the snapshots could not be armed at frame %u: %s", frame, StateLastError());
        StateDisarm();
        return false;
    }
    StateLogArmed();
    g_Frame = g_ArmFrame = frame;
    g_Checksums[frame % kHistory] = checksum;
    g_Resim = false;
    return true;
}

// Back to waiting for a level (the session's clocks and inputs off): a level ended, or the snapshots could not
// be armed; SyncTest starts again by itself in the next level, or at the next frame.
void Suspend(const char* why)
{
    if (g_Mode == Mode::Running)
        LogStats("paused");
    StopWatch("the session paused");
    LOG_INFO("Netplay: rollback session paused (%s)", why);
    input::SetPadSource(nullptr);
    audio::SetDeterministic(false);
    kernel::SetDeterministicPool(false);
    kernel::SetFixedClock(false);
    StateDisarm();
    g_Mode = Mode::Arming;
    g_Resim = false;
}

void Stop(const char* why)
{
    if (g_Mode == Mode::Off)
        return;
    if (g_Mode == Mode::Running)
        LogStats("end");
    StopWatch("the session ended");
    if (g_TestOut) {
        std::fclose(g_TestOut);
        g_TestOut = nullptr;
    }
    LOG_INFO("Netplay: rollback session ends (%s)", why);
    input::SetPadSource(nullptr);
    if (!g_Rebooting)
        audio::SetDeterministic(false);
    kernel::SetDeterministicPool(false);
    kernel::SetFixedClock(false);
    StateDisarm();
    g_Mode = Mode::Off;
    g_Resim = false;
}

// Arms a new session where the game is now (the memory changed shape, or the snapshots could not go on).
void Rearm(const void* frameStack, uint32_t frame, const char* why)
{
    if (g_Test != Test::None && g_Stats.rearms == 0)
        LOG_WARN("nettest: a new snapshot session at frame %u (%s): the checksums from here on are of a new start",
            frame, why);
    ++g_Stats.rearms;
    if (g_Stats.rearms <= 10)
        LOG_WARN("Netplay: new snapshot session at frame %u: %s", frame, why);
    else if (g_Stats.rearms == 11)
        LOG_WARN("Netplay: more new snapshot sessions; no longer logged");
    if (!Arm(frameStack, frame))
        Suspend("the snapshots could not be armed again");
}

// SyncTest: back `distance` frames, to play them again.
void BeginCheck(const void* frameStack)
{
    StateCopyPageHashes(g_HashesAtTarget);
    if (g_Stats.mismatches < 20 || (!Watching() && g_WatchRound < kWatchRounds)) // the words that differ
        StateKeepExpected();
    const uint32_t target = g_Frame;
    const uint32_t back = target - g_Distance;
    StopTheWorld();
    const bool loaded = StateLoad(back, frameStack);
    StartTheWorld();
    const StateCosts& c = StateCurrentCosts();
    g_Stats.loadMs += c.loadMs;
    g_Stats.loadMax = std::max(g_Stats.loadMax, c.loadMs);
    ++g_Stats.loads;
    if (!loaded) {
        LOG_ERROR("SyncTest: frame %u could not be loaded: %s", back, StateLastError());
        Rearm(frameStack, target, "after a failed load");
        return;
    }
    g_Frame = back;
    g_Resim = true;
    g_ResimTarget = target;
    g_ResimMismatch = false;
}

void EndCheck(uint64_t checksum)
{
    ++g_Stats.checks;
    if (!g_ResimMismatch)
        return;
    ++g_Stats.mismatches;
    if (g_Stats.mismatches > 20) {
        if (checksum != g_Checksums[g_ResimTarget % kHistory])
            StartWatch(g_HashesAtTarget);
        return;
    }
    const bool endDiffers = checksum != g_Checksums[g_ResimTarget % kHistory];
    std::string pages = endDiffers ? StateDescribeDifferences(g_HashesAtTarget, 16) : std::string();
    LOG_WARN("SyncTest: frames %u-%u played again differ, first at frame %u (%s at the end)%s%s",
        g_ResimTarget - g_Distance + 1, g_ResimTarget, g_FirstBadFrame, endDiffers ? "and still" : "but not",
        endDiffers ? "; pages that differ:" : "", pages.c_str());
    if (g_Stats.mismatches == 20)
        LOG_WARN("SyncTest: more mismatches; no longer logged");
    if (endDiffers)
        StartWatch(g_HashesAtTarget);
}

void NoteFrameTime(bool resimulated)
{
    const auto now = std::chrono::steady_clock::now();
    if (g_HaveLastBoundary && resimulated) {
        const double ms = std::chrono::duration<double, std::milli>(now - g_LastBoundary).count();
        g_Stats.resimMs += ms;
        g_Stats.resimMax = std::max(g_Stats.resimMax, ms);
        ++g_Stats.resimFrames;
    }
    g_LastBoundary = now;
    g_HaveLastBoundary = true;
}

} // namespace

uint32_t FrameBoundary(const void* frameStack, uint32_t swapCount)
{
    if (g_Mode == Mode::Off)
        return swapCount;
    if (g_Mode == Mode::Arming) {
        // Only the game's own frames in a level, once it has settled: not the movies (their player swaps from
        // threads of its own), not the front end (a "level" whose player has no class), not a level still loading
        // (the game allocates memory every frame then, and its main thread waits for the loading threads, which
        // the fixed clock would hold up). Settled: the same thread swapping, with no allocation, for a second.
        static uint32_t failures = 0, settled = 0, settledGeneration = 0;
        static DWORD settledThread = 0;
        if (!game::PlayerInLevel() || game::LevelPlayerClass().empty()) {
            settled = 0;
            return swapCount;
        }
        const uint32_t generation = kernel::MemoryLayoutGeneration();
        if (generation != settledGeneration || GetCurrentThreadId() != settledThread) {
            settledGeneration = generation;
            settledThread = GetCurrentThreadId();
            settled = 0;
        }
        if (++settled < uint32_t(std::max(30, GetSettings().fpsLimit)))
            return swapCount;
        g_MainThread = GetCurrentThreadId();
        g_Fps = uint32_t(std::max(1, GetSettings().fpsLimit));
        g_SwapBase = swapCount;
        g_Stats = Stats();
        g_HaveLastBoundary = false;
        for (FrameInputs& in : g_Inputs)
            in.frame = ~0u;
        SampleInputs(0);
        kernel::SetFixedClock(true, g_MainThread); // before the first save: the clock's state is saved with it
        g_Frame = 0;
        g_Resim = false;
        SetClock();
        audio::SetDeterministic(true); // the sound model's state is saved with it too
        kernel::SetDeterministicPool(true); // pool blocks from an arena the snapshots cover
        if (!Arm(frameStack, 0)) {
            kernel::SetDeterministicPool(false);
            audio::SetDeterministic(false);
            kernel::SetFixedClock(false);
            if (++failures == 600) // ten to twenty seconds of frames
                Stop("the snapshots could not be armed");
            return swapCount;
        }
        failures = 0;
        input::SetPadSource(&PadFromSession);
        audio::SetReplaying(false);
        audio::OnFrame();
        WatchSetFrame(0, false);
        g_Mode = Mode::Running;
        if (g_Test == Test::Record) {
            TestDumpMemory();
            g_TestOut = _wfopen(TestPath("frames").c_str(), L"wb");
            TestRecordFrame(0, StateChecksum(0));
            LOG_INFO("nettest: recording %u frames as '%s'", g_TestFrames, g_TestName.c_str());
        } else if (g_Test == Test::Compare) {
            TestCompareMemory();
            TestLoadFrames();
            TestInputs(0);
            TestCompareFrame(0, StateChecksum(0));
        } else {
            LOG_INFO("SyncTest: on, back %u frames every %u frames, at %u fps, the game's main thread %lu", g_Distance,
                g_Every, g_Fps, g_MainThread);
        }
        return g_SwapBase + g_Frame;
    }
    if (GetCurrentThreadId() != g_MainThread)
        return swapCount; // not the game's frames (e.g. a movie player's)
    if (!game::PlayerInLevel() || game::LevelPlayerClass().empty()) {
        Suspend("the level ended");
        return swapCount;
    }

    const bool wasResim = g_Resim;
    NoteFrameTime(wasResim);
    const uint32_t next = g_Frame + 1;

    if (kernel::MemoryLayoutGeneration() != StateLayoutGeneration()) {
        // The game allocated or freed memory: the snapshots no longer cover it exactly. Several frames in a row:
        // it is loading something; wait for it to settle (with the PC's clocks: it may wait on its own threads).
        static uint32_t lastRearmFrame = 0, rearmsInARow = 0;
        rearmsInARow = next == lastRearmFrame + 1 ? rearmsInARow + 1 : 0;
        lastRearmFrame = next;
        if (rearmsInARow >= 3) {
            Suspend("the game is allocating memory every frame (loading)");
            return swapCount;
        }
        g_Resim = false;
        if (!wasResim)
            SampleInputs(next);
        Rearm(frameStack, next, "the game allocated or freed memory");
        EnterFrame();
        return g_SwapBase + g_Frame;
    }

    uint64_t checksum = 0;
    StopTheWorld();
    const bool saved = StateSave(next, frameStack, checksum);
    StartTheWorld();
    if (!saved) {
        LOG_ERROR("Netplay: frame %u could not be saved: %s", next, StateLastError());
        g_Resim = false;
        if (!wasResim)
            SampleInputs(next);
        Rearm(frameStack, next, "after a failed save");
        EnterFrame();
        return g_SwapBase + g_Frame;
    }
    g_Frame = next;
    if (!wasResim) {
        const StateCosts& c = StateCurrentCosts();
        ++g_Stats.frames;
        g_Stats.saveMs += c.saveMs;
        g_Stats.saveMax = std::max(g_Stats.saveMax, c.saveMs);
        g_Stats.savedPages += c.savedPages;
    }

    if (wasResim) {
        if (checksum != g_Checksums[next % kHistory] && !g_ResimMismatch) {
            g_ResimMismatch = true;
            g_FirstBadFrame = next;
        }
        if (next == g_ResimTarget) {
            g_Resim = false;
            EndCheck(checksum);
        }
    } else {
        g_Checksums[next % kHistory] = checksum;
        if (g_Test != Test::Compare || !TestInputs(next))
            SampleInputs(next);
        if (g_Test == Test::Record)
            TestRecordFrame(next, checksum);
        else if (g_Test == Test::Compare)
            TestCompareFrame(next, checksum);
        if (g_Distance && next >= g_ArmFrame + g_Distance && next % g_Every == 0)
            BeginCheck(frameStack);
        if (g_Stats.frames % (g_Fps * 30) == 0)
            LogStats("running");
        if (Watching() && g_Stats.frames >= g_WatchUntil && !g_Resim)
            StopWatch("round over");
    }
    EnterFrame();
    return g_SwapBase + g_Frame;
}

bool Resimulating() { return g_Resim; }
bool RollbackActive() { return g_Mode == Mode::Running; }

void StartSyncTest(uint32_t distance, uint32_t every)
{
    Stop("a new one starts");
    g_Test = Test::None;
    g_Distance = std::clamp<uint32_t>(distance, 1, 30);
    g_WatchRound = 0;
    g_Watched.clear();
    g_Every = std::max<uint32_t>(every, 1);
    g_Mode = Mode::Arming;
}

void StartNetTest(bool record, const std::string& name, uint32_t frames)
{
    Stop("a new one starts");
    g_Distance = 0; // no rollback: frames are only saved (their checksums)
    g_Every = 1;
    g_Test = record ? Test::Record : Test::Compare;
    g_TestName = name;
    g_TestFrames = std::max<uint32_t>(frames, 60);
    g_TestFirstBad = ~0u;
    g_TestMismatches = 0;
    g_Mode = Mode::Arming;
}

void StopRollback() { Stop("asked for"); }

std::string RollbackStatus()
{
    char line[256];
    if (g_Mode == Mode::Off)
        return "rollback: off";
    if (g_Mode == Mode::Arming)
        return "rollback: starting at the next frame";
    const StateCosts& c = StateCurrentCosts();
    std::snprintf(line, sizeof(line),
        "SyncTest: frame %u, %u checks, %u mismatch(es), %u re-arm(s); %.1f MiB covered; last save %.2f ms, last "
        "load %.2f ms",
        g_Frame, g_Stats.checks, g_Stats.mismatches, g_Stats.rearms, c.trackedBytes / 1048576.0, c.saveMs, c.loadMs);
    return line;
}

void ResetRollbackForReboot()
{
    // The sound layer forgets its model with everything else (audio::ResetForReboot): the game's memory, which the
    // model's packets point into, is about to go.
    g_Rebooting = true;
    Stop("the game restarts");
    g_Rebooting = false;
}

} // namespace swrots::net
