# Controls

## Controllers

Xbox controllers (and anything Windows exposes through XInput) work with the original layout.
PlayStation controllers (DualShock 4, DualSense, DualSense Edge) work directly over USB or
Bluetooth, including rumble: Cross = A, Circle = B, Square = X, Triangle = Y, L1 = White,
R1 = Black, L2/R2 = triggers, Options = Start, Share/Create or a touchpad click = Back.
Controllers can be connected at any time. Up to four players: player 1 is the keyboard plus the
first controller, then Xbox-style controllers, then PlayStation controllers. Other pads (e.g.
Switch) need Steam Input or a similar XInput translator. In [co-op](coop.md#controllers) with one
controller, the keyboard and mouse are player 1 and the controller is player 2.

## Keyboard and mouse

Bindings live in `controls.ini` next to `swrots.exe`. The file is created with the defaults
below on first run. Each action takes one or more keys, separated by commas. Leave an action
empty to unbind it.

| Action | Xbox button | Default |
|---|---|---|
| MoveForward / MoveBack / MoveLeft / MoveRight | Left stick | W / S / A / D |
| ForceTargetUp / Down / Left / Right | Right stick | I / K / J / L, and mouse movement (below) |
| Jump | A | Space |
| FastAttack | X | Mouse1 (left button) |
| StrongAttack | Y | Mouse2 (right button) |
| CriticalAttack (also interacts with objects) | B | E, Mouse3 |
| Block (block / strafe) | Left trigger | Shift |
| PushGrasp | Right trigger | F |
| SaberThrow | White | Q |
| StunLightning | Black | R |
| ForceHeal | Left + right stick click | H |
| Pause | Start | Escape |
| Back | Back | Tab |
| MenuUp / MenuDown / MenuLeft / MenuRight | D-pad | Arrow keys |
| MenuAccept | A | Enter |
| MenuBack | B | Backspace |
| LeftStickClick / RightStickClick | Stick clicks | (unbound) |
| Walk (hold with the movement keys) | (a partial left-stick push, `WalkSpeed=0.5`) | LCtrl |

**Mouse movement is the right stick** while the game has the mouse: to deflect blaster bolts,
hold Block (Shift) and move the mouse in circles, as you would circle the right stick. Moving
the mouse also picks Force targets. In `controls.ini`:

```ini
[Mouse]
RightStick=1      ; 0 turns it off
Sensitivity=1.0   ; higher: less mouse movement for a full stick push
```

Key names: `A`-`Z`, `0`-`9`, `F1`-`F24`, `Numpad0`-`Numpad9`, `Space`, `Enter`, `Escape`, `Tab`,
`Backspace`, `Shift`, `Ctrl`, `Alt` (and `LShift`, `RShift`, `LCtrl`, `RCtrl`, `LAlt`, `RAlt`),
`Up`, `Down`, `Left`, `Right`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`,
`CapsLock`, `Mouse1`-`Mouse5`, and `Comma`, `Period`, `Minus`, `Plus`, `Semicolon`, `Slash`,
`Tilde`, `LBracket`, `RBracket`, `Backslash`, `Quote`.

Keyboard input only counts while the game window has focus. Mouse buttons count once you have
clicked into the game (which hides and captures the cursor); that click itself, and clicks on the
title bar or window buttons, are not game input. Alt alone does not release the mouse (Alt+F4 still
quits).

To quit without the keyboard, the main menu has **Quit Game** under Features, and so does the screen after the
title (under New Game and Load Game). Both ask first, in the game's own Yes/No popup.

## Button prompts

The buttons the game shows (menu footers, tips, the HUD's prompts, the pause screen's combat chart) follow the
device you used last: after a key or the mouse they show the keys bound above, as key caps in the game's own
style and font (mouse buttons as a mouse with that button lit, the arrow keys as arrows); after a controller
button or stick, the Xbox buttons again. Tips that tell you to "pull" a trigger say "press" with the keyboard (the
game's own PlayStation 2 wording). `[Controls] ButtonPrompts` in settings.ini (`Auto`, `Keyboard`, `Controller`)
fixes one look. The caps are made from your bindings when first shown (kept in `cache\prompts`).

How it works (`src/game/prompts.cpp`): every button icon is a 32x32 picture of the game's (the menus'
`s_buttons_xbox_*` and d-pad pictures, the tips' `xboxtipbutton-NN`) loaded and drawn through its texture
interface (vtable 0x561A30: load 0x1F650, release 0x1F5E0, draw 0x1F040) or taken from its handle by the menus'
picture draw (0x2049F0); the port knows which handle is which icon from its file name and draws the key's cap in
its place, and puts the caps into the tips' icon table (0x7F0608, whose sizes the tips' layout reads).

## Commander Cody

Cody (Versus and Heroes vs Villains) has a blaster, not a saber, and a meter where a Jedi's Force bar is. It starts
each round full, fills over time (about 35 a second, 1000 when full) and pays for his shots, rolls, squad and
bombardment:

| Pad (PlayStation) | Keyboard and mouse (default) | What he does | Meter |
|---|---|---|---|
| Y (Triangle), held | Mouse 2, held | fires his blaster (stops when the meter runs dry) | 20 a shot |
| X (Square) | Mouse 1 | rifle butt | - |
| White (L1) | Q | rolls the way the stick points (back when you hold back), else away from his opponent; no hit lands in the middle of the roll (not as it starts: roll before the blow, not as it lands); one a second | 150 |
| RT (R2) | F | calls two of his clones: they rappel down beside him and fight for 25 seconds, marked "CODY'S CLONE" (blue when they are yours, red when they are against you); one squad at a time, then 30 s before the next | 500 |
| B (Circle) | E | a bombardment: four shells fall around his opponent, knocking down and hurting his enemies only; 20 s before the next | 600 |

A clone cannot block: holding the block/target button only locks on. His armour takes saber hits harder than a
Jedi's guard would.

He waves his clones in and signals the strike, and says so; in Versus he has an intro and a win of his own ("Ready
weapons.", "Target eliminated."). His specials wait 8 seconds into each round. The
computer's Cody uses the same: he calls his squad and his bombardments when his opponent is near, and up close he
rolls away from the saber or strikes with his rifle butt, from the same meter.

## Droid Commander

Cody's Separatist counterpart, a B1 battle droid commander with yellow markings, plays the same way with the same
meter, costs and waits:

| Pad (PlayStation) | Keyboard and mouse (default) | What it does | Meter |
|---|---|---|---|
| Y (Triangle), held | Mouse 2, held | fires its blaster | 20 a shot |
| X (Square) | Mouse 1 | shoves its opponent away | - |
| White (L1) | Q | ducks under the saber (no hit lands in the middle of the duck); one a second | 150 |
| RT (R2) | F | waves for help: two battle droids drop in beside it and fight for 25 seconds, marked "COMMANDER'S DROID" | 500 |
| B (Circle) | E | the same bombardment as Cody's ("Fire!") | 600 |

Its intro and win: "Ready weapons." and "Jedi down. Situation normal." A blow that would destroy a battle droid at
once (a Jedi's droid finishers) only hurts it hard, as it does Cody.
