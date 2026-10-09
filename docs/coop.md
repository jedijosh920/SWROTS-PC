# Co-op

Two players in the story missions and the single-player bonus missions, on one PC. Player 2 joins by
themselves as soon as a controller is there for them: there is no button to press.

## Turning it on

- **In the game:** pause, then choose **Cooperative Mod** (under Quit Mission). Its screen has:
  - **Co-op**: on or off.
  - **Player 2 As Boss**: in boss fights, player 2 plays the boss; off, player 2 fights it beside player 1
    (below).
  - **Friendly Fire**: the players' blows hurt each other.
  - **Respawn Player 2**: on, player 2 comes back beside player 1; off, the mission is lost when they die.
  - **Shared Camera**: on, the camera keeps both players in the picture; off, it follows player 1 alone.

  Changes apply at once and are remembered (in `settings.ini`).
- **In `settings.ini`:** `Enabled=1` under `[Coop]` (see [settings](settings.md#co-op) for every key).
- **In the debug console:** `coop on` / `coop off`; `coop` alone says what co-op is doing.

Co-op is off by default. When it is off the game plays exactly as without it.

## Controllers

| Connected | Player 1 | Player 2 |
|---|---|---|
| One controller | keyboard and mouse | the controller |
| Two or more | keyboard, mouse and the first controller | the second controller |

`Input=1` always makes the keyboard player 1 and the first controller player 2; `Input=2` always gives
player 2 the second controller. Unplug player 2's controller and their character goes back to the game;
plug it in again and player 2 takes it again.

## Who player 2 plays

- **The mission's companion**, where there is one: Obi-Wan beside Anakin, Anakin beside Obi-Wan.
- **A character of their own**, placed beside player 1, where there is none: a Jedi when player 1 fights
  for the Jedi (Obi-Wan, or a Jedi Knight beside Obi-Wan), a clone trooper dressed as the level's own
  when player 1 fights with the clones (a 501st beside Anakin in the Jedi Temple). `Player2=` in `settings.ini` (or `coop player2 IVader`)
  chooses another; the debug menu's Characters tab lists the classes. Turning co-op off removes it.
- **The bonus missions made for two players**: with one controller the game leaves its second
  character (Cin Drallig beside Serra) to the computer; co-op gives it to player 2.

Player 2 gets the game's own second HUD (portrait, health and Force bars) and player 1's maximum health
and Force. Player 2 never loses a limb to a saber: a clone trooper as strong as player 1 would live on
without its arm (and blaster).

**A clone trooper player 2**: attack is the rifle butt, **heavy attack (Y / triangle) fires the blaster**
as the computer's clones do. A clone has no jump or Force moves of its own.

## Boss fights: player 2 plays the boss

Where the game makes a mission's boss its own second player (for the boss's health bar and the duel
camera), player 2 plays the boss, as in Versus: Count Dooku in the throne room, General Grievous on
Utapau, Mace Windu in Palpatine's office, Serra in the Jedi training arena, Anakin and Obi-Wan in the
Mustafar duels, old Obi-Wan on the Death Star. Everything else stays the game's: the boss's health and
health bar, the camera, its side, and the rules: beating the boss moves the story on, and losing is
player 1's game over. Versus, the bonus duels and the training maps are unchanged.

With **Player 2 As Boss** off (`Boss=0`, `coop boss off`) the computer plays the boss and player 2 fights
it beside player 1: the mission's companion where there is one (Obi-Wan against Dooku), else a Jedi
against a Sith boss (Obi-Wan, or a Jedi Knight beside Obi-Wan) and a 501st clone trooper against a Jedi
(Mace Windu, Obi-Wan beside Anakin). The boss keeps the second health bar, so player 2 has no HUD in these
fights. The setting can be changed during the fight; player 2 changes over at once.

## The camera and staying together

The camera follows player 1 and moves back and aside to keep player 2 in the picture too, as far as it
can. A player 2 out of the picture for half a second, or far away, is brought back to a spot player 1
just walked over. `Camera=1` makes the camera follow player 1 alone, as without co-op.

## Falling and dying

- A hit that would kill player 2 brings them back beside player 1 with full health instead, with a
  moment in which they cannot be hurt. With `Player2Death=1` the mission is lost instead, as when player 1
  dies.
- Falls into the void and the like never kill player 2: they are brought back. The story's companion
  stays the one the mission's scripts know.

## Friendly fire

`FriendlyFire=1` (or `coop friendlyfire on`) lets the two players' blows hurt each other, for duelling
for fun. They stay allies: enemies attack both, and the computer's characters do not change sides.

## Cutscenes

Player 2 keeps their character through cutscenes; the cutscene plays it. While one plays (and a moment
after) the camera is the game's own and player 2 is not brought back to player 1.

## Known issues

- **Health pickups** are player 1's: player 2 cannot pick them up.
- **A scripted moment can wait for the companion** to do something only the computer does (walk to a
  spot, open a door). Turn co-op off from the pause menu, let the moment play, and turn it on again.
- **The camera** can end up behind a wall in tight places. Behind the menu, the paused game is sometimes
  shown from far away.
- **A boss fight's scripted moments** (a boss's special attack, a step of the duel) may wait for the
  computer's boss. Turn co-op off from the pause menu to let the computer play the boss, and on again.
- **Bosses are not changed for players:** a boss keeps the health and moves the story gives it.
- **The pause menu's Cooperative Mod entry** is also there in Versus, where its settings do nothing.
- **A clone trooper player 2's shots are aimed by the game**, as the computer's clones' are, not exactly where the
  stick points.
- **Some settings are only in `settings.ini`** for now: `Input`, `Player2` and `StorySafety`.
- **Missions not tested from start to end in co-op**: the first mission has been; others may have
  moments like the above.

## Reporting a problem

Please attach `logs\swrots.log` (and `logs\swrots.previous.log` if the game was started again since). Its
first lines show your co-op settings; every co-op event is a line starting with `Co-op:` (player 2
joining or leaving, being brought back, cutscenes). If the game stops responding, a `Watchdog:` line
and where the game was stuck are written after 20 seconds.
