#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
// An online match: the strip's view follows only this PC's player's cursor (0 player 1, 1 player 2; -1 either).
void SetStripLocalPlayer(int player);

// A menu picture record (an entry of a screen's picture set, e.g. the arena screen's) drawn with a picture loaded
// from its file instead (its path without .stx; loaded the first time it is drawn), for this start of the game.
// An empty `file`: the record is not drawn at all.
void DrawRecordWith(uint8_t* record, const std::string& file);

// The bonus mission select screen's picture sets (select_bonus.xbl_xml, as `data`) with one more place each for every
// entry of `heads` (the Custom Missions screen's missions, missionsmenu.cpp): SmallArenaTextSet from 5 (copies of its
// first), LargeArenaTextSet from 6 (of its second), HeadSet from 6 (each named as the entry: a front-end picture the
// shell has, e.g. s_selduel_macehead; empty: a copy of its first), PlanetSet from 5. False when the sets are not
// as expected.
bool AddMissionPictures(std::vector<uint8_t>& data, const std::vector<std::string>& heads);

} // namespace swrots::game
