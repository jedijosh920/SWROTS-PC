#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains in the game's own main menu (Multi Player > Heroes vs Villains): the entry opens the port's
// screens in the game's style (the rules, the line-up, each place picked on the Versus character select), then the
// match starts (hvv.h). At every boot, after the image is loaded.
void InstallHvvMenu();

// The key (MenuKey) of the screen the front end last switched to since the game started, or 0.
uint32_t LastShellScreen();
// The arena screen's cursor on its Random row (online play's room: a random arena); false off that screen.
bool ArenaCursorToRandom();

} // namespace swrots::game
