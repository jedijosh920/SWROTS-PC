# Two players in the story missions (co-op)

Research towards a second player in the campaign and in the single-player bonus missions: how the game
does two players where it already has them, and what a story mission needs to get them. Work in progress;
nothing here is a player feature yet. Addresses are the retail Xbox executable's (NTSC-U), as loaded.

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
0 on the player). Cleared, the second player takes damage in a fight like the first (1500 to 1440 health
in a minute of the first mission). When the second player dies (+0x12C set, health 0) the story mission
goes on: no game over, no restart; the HUD empties player 2's health bar. What a story mission does when
player 2 dies is for the port to decide (a respawn beside player 1, or the game over player 1's death
brings).

## The pause menu

`interfc\pausescreen.xbl_xml` (in each level PAK) is a menu file in the format of the versus select screen
the port already patches as it loads (`RegisterResourcePatch`): screen items with a text id
(`IDS_PAUSE_RESUME_GAME`), their up and down neighbours, and an action (`Resume`, `Restart`, `Quit`, or
`Navigation` to a sub-screen such as `pause_settings`). A co-op entry needs an item in that chain and an
action the port handles, and a text for it.

## Still open

- Input: player 1 is the keyboard and the first controller together; a keyboard-only player 1 with the
  first controller as player 2 needs a setting.
- Story scripts that move a companion (cutscenes, doors) while it is a player.
- The camera with the players far apart (tested over a short distance only).
- Player 2's death: respawn or game over.
- A pause menu action of the port's own, and its text.
