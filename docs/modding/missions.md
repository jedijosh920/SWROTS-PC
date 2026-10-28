# Missions and modes

Missions, bonus missions, game modes and scripted scenes of your own, written as a small text script in
[Lua](https://www.lua.org/manual/5.4/) and shared as one folder. A mission plays on the developer map or on a
[custom map](custom-maps.md), with the game's characters or [custom characters](custom-characters.md), and can have
cutscenes, dialogue, objectives, timers and its own ending.

The game's own missions are not written this way: their levels wire the game's objects together (trigger volumes,
cinematic players, objective markers) in a binary level file made with the developers' editor, and their cutscenes
are pre-made cinematic files. A mission of a mod's own runs on the port's script instead, with functions that use the
game's own pieces where they can (its characters, moves, camera, HUD font, death and mission flow).

## Installing and playing one

Copy the mission's folder into `mods\missions\`, next to `swrots.exe`:

```
mods\missions\<id>\mission.ini     its name, map, player and cast
mods\missions\<id>\mission.lua     its script
mods\missions\<id>\lines.ini       its dialogue (optional)
```

Start it from the debug console with `mission <id>` (`mission` alone lists the installed ones). A mission that needs a
custom map or custom characters needs those installed too.

## mission.ini

```ini
[Mission]
Name=Duel of the Fates           ; as shown
Description=Obi-Wan and a Jedi Master against Darth Maul
Map=                             ; a custom map's id (mods\maps\<id>); empty or left out: the developer map's floor
Player=IObiwan                   ; the player's character (a class: the game's or a custom character's)
PlayerAt=0 -300 0                ; where the player starts: x z (the map's coordinates) and the heading (degrees)
Music=mus_ingamebattle28_lp      ; its music (optional): one of the game's music slots; mus_<slot>.hwx in the
                                 ; mission's folder plays in its place (see replacing-music.md); else the map's

[Cast]
; name=Class, x z heading, side[, costume]
master=ICinDrallig, -150 -300 0, ally
maul=IMaul, 0 300 180, enemy
```

The objectives are the game's own (the HUD's "NEW OBJECTIVE:", the pause screen's Objectives), named in
`[Objectives]` as `name=text` (at most 49):

```ini
[Objectives]
maul=Defeat Darth Maul
survive=Survive the waves
```

Tips are the game's own too (its tip box, shown after an objective's banner), in `[Tips]` as `name=text`, with
`| seconds` for how long it shows (default 6):

```ini
[Tips]
block=Hold the block button to deflect Maul's attacks | 6
```

Places characters can be sent to (see `walk` below) are named in `[Markers]`, as `name=x z`:

```ini
[Markers]
gate=0 120
pit=0 600
```

Force fields (the game's own laser door field from the Jedi Temple: drawn from both sides, it blocks characters while
on) are named in `[Fields]`, as `name=x z heading [scale] [off]`: at heading 0 the field stands across the z axis
(closing the way along z), 50 units wide and 100 high at scale 1 (2.75 is a wide door); `off` makes it start switched
off. `field(name, on)` switches it (below).

```ini
[Fields]
gate=0 0 0 2.75
```

Animations of other characters a character plays (`play_anim`, below) are named in `[Animations]`, as
`<cast name, player or class>=<animation>, <animation>...`, the animations' file names without `.bnm` (any character's,
from any level: the port loads them with the mission):

```ini
[Animations]
maul=anakin_atk_kill_lunge_humangb
quigon=dooku_death
```

A character's own animations need no entry. The animations go to the character's class (its base's, for a custom
character): every character of that class can play them.

The cast are placed in the level before it loads, so they come with their classes' sounds, as Heroes vs Villains'
fighters do (characters spawned later by the script have none). The script finds them with `cast("<name>")`. A side
is `ally` (on the player's side), `enemy`, `neutral` (attacks no one until hit) or `riot` (everyone's enemy); left
out, the class's own.

Coordinates are the map's own (the ones its `map.ini` uses; on the developer map, units from the middle of the floor),
`x` and `z` across the floor, heights (`y`) from the floor up. A character is about 90 units tall (its chest about 50, its eyes about 75).

## lines.ini

```ini
[Lines]
qg_mindful=quigon|Be mindful of your feelings, Obi-Wan.   ; speaker|text
obi_handle=player|We'll handle this.

[Seconds]
qg_mindful=2.5          ; how long the line lasts (optional: its voice's length, else from the text's)

[Voice]
obi_handle=obi_handle.wav   ; its voice: a WAV in the mission's folder (16-bit PCM; any rate, mono or stereo)
```

A line's speaker is a character of the cast (by its `[Cast]` name) or `player`. The line is then the game's own
dialogue: `say("qg_mindful")` has that character say it, with the game's subtitle (its speaker name and the text,
"QUI-GON: Be mindful...", shown with the game's subtitles on: a mission turns them on while it runs) and its voice in
the game's mix (the port adds the mission's lines to the game's dialogue archive, the voices made into its format in
`cache\voice\missions\`). A line without a voice is silent and shows for as long as `[Seconds]` (or its text) says.
During a cutscene the characters say nothing but the mission's lines. A speaker with no lines of its own in the level
(Palpatine's are his own levels') says it through another character's dialogue, under its own name. A line whose speaker is no character (any other
name) shows as the port's own subtitle. `say(id, c)` has another character say it.

## mission.lua

The script defines `main()`, which the port runs once the level is ready. Waiting functions (`wait`, `wait_until`,
`say`, ...) pause the script while the game goes on; everything else returns at once.

```lua
function main()
  local maul = cast("maul")
  cutscene(function()                 -- bars, everyone holds still, the player can skip it
    camera_orbit(maul, 200, 55, -40, 40, 4)
    title("DUEL OF THE FATES", "Theed, Naboo", 3)
    play(maul, "Maul_Intro")
    wait(4)
    say("qg_mindful")
  end)
  objective("maul")
  wait_until(function() return not alive(maul) end)
  complete("The Sith is defeated")
end
```

A mode is the same: a survival mode, for example, is a loop that spawns waves, counts them with `counter()` and ends
with `fail()` when the player falls:

```lua
function main()
  objective("survive")
  timer("TIME")                                   -- counts up
  for wave = 1, 10 do
    counter("WAVE", wave)
    local enemies = {}
    for i = 1, wave + 1 do
      enemies[i] = spawn("ICloneTrooper", math.random(-600, 600), math.random(-600, 600), 0, "enemy")
    end
    wait_until(function()
      for _, e in ipairs(enemies) do if alive(e) then return false end end
      return true
    end)
    notice("WAVE " .. wave .. " CLEARED", 3)
    wait(3)
  end
  complete("All waves survived in " .. math.floor(timer_value()) .. " seconds")
end
```

## The functions

Characters are handles (numbers) from `player()`, `cast()` or `spawn()`. A handle of a character that has left the
level is safe to use: functions on it do nothing, `alive()` and `exists()` are false.

### Flow

| Function | |
|---|---|
| `wait(seconds)` | pauses the script (the game's time: a pause of the game pauses it) |
| `wait_until(fn [, timeout])` | pauses until `fn()` returns true; false if the timeout (seconds) came first |
| `thread(fn)` | runs `fn` alongside `main` (e.g. a timer's check, a second objective) |
| `time()` | the game's time in seconds (stops while the game is paused) |
| `paused()` | true while the game is paused |
| `complete([text])` | the mission is complete: the end shows, then the main menu |
| `fail([text])` | the mission is failed: the end shows, then the mission starts again |
| `on_player_defeated()` | define it to decide what the player's defeat does (it runs once, the player kept standing with 1 health); without it the player's defeat is the game's own (its Game Over screen: Restart or Quit) |
| `log(text)` | writes to the game's log (`logs\swrots.log`), for testing |

### Characters

| Function | |
|---|---|
| `player()` | the player's character |
| `cast(name)` | a character of `mission.ini`'s `[Cast]` |
| `spawn(class, x, z [, heading, side, costume])` | a new character; returns it (nil when it cannot) |
| `remove(c)` | takes a character away |
| `alive(c)`, `exists(c)` | standing (health above 0); in the level |
| `health(c)` / `set_health(c, f)` | its health as a fraction of its most (0 to 1) |
| `max_health(c [, value])` | its most health (and sets it) |
| `invincible(c, on)` | no damage while on |
| `position(c)` | x, z, height |
| `distance(a, b)` | between two characters, across the floor |
| `place(c, x, z [, heading, height])` | moves it there; with a height above the floor it falls from there (below it, it falls away: a pit) |
| `face(c, other)` | turns it toward another |
| `side(c, "ally" / "enemy" / "neutral" / "riot")` | changes its side |
| `ai(c, on)` | its AI on or off (off: it stands still) |
| `attack(c, target)` | its AI goes after that target |
| `walk(c, marker [, timeout])` | it runs to a marker of `[Markers]` its own way (as the game's AI goes to places) and the script waits until it is there (true) or `timeout` seconds (default 20) passed (false); works in cutscenes too, and for characters whose AI is off (actors): their AI is on for the way, off again once there |
| `go(c, marker [, fight])` / `going(c, marker)` | the same without waiting (with `fight`, it fights what it meets on the way); whether it is still on its way |
| `marker(name)` | a marker's x, z |
| `field(name, on)` | a force field of `[Fields]` on (it lights up and blocks) or off (it fades away and lets through); a character the field stopped gives up its `go`/`walk`: send it again |
| `hold(on)` | everyone holds still and the player's controls are held (as in cutscenes) |
| `kill(c)` | defeats it at once |
| `boss(c)` | it is the mission's boss, as a story duel's: its health bar with its portrait on the HUD (the game's player 2); `boss(nil)`: none |
| `set_player(c)` | the player becomes that character where it stands (its class and health); the character itself leaves, and the character the player was stays where it stood, on the player's side: returns true and that character (e.g. `local _, master = set_player(padawan)`) |
| `class_of(c)` | its class's name |

### Moves

| Function | |
|---|---|
| `play(c, move [, speed])` | plays one of its moves by name: intros (`<Name>_Intro`), idles (`Idle_Flavor_v1`), falls (`Death`), reactions; false when it has none of that name |
| `play_anim(c, animation [, options])` | plays an animation: one of its moves (by its name or its animation's), or one of `[Animations]`. Options: `from` / `to` (the animation's own frame numbers, 60 a second; left out, its first / last), `loop` (`from`..`to` again and again until `stop_anim`), `hold` (stops on `to` and keeps that pose, e.g. a body that stays down), `speed` (1 normal; 0 holds the pose at `from`), `blend` (seconds to blend in from the pose it was in; default 0, at once). Neither `loop` nor `hold`: it plays to `to` (or its end) and the character carries on as before. A hit, its death or its own AI can still end it, except on an actor (`actor`). False when it has no such animation |
| `actor(c [, on])` | makes it a cutscene actor (`actor(c, false)`: as it was): it does only what the script says (moves, walks, sabers, lines): its AI is off, no hit reaches it (no damage, no reaction, no block), no one attacks or targets it, and an animation `play_anim` gives it stays on (put back if anything puts it away). A body that never gets up: `actor(c)`, then `play_anim(c, "dooku_death", {hold = true})` |
| `keep_body(c [, on])` | when it dies, its body stays where it fell (the game's own "cinematic death": no fade, not taken away). With `actor(c)` once it has fallen, nothing wakes it |
| `stop_anim(c)` | ends `play_anim`'s loop or held pose: the character goes back to what it does (its idle) |
| `anim_frames(c)` | its current animation's frame, first and last frames and name (`log(anim_frames(c))`), to find `from` / `to` |
| `moves(c)` | the names of its moves, as a table (`log(table.concat(moves(c), ","))` lists them) |
| `saber(c, on [, seconds])` | its sabers lit or put away, as the game's cinematics do: the blade grows or shrinks over `seconds` (default 0.4) with the saber's sound; 0 at once, silent. A saber put away stays away (also while it fights) until `saber(c, true)`; false when it has none |
| `saber(c)` | whether one of its sabers is lit |

### Camera

Heights are from the floor; `look` is the height on the character the camera looks at (default 50, the chest). A cutscene uses a 35 degree lens (the levels' own is about 80): a close shot is a narrow lens from a little way off, e.g. `camera_fov(25)` and `camera_move(c, 20, 70, 150, 15, 68, 130, 2, 62)` for a head-and-shoulders shot.

| Function | |
|---|---|
| `camera_orbit(c, radius, height, from, to, seconds [, look])` | circles the character from `from` to `to` degrees (0: in front of it) |
| `camera_move(c, x1, y1, z1 [, x2, y2, z2, seconds, look])` | from one place by the character to another, in its own frame at the start: `x` to its right, `y` up, `z` ahead of it |
| `camera_track(x, y, z, c [, look])` | stands at a point of the map and follows the character |
| `camera_fixed(x, y, z, tx, ty, tz)` | stands at a point of the map and looks at another |
| `camera_fov(degrees)` | the lens (field of view) until `camera_game()`; without a number, the game's |
| `camera_game()` | the game's own camera and lens again (cutscenes end with it) |

### Screen

| Function | |
|---|---|
| `cutscene(fn)` | runs `fn` as a cutscene: bars, the game's HUD hidden, everyone held; the player skips it with A or Start (its `wait`s and `say`s then end at once; `wait_until` does not) |
| `letterbox(on [, seconds])` | the game's own cutscene bars slide in or out (0.5 s); while they are in, the game counts it a cutscene (its HUD hidden) |
| `tip(name)` | a tip of `[Tips]` shows |
| `fade_out(seconds)` / `fade_in(seconds)` | to black and back |
| `say(id [, c])` | a line of `lines.ini` said by its speaker (or `c`), waiting until it is over (and, while another line is being said, until that one ends) |
| `speak(speaker, text [, seconds])` | a line written in the script, waiting until it is over |
| `subtitle(speaker, text [, seconds])` / `line(id)` | the same without waiting (they return the line's length) |
| `objective(name)` | an objective of `[Objectives]` becomes the new one: the game's own "NEW OBJECTIVE:" and the pause screen's Objectives list |
| `objective_done(name)` | it is completed |
| `title(text [, smaller, seconds])` | a big title in the middle |
| `notice(text [, seconds])` | a notice, upper middle |
| `timer(label [, seconds])` | a timer at the top: counting down from `seconds`, or up |
| `timer_value()` / `timer_stop([hide])` | its seconds; stops it (and hides it) |
| `counter(label, value)` / `counter(label)` | a counter, top right (e.g. `counter("WAVE", 3)`); without a value: removed |
| `hud(on)` | the game's own HUD (health, Force) shown or hidden |
| `skipping()` | true while a cutscene is being skipped |

### Sound

| Function | |
|---|---|
| `sound(file [, volume])` | plays a WAV of the mission's folder (16-bit PCM, mono or stereo) over the game's sound; returns its length in seconds |
| `stop_sounds()` | stops the mission's sounds |
| `music_volume(f)` | the music at `f` times the player's own music volume (0 silent, 1 as set), e.g. under a dramatic moment; put back at the mission's end |

A line of `lines.ini` with a `[Voice]` file is said with it (see lines.ini above).

### The game

| Function | |
|---|---|
| `set_var(name, value)` | sets a game variable, as the console's `set` (e.g. `set_var("timeScale", 0.5)` for slow motion) |
| `get_var(name)` | a game variable's value (text), or nil |
| `command(text)` | runs a debug console command |

The script has Lua's base functions, `string`, `table`, `math` and `coroutine`; no files, no loading of other code.

## Testing a script

A mission is read when the game starts it, so a change takes effect at the next `mission <id>`. Errors in the script
show at the bottom of the screen and in `logs\swrots.log` (`Mission: script error: ...`); `log()` writes there too.

## How it works (for developers)

`src/game/missions.cpp`: the mission's folder is read at each start of the game image; a start with the launch
argument `mission:<id>` (on the developer map, `custommap:<id>` for its map) lays out the level (the cast placed as the
dev map's characters, `devmapbuild.h`), sets the player's class and start and loads the script. Once the level runs, its
cast is woken and `main` runs as a Lua 5.4 coroutine (`third_party/lua`), resumed once a frame while the game is not
paused; the prelude's waits are loops of yields. The player's fatal hit goes to the mission (coop.h's fatal-hit guard),
so the game never shows its own Game Over. Cameras are the port's director over the game's camera (freecam.h), the
screen is drawn in the game's font (`src/debug/missionhud.cpp`). `[Animations]` are sequences added to the classes' scripts (`classsetup.h`)
playing the animations, which the class finds in its own folder (`resources.cpp` declares them there); `play_anim`'s
ranges, loops and holds are kept by a hook on the character's animation tick (its `TCharState`, vtable 0x58EFB8 +0x24),
before the sequence's end is reached. `actor` marks script actors (`characters.h`): the hit reaction and the enemy
check leave them out (see docs/research/characters.md, "Hits, targets and the dead").
