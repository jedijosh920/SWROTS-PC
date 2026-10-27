#pragma once

namespace swrots::debug {

// The screen of a mission of a mod's own (game/missions.h): cutscene bars, fades, the objective, subtitles, titles,
// notices, timer and counters, the end. Inside the overlay's ImGui frame, on the game thread.
void DrawMissionHud();

} // namespace swrots::debug
