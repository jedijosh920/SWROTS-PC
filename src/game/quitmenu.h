#pragma once

#include <cstdint>
#include <vector>

namespace swrots::game {

// Quit Game in the game's own menus: a row under the main menu's Features, and one under New Game and Load Game
// on the screen after the title. Each asks, in that screen's own Yes/No popup, "Are you sure you want to quit the
// game?"; Yes closes the game as the window's close button does, No (the popup's first choice) goes back to the
// menu. At every boot, after the image is loaded.
void InstallQuitMenu();

// The main menu's resource patch (hvvmenu.cpp has the one for main_menu.xml): adds the main menu's row.
void PatchQuitMainMenu(std::vector<uint8_t>& data);

} // namespace swrots::game
