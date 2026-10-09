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

// An adjustment of the game camera's placement outside a flight (co-op's shared camera): called once a frame
// with the game's placement and the field of view; true when it changed `rows`. Null for none.
using CameraAdjuster = bool (*)(float rows[16], float fov);
void SetCameraAdjuster(CameraAdjuster adjuster);

} // namespace swrots::game
