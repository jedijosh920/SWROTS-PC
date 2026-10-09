# Two players in the story missions (co-op)

Research towards a second player in the campaign and in the single-player bonus missions: how the game
does two players where it already has them, and what a story mission needs to get them. The port's co-op (`[Coop]` in
settings.ini) is built on it. Addresses are the retail Xbox executable's (NTSC-U), as loaded.

## Where the game already has two players

Versus, and four bonus missions. The mission list (`gameinfo\missionlist.txt`, in every level PAK) names
the second player of those missions next to the first, and marks them as multiplayer:

```
U102_SEP_Cargo_Hold    "... Player:Anakin Player2:Obiwan ... UnlockedType:Multiplayer ..."
U105_SEP_Escort        "... Player:Anakin Player2:Obiwan ... UnlockedType:Multiplayer ..."
U106_UTA_Survival      "... Player:Obiwan Player2:Anakin ... UnlockedType:Multiplayer ..."
U109_COR_JediSurvival  "... Player:Serra  Player2:Cin    ... UnlockedType:Multiplayer ..."
```

The story missions have `Player:` only. The bonus mission menu looks for these four `Player2:` values in
the mission's parameters (0x2D32C0) and keeps the second player's class (`IAnakin`, `IObiwan`,
`ICinDrallig`, `ISerra`) beside the first's, the same two-name layout Versus uses for its fighters.

Started directly (`map:u109_cor_jedisurvival` in `mods\Default_Xbox.cfg`), the mission has one player:
the level creates Serra through the player spawn (0xB1F90, called once from 0x2AB660 with the `player`
setting) and places Cin Drallig as an AI ally ("stalk"), with or without a second controller connected.
The second player of a co-op bonus mission comes from the menu's flow, not from a connected controller.

## Handing a character to the second controller

A character is the player's when its control mode (+0x390) is 2 and a controller is bound to it with
0x150580 (thiscall, the controller's index; -1 unbinds). This is how the port's live character change
binds the new player to controller 0. Bound to controller 1, a character in a story mission is a second
player:

- First mission: Obi-Wan (a companion, control mode 16) given mode 2 and bound to controller 1 moves with
  the second controller's stick, while Anakin stays with controller 0. Without input on the second
  controller he stands still; the companion AI no longer moves him.
- Missions without a companion can get one from the port's spawn (an ally), bound the same way.

## The game manager's player slots

The game manager (`[0x7EB964]`) keeps the players: their number at +0x1E4 (at most 2) and their
instance ids in slots at +0x2A4 (player 1) and +0x2A8 (player 2). In the co-op bonus mission slot 2
holds Cin Drallig's id and the count is 2; in the first mission slot 2 is empty and the count is 1.

- 0x27AC40 (thiscall, character) adds a player: it binds the character to the next player's controller
  from the launch settings (`[0x7F33FC]+0x20 + slot*4`, 0x150580), stores its id in the next slot and
  counts it. This is what the co-op bonus missions' setup goes through.
- 0x27ABE0 (thiscall, character) tells whether a character is a player: its id is in slot 1 or 2.
- 0x27AB30 (thiscall, slot) returns the slot's character if the slot is below the count and the character
  is alive (+0x12C not set, health +0x130 above 0).

## Player 2's HUD

The HUD has three vitals objects (`HudVitals`, vtable 0x5A82B0) in every mission; the third one's item
names slot 2 (item +0x84). It finds its character through 0x27AB30, picks the portrait from the class
(+0xD "picked", +0xC "gave up", +0x10 the face) and draws the health and Force bars. In the first
mission, with player 2's id in slot 2 and the count set to 2, the game draws Obi-Wan's portrait, health
and Force bars at the bottom right, as in the co-op bonus missions.

## The camera

The gameplay camera keeps the targets of its focus lists (`TCamComPrimaryFocusList`, vtable 0x56CB0C)
in view. A list's targets are an array at +0x18 (capacity), +0x1C (count), +0x20 (the entries, at first
stored inside the object at +0x40), 0x70 bytes each: +0 "this entry is the player" (resolved through
0xA30F0, always player 1), +4 the target, +8 the target's instance id (its vfunc +0x5C). Resolving a list
again (vtable slot 5, 0xC4650) fills in +8 and drops entries without a target.

In the co-op bonus mission seven lists hold two targets: the player and Cin. In the first mission every
list holds the player only (with room for one). 0xC4C10 (thiscall on the array at +0x18: new count, the
entry to copy) grows the array the game's way; appending player 2 to every list that holds the player,
then resolving it, makes the camera widen and turn to keep both in view.

## Damage and death

Story companions carry the "Invincible?" property (character +0x5D6, 1 on the first mission's Obi-Wan,
0 on the player). Cleared, the second player takes damage in a fight like the first.

Health is a float at +0x130 (maximum +0x134). Every change goes through ICharacter's health change
(0x151500, thiscall (float change, a, b), vtable slot 0x384): it returns at once while "Invincible?" is
set, else adds the change (a hit is scaled by the difficulty) and keeps it under the maximum. The Jedi
classes' slot 0x384 is their hit (0x269C20): with the game's `god` it refills player 1 only, then calls
0x151500 and, when health is at or below 0, starts the death. Most callers pass `(change, 0, 0)`.

A death is a sequence: health reaches 0, the character falls, and some 4 seconds later its "Killed"
handler (0x1507A0, slot 0x194) sets +0x12C ("dead"), removes it from the AI's view and, for player 1,
calls the Jedi's slot 0x430 (0x267B30). Health given back during the fall does not stop it (dead with
full health). Player 1's slot 0x430 has the game manager start the loss (0x27A9F0: in a story mission,
the Game Over screen) and set its delay (0x27AB70). When the second player dies, nothing of that runs:
the story mission goes on and the HUD empties player 2's bars.

## Cutscenes

0xA3720 is the engine's "a cutscene is playing" (the camera, the AI and the characters ask it): the
letterbox is up (`[0x69226C]`, 0x12E190), a cinematic plays (`[0x695C88]`), or scripted cameras hold the
view (`[0x68DA44]` > 0). It reads the letterbox without checking it, so it is only asked in a running
level. While it is true the letterbox keeps player 1 alive (0x12E060). The direct start of the Cin
Drallig bonus mission plays one from about 4 to 7.5 seconds; player 1's control mode stays 2 in it.

## What the port does (game/coop.cpp)

- **Joining.** In a story or single-player bonus mission (a player from the player spawn, a player count
  of 1), 1.5 seconds after the start, when a controller is there for player 2: the companion (a living
  Jedi with control mode 16 whose AI data's "Target Player" (+0x50) is clear: the scripted battle droids
  of the first mission have mode 16 too, R2-D2 and allied clones are no Jedi) or, without one, a
  character spawned beside player 1 as an ally. It gets control mode 2, controller 1 (0x150580), its
  "Invincible?" cleared, its id in slot 2 with a player count of 2 (the second HUD), and an entry in
  every focus list that holds player 1; lists resolved later (slot 5 is hooked) get one too.
- **Cutscenes.** Player 2 keeps the character through a cutscene (the cutscene plays it as it does the
  AI's; both end on time). Letting go of it for the cutscene and taking it again left it standing still
  for good (Cin Drallig after her bonus mission's opening, Obi-Wan in the first mission) although its
  control mode, controller and port were all set again; unbinding the controller for the cutscene alone
  did it too. The cutscene also left the last element of its transform (+0x18C, copy +0x75C) not a
  number, repaired when player 2 takes a character. With story safety the leash and the shared camera
  wait for the cutscene's end (and a second).
- **Leaving.** The controller goes or co-op is turned off: the character is unbound, gets its control mode and "Invincible?" back, slot 2 and the count get their
  mission's values back, its focus list entries are emptied and resolved away, and player 2's HUD is
  hidden. Unbinding needs the port's own help: 0x150580 with -1 writes -1 to the character's controller
  (+0x43C) before the input manager's unbind (0x8ADD0) reads it there, so the port's entry
  (`[0x68D4F4]` +0x1D8 + port*4, its +0x7C the character it drives) kept driving the companion (it stood
  still with an idle controller). The port clears that entry itself.
- **Kept.** While player 2 plays, their control mode, controller (+0x43C) and the port's entry are checked
  every frame and set again when something changed them (seen after a cutscene in the first mission).
- **HUD.** Player 2's vitals are the HUD's `EnemyVitals` group (`interfc\hud.xbl_xml`; the story duels show
  the opponent there, from slot 2). Its handlers (vtable 0x5A80A0, update 0x25CD10: +4 the item, +8 the
  group whose +0x84 is the slot, +0x10 the fade time) fade the item in (alpha at item +0xB) while slot 2
  has a living character and never fade it out; the portrait (HudVitals 0x5A82B0) is picked once (+0xC
  gave up, +0xD picked). The port sets the alpha and fade time to 0 when player 2 leaves, and has the
  portrait picked again when they join.
- **Shared camera.** The free camera's hook on the master camera's placement (0x129990) lets co-op move it:
  the game's camera (aimed at player 1) is moved along its right and up axes towards the players' middle
  (at most 300) and back along its view until both players' middles are within 80% of the picture (at
  most 450), eased at 3 per second. The game takes the moved placement for the render, and its own camera
  keeps working from its own state (no drift seen). `Camera=1` turns it off.
- **Instant deaths.** Falls into the void and kill zones go through ICharacter's instant kill (0x152D30,
  slot 0xC8; the Jedi's 0x280C30 ends in it): health 0 and Killed, past the health change; an "Invincible?"
  character is spared. Hooked: player 2 is spared and placed on a spot player 1 walked over (positions
  recorded while player 1 is not falling), so the story's companion is never replaced by a new character
  (the mission's scripts know the original). A new character, when one is needed, wears the costume and
  texture set of the one before.
- **Camera and leash (before the shared camera).** Player 2 is in no focus list: in the first mission's open
  hangar, any list holding two targets makes the camera cut to a far, wide shot of the whole hangar,
  whatever the players' distance (confirmed live: co-op off, the usual camera; on, the wide one). Player 2
  is then kept in the picture: the game camera's placement (the master camera, 0x129990: rows right, up,
  forward, position) and field of view (0.925 rad, up and down, as the renderer gets it, 0x211C30) tell
  whether their middle is on screen; out of it for a second, or beyond 450 for a second (700 for half a
  second), they are placed beside player 1. With `Camera=1`: beyond 300 units apart (a character is about 70 tall) player 2 leaves the focus
  lists, and comes back into them within 220: in open places (the first mission's hangar) the camera
  pulls far back to frame two players a few hundred units apart, out into space; in tight ones it cannot,
  so 500 apart looked fine there. Beyond 500 for 2 seconds (900 for half a second), player 2 is placed
  beside player 1. The focus lists' own settings are MaximumPitch, MaximumYaw and KeepPositionFixed.
- **As strong as player 1.** Player 2's maximum health and Force are player 1's (current values kept in
  proportion, kept up while playing); a companion gets its own back when player 2 leaves. Its moves stay
  its class's.
- **Removal.** Turning co-op off removes the character it spawned. A character removed while it is player
  2's (the port's `despawn`) is let go of first: its focus list entries would otherwise point at a freed
  object, and the game's next resolve (0xC4688, its vfunc +0x5C) crashed.
- **Boss fights.** A boss in slot 2 (an AI enemy: Dooku, Grievous, Mace, Serra, the Mustafar duelists, old
  Obi-Wan) is player 2's with `Boss=1`: control mode 2 and controller 1 only; its health, health bar,
  invincibility, side, the camera and the game's rules stay as they are (no respawn, rescue, leash or
  shared camera). Its defeat ends player 2's part in the mission.
- **Missions with a player 2 of their own.** The co-op bonus missions started without a second
  controller leave their player 2 (Cin Drallig) to the AI with control mode 16 in slot 2 (count 2); the
  port plays it like a companion and gives it back to the AI as it was. The story duels count their
  opponent in slot 2 (for its health on the HUD): left alone, as is Versus (no player from the spawn).
- **Death.** 0x151500 is hooked. Under the respawn rule a change that leaves player 2 at or below 0
  (and not yet dead) leaves 1 instead, so no death starts, and on the next frame player 2 is placed
  beside player 1 (slot 0x1F4) with full health and 2 seconds of "Invincible?". A player 2 who dies
  anyway (a fall) is given a new character 3 seconds later. Under the game over rule, once player 2's
  death has played out (+0x12C, at most 6 s), the port calls 0x27A9F0 and 0x27AB70 as player 1's death
  does: the Game Over screen, player 1 alive.

## The pause menu

`interfc\pausescreen.xbl_xml` (in each level PAK; asked for as `interfc\pausescreen.xml`) is a compiled
menu: a u32, the screen's name and its first item's (length-prefixed strings), a u32 count of top-level
items (39), then the items. An item: the string `screenItem` and its name, a u32, its position (x, y as
floats; the rows are 24 apart, x 154), scale, colour, a text id (`IDS_PAUSE_QUIT_MISSION`), its down and
up neighbours, a target screen for `Navigation`, its action (`Resume`, `Restart`, `Quit`, `Navigation`)
and its children (each row has one, its highlight bar). The panel has two empty rows (`darktwobars`)
under Quit Mission.

The actions are menu handlers registered by name: the HUD's setup (IVaderHUD 0x2A8600) appends
`{name, prototype}` pairs to its list at +0x268 (0x157300); a prototype's vtable slot 10 makes the item's
copy, slot 4 takes events (thiscall (event, value)): 0x23 is "chosen". Continue's (0x2B2550, vtable
0x5CFCE8, events 0x2B3D10) first asks the Navigation handler's (0x2B3CA0), then continues the game and
keeps the button press from reaching it.

Texts: the menus hash a text id (0x222BA0, the same hash as item and screen names) and look it up in the
language's string table (0x23B6E0, thiscall `(out*, key*)`, a binary search; `out` stays null for an
unknown id). A text is wide characters with its LENGTH as the first character (a text without it loses its
first letter). `game/menus.cpp` hooks the lookup and answers ids of the port's own (`IDS_PORT_...`).

Screens: `gameinfo\guilist.txt` (in each level PAK) lists the in-game menus' screens, one compiled file a
line; the GUI manager (0xD5270) loads each (0x245B60, a type-31 resource) and keeps them in the HUD's list
(+0x1F4 count, +0x1F8 array; a screen's name key at +0x38; the current screen at +0x1FC). A screen of the
port's own is appended to the list, DECLARED to the level (type 31) and served from memory when asked
for: a loose file (cache\disc) decoded to nothing and a preloaded one made an empty screen.

The On/Off rows of Settings (Subtitles, Vibration) are `SettingsScreenOnOffControl` (factory 0x2D9690,
vtable 0x5D7000): value at +0x23C; slot 3 draws (it re-reads the profile's subtitles byte, profile+0x2A,
when the row's +0x84 is 1), slot 4 takes events (0x23 / 0x1F / 0x20 toggle, then slot 28 applies),
slot 27 resets to the default. Its base (0x2D7DF0) answers back (0x26) with the "settings won't be saved"
question; the plain handler (0x2C62D0) just goes back.

The port's **Cooperative Mod** screen (`pause_coop`) is built from the pause Settings screen
(`interfc\pause_settings.xml`): its Subtitles row copied for each option, the other rows removed (the item
count lowered: rows moved off screen still drew), the title replaced. The rows' handler `CoopOption` is a
copy of the On/Off vtable with draw (shows the setting), events (back goes straight back), copy, default
and apply (writes settings.ini) replaced. The pause screen gets a row "Cooperative Mod" copied from its
Settings row (a `Navigation` row) under Quit Mission, its target `pause_coop`.

## Player 2 as a clone trooper

A clone's moves are its script class's sequences (GScript_Vader\CloneTrooper.cpp; script vtable
0x5E53D0, one object for all clones), thiscall with two stack arguments; the character they run for is
the current script context (`[0x6964C0]`), which is the character's script (+0x434) +0x1C. Under player
controls the attack button plays the rifle butt (0x356B80, `ctroop_atk_riflebutt`).

The clones' shooting is not a sequence of its own: their script's StateManager (0x35AD70, every frame)
runs, for a clone the AI moves, its combat (0x35AE70, cdecl (instance, script)): close up the rifle butt,
otherwise, after a random 40 to 520 frames and facing its target within 10 degrees (0x172F00), the
shooting loop as an upper-body layer over whatever the clone does (0x173B40 `ctroop_atk_shooting_loop`,
layer 2; stopped with 0x173B80 (2, 5)), and three frames on a bolt at its target (0x3C0090 (instance,
script, 0, spread, [instance+0x3EC], [instance+0x3D8])), the next muzzle (0x1760B0) and the blaster sound
(0x17AD20 ([instance+0x3E4], 100, 1, 800, -1, 0)). Under player controls the StateManager takes its other
way (0x173D70). Playing CloneBlastAttack for the player instead (an AI sequence, through the melee attack)
made the clone step and shuffle. The port makes the same layer, bolt and sound calls for player 2's
clone, in its script's turn, while heavy attack is held (a bolt every 300 ms at its target).

A saber cut is the dismemberment manager's (TDismembermentManager); it tells the character's script
(0x2DB950, stdcall (character, kind, value, part)), and the clone's callback (CloneTrooper_Callbacks.h,
0x35AB80) hides the arm, marks it lost (the script instance's +0x32C / +0x32D: no more shooting) and plays
`Dismember_RightArm`. A clone dies of the hit as a rule; player 2 (player 1's health) lived on armless, so
player 2's characters are not told. Its texture set and costume come from the level's own clones
(the temple: costume hordeTrooper, set _var01, the 501st).

## Still open

- Story scripts that move a companion outside cutscenes (doors, "wait for Obi-Wan" moments).
- Menus of the port's own beyond On/Off rows (lists, sliders), e.g. the player 2 character.
