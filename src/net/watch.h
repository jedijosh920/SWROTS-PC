#pragma once
// Netplay diagnostics: who writes a word of the game's memory. Up to four words are watched with the CPU's debug
// registers (data breakpoints on write) on every thread that runs game code; each write is noted (the thread, the
// instruction after the write, the value written, the frame and whether it was played again) without stopping the
// game, and a summary is logged. SyncTest (net/rollback.cpp) uses it on the words it finds differing, to tell which
// code and which thread make the game differ when a frame is played again.

#include <cstdint>

namespace swrots::net {

constexpr int kWatchSlots = 4;

// Watches the 4-byte words at `addresses` (aligned down to 4; at most kWatchSlots) on every game thread from now
// on, replacing any earlier watch. Not on a suspended world: it starts a helper thread to set the main thread's
// registers. False when it could not.
bool WatchWords(const uint32_t* addresses, int count);
// Stops watching (all threads) and logs what was seen since WatchWords, grouped by word, thread and instruction.
void WatchStopAndLog(const char* title);
bool Watching();

// The frame being played and whether it is played again, noted with each write.
void WatchSetFrame(uint32_t frame, bool resimulated);
// While on, writes are not noted: the snapshots' own saves and loads (they copy memory back on the main thread).
void WatchSuppress(bool on);

} // namespace swrots::net
