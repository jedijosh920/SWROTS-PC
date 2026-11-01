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

} // namespace swrots::net
