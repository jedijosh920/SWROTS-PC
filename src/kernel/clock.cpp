// The fixed clock (see clock.h).

#include "kernel/clock.h"

#include <windows.h>

#include <algorithm>
#include <atomic>

namespace swrots::kernel {

namespace {

// An origin of one hour: no clock reads zero (a value some code treats as "never").
constexpr uint64_t kOrigin = 3600 * kClockUnitsPerSecond;
// The system time the fixed clock starts at: 2005-05-19 00:00 UTC (the game's release), as a FILETIME.
constexpr uint64_t kSystemTimeOrigin = 127610208000000000ull;
// Each read on the main thread moves the clock on by this share of a frame.
constexpr uint32_t kStepsPerFrame = 1024;

std::atomic<bool> g_On{ false };
std::atomic<uint32_t> g_Thread{ 0 };
std::atomic<uint64_t> g_FrameLength{ kClockUnitsPerSecond / 30 };
// The clock's state, saved and restored with the game (FixedClockState): only what the game's frames do moves it.
struct ClockState {
    uint64_t now;      // the time shown, in 100 ns units
    uint32_t reads;    // reads on the main thread since the frame started
    uint32_t pad;
};
ClockState g_State = { kOrigin, 0, 0 };
uint32_t g_ReadsMax = 0;

} // namespace

void SetFixedClock(bool on, uint32_t thread)
{
    g_Thread = thread;
    g_ReadsMax = 0;
    g_State = { kOrigin, 0, 0 };
    g_On = on;
}

bool FixedClockOn() { return g_On; }

void SetFixedClockFrame(uint32_t frame, uint32_t framesPerSecond)
{
    if (!framesPerSecond)
        framesPerSecond = 30;
    const uint64_t length = kClockUnitsPerSecond / framesPerSecond;
    g_FrameLength = length;
    // The frame's nominal start, or later when the frames before read the clock past it (a wait loop): the clock
    // never goes back.
    const uint64_t start = kOrigin + uint64_t(frame) * length;
    if (g_State.now < start)
        g_State.now = start;
    g_ReadsMax = std::max(g_ReadsMax, g_State.reads);
    g_State.reads = 0;
}

uint64_t FixedClockNow()
{
    if (GetCurrentThreadId() != g_Thread)
        return g_State.now;
    // Uncapped: a loop on the main thread waiting for the clock to pass a point (the engine's frame limiter) ends,
    // after the same number of reads every time the frame is played.
    ++g_State.reads;
    g_State.now += g_FrameLength / kStepsPerFrame;
    return g_State.now;
}

uint32_t FixedClockTickCount() { return uint32_t(g_State.now / 10000); }

uint64_t FixedClockSystemTime() { return kSystemTimeOrigin + g_State.now; }

uint32_t FixedClockReadsMax() { return g_ReadsMax; }

void* FixedClockState(uint32_t& size)
{
    size = sizeof(g_State);
    return &g_State;
}

} // namespace swrots::kernel
