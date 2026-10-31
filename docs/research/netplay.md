# Netplay: playing Versus and co-op online

Work in progress on the `feature/online-netplay` branch. This page is the design, the decisions taken, and where
the work stands. Nothing here is playable yet: phase 1 (below) is a developer tool.

## Goals and rules

- **Online Versus and Cooperation**, the story co-op included (the bonus missions for two first).
- **No sign-up**: no account, no login. A player only has a local nickname (`settings.ini`).
- **An Online entry in the main menu**, between Multi Player and Settings.
- **Rollback netcode**: each player's own inputs show at once; the other player's are predicted, and when they
  arrive and differ, the game goes back to that frame and plays the frames since again, before showing the next.
- **No server of ours**: the two games talk to each other directly (peer to peer, UDP).
- **No mods online** for now: `mods\` is ignored for the session, custom characters and maps are hidden. The port's
  own fighters (Yoda, Commander Cody, the Droid Commander) stay: they are part of the port, the same on both PCs.

## Decisions

| Question | Decision |
|---|---|
| Rollback library | Our own, built after GGPO's design (input queue with prediction, a save and load per frame, time sync by frame advantage, redundant inputs in each packet, SyncTest). No GGPO code is copied. |
| Reaching the other player | LAN discovery, IPv6 when both have it, UPnP / NAT-PMP on the host's router, then **STUN on by default, can be turned off**: a public STUN server tells each game its public address, the players swap room codes, and both games send at once (UDP hole punching). Only that exchange touches the STUN server; it sees the player's address and nothing of the game. A list of servers in `settings.ini`. |
| What cannot connect | Symmetric NAT and carrier-grade NAT over IPv4 (mobile networks, some shared-IPv4 offers): only a relay server would help, and there is none. IPv6 often gets around it. |
| Co-op scope | Story missions too, after the bonus missions. |

## How rollback fits a native port

An emulator saves its machine's state as a block of memory. Here the game's code runs natively, so its state is
spread over the process: what has to be saved, and how fast, decides everything else. Hence phase 1.

### The frame boundary

The game ends every frame with `D3DDevice_Swap`, which the port implements (`d3d/device.cpp`). Its replacement is a
thunk that pushes the game's callee-saved registers (ebp, ebx, esi, edi), its x87 control word and its SSE control
register before calling the port's code: the game's part of the main thread's stack, from those registers up, is
the whole execution context at the boundary. Saving it with the memory, and copying it back on a load, makes the
game return from the earlier frame's Swap as if it had just been called there. A rollback is then just "load, and
let the game run": the frames are played again by the game's own loop, not shown (no present, no wait for the
display), until the frame that was being played.

This needs the game to call Swap from the same place, at the same stack depth, every frame of a match; the
snapshots check it (`the frame boundary is not where it was`).

### What a snapshot covers (`net/state.cpp`)

| Memory | How changes are found |
|---|---|
| The Xbox's contiguous memory (reserved by the loader at `0x80000000`) | Windows' write watch (`MEM_WRITE_WATCH`, already on) |
| The game's virtual and system memory | Write watch (reservations now ask for it), else compared |
| The image's writable sections (`.data`, `.bss`) | Compared with the shadow copy, page by page |
| Pool blocks (on the PC's heap) | Compared |
| The main thread's Xbox thread state and TLS | Compared |
| Port variables that take part in the simulation (`RegisterHostState`) | Compared |
| The main thread's stack, from the frame boundary up | Copied whole every frame |

A shadow copy holds every page as it was at the last save. A save looks at the pages that may have changed, keeps
what the shadow had for each one that did (the journal) and updates the shadow; a load writes the shadow back over
what changed since the last save, then undoes the journal newest first. Each page has a hash; the frame's
checksum is their sum, kept up to date page by page. Pages can be left out of the checksum (`ExcludeFromChecksum`)
when they differ without the game doing anything differently.

The game's other threads are suspended, and the dispatcher lock held (no DPC runs), while a snapshot is saved or
loaded.

When the game allocates or frees memory (`kernel::MemoryLayoutGeneration`), the snapshots no longer cover its
memory exactly: the session starts again from there (a "re-arm"). How often this happens during a duel and a
mission is one of the things phase 1 measures; if it is frequent, the allocator's own state has to become part of
the snapshot.

### Determinism

Both PCs (and one PC playing a frame again) must do exactly the same thing with the same inputs:

- **Clocks** (`kernel/clock.cpp`): during a session every clock the game reads shows the frame's time: the engine's
  timer (`0x227170`), the performance counter, `KeTickCount`, `KeQueryInterruptTime`, `KeQuerySystemTime`. On the main
  thread each read moves the time on by 1/1024 of a frame, so a loop waiting on the clock ends after the same number
  of reads. The performance counter's frequency is now always 10 MHz, on every PC.
- **Inputs** (`input::SetPadSource`): the game's reads of the controllers get the frame's recorded pads, the same
  however often it reads them; packet numbers come from the inputs, not from when they were read.
- **Random numbers**: the game's own generator lives in its memory and is saved with it. The port's own code uses
  `std::rand` seeded from the clock in places (`game/hvv.cpp`): to be replaced by a seeded generator kept in the
  snapshot before Heroes vs Villains can go online.
- **Settings that change the simulation** (difficulty, frame rate, developer variables, cheats, `timeScale`) are the
  host's; the debug menu and console are off online.
- **The same game**: same port version, same executable (MD5 already checked), same disc files; checked when the
  players connect.

Not handled yet, to be looked at with SyncTest's results: the game's other threads (streaming, sound) and kernel
objects (events, timers) whose state is the PC's, not the game's memory; sounds started in a frame played again
(they would play twice); host resources (textures, sound buffers) created or released during a match.

## Phases

| # | Step | Done when |
|---|---|---|
| 1 | Snapshots and SyncTest | A duel runs 10 minutes in SyncTest without a mismatch; costs measured. **Decides the rest**: if rollback is too slow or cannot be made deterministic, delay-based lockstep first. |
| 2 | Determinism | Two instances given the same inputs agree on every checksum |
| 3 | Transport and session | A duel over the LAN with rollback |
| 4 | Online menus | The main menu's entry, the lobby, the character select in step |
| 5 | NAT traversal (UPnP, STUN, room codes) | A duel over the Internet |
| 6 | Co-op | Bonus missions for two, then story missions, online |
| 7 | Finishing | Network stats, rematch, player guide (`docs/online.md`) |

### Phase 1: SyncTest

SyncTest checks that the game can be played again identically, on one PC, with no network: every few frames it
goes back a few frames and plays them again, and the checksums must come out the same. It is the rollback core
the network session will use, with the local inputs recorded.

Start a duel (e.g. `map:u110_cor_training_00` and `v_versusMode:1` in `mods\Default_Xbox.cfg`), then in the debug
console:

```
synctest 4          back 4 frames, every frame
synctest 7 10       back 7 frames, every 10 frames
synctest            the session's state
synctest off        stops it
```

Or unattended: `SWROTS_COMMANDS="15:synctest 4"` with `SWROTS_TEST_INPUT=16` (a busy player 1).

The log has every 30 seconds:

```
SyncTest (running): 900 frames, 900 checks, 0 mismatch(es), 0 re-arm(s); save 0.41 ms avg / 1.20 max,
  210.0 pages/frame; load 0.90 ms avg / 2.10 max; resimulated frame 3.10 ms avg / 5.00 max; journal ...
```

and, for a mismatch, the first frame that differed and the pages that still differ at the end (which memory, which
address, to look up in the symbol map). The session starts again by itself after a level change, a failed save or
a change in the game's memory layout (`re-arm`).

What to measure on a duel and on a story mission:

- **save** and **load** times, and **pages/frame**: the snapshot cost at every frame;
- **resimulated frame**: what playing a frame again costs (the game's update and its draw calls, not shown);
- **re-arms**: how often the game allocates memory in play;
- **mismatches**: what is not deterministic yet.

The budget at 30 fps (the game's own rate): 33 ms per frame for a frame shown plus up to 7 frames played again.

### First SyncTest results (2026-10-30)

A Release build (MSVC 19.44) on Windows, the developer map with Grievous against a computer Obi-Wan, player 1 driven
by `SWROTS_TEST_INPUT`, `FpsLimit=60`. Five minutes, no hang, no crash.

| | `synctest 4` (every frame) | `synctest 7 10` |
|---|---|---|
| Memory covered | 28.9 MiB in ~510 regions (25.2 MiB write-watched, 3.7 MiB compared: the image's `.data` and its library sections) | same |
| Save per frame | 4.1 ms avg, 12.6 ms max, ~200 pages | 3.8 ms avg, 11.7 ms max, ~160 pages |
| Load | 4.0 ms avg, 8.0 ms max | 3.9 ms avg, 7.9 ms max |
| A frame played again | 6.4 ms avg, 28.9 ms max | 5.3 ms avg, 26.3 ms max |
| Re-arms (the game allocated memory) | 44 in 3,600 frames | 35 in 5,300 frames |
| Frame rate shown | ~26 fps (5 frames of work per frame) | ~50 fps |
| Mismatches | every check | every check |

What it tells us:

- **The approach works mechanically**: the game carries on after thousands of loads, from the Swap thunk's restored
  stack, and plays frames again without showing them.
- **Costs**: a rollback of 7 frames is ~4 ms (load) + 7 × ~6 ms (frames) + the save each frame: too much for 60 fps,
  within reach at the game's own 30 fps. The save is the first thing to make cheaper: most of it is comparing the
  3.7 MiB that are not write-watched (to watch the image's data too: protect its pages and note the first write to
  each, or move the comparison to a copy made with the write watch), and hashing changed pages.
- **Clock**: the engine's frame limiter reads the clock ~1,030 times per frame (it spins until the frame's time has
  passed). Harmless with the uncapped fixed clock, but the main thread's reads must stay the same from one play of a
  frame to the next.
- **Determinism, the real work ahead**: every check differs, on a few dozen pages. The report's words point at:
  - floats that look like accumulated times (`0x46435586` ≈ 12501.3 against `0x4643553A`): a time source that is not
    yet the fixed clock, or one read a different number of times;
  - a pointer and counters that move together (`0078B544`, `0078B560`): a per-frame allocator or a ring buffer whose
    position depends on something outside the frame (the renderer, the sound thread);
  - ASCII text in the first play only (`"x380"`, `"al  "`): scratch buffers written by code that only runs when a
    frame is shown, or by another thread;
  - values that look random (`0065E91C`): a generator seeded or advanced outside the frame.

  Next: name these addresses with the symbol map, and find which thread writes them (a write watch per page between
  two boundaries, or hardware breakpoints on the first words found).

### What makes a frame played again differ (2026-10-30, continued)

`net/watch.cpp` watches the words SyncTest finds differing with the CPU's debug registers (data breakpoints on
write, on every game thread) and logs who writes them. Over two five-minute runs, with the game disassembled from
the player's own `default.xbe` (`objdump` on its sections mapped at their addresses):

1. **Every differing word is written by the game's main thread**, never by its streaming, sound or DPC threads.
   Some are written with addresses of the main thread's stack, or copied from stack slots (`0x22DB73`:
   `mov [0x7E63AC], edx` with `edx = [esp+0x2c]`), and earlier reports found text in game memory: the game reads
   **uninitialised locals**, whose leftovers depend on what ran on the stack before (the port presents and draws
   its menu in frames shown, not in frames played again). The Swap thunk now zeroes the stack below the game's
   frame at every boundary while rollback runs.
2. **The port's D3D layer** writes the graphics libraries' data (`D3D` section: render states, matrices), which
   differs when a frame is not drawn the same way. It only shapes the picture: those sections stay in the snapshot
   but out of the checksum.
3. **Sound** (what remains): lists, counters and time-like floats of the game's sound code differ. The game asks
   the port's DirectSound layer whether a buffer still plays (`GetStatus`), where it is (`GetCurrentPosition`), the
   audio clock (`DirectSoundGetSampleTime`, from the PC's clock), whether a stream packet is done (callbacks from
   `DirectSoundDoWork`): all answered from XAudio2's real playback, which moves on in real time, not with the
   frames. During a rollback session these answers must come from a model of the playback on the fixed clock (a
   buffer started at frame *n* at frequency *f* is at sample *(t - t_n) × f*; a stream packet is done once the
   model has played it), saved with the snapshots, with XAudio2 only following it for what is heard. Sounds started
   in frames played again must not be heard twice (the model knows which sounds it already started).

Next: the deterministic audio model, then SyncTest again on the developer map and in a story mission.

### Sound model and deterministic pool (2026-10-30, runs 7 and 8)

- **The sound model works**: the sound code no longer shows among the differences. The game has ~280 sound buffers
  and streams in the developer map duel (the model has room for 2048).
- **Pool blocks** from the deterministic arena: new snapshot sessions caused by allocations dropped from 44 to 2 in
  five minutes (most allocations during play were pool blocks). The arena adds 48 MiB to the snapshots, write-watched.
- **What still differs**, in order of importance:
  1. **The game's worker thread** (`0x79230`, started with the level) takes jobs from a queue in game memory
     (`[0x68D094]`). A rollback restores the queue, and the thread runs the jobs again: it closes the same handles
     twice (`NtClose ... C0000008` in the log). Jobs done once by the PC (closing a file, an event) cannot be undone
     by restoring memory. During a session this thread has to run in step with the frames (its jobs done at frame
     boundaries on the main thread, or held while frames are played again), and handles (kernel objects) made or
     closed in a frame need the same treatment as pool blocks: closes held until no rollback can come back, new
     handles numbered the same in every play.
  2. **A 128-byte block copied every frame** to `0x78B520` (by `0x214070`, called through a table at `0x59CD44`):
     pointers into the PC's heap and small counters that differ (0x11 against 0x0B). Probably a consequence of 1 (its
     source is filled by the worker's results), to check once 1 is done.
- **Costs**: with the debug-register watch on, every write to a watched word raises an exception (tens of thousands
  per round), which slowed run 8 down threefold (save 15 ms instead of 4); the watch is a diagnostic only.

## Lockstep first (decided 2026-10-30), rollback after

Lockstep (each PC waits for the other's inputs, with 2-4 frames of input delay) needs no snapshots, but it needs the
same thing at its root: **two PCs must start the match in the same state** and compute the same frames.

### nettest: two runs compared

`nettest record <name>` / `nettest compare <name>` (console) record the memory at the session's start and every
frame's inputs and checksum, then compare a second run playing the same inputs. Two runs of the developer map duel
on the same PC, one after the other:

- **509 of 512 memory regions are at different addresses** in the two runs: the game's pool blocks on the PC's heap,
  its virtual memory (placed by Windows), the netplay pool arena, the port's own state in `core.dll` (relocated by
  ASLR: the port's jumps patched into the game's libraries differ too).
- So **every frame differs**, from the first.

Two PCs will differ in the same ways, and more: everything the game holds that the PC chose (an address, a handle,
an object of the port's) differs, and so does everything that depended on timing while the level loaded (how many
frames the loading screen lasted, which loading thread allocated first, where the sounds started during the load
are).

### What a deterministic match start needs

The game's "machine" is the port's kernel and libraries, so they must behave like a deterministic machine from the
moment the match's level starts loading (the in-process reboot into it), not only from a settled frame:

1. **A fixed host address space**: `core.dll` at a fixed base; the pool arena at a fixed address and used from the
   reboot on; the game's virtual memory placed by the port in a fixed range (first-fit) rather than by Windows; the
   port's objects the game holds (sound objects, thread objects, kernel handles) from arenas or numbered by the port.
2. **Deterministic scheduling of the game's threads**: they already share one CPU core; they must also take turns in
   the same order on every PC: one game thread runs at a time and hands over only where it waits (a kernel wait, a
   yield, an event), in an order the port decides, not when Windows preempts it. This is what makes the loading
   threads, and the in-match worker thread that rollback replays (`0x79230`), deterministic.
3. **The fixed clock from the reboot on**, moved by that scheduler (time passes when the game waits), and the sound
   model on from the start.
4. Then nettest again: the memory at the session's start should match, then the frames.

This is the foundation of both netcodes (rollback also needs the PCs to agree); it is only switched on for a netplay
match, so single-player games keep running exactly as they do.

### Deterministic addresses: done (nettest, 2026-10-30)

With `nettest` restarting the level with deterministic addresses (the fixed range at `0x20000000` for the game's
virtual memory and pool blocks, `core.dll` at `0x60000000` kept free by the loader until it loads, code stubs at
`0x9F0000`, the game threads' stacks, objects and TLS in the fixed range's thread zone):

| | Run 1 | Fixed range | + core.dll base | + stubs and threads |
|---|---|---|---|---|
| Regions at different addresses | 509 / 512 | 2 / 544 | 2 / 544 | **0 / 544** |
| Pages differing at the match's start | 3,024 | 329 | 330 | ~40 regions, 8,430 words |

What still differs is **content that depends on the PC's timing while the level loads**: frame counters (`0x646268`:
0x91C against 0x9DC frames), accumulated times and positions (floats differing in their last bits), values that
look random (seeded or advanced by time), and one pointer outside the fixed range (`0x24000058`: 0x12AF0000 against
0x140B0000, to find). That is the next step: the fixed clock and a deterministic scheduler of the game's threads from
the reboot on (see above), so that loading takes the same number of frames and the same course on every PC.

Note for real matches: code stubs are taken in the order the port makes them, from the first start of the game. Two
PCs that did different things before a match (one played a mission) may have made stubs in a different order; the
stubs should be made at fixed places per patch site, or all made at startup.

### The deterministic scheduler (kernel/sched.cpp)

While a netplay match asks for it (`kernel::SetDeterministicBoot`, from the next start of the game: nettest restarts
the level with it), the game's threads take turns:

- One game thread runs at a time: it holds the baton. It hands it over only where it waits (`GameWait`: every kernel
  wait, sleep and critical section), yields (`KeDelayExecutionThread(0)`, `NtYieldExecution`) or waits for the
  vertical blank, to the next thread in creation order whose wait is over (its handles polled without blocking, or
  its timeout reached on the fixed clock).
- When no thread can run, the fixed clock jumps to the earliest timeout or timer: time passes only when the game
  waits, the same on every PC. Every scheduled thread's clock reads move it on (a busy wait on the clock ends).
- Timers and DPCs are fired and run by the scheduler, on the thread holding the baton, when the clock reaches them
  (no timer thread, no DPC thread).
- Asynchronous file reads and writes finish before they return, their completion routines run at the thread's next
  alertable wait.
- The sound model and the fixed clock are on from the start.
- What only the PC can end (an event set by a host thread) is waited for in real time and counted in the log
  (`wait(s) only the PC could end`): those are where determinism is still lost.
- The vertical blank is the fixed clock's: the game runs as fast as the PC allows (pacing to real time is for later,
  without touching the fixed clock).

### Deterministic match start: done (nettest, 2026-10-30, evening)

With the scheduler, the last differences were values the host put into the game's memory. Each was found with
`SWROTS_WATCH_BOOT=addr[,addr...]` (debug registers on every game thread from the restart on; the writers, their
instruction and the return addresses found on their stack are logged when the session starts) and nettest's report
of pointers outside the deterministic places (what the allocation is: its size, a thread's stack, a heap, its first
words):

| Source | Fix |
|---|---|
| Host handle values (event handles at `0x646268`) | the game's handles numbered in its own order while the scheduler runs (`kernel::ToGameHandle`, translated back at every export); thread ids follow; symbolic link pseudo handles restart at each boot |
| XAPI's heap's large blocks (`NtAllocateVirtualMemory(MEM_COMMIT)` with no base, which reserves too) left to Windows | placed in the fixed range like reservations (also tracked for the reboot and write-watched) |
| The port's code leaving heap pointers and return addresses on the game's stack, read back through the game's uninitialised locals (a half-filled vertex shader constant buffer, a vector's unused lane) | gates (`core/gates.h`): every kernel export, XDK replacement (but Swap) and port hook of a game function (`PatchHook`) is called through a gate at a fixed address that zeroes the stack below after it; the resource hooks also zero it before calling the game's decoders (`ScrubStack`) |
| Memory files over the port's `std::vector` (host heap addresses kept by the engine) | the data copied to `GameAlloc` memory for the decode; a manual file's (kept) to the engine's memory |
| Strings the port hands to the game (menu handler names, the port's texts) | from a pool in core.dll (`GameString`, `GameText`) |
| nettest itself: a frame that starts a new snapshot session got no recorded frame (the recording shifted by one) | recorded and compared as any other |

Result: **the memory at the match's start is identical in two runs (545 regions, 0 words differ), and 1,800 frames
(a minute of the duel, with a new snapshot session at frame 920 when the game loaded something) played from the
recorded inputs have the same checksum as the recording, every one.** The scheduler's traces match too.

Next: the lockstep session itself (transport, the Online menu, the lobby), real-time pacing, and the stubs at fixed
places per patch site (see the note above) so that two PCs that did different things before a match still agree.

### Online play, first cut (net/session.cpp, net/transport.cpp; 2026-10-31)

From the main menu's Online entry (next section) or the console (`online host`, `online join <room code or
host[:port]>`, `online leave`, `online`). The host opens a UDP port (27960, `[Online] Port`), asks STUN for its public address (on by default;
`[Online] Stun=0` or `SWROTS_ONLINE_STUN=0` off) and shows a room code (15 characters, checked: its public address and port, the low half of its address on its
network, whether that port is the same; the guest takes the high half from its own address, both on one home network).
The guest calls both addresses; both check they run the same build and the same game files (names and sizes, the
small files and the mods whole); the host's profile (saved by the game for its next start), its saves and its gameplay
settings go to the guest; both start the game again (the scheduler, fixed addresses, the match's own save and utility
drives under `netplay\online`) at the main menu (the title without a profile). Every Swap each PC sends its input for
3 frames ahead and waits for the other's; player 1 is the host, player 2 the guest; ports 0 and 1 are connected on
both PCs. A hash of the scheduler's course and the game's data is compared every 60 frames
(`SWROTS_ONLINE_CHECK_EVERY` for more); a desync writes the data checked to `netplay\desync-*.bin`. The pace follows
the game's own time (the fixed clock).

What a start of the game from the dashboard needed under the scheduler, found on the way:

- the sound model's streams move on at every scheduler turn (they waited for rollback's frames);
- resuming a thread yields the turn, and thread suspensions are the scheduler's (the movie player suspends itself);
- the game's four rdtsc reads come from the fixed clock;
- a critical section's owner is the thread object, not the host's thread id;
- NtQueryVirtualMemory shows each placed reservation as its own allocation;
- the hooks' trampolines are made again at every start (statics from the first start pointed at stubs a later start
  had replaced: the front end's faults at 0x50965 and 0x532F0).

Result on one PC (two copies of the game, `netplay-online.bat`, random test inputs on both): 4,200 frames in lockstep,
from the intro through the title into the menus, checked every frame, the same on both.

### The Online menu (game/onlinemenu.cpp, debug/onlinehud.cpp; 2026-10-31)

The main menu gets a row **Online** between Multi Player and Settings (a copy of Settings' row; Settings and Features
move one row down, onto the main menu's spare fifth veil). It opens the port's screen `online`, built from the
Settings screen: **Host Game**, **Join Game**, **Room Code**, **Status**, **Leave**.

- Host Game opens a room; the code shows on the Room Code row and goes to the clipboard (A on that row copies it again).
- Join Game opens the game's own code keyboard (Settings > Codes: A-Z, 0-9, Back, Space, Done) on a screen
  `online_code`; its keyboard is the game's with the port's Done (joins with the code typed, or refuses it with the
  game's error sound) and holds 15 letters (the game's text buffer holds 16 characters). **Ctrl+V** or **Y** pastes
  a code from the clipboard. The code's 15 characters were chosen for that keyboard.
- In a match (the screens are then played on both PCs alike), the screen shows the same texts on both, Host and Join do
  nothing, and Leave ends the session on both.
- Over the picture (the overlay, drawn by each PC alone): a small `ONLINE P1 (HOST) 23 MS` corner line in a match, a
  banner while the match starts, a red one on a desync, and why a session ended for 10 s.
