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

} // namespace swrots::net
