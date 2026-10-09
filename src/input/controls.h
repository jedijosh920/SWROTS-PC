#pragma once
// Keyboard and mouse bindings, kept in controls.ini next to the executable.
// Each game action (named as in the game's manual) maps to one or more keys;
// the keys drive the Xbox controller inputs of player 1. Controllers keep
// their normal layout.

#include <windows.h>

#include <string>

namespace swrots::input {

// Xbox controller state produced by the keyboard (matches XINPUT_GAMEPAD's
// meaning, with the Xbox's analog buttons).
struct KeyboardPad {
    WORD buttons;            // D-pad, Start, Back, thumb clicks (Xbox bit layout)
    BYTE analog[8];          // A, B, X, Y, Black, White, left trigger, right trigger
    SHORT lx, ly, rx, ry;    // sticks
};

// Loads `path` (creating it with the default layout if missing).
void LoadControls(const std::wstring& path);
// Current keyboard/mouse state as controller input; empty when the game
// window is not focused.
KeyboardPad ReadKeyboardPad();
// Raw mouse motion while the game has the mouse (window thread): read as the right stick.
void AddMouseMotion(LONG dx, LONG dy);
// Mouse wheel turns while the game has the mouse (window thread), in WHEEL_DELTA units.
void AddMouseWheel(SHORT delta);
// Takes the mouse motion and wheel turns since the last call, for the free camera (whose input
// the game does not get).
void TakeMouseInput(LONG& dx, LONG& dy, LONG& wheel);

// The free camera's controls: keyboard and mouse, and player 1's controller.
struct FreeCameraControls {
    float right, forward, up; // movement, each -1 to 1
    float lookX, lookY;       // controller look, -1 to 1 (up is positive)
    LONG mouseDx, mouseDy;    // mouse counts
    float speed;              // hold multiplier: faster above 1, slower below
    int speedSteps;           // base speed changes asked for (wheel, D-pad)
};
// Reads them (nothing while the game window is inactive or the debug menu is open).
FreeCameraControls ReadFreeCameraControls();
// Whether player 1's input is held back from the game (the free camera has it).
void HoldPlayerInput(bool hold);

// Co-op is running (game/coop.h): with one controller (or Input=1 in [Coop]) the keyboard alone is player 1
// and the first controller player 2; with two, as without co-op.
void SetCoopInput(bool active);
// The controller player 2 has, if any (for co-op to know whether player 2 can play).
bool Player2HasController();
// Co-op's clone trooper (a character without a heavy attack): player 2's heavy attack (Y) held is shooting
// (Player2Shooting), and the game does not see it.
void SetPlayer2ShootButton(bool active);
bool Player2Shooting();

// The game rebooted in-process: its controller ports are closed again.
void ResetPortsForReboot();

} // namespace swrots::input
