#pragma once

#include <string>

struct ImDrawList;
struct ImVec2;
struct IDirect3DDevice9;

namespace swrots::debug {

// The game's own HUD font (interfc\fonts\font_large_white: a DXT5 glyph sheet and its glyph boxes, from the disc's
// PAKs), for the port's overlays (Heroes vs Villains' titles and scoreboard), so they look like the game's. Capitals
// only, as the game's: text is drawn upper case. Game thread, inside the overlay's frame.

// Text `height` pixels tall at `at` (its left, top), in `colour` (ABGR, as ImGui's), with a dark shadow. False
// when the font is not there (then nothing is drawn).
bool GameFontText(ImDrawList* draw, const ImVec2& at, float height, unsigned colour, const std::string& text,
    bool shadow = true);
// Its width at that height.
float GameFontWidth(float height, const std::string& text);
bool GameFontReady();

// The game's fonts: the HUD's (font_large_white, as above) and the menus' (font_small_white).
enum class GameFont { Hud = 0, Menu = 1 };
bool GameFontTextOf(GameFont font, ImDrawList* draw, const ImVec2& at, float height, unsigned colour,
    const std::string& text, bool shadow = true);
float GameFontWidthOf(GameFont font, float height, const std::string& text);

// The device's life: the sheet is a texture of the device.
void GameFontDeviceCreated(IDirect3DDevice9* device);
void GameFontDeviceReleasing();

} // namespace swrots::debug
