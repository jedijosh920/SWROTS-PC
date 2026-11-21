#pragma once

#include <cstdint>
#include <vector>

namespace swrots::game {

// Custom Missions in the game's own main menu (Single Player > Custom Missions, under Bonus Missions): the row opens
// a copy of the game's Select Bonus Mission screen listing the installed missions (missions.h, any mod's), with their
// pictures, player, location and description; choosing one starts it. Without missions there is no row. At every
// boot, after InstallMissions.
void InstallMissionsMenu();

// The main menu's resource patch (hvvmenu.cpp has the one for main_menu.xml): adds the Single Player row.
void PatchMissionsMainMenu(std::vector<uint8_t>& data);

// A main menu row was chosen (its name's key, MenuKey): the Custom Missions row puts the bonus mission list in its
// mode, any other takes it out.
void NoteMenuRowChosen(uint32_t key);

} // namespace swrots::game
