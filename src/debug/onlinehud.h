#pragma once

namespace swrots::debug {

// Online play's notices over the game's picture (net/session.h): in a match a small "ONLINE" line with the ping in a
// corner; while the match starts, what it waits for; when the two games part (a desync) a warning; when the session
// ends, why, for a few seconds. In the game's own font (gamefont.h). Inside the overlay's ImGui frame, on the game
// thread; drawn by this PC only (nothing of the game's own changes), so the two PCs may show different things.
void DrawOnlineHud();

} // namespace swrots::debug
