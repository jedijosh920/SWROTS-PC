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

## Still open

- The camera follows player 1 only; the second player walks out of view.
- The HUD shows player 1 only (the co-op bonus missions show both players' portraits and bars).
- Combat: the second player's attacks and Force powers, enemies targeting them, their health and death.
- Story scripts that move a companion (cutscenes, doors) while it is a player.
- Input: player 1 is the keyboard and the first controller together; a keyboard-only player 1 with the
  first controller as player 2 needs a setting.
- A way to start it: a debug command first, then an option in the game's own pause menu.
