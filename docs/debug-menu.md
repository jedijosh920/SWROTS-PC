# Debug menu and console

A developer overlay for testing and modding: the game's console, playing as and spawning any
character, and your character's data and quick helpers. Enable it in `settings.ini`:

```ini
[Debug]
DebugMenu=1
MenuKey=~         ; the key that opens it: ~ (default), F1-F24, Insert, Home, a letter...
DebugDisplays=1   ; lets the engine draw its fps counter, profiler and memory display
```

Press **~** (the key left of 1) in game to open or close it. While it is open the game gets no keyboard or mouse input
and the cursor is free; click into the game after closing it to take the mouse back.

## Console tab

The game's own console, whose window the Xbox release left out. Its colours are those of the
engine's debug windows in Indiana Jones and the Emperor's Tomb and Marc Ecko's Getting Up (same engine). Type a command and press Enter;
Up/Down recall earlier commands.

| Command | What it does |
|---|---|
| `help` | lists these commands and every command the game registered |
| `listvars [text]` | lists every variable (name, type, current value), or those whose name contains `text` |
| `get <variable>` | shows one variable |
| `set <variable> <value>` | changes a variable, e.g. `set timeScale 0.5`, `set god true` |
| `<variable>=<value>` | the same, in the `vars_xbox.cfg` form |
| `toggle <variable>` | flips an on/off variable |
| `duelist [<slot> <class>]` | lists the versus select screen's fighters, or puts a character class in a slot (see below) |
| `player [<class>\|- [<costume>] [skin <set>] [mesh <mesh>\|off]\|off]` | plays story levels as another character, costume, texture set or body (see below) |
| `saber [red\|green\|blue\|purple\|<r> <g> <b>\|off]` | the player's saber colour, its own only, at once (see below) |
| `spawn <class> [<costume>] [skin <set>] [mesh <mesh>] [ally\|enemy\|neutral\|riot] [scale <size>]` | a character in front of you, at once (see below) |
| `infiniteforce [on\|off]` | your Force stays full |
| `coop [on\|off \| input auto\|keyboard\|controllers \| death respawn\|gameover \| camera shared\|player1 \| friendlyfire on\|off \| boss on\|off \| player2 <class>\|auto \| storysafety on\|off]` | co-op: what player 2 is doing, and its settings (saved to `settings.ini`, see [settings](settings.md#co-op)) |
| `peek <hex offset> [count]` | research: your character's fields from that offset, as hex and as numbers, in the console and log |
| `team [spawned] [<hex teams>]` | research: your (or the last spawned character's) teams and AI data |
| `findrefs [spawned]` | research: where the game keeps pointers to your character (or the last spawned one), with the type of the object holding each, in the console and log |
| `memory` | the game's memory use and the characters spawned in this level (as the Game tab) |
| `scale [<size>] [spawned]` | your size (or the last spawned character's): `scale 2` a giant, `scale 0.5` half size, 1 your own |
| `characters` | every character in the level: side, AI controller, health, distance, target |
| `despawn` | removes the characters you spawned in this level (the Characters tab's Remove spawned) |
| `restart` | restarts the running mission (as the pause menu's Restart Mission does) |
| `autorestart [on\|off]` | whether a `player` change applies at once during a mission: live, or by restarting it (on by default) |
| `variants <class>` | lists a character class's costumes and texture sets |
| `meshes [text]` | lists the character bodies on the disc and under `mods\` (those containing the text) |
| `unlockprofile` | the game's developer cheat: unlocks everything in the signed-in profile (story, fighters, arenas, bonus missions, concept art); the game saves it with the profile, so back up `saves\` first to keep your progress |
| `freecam [on\|off]` | the free camera (see below); alone, turns it on or off |
| `screenshot [name]` | saves the game's picture as the window shows it, without the debug menu, to `screenshots\<name>.png` (no name: the date and time) |
| `clear` | empties the console (also the Clear button) |
| anything else | runs through the game's console: the game's own commands |

Variables take effect immediately. To set them at every start, put them in `mods\vars_xbox.cfg`
(`name=value`, one per line). The full list with descriptions is in
[engine variables](research/engine-variables.md).

### Versus fighters

Yoda has his own cell on the versus select screen, after Random, with his name, title and bust; he
fights like any fighter (he is always unlocked, and Random can pick him), with Anakin's intro and
win cameras (the disc has none of his own). He was made for fighting clones and droids, so he has
no reactions of his own for most of what a duelist does to him and uses Anakin's; the port gives him
his own block reaction for a duelist's blows (Anakin's blocks left him floating), wherever he plays.
Some reactions can still look off. In Yoda against Yoda, player 2 is a dark "Sith" Yoda
with a red saber, as the game's own fighters have a different look against themselves; the port
makes his darker textures from the normal ones. Yoda is otherwise untouched. He has no full-body
picture on the disc, so the arena screen shows his bust (both players' are the normal bust). The
port loads him from the Jedi Temple level's PAK only for duels he is in.

Besides Yoda, the select screen has the game's nine fighters, slots 0-8: `IAnakin`, `IObiwan`, `IDooku`, `IGrievous`,
`IMace`, `ISerra`, `ICinDrallig`, `IVader`, `IOldObiwan`. `duelist <slot> <class>` puts another
character class in a slot until the game is closed. For example, `duelist 0 IYoda`, then choose
Anakin in Versus: Yoda fights instead. The arenas do not contain Yoda; the port loads him from the
Jedi Temple level's PAK (see [how loading works](modding/how-loading-works.md#what-the-port-changes)).
The select screen still shows the slot's original portrait, and a character without duel cameras
of its own (the intro and win shots) uses those of the character whose slot it took. `duelist` refuses class names the game
does not know. A character in another's slot uses the first of its variants whose model is on the
disc (e.g. the battle droid's `hordeBattleDroid`), for both players; characters without a combat
moveset may not work as fighters.

### Playing as another character

Any story level can be played as another character, in any of its costumes, texture sets or in
another character's body. The **Characters tab** (below) does it with lists; the `player` command
does the same from the console. The choice lasts until the game is closed, and applies to every
level you play.

```
player <class> [<costume>] [skin <set>] [mesh <mesh>|off]
player - mesh <mesh>       each level's own character, in another body
player off                 back to each level's own character
player                     shows the current choice
```

| Example | What you get |
|---|---|
| `player IVader` | Vader, in Order 66 or anywhere else |
| `player IAnakin duel` | Anakin without robe or hood (his Mustafar outfit), also in Order 66, where he normally wears his cloak |
| `player ICloneTrooper horde skin 1` | a 501st horde trooper (blue markings) |
| `player IObiwan mesh anakinduel` | Obi-Wan's moves in Anakin's duel outfit |
| `player - mesh obi` | each level's own character, in Obi-Wan's body |
| `player IYoda` | Yoda (his blocks are his own; some other reactions borrow Anakin's) |

**When it applies.** During a mission, a change happens at once, where you stand: the new character
replaces yours as if the level had started with it (camera, controls, health and Force bars and HUD
portrait follow it; your old character is removed from the level, not hidden). Your health carries over as a
share of the new character's maximum (half stays half), and *Back to normal* gives the level's
character its own maximum back. The console prints
`changed at once`. When that cannot be done, it prints why and restarts the mission with the new
character instead, as the pause menu's Restart Mission does: from the mission's start, not your last
checkpoint. That happens when a spawned character's class already wore the body in this level (see
*Spawning*), or for a class without a body on the disc. `autorestart off` (or `[Debug] AutoRestart=0` in `settings.ini`) keeps
the mission running instead; the change then applies from the next level start or restart
(`restart`). Outside a mission (in the menus, or while a level loads) a change simply waits for the
next level.

**Classes.** A class is the character's behaviour: its moves, weapons, health and AI, e.g. `IAnakin`,
`IObiwan`, `ICloneTrooper`, `IBattleDroid`, `IYoda`. The Characters tab lists the classes that have
costumes; the game knows more (`player` refuses names it does not know). Characters a level does
not contain are loaded from other levels' PAKs, like versus fighters (see
[how loading works](modding/how-loading-works.md#what-the-port-changes)). Missions may expect their
own character in cutscenes and scripted moments, and some classes were never made to be played:
they may lack moves, or not respond to every control. A class with no body on the disc is refused:
`ICommanderCody` (only his portraits are left; his model, definition and moves were cut, and he
crashes the game whatever body he wears) and `IPoggle` (a static model, no skeleton).

**Costumes.** Each class has a list of costumes (the game's "Mesh Choice"); `variants <class>` lists
them by number and name:

```
> variants IAnakin
  IAnakin's costumes (player IAnakin <name or number>):
   0  Anakin                   Anakin\Anakin
   1  Anakin_Duel              AnakinDuel\AnakinDuel
   2  Anakin_Cloak             Anakincloak\Anakincloak
   3  Anakin_Hood_Down         AnakinHoodDown\AnakinHoodDown
   ...
```

A costume can be given by its number, its name, or a part of its name: the shortest name containing
it wins, so `duel` is `Anakin_Duel` rather than `Anakin_NPC_Duel`. Without one, the player wears the
costume the level picks (Anakin's cloak in Order 66). Some costumes the game lists were cut and their
models are not on the disc (Sidious, Emperor, Luke, Commander Cody, dirty Obi-Wan, the male padawan;
marked in the list); those fall back to the usual costume. A class whose usual costume was cut gets
the first one that is on the disc (the battle droid's `hordeBattleDroid`).

**Texture sets (skins).** Some classes have alternative textures, which the levels pick for each
character they place (the game's "Starting texture set"). The clone trooper's first set is the
**501st**'s blue markings of the Jedi Temple levels; its plain textures are the 212th's orange of
Utapau. `skin 1` (or `skin var01`) picks the first set, `skin 0` the plain textures; without `skin`
the plain textures are used. `variants <class>` lists a class's sets. Only first sets have textures
on the disc:

| Class | Set 1 (`_var01`) textures on the disc |
|---|---|
| `ICloneTrooper` | horde trooper (501st), sniper (`cloneSniperNewMesh`) |
| `IBlazeTrooper` | sky trooper |
| `IBattleDroid` | horde battle droid |
| `IGrappleDroid` | grapple droid |

The stormtrooper, buzz droid, clone walker, flying battle droid and Commander Cody list sets too, and
the clone and stormtrooper a second one, but no textures for them are on the disc: those look
plain. A set's textures are loaded from whichever level has them (a 501st trooper works on Utapau).

**Meshes (bodies).** `mesh <mesh>` dresses the player in any character body while keeping the class's
moves. `meshes` lists them (`meshes obi`: those containing "obi"); a mesh is named by its folder,
its file or `folder\file` (`obi`, `obi\obi`). Only bodies are offered: the disc's other character
meshes (dismembered limbs, debris, vehicles, the lightning effect) crash the game as a body and are
refused. A body made for one character has its skeleton's animations bound for that character; on
another class the game rebuilds that binding (into `cache\disc\`, safe to delete), and the few
animations no character has (a cut part of Anakin's force jump) borrow a neighbouring one. You wear
your own copy of another class's body, so the level's characters in that body (the clones, when you
wear the clone trooper body) keep their own animations. Bodies
shaped like the class's look right (any human on a Jedi); others may stretch or twist (C-3PO on
Anakin works, but moves like Anakin). `mesh off` goes back to the costume's own body.

**Your own meshes.** Put a mesh with its textures under `mods\meshes\chars\<folder>\` and it appears in
`meshes` and the Characters tab; name it `<folder>` or `<folder>\<file>`. A mesh without its textures
is drawn plain green. See [swapping characters](modding/swapping-characters.md) for how a mesh
names its textures.

**Saber colour.** `saber red` (or `green`, `blue`, `purple`, or any colour as `saber 255 128 0`) gives
the player's saber its own colour at once, without a restart; other characters keep theirs, and
power-ups no longer change it, nor do the scripted parts of a level that colour the sabers themselves (the
Mustafar duel's cutscenes and saber locks). It lasts until the game is closed (`saber off` gives the game's colours
back from the next level start). The four names are the game's own tuned colours. Only characters
with sabers (the Jedi-like classes) take it. The game's own `sabercolor` (below) is different: it
redefines a colour for every saber that has it, so `sabercolor blue 255 0 0` turns every blue saber
red, and the power-up's brighter blade (which is not one of the four colours) shows the original
colour while it lasts.

**Spawning.** `spawn <class> [<costume>] [skin <set>] [mesh <mesh>]` puts a character in front of
you, facing you, in the running mission at once: `spawn IVader`, `spawn ICloneTrooper horde skin 1`
(a 501st trooper), `spawn IObiwan duel`, `spawn IAnakin mesh stormtrooper` (Anakin's moves in a
stormtrooper's body, worn as its own copy, so the level's characters in that body keep theirs). It is loaded from another level if this one lacks it (a short pause
the first time), and fights for its class's side with its own AI (clones and droids attack Jedi, Jedi
fight clones), or for the side you give it: `spawn IBattleDroid ally` fights for you, `spawn
ICloneTrooper enemy` against you. A side is the game's own: an ally fights the level's enemies and
they fight it, an enemy fights you and your allies. `neutral` attacks no one and no one attacks it,
until it is hit: then it riots. `riot` attacks everyone, and everyone attacks it. `scale <size>` makes it bigger or smaller (`spawn IVader scale 3`). `characters` lists every character in the level with its side, AI controller, health, distance
and what it is attacking. Spawned characters stay until the mission restarts or the level changes, or until
`despawn` removes them (those still in the level, the dead ones too if their bodies remain). Tested with
100 at once (about 7 MiB more memory, no errors); large crowds lower the frame rate. A spawn
whose costume's body another class wore earlier in this
level as its own costume is refused: a body's animation binding is built for the classes in it and
stays for the level. Bodies you wear as another class are your own copies, so they never get in the
way.

**Every move, in every level.** A level normally loads only the moves its own characters use: in a
level without lightsaber duels, a hero you spawn (and your own Jedi against him) would lack the duel
moves: blocks against a lightsaber, the clashes that lead to a saber lock, some specials. With the
debug menu on, every level loads all of its characters' optional moves (`[Debug] OptionalMoves`, see
[settings](settings.md)), so duels look and play as in the duel levels anywhere.

**Versus.** `player` and `spawn` are for story levels: Versus creates its fighters its own way, so
there they do nothing (a change waits for the next story level). Use `duelist` (above) to choose the
fighters.

**If something goes wrong.**

- *Nothing happens after Apply or `player`*: look at the lines the command printed (under the
  Characters tab's buttons, or in the Console tab). An unknown class, costume or set is refused there.
- *The game freezes or crashes on a combination*: close it; the choice is not saved, so the next start
  is normal. A crash leaves `logs\swrots.log` with the details (and `logs\Message.log`, the game's
  own error report). If a restart cannot stop the game, the port starts the game process again,
  which takes longer but keeps your choice.
- *Plain green body*: the mesh's textures are missing (your own mesh without its `.stx` files).
- *The HUD portrait is the missing-texture pattern or another character's*: the game has portraits
  for twelve characters only (Anakin, Obi-Wan, Dooku, Grievous, Mace, Serra, Cin Drallig, Vader, old
  Obi-Wan, Yoda, Cody and the bodyguard); the port loads them from another level if need be.

How it works, with addresses: [characters](research/characters.md).

### Free camera

`freecam` detaches the view from the game's camera, starting where it was, and lets you fly it
anywhere; the player stands still (your input goes to the camera), and the game keeps running as
it was. The game's camera effects -- the shake of a hit, the zoom of a fight -- are kept off the
flown view; outside the free camera they are as always. Close the debug menu to fly. `freecam` again
(or `freecam off`) gives the view and the controls back. It is off again after a level change or
restart. How it works: [the camera system](research/camera-system.md).

| | Keyboard and mouse | Controller |
|---|---|---|
| Look | Mouse | Right stick |
| Move | W A S D | Left stick |
| Up / down | E or Space / Q or Ctrl | RB / LB |
| Faster / slower while held | Shift / Alt | RT / LT |
| Base speed | Mouse wheel | D-pad up / down |

For screenshots, freeze the game and hide the HUD first: `set timeScale 0` and `set hud 0` (and
`set timeScale 1`, `set hud 1` afterwards). The camera flies on real time, so it moves with the game
frozen. `hud 0` hides the HUD but not the on-screen tips. `screenshot` then saves the picture to
`screenshots\` without the debug menu in it.

Some of the game's commands:

- `sabercolor <blue|green|red|purple> <r> <g> <b>` redefines a blade colour (0-255 each), e.g.
  `sabercolor blue 255 128 0` makes blue blades orange.
- `setbind`, `addbind`: controller bindings, as in `binds_xbox.cfg`.

Not everything works: the Xbox release removed a lot of the developer-only code, so some variables
(debug drawing such as `saberdebug`) and commands exist but do nothing.

## Characters tab

Two areas: **Play as** (who you are) and **Spawn** (characters you add to the mission).

**Play as** is the `player` command with lists, for picking a character without typing names.

- **Class**: the classes that have costumes, with a filter box; greyed out *(cut)* ones cannot be
  played (see above). *Default* keeps each level's own character. *All classes* lists every class the
  game registers; most of those are not characters (props, weapons, effects).
- **Costume**: the picked class's costumes, numbered as in `variants`; greyed out ones were cut and
  are not on the disc, and hovering one shows its mesh. *Default* is the costume the level picks.
  Below them, the class's **texture sets**, if it has any: *Default* (the plain textures), then the
  sets (`_var01` is the clone trooper's 501st).
- **Mesh**: every character body on the disc and under `mods\`, with a filter box. *Default* keeps
  the costume's body.

The buttons:

- **Apply** runs the `player` command for the picks (shown in grey under the buttons, so you can
  type it next time). During a mission your character changes at once, where you stand (or, when
  that cannot be done, the mission restarts with it: see *When it applies* above).
- **Back to normal** runs `player off`.
- **Restart mission** restarts the running mission (greyed out outside a mission).
- **Apply at once** is `autorestart`: off, Apply only sets the character for the next level start.

What a command printed (the choice, or why it was refused) is in the Console tab.

**Spawn** uses the Play-as picks above (class, costume, skin and body), with **How many** (1 to 5), **Side** (*Default*: its class's own; *Ally*, *Enemy*, *Neutral*, *Riot*: as
`spawn ... ally|enemy|neutral|riot`), **Size** and **Spawn** (`spawn`, once per character). **Remove spawned** (`despawn`) removes the
characters you spawned in this level; their count is shown beside it.

## Game tab

Your character's live data and quick helpers, the game's own variables (the ones `set` changes) and
switches, in one place.

- **Player**: your class and costume, position and facing, health and maximum health, and Force
  and maximum Force (Jedi-like characters), read from your character as you play (outside a mission: none; `player`
  in the console prints the same). **God mode**, **Infinite Force** (`infiniteforce`: your Force stays full; the game
  has no switch for it), **Refill health**, **Max health** (100 to 1000, filled
  at once: a clone or droid has 50, a Jedi 1000; until the mission restarts), **Size** (`scale`: 0.25 to
  4 times your own, kept when you change character), buttons setting the
  **Force level**, **combat skill** and **Force power level** (0 to 3: they unlock moves and powers; the
  game's variables only set them, so the current level is not shown), and **your saber** colour
  (`saber`: the game's, red, green, blue, purple, or any colour; yours only, at once).
- **World**: the **time scale** (slow motion below 1, with 1/4, 1/2 and 1x buttons), **AI disabled**
  (characters stop acting), the **HUD**'s opacity (0 hides it, for clean screenshots) and the
  **difficulty**.
- **Camera**: the **free camera** (`freecam`); close the menu to fly.
- **Debug displays**: the engine's own displays (fps counter, frame profiler and memory display) and
  its fps counter, then a **memory** line: the Xbox memory the game uses (of 128 MiB the port gives
  it), other memory it allocated, and the characters spawned in this level.

## How it works (for contributors)

`src/debug/console.cpp`, `src/debug/menu.cpp`; addresses in `src/game/game.h`.

- The engine console object (console.cpp in the game, vtable 0x55FF58) is created at startup and kept at
  `[[0x66F7A4]+0x48]`. Its `Execute(line, echo)` (slot +0x04) splits the line, looks the first word up
  in the command table and runs the command's handler.
- The Xbox build keeps the command handling but stubs out the rest:
  - The print methods (+0x50 engine string, +0x54 C string) do nothing; the port points them at its log.
  - The global console pointer `0x65E360` that the commands print through is never set; the port sets it.
  - `help` and `listvars` enumerate through stubbed table methods, and `set` calls a stubbed registry
    method (+0x08). The port implements these itself from the registry's working parts: find (+0x04),
    set (+0x1C, as `vars_xbox.cfg` uses) and the variable objects' Get / TypeName methods.
- Console tables (variables at console +0x1C, commands at +0x2B4) are hash tables of 37 buckets of
  `{value, name, next}` nodes.
- Commands are queued by the menu and run on the game thread after a frame is presented.
- New port commands go in `RunPortCommand` (`src/debug/console.cpp`).
- The character commands (`player`, `variants`, `meshes`, `restart`, `autorestart`, `duelist`) need no
  engine console object, so they also run while it does not exist (in the menus' early life, during
  loading). The Characters tab builds `player` commands and queues them like typed ones.
