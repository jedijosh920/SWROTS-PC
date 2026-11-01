#pragma once
// Client/server netplay's study, phase 1: the gameplay state of a Versus duel (docs/research/netplay.md, "Client/
// server with rollback"). For now a recorder: every frame, both fighters' objects (player 1's and the game's player
// 2) and the frame's pads, written to netplay\duel-<name>.bin for offline comparison (which fields change with the
// fight).

#include <cstdint>
#include <string>

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

} // namespace swrots::net
