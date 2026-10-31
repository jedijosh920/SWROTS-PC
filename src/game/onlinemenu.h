#pragma once

#include <cstdint>
#include <vector>

namespace swrots::game {

// Online play in the game's own main menu: an Online row between Multi Player and Settings opens the port's screen
// (Host Game, Join Game, the room code, the session's status, Leave); Join Game opens the game's code keyboard to
// type a room code (Ctrl+V pastes one). The session itself is net/session.h. At every boot, after the image is loaded.
void InstallOnlineMenu();

// The main menu's resource with the Online row (from the main menu's resource patch, hvvmenu.cpp).
void PatchMainMenuOnline(std::vector<uint8_t>& data);

} // namespace swrots::game
