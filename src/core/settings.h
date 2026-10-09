#pragma once
// Player settings (graphics, display, frame rate), kept in settings.ini next to
// the executable. Read at startup; Save() writes the current values
// back, for in-game option menus.

#include <string>

namespace swrots {

struct Settings {
    // Display
    int width = 1280;         // window client size (ignored in fullscreen)
    int height = 720;
    bool fullscreen = false;  // borderless window covering the monitor
    bool vsync = false;
    bool stretch = false;     // fill the window, ignoring the aspect ratio (no black bars)
    // Rendering
    int resolutionScale = 0;  // internal resolution = Xbox resolution (640x480) x this, 1-8; 0 = auto
    bool widescreen = true;   // 16:9 (the game renders anamorphic widescreen); off: 4:3 pillarbox
    int anisotropy = 16;      // texture filtering, 1 (off) - 16
    bool bloom = true;        // the game's full-screen bloom (soft glow around bright areas)
    // Game
    int fpsLimit = 30;        // 30 (original) or 60 (experimental)
    // Co-op: a second player in the story and single-player missions (game/coop.h)
    bool coop = false;        // on: player 2 plays the mission's companion, or a character beside player 1
    int coopInput = 0;        // 0 auto, 1 keyboard is player 1 and the first controller player 2, 2 two controllers
    int coopDeath = 0;        // when player 2 dies: 0 they come back beside player 1, 1 game over
    bool coopStorySafety = true; // co-op leaves cutscenes to the game (no leash, no shared camera, no joining)
    bool coopFriendlyFire = false; // the two players' blows hurt each other (they stay allies)
    int coopCamera = 0;       // 0 a shared camera that keeps both players in view, 1 it follows player 1 alone
    std::string coopPlayer2;  // player 2's class when the mission has no companion (e.g. "IObiwan"); empty: automatic
};

const Settings& GetSettings();
Settings& EditSettings();

// Loads `path` (creating it with defaults if missing); later Save() calls write there.
void LoadSettings(const std::wstring& path);
void SaveSettings();

} // namespace swrots
