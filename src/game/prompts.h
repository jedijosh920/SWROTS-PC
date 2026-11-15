#pragma once

#include <string>

namespace swrots::game {

// The on-screen button prompts for the keyboard and mouse (docs/controls.md): while the player's last input was the
// keyboard or the mouse (input::KeyboardPrompts), every Xbox button the game shows (menu footers, tips, the HUD's
// prompts, the pause screen's moves) is drawn as the key bound to it in controls.ini instead: key caps in the game's
// two icon looks, labelled with the game's own font, made at run time. With a controller, the game's own again.
// At every boot; `cache`: where the made pictures are kept.
void InstallPrompts(const std::wstring& cache);

// A button's picture in the menus' look, made the first time: a key cap labelled `label` (`keyboard`) or a controller's
// small round button (Start, Back) named so. Its texture's width, and its drawn part's columns in it (32 high).
bool ButtonPicture(bool keyboard, const char* label, void*& picture, int& width, int& left, int& right);

} // namespace swrots::game
