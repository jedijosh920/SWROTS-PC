// Netplay's rollback core and SyncTest (see rollback.h).

#include "net/rollback.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "game/characters.h"
#include "input/controls.h"
#include "kernel/clock.h"
#include "kernel/kernel.h"
#include "net/state.h"

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

void StopTheWorld()
{
    g_OldIrql = kernel::RaiseIrql(kernel::kDispatchLevel);
    g_SuspendedCount = 0;
    kernel::ForEachThread(
        [](kernel::XboxThread* t, void*) {
            if (t->threadId == GetCurrentThreadId() || !t->hostHandle || g_SuspendedCount == std::size(g_Suspended))
                return;
            if (SuspendThread(t->hostHandle) != DWORD(-1)) {
                // SuspendThread only asks; reading the context waits until the thread has stopped.
                CONTEXT context = {};
                context.ContextFlags = CONTEXT_CONTROL;
                GetThreadContext(t->hostHandle, &context);
                g_Suspended[g_SuspendedCount++] = t->hostHandle;
            }
        },
        nullptr);
}

void StartTheWorld()
{
    for (uint32_t i = 0; i < g_SuspendedCount; ++i)
        ResumeThread(g_Suspended[i]);
    g_SuspendedCount = 0;
    kernel::LowerIrql(g_OldIrql);
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

// --- the session -------------------------------------------------------------------------------------------------

StateConfig Config()
{
    StateConfig config;
    config.window = std::max<uint32_t>(8, g_Distance);
    return config;
}

void SetClock() { kernel::SetFixedClockFrame(g_Frame, g_Fps); }

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
    LOG_INFO("Netplay: rollback session paused (%s)", why);
    input::SetPadSource(nullptr);
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
    LOG_INFO("Netplay: rollback session ends (%s)", why);
    input::SetPadSource(nullptr);
    kernel::SetFixedClock(false);
    StateDisarm();
    g_Mode = Mode::Off;
    g_Resim = false;
}

// Arms a new session where the game is now (the memory changed shape, or the snapshots could not go on).
void Rearm(const void* frameStack, uint32_t frame, const char* why)
{
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
    if (g_Stats.mismatches < 20) // the words that differ, for the mismatches logged
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
    if (g_Stats.mismatches > 20)
        return;
    const bool endDiffers = checksum != g_Checksums[g_ResimTarget % kHistory];
    std::string pages = endDiffers ? StateDescribeDifferences(g_HashesAtTarget, 16) : std::string();
    LOG_WARN("SyncTest: frames %u-%u played again differ, first at frame %u (%s at the end)%s%s",
        g_ResimTarget - g_Distance + 1, g_ResimTarget, g_FirstBadFrame, endDiffers ? "and still" : "but not",
        endDiffers ? "; pages that differ:" : "", pages.c_str());
    if (g_Stats.mismatches == 20)
        LOG_WARN("SyncTest: more mismatches; no longer logged");
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
        SetClock();
        if (!Arm(frameStack, 0)) {
            kernel::SetFixedClock(false);
            if (++failures == 600) // ten to twenty seconds of frames
                Stop("the snapshots could not be armed");
            return swapCount;
        }
        failures = 0;
        input::SetPadSource(&PadFromSession);
        g_Mode = Mode::Running;
        LOG_INFO("SyncTest: on, back %u frames every %u frames, at %u fps", g_Distance, g_Every, g_Fps);
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
        SetClock();
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
        SetClock();
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
        SampleInputs(next);
        if (g_Distance && next >= g_ArmFrame + g_Distance && next % g_Every == 0)
            BeginCheck(frameStack);
        if (g_Stats.frames % (g_Fps * 30) == 0)
            LogStats("running");
    }
    SetClock();
    return g_SwapBase + g_Frame;
}

bool Resimulating() { return g_Resim; }
bool RollbackActive() { return g_Mode == Mode::Running; }

void StartSyncTest(uint32_t distance, uint32_t every)
{
    Stop("a new one starts");
    g_Distance = std::clamp<uint32_t>(distance, 1, 30);
    g_Every = std::max<uint32_t>(every, 1);
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

void ResetRollbackForReboot() { Stop("the game restarts"); }

} // namespace swrots::net
