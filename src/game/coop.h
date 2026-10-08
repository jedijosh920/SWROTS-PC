#pragma once

#include <cstdint>
#include <string>

namespace swrots::game {

// Co-op: a second player in the story and single-player bonus missions ([Coop] in settings.ini). With it
// on, player 2 plays the mission's companion (Obi-Wan beside Anakin), or a character spawned beside player
// 1 in missions without one, as soon as a controller is there for them, and gives the character back to
// the game's AI when the controller goes. Player 2 has the game's own second HUD (portrait, health, Force),
// is kept in view by the camera and takes damage; when they would die they come back beside player 1 or,
// with Player2Death=1, the mission is lost. Versus and the bonus missions made for two players are left
// as they are. Applied anew at every level start.

// At every boot, after the image is loaded.
void InstallCoop();

// Once a frame, on the game thread.
void CoopFrame();

// For the `coop` command and menus: what co-op is doing in the running level.
struct CoopState {
    bool enabled = false;      // the setting
    bool levelAllows = false;  // a story or single-player mission is running (not Versus, not a two-player one)
    bool controller = false;   // player 2 has a controller
    bool playing = false;      // player 2 plays a character now
    std::string player2;       // that character's class, or the one waiting for player 2
    bool spawned = false;      // co-op spawned it (the mission has no companion)
    float health = 0, maxHealth = 0; // player 2's, while playing
    std::string reason;        // why player 2 does not play, when not
};
CoopState GetCoopState();

// The character player 2 plays, or null (for research tools such as `peek`).
uint8_t* CoopPlayer2();

} // namespace swrots::game
