#pragma once
// The deterministic scheduler (netplay): while it runs, the game's threads take turns. One runs at a time (it holds
// the baton) and hands over only where it waits (a kernel wait, a sleep, a yield, the vertical blank), to the next
// thread in a fixed order whose wait is over; when none is, the fixed clock (kernel/clock.h) moves on to the
// earliest timeout or timer. Timers and DPCs are run by the scheduler, at those points. So the game's threads do
// the same things in the same order, and see the same times, on every PC: what a lockstep match needs from the
// level's loading on. Off, nothing changes. See docs/research/netplay.md.

#include <windows.h>

#include <cstdint>

namespace swrots::kernel {

struct XboxThread;

// From the next start of the game (BootInit) on, or off at the next one. A match turns it on before the reboot into
// its level.
void SetDeterministicBoot(bool on);
bool DeterministicBootRequested();
// Whether the scheduler runs (this start of the game).
bool SchedActive();
// Whether the calling thread is a scheduled game thread holding the baton.
bool SchedHolding();

// Each start of the game (BootInit, before any game thread): on or off as requested.
void SchedBoot();
// The game rebooted: every thread is gone.
void SchedReset();

// A game thread made (CreateXboxThread, on its creator, before it starts): it takes its turn after the others.
void SchedAddThread(XboxThread* t, bool suspended);
// A thread made suspended takes turns once resumed (NtResumeThread).
void SchedResumed(HANDLE thread);
// On the new thread, before any game code: waits for its first turn.
void SchedThreadStart(XboxThread* t);
// On a thread whose game code returned or that exits: the baton goes on.
void SchedThreadEnd();

// A scheduled thread's wait (GameWait): handles (none: a sleep), all of them or any, a timeout in milliseconds of
// the fixed clock (INFINITE), alertable. Returns as WaitForMultipleObjectsEx would.
DWORD SchedWait(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms, BOOL alertable);
// A yield (KeDelayExecutionThread with no delay, NtYieldExecution): the others whose wait is over run first.
void SchedYield();
// Until the fixed clock reaches `time` (100 ns units): the vertical blank.
void SchedWaitUntil(uint64_t time);

// I/O completions the scheduler delivers at the thread's next alertable wait (asynchronous reads are made
// synchronous while it runs, their completion routine kept for then).
using ApcFunction = void(__stdcall*)(void* context, void* ioStatus, ULONG reserved);
void SchedQueueApc(ApcFunction routine, void* context, void* ioStatus);

// Called by the thread holding the baton each time it hands it over (a wait, a yield): what moves with the fixed
// clock and the game waits for (the sound model's stream packets: audio::OnFrame), at points that are the same on
// every PC.
void SetSchedTurnHook(void (*hook)());

// A hash of every turn so far (which thread, at what time): two PCs whose games did the same have the same (netplay's
// check that a match has not parted).
uint64_t SchedTraceHash();

// Statistics for the log: switches, waits that only the PC could end (not deterministic), clock jumps.
void SchedLogStats();

} // namespace swrots::kernel
