#pragma once

namespace swrots::game {

// True widescreen (Widescreen=2, d3d/d3d.h): the renderer's view is widened at the sides so that the game
// draws, and does not cull, what the wide picture shows beside its 4:3 frame. At every boot, after the
// image is loaded.
void InstallWidescreen();

// Whether the loading screen is up (from its set-up to its release, while a level loads): its picture keeps
// its 4:3 shape with black at the sides (d3d/draw.cpp).
bool LoadingScreenUp();

} // namespace swrots::game
