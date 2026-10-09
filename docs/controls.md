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
quits). On-screen button prompts still show
Xbox buttons; keyboard prompts are planned.
