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
// A frame is read in 1024 steps at most.
constexpr uint32_t kStepsPerFrame = 1024;

std::atomic<bool> g_On{ false };
std::atomic<uint32_t> g_Thread{ 0 };
std::atomic<uint64_t> g_FrameStart{ kOrigin };
std::atomic<uint64_t> g_FrameLength{ kClockUnitsPerSecond / 30 };
// Reads on the main thread this frame; only the main thread touches it.
uint32_t g_Reads = 0;
uint32_t g_ReadsMax = 0;

} // namespace

void SetFixedClock(bool on, uint32_t thread)
{
    g_Thread = thread;
    g_Reads = g_ReadsMax = 0;
    g_On = on;
}

bool FixedClockOn() { return g_On; }

void SetFixedClockFrame(uint32_t frame, uint32_t framesPerSecond)
{
    if (!framesPerSecond)
        framesPerSecond = 30;
    const uint64_t length = kClockUnitsPerSecond / framesPerSecond;
    g_FrameLength = length;
    g_FrameStart = kOrigin + uint64_t(frame) * length;
    g_ReadsMax = std::max(g_ReadsMax, g_Reads);
    g_Reads = 0;
}

uint64_t FixedClockNow()
{
    const uint64_t start = g_FrameStart;
    if (GetCurrentThreadId() != g_Thread)
        return start;
    const uint32_t step = std::min(g_Reads, kStepsPerFrame - 1);
    ++g_Reads;
    return start + g_FrameLength * step / kStepsPerFrame;
}

uint32_t FixedClockTickCount() { return uint32_t(g_FrameStart.load() / 10000); }

uint64_t FixedClockSystemTime() { return kSystemTimeOrigin + g_FrameStart.load(); }

uint32_t FixedClockReadsMax() { return g_ReadsMax; }

} // namespace swrots::kernel
