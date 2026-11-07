#pragma once

namespace swrots::game {

// Versus mode (duels). At every boot, after the image is loaded: re-applies the select screen's slot
// changes (the duel starts after a reboot and reads the table again) and installs the duel camera
// fallback.
void InstallVersus();

// Puts a character class (e.g. "IYoda") in a versus select slot (0-8) until the game is closed.
// False when the game knows no such class.
bool SetDuelist(int slot, const char* className);

// The class in a slot, as the game currently has it.
const char* Duelist(int slot);

// A duel's round, as the game's Versus logic has it (its game mode object [0x7EB964], the round's state +0x240:
// 3 the fighters' intros, 4 the fight, 5 the round's end (its result 2.7 s later), 6 after it); -1 without a duel.
int VersusRoundState();
// Ends the round being fought as a draw, as the game ends one (0x27CA80's state 4, both fighters living): the
// fighters lose their controllers, the round's end and its camera, then the game's own result 2.7 s later:
// IDS_ROUND_DRAW with both still living, no point given, the next round. False when no round is being fought.
bool VersusRoundDraw();

} // namespace swrots::game
