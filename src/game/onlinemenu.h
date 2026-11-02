#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Online play in the game's own main menu: an Online row between Multi Player and Settings opens the port's screen
// (Host Game, Join Game, the room code, the session's status, Leave); Join Game opens the game's code keyboard to
// type a room code (Ctrl+V pastes one). The session itself is net/session.h. At every boot, after the image is loaded.
void InstallOnlineMenu();

// The main menu's resource with the Online row (from the main menu's resource patch, hvvmenu.cpp).
void PatchMainMenuOnline(std::vector<uint8_t>& data);

// Kept for the main menu's hook (hvvmenu.cpp): no row is refused any more (false).
bool OnlineRefusesMainMenuEvent(uint8_t* handler, int event);
// Online play's guest follows the host's front-end screens: whether it follows this one (the room, the Versus
// selects), and the change to it (the Versus select as the Versus row opens it).
bool OnlineFollowsScreen(uint32_t key);
bool OnlineGoToScreen(uint32_t key);
// The room's Start: the Versus fighters' select (tests: the console's online start).
bool OnlineStartVersus();
// The session ended without this player leaving (`why`): the Online screen with the game's popup saying so.
bool OnlineShowLost(const std::string& why);

} // namespace swrots::game
