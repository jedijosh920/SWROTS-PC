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
