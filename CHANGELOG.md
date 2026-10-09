# Changelog

What changed in each release of SWROTS-PC, newest first. Downloads are on the
[releases page](https://github.com/jedijosh920/SWROTS-PC/releases).

## [v0.4.0](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.4.0) - 2026-10-09

Two-player co-op in the story missions.

### Added

- **Co-op.** A second player joins the story missions and the single-player bonus missions as soon as a
  controller is there for them, no button needed: they play the mission's companion (Obi-Wan beside
  Anakin), or a character of their own for player 1's side where there is none, with the game's own
  second HUD and player 1's health and Force. One controller is enough: the keyboard and mouse are then
  player 1. A shared camera keeps both in the picture; a player 2 left out of it, or falling into the void,
  is brought back to player 1, and a fatal hit brings them back instead of ending the mission (or, with
  `Player2Death=1`, ends it as player 1's death does). Turn it on and change it in the pause menu's new
  **Cooperative Mod** screen; more in `settings.ini` (`[Coop]`) and the debug console's `coop`. In the boss fights
  (Dooku, Grievous, the Mustafar duels...) player 2 plays the boss, as in Versus. Versus is unchanged. See the [co-op guide](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/coop.md),
  with its known issues.
- **Boss fights for two**: player 2 plays the mission's boss (`Boss=0` leaves it to the computer).
- **Friendly fire** for co-op (`FriendlyFire=1`): the players' blows hurt each other; they stay allies.
- **A hang watchdog**: if the game stops responding for 20 seconds, the log says where it is stuck.

### Fixed

- **The game restarted in a new window** when a mission ended right as a movie began (after the Mace
  Windu and Mustafar duels): returning to the menu could not stop the movie player, and the game restarted
  its process instead.
- **`despawn` in the debug console could crash the game** a moment later: clearing what pointed at the
  removed characters also cleared their own weapons' link back to them.

- **Log lines with folder names in other alphabets** (Cyrillic, for example) came out empty.

## [v0.3.1](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.3.1) - 2026-10-05

Characters keep their own sounds in other levels, the duel camera follows a character change again,
and bug reports' logs tell more.

### Added

- **`screenshot [name]`** in the debug console saves the game's picture, without the debug menu, to
  `screenshots\`.
- **More in the log, for bug reports**: `logs\swrots.log` now starts with the SWROTS-PC version, the system
  (Windows, or Wine/Proton and the Linux or macOS under it), the CPU, the GPU and its driver, and the sound
  output's channels and speaker layout. Every 30 seconds it notes the average frame rate, the slowest frame
  and how many frames took long enough to stutter.

### Fixed

- **Characters' sounds in levels they are not from.** A character played in or spawned into a level
  that does not have them now has their own sounds, taken from the levels that do: a Sith's saber
  swings and hits, Force lightning. Levels played as they come are unchanged. Vader's breathing is
  still missing after a live change into him in a level without Vader: it comes with a set of sounds
  the game prepares only while a level loads (a level started as Vader has it).
- **The duel camera after a character change.** Changing from the level's own character in a duel
  (Obi-Wan on Mustafar) left the camera where it was; it follows the new character again.

## [v0.3.0](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.3.0) - 2026-10-04

Play as anyone, spawn anyone, and a free camera.

All of these are in the debug menu (`[Debug] DebugMenu=1` in `settings.ini`, then ~ in game); see
the [debug menu guide](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/debug-menu.md).

### Added

- **Play story levels as any character.** Pick a class, a costume, a texture set (the 501st clones)
  and a body (any character model on the disc or under `mods\`) in the **Characters** tab, or with
  `player` in the console: Vader in Order 66, Anakin without his robe in the temple, Obi-Wan in a
  stormtrooper's body. The change happens **at once**, where you stand: the camera, controls, HUD
  portrait and health and Force bars follow the new character, and your health carries over as a
  share. **Back to normal** returns to the level's own character. When a change cannot be made at
  once, the mission restarts with the new character instead (`[Debug] AutoRestart=0`: from the next
  level start). Classes the game cut, such as Commander Cody, are refused.
- **Duels anywhere.** With the debug menu on, every level loads all of its characters' moves (the
  duel moves among them: blocks against a lightsaber, the clashes that lead to a saber lock), so a
  hero fights properly in levels that never had one (`[Debug] OptionalMoves`).
- **Spawn any character** in front of you, loaded from other levels if need be: up to 5 at a time
  from the **Spawn** area, or `spawn`. Pick their costume, texture set, body and size, and their
  side: **Ally** (fights the level's enemies with you), **Enemy**, **Neutral** (fights no one until
  hit, then riots) or **Riot** (attacks everyone). **Remove spawned** (`despawn`) clears them.
- **Free camera** (`freecam`, or the Game tab): fly the view with the mouse and keyboard or a
  controller, also with the game frozen; the game's camera shake and zoom stay off it.
- **Game tab**: your character's live health, Force and position; god mode, infinite Force,
  refill and maximum health; your **size** (0.25 to 4 times); **your own saber colour** (any colour,
  yours only, kept through power-ups and cutscenes); time scale, AI, HUD opacity, difficulty and
  the game's debug displays.
- Console: `player`, `variants`, `meshes`, `restart`, `autorestart`, `spawn`, `despawn`, `scale`,
  `saber`, `infiniteforce`, `memory`, `characters` (every character in the level with its side,
  health and target), and the research commands `peek`, `team` and `findrefs`.
- `duelist` puts any class in a versus select-screen slot.
- For contributors: `SWROTS_COMMANDS`, `SWROTS_TEST_INPUT` and `SWROTS_FREECAM` for unattended tests
  ([contributing](https://github.com/jedijosh920/SWROTS-PC/blob/main/CONTRIBUTING.md)); research
  notes on [characters](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/research/characters.md)
  and [the camera](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/research/camera-system.md).

### Fixed

- Crashes in the game's own code with characters in levels not made for them: a move the level did
  not load (now loaded, see above, or else skipped), and a Jedi brute with Anakin's fifth combo
  unlocked (an animation the game never had: a close one stands in).

### Known issues

- Playing as or spawning characters works in story levels only, not in Versus.
- A swapped or spawned character can appear at another spot for a moment before it is placed.
- Spawned characters do not follow you yet.
- Some missions expect their own character in cutscenes and scripted moments.
- At large sizes the camera stays where it is for a normal-sized character.

## [v0.2.1](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.2.1) - 2026-10-01

A fix for crashes after a level change on some graphics drivers.

### Fixed

- **Crash after a level change, a new versus match or a return to the menu** on some graphics
  drivers, e.g. Windows in Parallels Desktop on a Mac ("A critical graphics error has occurred")
  ([#2](https://github.com/jedijosh920/SWROTS-PC/issues/2),
  [#3](https://github.com/jedijosh920/SWROTS-PC/issues/3)). The game restarts itself for every level,
  creating its graphics device again; two caches of the port's (vertex layouts, used for movies and
  screen-space drawing) were never released, so they kept the old device alive and were handed to
  the new one. Most drivers tolerate that; stricter ones end the game. They are now released with
  their device.

### Added

- The log warns when a released graphics device is still in use (`Graphics device still referenced`).
- For contributors: `SWROTS_REBOOT_EVERY=<seconds>` restarts the game on a timer, to test restarts
  ([contributing](https://github.com/jedijosh920/SWROTS-PC/blob/main/CONTRIBUTING.md)).

## [v0.2.0](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.2.0) - 2026-09-30

Yoda joins Versus.

### Added

- **Playable Yoda in Versus.** His own cell on the select screen, after Random, with the name,
  title and bust the disc kept for him (a fighter the game planned but never finished). He is
  always unlocked, Random can pick him, and he fights in every arena with Anakin's intro and win
  cameras (the disc has none of his own). No settings or debug options needed.
- **Sith Yoda.** In Yoda against Yoda, player 2 is a dark Yoda with a red saber, as the game's own
  fighters get a different look against themselves. His darker textures are made from the normal
  ones when the duel loads; Yoda looks as always everywhere else.
- **Yoda blocks a duelist's blows with his own animation.** He was made for fighting clones, and
  fell back to Anakin's blocks, which left him floating. Some of his other reactions can still
  look off.
- **Characters a level never had are loaded from the rest of the disc**: their model, textures,
  animations and tables come from whichever level has them. This is how Yoda fights in the versus
  arenas.
- **`unlockprofile`** in the debug console: the developers' own cheat, left out of the retail
  game, unlocks everything in the signed-in profile (story, fighters, arenas, bonus missions,
  concept art). The game saves it with the profile, so back up `saves\` first to keep your
  progress.
- Docs for developers: [the versus roster](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/research/versus-roster.md)
  (how Yoda was added, with addresses) and
  [adding versus fighters](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/modding/adding-versus-fighters.md).

### Known issues

- The menus show Yoda's normal bust for player 2 in Yoda against Yoda, and he has no full-body
  picture on the select screen (the disc has none).

## [v0.1.1](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.1.1) - 2026-09-30

A fix for missing audio and frozen cutscenes.

### Fixed

- **No sound and frozen cutscenes after the first launch** on some systems
  ([#1](https://github.com/jedijosh920/SWROTS-PC/issues/1)). The game's audio needs a Windows
  component (COM) that the port relied on something else to set up. On the first launch the
  installer did, so sound worked; on later launches, on systems where nothing else did, audio
  failed to start (`XAudio2 initialisation failed (800401F0)` in the log). Cutscenes are timed by
  their sound, so they froze too, e.g. the opening "A long time ago..." of the first mission. The
  port now sets COM up itself before starting audio.
- **If audio cannot start at all** (no audio device, a broken driver), the game now plays on
  silently: sounds and movies keep time instead of freezing, and the log says
  `the game runs without sound`. Before, a half-started audio engine made every sound fail.
- A paused sound given a new play position no longer starts playing, as on the Xbox; a sound
  without a voice no longer risks a crash there.

### Added

- For contributors: `SWROTS_NO_AUDIO=1` runs the game as without an audio device, to test the
  silent path ([contributing](https://github.com/jedijosh920/SWROTS-PC/blob/main/CONTRIBUTING.md)).

## [v0.1.0](https://github.com/jedijosh920/SWROTS-PC/releases/tag/v0.1.0) - 2026-09-28

The first public release: the game boots, plays its movies and menus, and is playable through
level changes, restarts and saving.

### The port

- The game's own code runs natively as a 32-bit Windows program, with no emulation layer. The
  Xbox kernel, Direct3D (to Direct3D 9), audio (to XAudio2) and controller libraries are replaced
  by native Windows code.
- Level changes and mission restarts restart the game in the same window, as the console
  reboots into itself.
- First-run setup from your disc image (`.iso` / `.xiso`, or a `.7z` / `.zip` / `.rar` containing
  one): it checks that it is the supported North American release, copies the game files into
  `GameData\` and adds a Start menu shortcut (and optionally a desktop one).
- Saves in `saves\`, temporary files in `cache\`, a log in `logs\swrots.log` (the previous run's
  in `logs\swrots.previous.log`).

### Graphics

- Any window size, borderless fullscreen, optional VSync, and a picture scaled to fit the window
  at the right aspect ratio (or stretched, `Stretch=1`).
- 16:9 widescreen (the game's anamorphic mode) or 4:3.
- Higher internal resolution (`ResolutionScale`, automatic by default: 2x for 720p, 3x for 1080p
  and 1440p, 5x for 4K), anisotropic filtering up to 16x, the game's bloom on or off, sharp on
  high-DPI displays.
- 30 fps like the original; 60 fps experimental (`FpsLimit=60`).

### Input

- Keyboard and mouse, rebindable in `controls.ini`, with mouse sensitivity and walk speed.
- Xbox controllers (XInput) and PlayStation controllers (DualShock 4, DualSense) with rumble.

### Modding

- Mods as loose files in `mods\`: textures, models and other game files replace the packed
  copies without rebuilding the level archives; disc files (configs, movies, audio banks) too.
- Asset dumping (`DumpResources=1`) and resource logging (`LogResources=1`) to find the files a
  level loads and the paths a mod must use.
- Guides: [modding](https://github.com/jedijosh920/SWROTS-PC/blob/main/docs/modding/README.md), including a worked character swap.

### Developer tools

- A debug menu (`DebugMenu=1`, opened with ~): the game's own console, which the Xbox release had
  no window for, with its variables and commands, and switches for god mode, AI and the engine's
  hidden debug displays (fps counter, profiler, memory display).
- A flight recorder (the last 3 seconds of frames), Xbox library call tracing and file-open
  tracing, for bug reports and research.

### Known issues

- On-screen button prompts and some messages still refer to the Xbox and its controller.
- 60 fps is experimental: some of the game's logic is timed for 30 fps.
- The keyboard and the first controller are both player 1; two-player modes need two controllers.
- Only the North American release is supported.
