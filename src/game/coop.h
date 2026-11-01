#pragma once

#include <cstdint>
#include <string>

namespace swrots::game {

// Co-op: a second player in the story and single-player bonus missions ([Coop] in settings.ini). With it
// on, player 2 plays the mission's companion (Obi-Wan beside Anakin), or a character spawned beside player
// 1 in missions without one, as soon as a controller is there for them, and gives the character back to
// the game's AI when the controller goes. Player 2 has the game's own second HUD (portrait, health, Force),
// is kept in view by the camera while near (a player 2 left far behind is brought back) and takes damage;
// when they would die they come back beside player 1 or, with Player2Death=1, the mission is lost. In the
// bonus missions made for two players, player 2 plays the game's own second character when the game left
// it to the AI; in boss fights (the boss is the game's player 2: the story duels) player 2 plays the boss,
// as in Versus. Versus itself is left as it is. Applied anew at every level start.

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
    bool boss = false;         // it is the mission's boss (a boss fight: the boss is the game's player 2)
    float health = 0, maxHealth = 0; // player 2's, while playing
    float distance = 0;        // between the players, while playing
    bool cutscene = false;     // a cutscene is playing
    std::string reason;        // why player 2 does not play, when not
};
CoopState GetCoopState();

// The character player 2 plays, or null (for research tools such as `peek`).
uint8_t* CoopPlayer2();
// Player 2's character as the game plays it: co-op's, else the game's own second player (a Versus opponent, a boss
// fight's boss, a two-player bonus mission's second character), or null.
uint8_t* GamePlayer2();
// The game's own player slots (0: player 1, 1: player 2): their characters, or null.
uint8_t* GamePlayerSlot(int slot);

// A character is about to be removed from the level (the port's despawn): when it is player 2's, player 2
// lets go of it first (the camera must not keep it).
void CoopCharacterRemoving(uint8_t* character);
// A fatal hit on a character other than player 2 (a health change leaving it none, or an instant kill: a fall,
// a kill zone) is first offered to this guard (a mode's, e.g. Heroes vs Villains): when it returns true the
// character lives on with a little health (an instant kill does nothing) and the guard's owner deals with it.
// Null removes it.
using FatalHitGuard = bool (*)(uint8_t* character);
void SetFatalHitGuard(FatalHitGuard guard);
// Every health change of a character (`change` < 0: damage), after it is made, with the game's two other arguments
// (`attacker`: the character that dealt it, or null). Null removes it.
using DamageObserver = void (*)(uint8_t* character, float change, uint32_t source, uint8_t* attacker);
void SetDamageObserver(DamageObserver observer);
// A health change before it is made, which it may change (a mode's damage scale): the change to make. Null removes it.
using DamageFilter = float (*)(uint8_t* character, float change);
void SetDamageFilter(DamageFilter filter);
// A health change made as the game makes one (its death when it reaches 0), without the filter or the observer
// (online play: the guest's fighters take the host's changes only).
void ChangeHealthDirect(uint8_t* character, float change);

} // namespace swrots::game
