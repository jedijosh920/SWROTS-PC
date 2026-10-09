# Settings

Player settings live in `settings.ini` next to `swrots.exe`. The file is created
with defaults on first run. Unknown or out-of-range values are corrected when the game starts.
The game reads it at startup; in-game option menus will write the same file.

```ini
[Display]
Width=1280          ; window size (client area, in screen pixels; at least 640x360)
Height=720
Fullscreen=0        ; 1: borderless window covering the primary monitor
VSync=0             ; 1: also wait for the display's refresh when presenting
Stretch=0           ; 1: fill the window (no black bars, the picture may look squashed or stretched)

[Graphics]
ResolutionScale=0   ; internal resolution = 640x480 x this (1-8); 0 = auto (fits the window/screen)
Widescreen=1        ; 1: 16:9 (the game renders anamorphic widescreen); 0: 4:3 with side bars
Anisotropy=16       ; texture filtering quality, 1 (off) to 16
Bloom=1             ; the game's full-screen bloom; 0 turns it off

[Game]
FpsLimit=30         ; 30 like the original; 60 is experimental

[Coop]
Enabled=0           ; 1: a second player in the story and single-player missions (see below)
Input=0             ; 0 auto, 1 the keyboard is player 1 and the first controller player 2, 2 two controllers
Player2Death=0      ; when player 2 dies: 0 they come back beside player 1, 1 the mission is lost
StorySafety=1       ; 1: in cutscenes co-op leaves the characters and camera to the game
Camera=0            ; 0: a shared camera keeps both players in the picture; 1: it follows player 1 alone
FriendlyFire=0      ; 1: the two players' blows hurt each other (they stay allies)
Boss=1              ; 1: in boss fights player 2 plays the boss, as in Versus; 0: the computer does
Player2=            ; player 2's character where the mission has no companion (e.g. IAnakin); empty: automatic
```

Notes:

- `ResolutionScale` affects every 3D and 2D element, including the HUD and menus.
  Higher values need more video memory; if the game reports it cannot create the frame
  buffer, lower the value.
- The frame is scaled to fit the window at the chosen aspect ratio, with black bars where the
  window's shape differs (e.g. a maximized window, whose height excludes the title bar and taskbar).
  Resizing the window by its edges keeps the game's aspect ratio, so a resized window has no bars.
  `Stretch=1` fills any window instead.
- `ResolutionScale`: 1 is the original Xbox resolution; auto picks 2 for 720p, 3 for 1080p and 1440p,
  5 for 4K.
- `Bloom`: many levels add a blurred copy of the scene (about 35%) for a soft glow around bright
  areas, as on the Xbox. Set it to 0 for a sharper, flatter image. Lightsaber glows are part of the
  blades and are unaffected.
## Folders next to the game

| Folder | Contents |
|---|---|
| `saves\` | Save games. This is the Xbox save drive (E:); the game's saves are under `UDATA\4c410017\` |
| `cache\` | Temporary files (the Xbox utility drive, Z:). Safe to delete when the game is not running |
| `mods\` | Mod files (see [modding](modding/README.md)) |
| `GameData\` | The game files extracted from your disc (required) |

- `FpsLimit` overrides `fpsLimit` in the game's `vars_xbox.cfg`. The game was made for 30 fps; 60 is
  experimental: some of the game's logic is timed for 30 and can glitch for a frame at 60. Proper 60 fps
  support is planned.

## Co-op

Two players in the story missions: see the [co-op guide](coop.md) for how it works. The pause menu's
**Cooperative Mod** screen changes `Enabled`, `Boss`, `FriendlyFire`, `Player2Death` and `Camera` while
playing.

- `Input`: with auto, one controller means the keyboard and mouse are player 1 and the controller is
  player 2; with two or more controllers, the first is player 1's (with the keyboard) and the second
  player 2's. `1` always makes the keyboard player 1 and the first controller player 2; `2` always gives
  player 2 the second controller.
- `Player2Death`: with `0`, a hit that would kill player 2 sends them back beside player 1 with full
  health; with `1`, the mission is lost when player 2 dies, as when player 1 does.
- `Camera`: `1` leaves the camera to follow player 1 alone, as without co-op (player 2 is still brought
  back when out of the picture).
- `FriendlyFire`: `1` lets the players hurt each other; enemies and allies stay as they are.
- `Boss`: in boss fights (Dooku, Grievous, the Mustafar duels...) player 2 plays the boss with `1`; `0`
  leaves the bosses to the computer and those missions single-player.
- `StorySafety`: with `1` (recommended), while a cutscene plays and a moment after, player 2 is not
  brought back to player 1, the camera is the game's own, and player 2 does not join.
- `Player2`: a character class (e.g. `IVader`, `ICloneTrooper`) for player 2 where the mission has no
  companion; empty chooses one for player 1's side.
- The debug console's `coop` shows what co-op is doing and changes these settings while playing.

## Developer options

An optional `[Debug]` section in `settings.ini`:

| Key | Effect |
|---|---|
| `DebugMenu=1` | The debug menu (~): the game's console and developer switches, see [debug menu](debug-menu.md) |
| `MenuKey=~` | The key that opens the debug menu (`~`, `F1`-`F24`, `Insert`, a letter...) |
| `DebugDisplays=1` | Lets the engine draw its debug displays: the fps counter (`fps=true` in `mods\vars_xbox.cfg`), the frame profiler and the memory display |
| `AutoRestart=0` | Character changes in the debug menu (`player`) wait for the next level start instead of restarting the running mission |
| `OptionalMoves=0` | Levels load only the moves their own characters use, as on the console (the default without the debug menu). With the debug menu it is on: every level loads its characters' optional moves (the duel moves among them: blocks against a lightsaber, the clashes that lead to a saber lock, some specials), so a character played or spawned where the level does not expect it has its whole move set. About 2 MiB more memory |
| `FlightRecorder=1` | Keeps the last 3 seconds of frames; Ctrl+Shift+F10 saves them to `flight\` |
| `TraceSdk=1` | Logs the game's calls into Xbox libraries (verbose) |
| `LogResources=1` | Logs every resource the game loads, with the path a mod file must use |
| `TraceOpen=<text>` | Logs the game functions that open any file whose path contains `<text>` |
