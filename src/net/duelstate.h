#pragma once
// Client/server netplay's study, phase 1: the gameplay state of a Versus duel (docs/research/netplay.md, "Client/
// server with rollback"). For now a recorder: every frame, both fighters' objects (player 1's and the game's player
// 2) and the frame's pads, written to netplay\duel-<name>.bin for offline comparison (which fields change with the
// fight).

#include <cstdint>
#include <string>
#include <unordered_set>

namespace swrots::net {

// At every Swap, on the game thread.
void DuelRecorderFrame();
// Records `frames` frames from the next one on (0 stops), as netplay\duel-<name>.bin.
void StartDuelRecorder(const std::string& name, uint32_t frames);
std::string DuelRecorderStatus();
// The recorder's screenshots (rec-<frame>.png): every `every` frames from `from` to `to` (0: none).
void SetDuelRecorderShots(uint32_t from, uint32_t to, uint32_t every);

// Capture/apply of a duel's state (phase 1's second step): does the game keep a fighter's state written back to it?
// `save` captures both fighters now; `load` writes the capture back into the live fighters, either whole objects
// ("full": only the same fighters, the same round) or only the gameplay fields found by the recorder ("fields":
// position, orientation, velocity, health, Force, the action and its timers, the state flags) or only the position
// ("pos"). The 8 frames after a load are logged (what the game kept). `test <a> <b> <mode>`: save in <a> frames,
// load <b> frames later.
std::string DuelSnapCommand(const std::string& verb, const std::string& arg1, const std::string& arg2);

// Plays a recording's fighters into the live duel: from its frame `from`, `frames` frames, each frame the gameplay
// fields of the fighters in `mask` (1: player 1, 2: player 2, 3: both) written into the live ones; a screenshot
// (play-<frame>.png) every `shotsEvery` frames (0: none). The live duel's own inputs still play.
std::string StartDuelPlay(const std::string& name, uint32_t from, uint32_t frames, uint32_t shotsEvery, uint32_t mask);

// --- Client/server netplay (phase 2) ---
// A fighter's gameplay state as the host sends it: its fields (net/duelstate.cpp's kFields: position, orientation,
// velocity, health, Force, the action's counters, the state flags; nothing that points into memory), its running
// sequence (an index in its class's list: the same on both PCs), the sequences it started and the ticks since the
// running one started.
constexpr uint32_t kFighterFieldBytes = 232;
#pragma pack(push, 1)
struct FighterState {
    int16_t seq = -1;
    uint8_t valid = 0, reserved = 0;
    uint32_t starts = 0, ticks = 0;
    uint8_t fields[kFighterFieldBytes] = {};
};
#pragma pack(pop)
// The Versus duel's two fighters (the game's player slots); false when there is no duel now.
bool DuelFighters(uint8_t*& p1, uint8_t*& p2);
bool CaptureFighter(const uint8_t* fighter, FighterState& out);
// Writes a state into a live fighter of this game (slot 0 player 1, 1 player 2). The other player's fighter (played here
// from the host's pad, a little late): its fields, its health through the game's own health change, its sequence
// started again where this game's went another way (`leadTicks` ahead of the host's ticks). This PC's own fighter
// (`own`, played here at once from this PC's pad): its health and Force, its place only when far off, its sequence
// only when it stays parted.
void ApplyFighter(int slot, uint8_t* fighter, const FighterState& s, uint32_t leadTicks, bool own);
std::string ApplyStats();
// A Cooperative mission's other characters (its enemies, its allies): the host's, by their instance id (the same level
// on both games: the same ids), applied as the other player's fighter is (its place, its sequence, its health through
// the game's own change: its death the game's). A new level forgets them.
void ApplyCharacter(uint32_t id, uint8_t* character, const FighterState& s, uint32_t leadTicks);
void ApplyCharactersReset();
// A Cooperative mission (the own fighter's reactions: the host's sequence, its own place) or a duel (taken whole).
void SetCoopApply(bool coop);
// The guest's own fighter is played ahead of the host's ([Online] Predict=1), not on the host's time.
void SetPredictApply(bool predict);
// How far this game's play of the host's pads was from the host's states since the last call (a log line).
std::string FollowStats();
// This PC's own fighter, at its last state: in a grab (either game's), or taking the host's whole (its place too).
bool OwnTakesHostWhole();
// Diagnostics: a character's place this frame (its steps back and jumps counted), and the counts since the last call.
void NoteMotion(const uint8_t* character);
std::string MotionStats();
// This PC's own player, whose steps MotionStats counts apart too.
void SetOwnMotion(const uint8_t* character);
// The guest's followed characters: this game's own starts of their sequences refused (their locomotion's, their
// reactions'), the host's alone started (ApplyCharacter). Installed at each start (the behaviour's vtable slot); the
// set given again as it changes (empty: none held).
void InstallSequenceHold();
void HoldSequences(const std::unordered_set<uint8_t*>& characters);
uint32_t HeldStartsRefused();
// A state's position (+0x180), and a state moved on (its position and copies by `delta`, its sequence's time by
// `ticks`): the guest's characters between two of the host's states, carried on as they were going.
bool StatePosition(const FighterState& s, float out[3]);
void MoveState(FighterState& s, const float delta[3], uint32_t ticks);
// Between two of the host's states: the character's place alone (its transform, its position and copies), its
// animation left to run on (writing a state's action frame and timers again would set its pose back each frame).
void PlaceCharacter(uint8_t* character, const FighterState& s);
// A character this game spawned alone (none of the host's is it): out of sight, far below the level.
void ParkCharacter(uint8_t* character);
// Moves a character on the ground by `delta` (its x and z), its pose and sequence its own.
void NudgeCharacter(uint8_t* character, const float delta[3]);
uint32_t CharactersApplied();
// An object of a character's class (its behaviour at +0x434).
bool IsCharacter(const uint8_t* object);
// A character's id, sequence, place and health (diagnostics).
std::string DescribeCharacter(uint32_t id, const uint8_t* character);
// Whether a character plays a sequence (a level's templates, waiting to be spawned from, do not).
bool CharacterActive(const uint8_t* character);
// Whether a character lives (not dead, its health above 0).
bool CharacterAlive(const uint8_t* character);
// A state's character living (not dead, some health).
bool StateLiving(const FighterState& s);
// A character's current sequence (-1: none).
int16_t CharacterSequence(const uint8_t* character);
// Whether either fighter of the duel plays its intro or outro now, and a line about both and the camera (diagnostics).
bool DuelStaged();
std::string DescribeDuel();

} // namespace swrots::net
