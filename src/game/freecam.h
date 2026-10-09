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
// gave the renderer (0 when not seen yet). False before the game has placed it.
bool GameCameraPlacement(float rows[16], float& fov);

} // namespace swrots::game
