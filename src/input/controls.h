#pragma once
// Keyboard and mouse bindings, kept in controls.ini next to the executable.
// Each game action (named as in the game's manual) maps to one or more keys;
// the keys drive the Xbox controller inputs of player 1. Controllers keep
// their normal layout.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

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
// The keys bound to an action (named as in controls.ini, e.g. L"Jump"): virtual keys in their order; none when
// unbound.
std::vector<int> ActionKeys(const wchar_t* action);
// Whether the mouse moves the right stick ([Mouse] RightStick).
bool MouseIsRightStick();

// The on-screen button prompts: true while the player's last input was the keyboard or the mouse (keyboard prompts),
// false while it was a controller (the game's own). [Controls] ButtonPrompts in settings.ini can hold either;
// SWROTS_PROMPTS=keyboard|controller forces one for tests.
bool KeyboardPrompts();
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
// A developer tool's scripted player 1 (on top of the real input): buttons held for that long ("A", "B", "X", "Y",
// "Black", "White", "LT", "RT", "Start", "Back", "Up", "Down", "Left", "Right", several joined with '+'), and the left
// stick held at (x, y) (-1..1) for that long. Any thread.
void ScriptButtons(const char* buttons, double seconds);
void ScriptStick(float x, float y, double seconds);
// Whether player 1's input is held back from the game (the free camera has it).
void HoldPlayerInput(bool hold);
// While it is held back: whether player 1 pressed A or Start since the last call (to skip what holds it, an intro).
bool TakeHeldPress();

// For SWROTS_TEST_BUTTONS' screen marks (@<screen>): the front end switched screens; `matches` tells whether a mark's
// screen name is the one switched to.
void NoteMenuScreen(bool (*matches)(const char* name, uint32_t key), uint32_t key);

// Co-op is running (game/coop.h): with one controller (or Input=1 in [Coop]) the keyboard alone is player 1
// and the first controller player 2; with two, as without co-op.
void SetCoopInput(bool active);
// The controller player 2 has, if any (for co-op to know whether player 2 can play).
bool Player2HasController();
// Co-op's clone trooper (a character without a heavy attack): player 2's heavy attack (Y) held is shooting
// (Player2Shooting), and the game does not see it.
void SetPlayer2ShootButton(bool active);
bool Player2Shooting();
// A game controller port's left stick as the game last read it (the keyboard's movement keys included for player 1),
// -1..1 each, up positive; false when the port is not open.
bool LeftStick(int port, float& x, float& y);

// A controller's state as the game reads it (the Xbox's XINPUT_GAMEPAD): digital buttons (d-pad, Start, Back, the
// sticks' clicks), the 8 analog buttons' pressure (A, B, X, Y, Black, White, the triggers), the sticks.
struct GamePad {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t thumbs[4]; // left x, left y, right x, right y
};
static_assert(sizeof(GamePad) == 18, "GamePad is the Xbox's XINPUT_GAMEPAD");

// What a port's player holds now, as the game would read it without netplay (the keyboard and mouse, controllers,
// the test inputs). Netplay samples it once a frame.
void ReadLivePad(int port, GamePad& pad);
// Netplay (net/rollback.h): the game's reads of ports 0-3 are answered by `source` instead of the PC's devices, as
// long as it returns true: the pad for the frame being played and its packet number (which moves on whenever
// the pad changes). Null for none.
using PadSource = bool (*)(int port, GamePad& pad, uint32_t& packet);
void SetPadSource(PadSource source);

// The game rebooted in-process: its controller ports are closed again.
void ResetPortsForReboot();

} // namespace swrots::input
