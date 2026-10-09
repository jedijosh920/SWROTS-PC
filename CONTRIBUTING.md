# Contributing to SWROTS-PC

Thanks for helping. This page covers the ground rules, the development setup, and how to find your
way around the code and the game.

## Ground rules

- **No game content in the repository.** Never commit files from the game disc, extracted assets,
  dumps, or code copied or decompiled from the game's executable. Addresses, offsets, structure
  layouts and descriptions of how the game works are fine: they are facts about the executable,
  not its content. Tools that work on the user's own copy of the game are fine too.
- **Facts come from the game's own executable.** When naming or describing something, trust what the
  executable itself says (its strings, class names returned by `GetTypeName`, source paths, config
  keys) over guesses and old notes. Names borrowed from related games (Indiana Jones and the
  Emperor's Tomb, same engine) are marked as such.
- **Keep third-party licences intact.** The project is GPL-3.0-or-later. Code adapted from
  GPL-compatible projects keeps its notices (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
  Do not add code under licences incompatible with the GPL.
- **Match the surrounding code**: its naming, comment density and style. Comments explain why and
  what the game does, with the addresses involved.
- **Keep the documentation current.** A change a player or modder would notice updates the matching
  page in `docs/` in the same pull request.

## Setup

1. Install Visual Studio 2022 or newer (Desktop development with C++), CMake 3.20+, and Git.
   Python 3 is needed only for the tools in `tools/` (`pip install capstone`).
2. Build (32-bit only; the game's code is 32-bit x86):

   ```bat
   cmake -S . -B build -A Win32
   cmake --build build --config Release
   ```

3. Run `build\bin\swrots.exe` and give it your disc image, or copy an already installed `GameData\`
   folder into `build\bin\` (a real copy; do not link to your game folder).

Everything the game reads and writes lives next to the executable: `GameData\`, `saves\`, `cache\`,
`mods\`, `logs\`, `settings.ini`, `controls.ini`.

## Useful developer settings

In `settings.ini` (details in [settings](docs/settings.md)):

```ini
[Debug]
DebugMenu=1        ; the ~ menu: the game's console, variables, switches
DebugDisplays=1    ; the engine's fps counter, profiler and memory display
LogResources=1     ; every resource a level loads, with its path
TraceSdk=1         ; the game's calls into Xbox libraries (verbose)
TraceOpen=<text>   ; which game functions open files whose path contains <text>
FlightRecorder=1   ; Ctrl+Shift+F10 saves the last 3 seconds of frames and draw calls
```

In `mods\Default_Xbox.cfg`, `map:<level>` boots straight into a level, skipping the menus (level
names are the PAK names in `GameData\pak` without `res_` and `.pak`), e.g. `map:u112_mus_lava`.
A versus duel starts directly with `map:u110_cor_training_00` and `v_versusMode:1` on the next line.

Environment variables: `SWROTS_NO_DIALOGS=1` (no message boxes, for unattended runs),
`SWROTS_WATCHDOG=<seconds>` (dumps every game thread's stack after that time, for hangs),
`SWROTS_DRAWLOG_FRAME=<n>` or F11 (logs every draw of one frame), `SWROTS_SHADER_DUMP=<dir>`
(writes the generated HLSL shaders), `SWROTS_ALL_CORES=1` (lets game threads use every CPU core;
for tests only, the game expects one), `SWROTS_NO_AUDIO=1` (runs as without an audio device: the
game plays silently, movies and sounds keeping time on the clock), `SWROTS_REBOOT_EVERY=<seconds>`
(restarts the game in-process that often, as a level change does, to test what a restart leaves
behind; best with a level booted from `Default_Xbox.cfg`, since a restart mid-movie falls back to a new
process), `SWROTS_DUELISTS=<slot>=<class>[,...]` (the console's `duelist` changes from the start, e.g.
`0=IYoda`, for unattended versus tests), `SWROTS_PLAYER="<class>[ <costume>][ skin <set>][ mesh <mesh>]"` (the
console's `player` from the start, e.g. `IVader` or `"IAnakin duel"`; `-` for the class keeps each
level's own, e.g. `"- mesh obi"`; the port also sets it itself when a restart falls back to a new
process, to keep the player's choice), `SWROTS_FREECAM=<seconds>` (turns the free camera on that long
after the start, e.g. in a duel booted from `Default_Xbox.cfg`), `SWROTS_TEST_INPUT=<seconds>` (from
that many seconds after the start, player 1 plays by itself like a busy player: the left stick turning
round and the face buttons, triggers and white and black buttons pressed in turn, never Start or Back;
for testing what only happens while the player moves and fights; `<seconds>:run` only holds the stick
forward), `SWROTS_TEST_PAD2=<seconds>` (a second controller playing by itself from that many seconds
after the start, for co-op tests; `<seconds>:run` only holds its stick forward, `:until<seconds>` unplugs it
then, e.g. `9:until30`; `:shoot` makes its attacks heavy attacks; `:start<s>` presses player 1's Start then, `:pick<s>` Up then A (the pause entry above the first), `:a<s>`, `:b<s>` and `:down<s>` press A, B or Down
then, to walk the menus), `SWROTS_COMMANDS="<seconds>:<command>;..."` (runs debug console
commands that many seconds after the start, e.g. `"9:player IVader;12:spawn ICloneTrooper enemy;15:despawn"`;
`screenshot <name>` among them saves what the game shows then to `screenshots\<name>.png`),
`SWROTS_BACKGROUND=1` (starts the game minimised and silent without taking the focus, so a test run
leaves the desktop alone; it keeps drawing, and the screenshots work).

## Finding your way around

Read [how it works](docs/architecture.md) first. In short:

| Folder | What |
|---|---|
| `src/loader/` | `swrots.exe`: reserves the game's address range, loads `core.dll` |
| `src/core/` | Startup, installer, window, settings, logging, crash reports |
| `src/kernel/` | The Xbox kernel functions the game imports, on Windows; the in-process reboot |
| `src/xapi/` | Hooks on the game's statically linked Xbox libraries; traps on everything not replaced |
| `src/d3d/` | The Xbox Direct3D 8 layer on Direct3D 9, shader translation, the flight recorder |
| `src/audio/` | DirectSound on XAudio2 |
| `src/input/` | Controllers, keyboard and mouse |
| `src/game/` | Facts about this one executable (`game.h`), mod loading, developer options, game-bug guards |
| `src/debug/` | The debug menu and the bridge to the game's console |
| `tools/` | Symbol reconstruction, engine variable extraction (run on your own copy of the game) |
| `docs/` | Player, modder and research documentation |

Every game address lives in `src/game/game.h` or next to the code that patches it, with a comment on
what is there. The port supports exactly one executable (MD5 `6460ef37862de3af364a0563a0a18c4e`),
so addresses are constants.

### Symbols

`tools/symbols/` builds `build\bin\symbols\swrots.map`, a reconstructed symbol table for the game
(class names, methods, source files), from your own copy of the game and optionally Indiana Jones and
the Emperor's Tomb. Crash reports name game functions from it. It is never committed. See
[tools/symbols/README.md](tools/symbols/README.md).

## Debugging

- `logs\swrots.log` has startup, file access, reboots, warnings and crash reports with named stack
  frames. `logs\swrots.previous.log` keeps the previous run.
- The engine's own warnings (discarded by the retail game) appear as `[engine] ...` lines.
- A crash report lists the faulting address, registers and the game functions on the stack.
- For hangs, `SWROTS_WATCHDOG` dumps all threads; for graphics glitches, the flight recorder saves
  thumbnails and every draw of the last frames.

## Submitting changes

1. Fork, create a branch, keep commits focused, and describe *why* in the commit message.
2. Build in Release and test what you changed in the game. Say in the pull request what you tested
   (level, settings, controller).
3. Run `python tools/check_repo.py`. CI runs it too: it rejects game or binary files, very large
   files, email addresses other than GitHub no-reply ones, and local user paths. Commit with your
   GitHub no-reply address (GitHub: Settings, Emails) if you want your email kept private.
4. Update the documentation that your change affects.
5. Open a pull request using the template.

## Where help is needed

- Playing through the whole game and reporting problems with logs.
- Testing on different GPUs, monitors (high DPI, ultrawide, multi-monitor) and controllers.
- PC button prompt artwork (original art: keyboard, mouse and PlayStation glyphs).
- Research into the engine: file formats, the game's systems, its developer features.
- Modding tools and guides.
