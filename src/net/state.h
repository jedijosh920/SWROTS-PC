#pragma once
// Netplay's snapshots of the game (rollback): everything the game's simulation keeps, saved at the end of each
// frame and brought back to an earlier frame when a rollback needs it.
//
// The game runs natively, so its state is its memory: the Xbox's contiguous memory and the virtual memory it
// allocated (write-watched by Windows: only the pages written since the last save are looked at), the writable
// sections of its image, its pool blocks, the main thread's thread-local storage and Xbox thread state, and the
// port's own variables that take part in the simulation (RegisterHostState). Plus the main thread's stack from the
// frame boundary up (the D3D Swap: d3d/device.cpp), with the game's registers on it, so that after a load the game
// carries on from the earlier frame's Swap as if it had just returned from it.
//
// Each save keeps, for every page that changed since the previous one, what the page held before (a journal of the
// last frames); a load undoes the journal back to the frame asked for. A shadow copy of every page holds its content
// at the last save, which tells what changed and gives the journal its "before". Every page has a hash, and the
// frame's checksum is their sum, kept up to date page by page (for SyncTest and desync detection).
//
// Arm, save and load run on the game's main thread at a frame boundary, the game's other threads suspended (the
// caller does that: net/rollback.cpp). They take no lock and allocate nothing; failures are only logged afterwards
// (StateLastError).
// See docs/research/netplay.md.

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::net {

// What a session of snapshots keeps: `window` frames back at most (the longest rollback), with `journalMiB` of
// journal for the pages that changed meanwhile.
struct StateConfig {
    uint32_t window = 8;
    uint32_t journalMiB = 96;
    uint32_t stackKiB = 256; // the most of the main thread's stack a frame may use above the frame boundary
};

// Starts a session at a frame boundary on the game's main thread, in two steps:
//  - StatePrepare, with the game's other threads running: finds the memory to keep and allocates the buffers
//    (it takes locks, allocates and logs, which must not happen while those threads are suspended: one of them
//    may hold the lock). `frameStack`: where the game's part of the main thread's stack starts at the frame
//    boundary (see the Swap thunk in d3d/device.cpp); every later save and load must come from the same place.
//  - StateArm, with them suspended: takes the shadow copy and saves `frame` (the frame about to be played). It
//    takes no lock and allocates nothing. False when the game's memory changed shape since StatePrepare.
// StateLogArmed then logs what the session covers (threads running again).
bool StatePrepare(const StateConfig& config, const void* frameStack);
bool StateArm(const void* frameStack, uint32_t frame, uint64_t& checksum);
void StateLogArmed();
void StateDisarm();
bool StateArmed();
// The memory layout generation (kernel::MemoryLayoutGeneration) the session was armed with: when the game
// allocates or frees memory, the session no longer covers its memory exactly and must be armed again.
uint32_t StateLayoutGeneration();

// Saves the state at the start of `frame`, the frame after the last one saved; its checksum. False, logged
// afterwards (StateLastError), when the journal is full or the stack is not where it was.
bool StateSave(uint32_t frame, const void* frameStack, uint64_t& checksum);
// Brings everything back to `frame` (a saved frame still kept: StateOldestFrame..StateLatestFrame), the frames
// after it forgotten. The main thread's stack included: the caller returns into that frame's boundary.
bool StateLoad(uint32_t frame, const void* frameStack);
uint32_t StateOldestFrame();
uint32_t StateLatestFrame();
// The checksum saved with a kept frame (0 when it is not kept).
uint64_t StateChecksum(uint32_t frame);
// Why the last save or load failed.
const char* StateLastError();

// Every page's hash, to compare two moments (SyncTest), and the pages that differ between such a copy and now,
// described (which memory, which address), at most `max` of them.
void StateCopyPageHashes(std::vector<uint64_t>& out);
// Keeps a copy of every page as it was at the last save, so that StateDescribeDifferences also tells which words
// differ and what they held (a diagnostic: it copies all the memory covered).
bool StateKeepExpected();
std::string StateDescribeDifferences(const std::vector<uint64_t>& before, size_t max);

// Port code that keeps simulation state in its own variables registers them (before a session starts): they are
// saved and restored with the game.
void RegisterHostState(const char* name, void* data, uint32_t size);
// Memory that is saved and restored but left out of the checksums (it differs without the game doing anything
// differently: e.g. a buffer the PC writes into). Page granularity.
void ExcludeFromChecksum(const void* data, uint32_t size, const char* why);

// What the last save and load cost, and what the session covers.
struct StateCosts {
    double saveMs = 0, loadMs = 0;
    uint32_t savedPages = 0;    // pages that changed in the last saved frame
    uint32_t loadedPages = 0;   // pages written back by the last load
    uint32_t journalPages = 0;  // pages the journal holds now
    uint64_t trackedBytes = 0;  // memory the session covers
    uint64_t watchedBytes = 0;  // of which write-watched
    uint32_t regions = 0;
};
const StateCosts& StateCurrentCosts();

} // namespace swrots::net
