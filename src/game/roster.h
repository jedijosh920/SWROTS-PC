#pragma once

#include <cstdint>
#include <string>

namespace swrots::game {

// The versus roster beyond the game's nine fighters: extra select-screen slots (from 10; 9 is the
// game's Random) for characters on the disc that were never versus fighters. At every boot.
void InstallRoster();

// Versus: both players chose this class (a mirror duel).
bool BothPlayersChose(const char* className);

// Copies the game's nine fighter classes (kDuelistClasses) into the roster's table, which the duel
// reads by slot; after every change to the game's table (the duelist command).
void SyncRosterClasses();

// The duelist whose duel cameras the extra fighter in `slot` uses (e.g. "Anakin", for
// cinematics\introcamera\Anakin_Intro_Cam.cin), or null for a slot that is not an extra fighter's.
const char* ExtraFighterCameras(int slot);
// The character class of a select-screen slot (0-8 the game's fighters, from 10 the extra ones), or null (Random,
// or no such slot).
const char* SelectSlotClass(int slot);

// Draws the select screen's strip of fighters' heads (in place of its own draw, 0x2D56F0) when they do not fit, a
// scrolling part of it; false: they fit (the game's own draw then).
bool DrawSelectStrip(uint8_t* screen, uint32_t context);

// A menu picture record (an entry of a screen's picture set, e.g. the arena screen's) drawn with a picture loaded
// from its file instead (its path without .stx; loaded the first time it is drawn), for this start of the game.
void DrawRecordWith(uint8_t* record, const std::string& file);

} // namespace swrots::game
