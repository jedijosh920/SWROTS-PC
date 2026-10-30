#pragma once
// The fixed clock: while it is on, every clock the game reads (the engine's timer, the performance counter,
// KeTickCount, the kernel's interrupt and system times) shows the time of the game frame being played, not the
// PC's. Netplay (net/rollback.h) needs it: two PCs, or one PC playing a frame again after a rollback, must see
// exactly the same times for the game to do exactly the same thing.

#include <cstdint>

namespace swrots::kernel {

// Units of the fixed clock: 100 ns, as the kernel's times.
inline constexpr uint64_t kClockUnitsPerSecond = 10000000;

// Turns the fixed clock on or off. `thread` is the host thread id of the game's main thread (the one that plays
// the frames): only its reads move the clock on within a frame (below); other threads read the frame's start.
void SetFixedClock(bool on, uint32_t thread = 0);
bool FixedClockOn();

// The frame being played: its start is `frame` frames of 1/`framesPerSecond` s after the fixed clock's origin.
// Called at every frame boundary, including after a rollback (with the frame the game is back at).
void SetFixedClockFrame(uint32_t frame, uint32_t framesPerSecond);

// The fixed time, in 100 ns units. On the main thread each read moves it on by a 1/1024 of a frame (at most to
// the frame's end), so a loop that waits for the clock to pass a point still ends, and does so after the same
// number of reads every time it is played.
uint64_t FixedClockNow();

// The same in milliseconds (KeTickCount), and as a system time (KeQuerySystemTime: a fixed date plus the fixed
// time, so nothing the game derives from the date differs either).
uint32_t FixedClockTickCount();
uint64_t FixedClockSystemTime();

// Reads of the clock on the main thread since the frame started, the most a frame has had since the clock was
// turned on (statistics for the netplay log).
uint32_t FixedClockReadsMax();

} // namespace swrots::kernel
