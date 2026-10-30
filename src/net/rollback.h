#pragma once
// Netplay's rollback core: the game played frame by frame from the D3D Swap (the end of each frame), every frame's
// state saved (net/state.h), the inputs the frame is played with recorded, and, when a rollback is asked for, the
// state brought back to an earlier frame and the frames since played again with their inputs, without being
// shown (a resimulation).
//
// For now its only user is SyncTest, the check that the game can be played again identically: at every
// `every`-th frame it goes back `distance` frames and plays them again, and the checksums must come out the same.
// The network session builds on the same core. See docs/research/netplay.md.

#include <cstdint>
#include <string>

namespace swrots::net {

// The end of a game frame (the D3D Swap, on the game's main thread): `frameStack` is where the game's part of the
// stack starts (its saved registers, then the Swap's return address and argument: see the Swap thunk in
// d3d/device.cpp). Saves the state, and may bring an earlier frame back (the caller then returns into that frame's
// Swap). Returns the value the Swap returns to the game.
uint32_t FrameBoundary(const void* frameStack, uint32_t swapCount);

// Whether the frame being played is a resimulation (nothing is shown, nothing waits for the display).
bool Resimulating();
// Whether a rollback session runs (the game's clocks and inputs are then the session's).
bool RollbackActive();

// SyncTest: from the next frame on, every `every` frames, back `distance` frames and played again.
void StartSyncTest(uint32_t distance, uint32_t every);
void StopRollback();
// A line about the session for the console.
std::string RollbackStatus();

// The game is restarting (a level change, a reboot): any session ends.
void ResetRollbackForReboot();

} // namespace swrots::net
