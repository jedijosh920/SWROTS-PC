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
- **Leaving.** The controller goes, co-op is turned off, or a cutscene plays (with story safety): the
  character is unbound, gets its control mode and "Invincible?" back, slot 2 and the count get their
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
- **Camera and leash.** Beyond 600 units apart (a character is about 70 tall) player 2 leaves the focus
  lists, and comes back into them within 450: framing two players far apart pulls the camera far out,
  in the first mission's open hangar into space. Beyond 900 for 3 seconds (1500 for 1), player 2 is
  placed beside player 1.
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

`interfc\pausescreen.xbl_xml` (in each level PAK) is a menu file in the format of the versus select screen
the port already patches as it loads (`RegisterResourcePatch`): screen items with a text id
(`IDS_PAUSE_RESUME_GAME`), their up and down neighbours, and an action (`Resume`, `Restart`, `Quit`, or
`Navigation` to a sub-screen such as `pause_settings`). A co-op entry needs an item in that chain and an
action the port handles, and a text for it.

## Still open

- Story scripts that move a companion outside cutscenes (doors, "wait for Obi-Wan" moments).
- A pause menu entry of the port's own, and its text.
