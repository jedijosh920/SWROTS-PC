#pragma once

namespace swrots::game {

// Free camera (the `freecam` console command). At every boot, after the image is loaded; every
// reboot (level change, restart) turns it off.
void InstallFreeCamera();

// Detaches the view from the game's camera (from the next frame, where the game camera was) and
// holds player 1's input back from the game, or gives both back.
void SetFreeCamera(bool on);
bool FreeCameraOn();
// The game camera's latest placement (rows right, up, forward, position; up is +Y) and the field of view it
// gave the renderer (0 when not seen yet), as shown (with any adjustment below). False before the game has
// placed it.
bool GameCameraPlacement(float rows[16], float& fov);
// The view matrix the renderer drew the latest frame's main pass with (world to view, rows; a duel arena's camera is
// placed in the arena's own frame, so this, not the placement, takes world points to the screen), and the field of view.
bool GameRenderedView(float view[16], float& fov);

// An adjustment of the game camera's placement outside a flight (co-op's shared camera): called once a frame
// with the game's placement and the field of view; true when it changed `rows`. Null for none.
using CameraAdjuster = bool (*)(float rows[16], float fov);
void SetCameraAdjuster(CameraAdjuster adjuster);
// A developer tool's placement of the camera outside a flight, after any adjustment (same form; true when it placed
// it). Null for none.
void SetCameraDirector(CameraAdjuster director);
// The field of view the renderer gets outside a flight (radians; a cutscene's lens), 0 for the game's own.
void SetCameraFov(float radians);

} // namespace swrots::game
