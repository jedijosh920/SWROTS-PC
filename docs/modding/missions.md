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

[Cast]
; name=Class, x z heading, side[, costume]
master=ICinDrallig, -150 -300 0, ally
maul=IMaul, 0 300 180, enemy
```

The cast are placed in the level before it loads, so they come with their classes' sounds, as Heroes vs Villains'
fighters do (characters spawned later by the script have none). The script finds them with `cast("<name>")`. A side
is `ally` (on the player's side), `enemy`, `neutral` (attacks no one until hit) or `riot` (everyone's enemy); left
out, the class's own.

Coordinates are the map's own (the ones its `map.ini` uses; on the developer map, units from the middle of the floor),
`x` and `z` across the floor, heights (`y`) from the floor up. A character is about 180 units tall.

## lines.ini

```ini
[Lines]
qg_mindful=QUI-GON|Be mindful, Obi-Wan.
obi_handle=OBI-WAN|We'll handle this.

[Seconds]
qg_mindful=2.5          ; how long the line shows (optional: from its length)

[Voice]
qg_mindful=qg_mindful.wav   ; its voice file in the mission's folder (reserved: voices are not played yet)
```

`say("qg_mindful")` shows the line as a subtitle (the speaker's name above it) and waits until it is over. Dialogue is
written once here and used by id, so voices can be added later without touching the script.

## mission.lua

The script defines `main()`, which the port runs once the level is ready. Waiting functions (`wait`, `wait_until`,
`say`, ...) pause the script while the game goes on; everything else returns at once.

```lua
function main()
  local maul = cast("maul")
  cutscene(function()                 -- bars, everyone holds still, the player can skip it
    camera_orbit(maul, 300, 120, -40, 40, 4)
    title("DUEL OF THE FATES", "Theed, Naboo", 3)
    play(maul, "Maul_Intro")
    wait(4)
    say("qg_mindful")
  end)
  objective("Defeat Darth Maul")
  wait_until(function() return not alive(maul) end)
  complete("The Sith is defeated")
end
```

A mode is the same: a survival mode, for example, is a loop that spawns waves, counts them with `counter()` and ends
with `fail()` when the player falls:

```lua
function main()
  objective("Survive")
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
| `on_player_defeated()` | define it to decide what the player's defeat does (default: `fail("You were defeated")`); the player is kept standing with 1 health while it runs |
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
| `place(c, x, z [, heading])` | moves it there |
| `face(c, other)` | turns it toward another |
| `side(c, "ally" / "enemy" / "neutral" / "riot")` | changes its side |
| `ai(c, on)` | its AI on or off (off: it stands still) |
| `attack(c, target)` | its AI goes after that target |
| `hold(on)` | everyone holds still and the player's controls are held (as in cutscenes) |
| `kill(c)` | defeats it at once |
| `set_player(c)` | the player becomes that character where it stands (its class and health); the character itself leaves |
| `class_of(c)` | its class's name |

### Moves

| Function | |
|---|---|
| `play(c, move [, speed])` | plays one of its moves by name: intros (`<Name>_Intro`), idles (`Idle_Flavor_v1`), falls (`Death`), reactions; false when it has none of that name |
| `moves(c)` | the names of its moves, as a table (`log(table.concat(moves(c), ","))` lists them) |

### Camera

Heights are from the floor; `look` is the height on the character the camera looks at (default 90, the chest).

| Function | |
|---|---|
| `camera_orbit(c, radius, height, from, to, seconds [, look])` | circles the character from `from` to `to` degrees (0: in front of it) |
| `camera_move(c, x1, y1, z1 [, x2, y2, z2, seconds, look])` | from one place by the character to another, in its own frame at the start: `x` to its right, `y` up, `z` ahead of it |
| `camera_track(x, y, z, c [, look])` | stands at a point of the map and follows the character |
| `camera_fixed(x, y, z, tx, ty, tz)` | stands at a point of the map and looks at another |
| `camera_game()` | the game's own camera again (cutscenes end with it) |

### Screen

| Function | |
|---|---|
| `cutscene(fn)` | runs `fn` as a cutscene: bars, the game's HUD hidden, everyone held; the player skips it with A or Start (its `wait`s and `say`s then end at once; `wait_until` does not) |
| `letterbox(on)` | the cutscene bars on their own |
| `fade_out(seconds)` / `fade_in(seconds)` | to black and back |
| `say(id)` | a line of `lines.ini`, waiting until it is over |
| `speak(speaker, text [, seconds])` | a line written in the script, waiting until it is over |
| `subtitle(speaker, text [, seconds])` / `line(id)` | the same without waiting (they return the line's length) |
| `objective(text)` | the objective, top left (empty: none) |
| `title(text [, smaller, seconds])` | a big title in the middle |
| `notice(text [, seconds])` | a notice, upper middle |
| `timer(label [, seconds])` | a timer at the top: counting down from `seconds`, or up |
| `timer_value()` / `timer_stop([hide])` | its seconds; stops it (and hides it) |
| `counter(label, value)` / `counter(label)` | a counter, top right (e.g. `counter("WAVE", 3)`); without a value: removed |
| `hud(on)` | the game's own HUD (health, Force) shown or hidden |
| `skipping()` | true while a cutscene is being skipped |

### The game

| Function | |
|---|---|
| `set_var(name, value)` | sets a game variable, as the console's `set` (e.g. `set_var("timeScale", 0.5)` for slow motion) |
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
screen is drawn in the game's font (`src/debug/missionhud.cpp`).
