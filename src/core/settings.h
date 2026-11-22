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
    int widescreen = 1;       // 0: 4:3 pillarbox; 1: 16:9 (the game renders anamorphic widescreen);
                              // 2: true widescreen at any shape (21:9, 32:9...): the game's 4:3 picture
                              //    extended at the sides (Hor+), the HUD at the screen's edges
    float aspectRatio = 0.0f; // Widescreen=2: the picture's shape (width / height); 0 = the screen's or window's
    bool hudAnchor = true;    // Widescreen=2: HUD pieces near the sides follow the screen's edges; off: all centred
    float hudAspect = 0.0f;   // Widescreen=2: the HUD keeps within this shape, centred (e.g. 16:9); 0 = the whole width
    int anisotropy = 16;      // texture filtering, 1 (off) - 16
    bool bloom = true;        // the game's full-screen bloom (soft glow around bright areas)
    // Game
    int fpsLimit = 30;        // 30 (original) or 60 (experimental)
    int buttonPrompts = -1;   // on-screen buttons: -1 the last device used, 0 the controller's, 1 the keyboard's
    // Co-op: a second player in the story and single-player missions (game/coop.h)
    bool coop = false;        // on: player 2 plays the mission's companion, or a character beside player 1
    int coopInput = 0;        // 0 auto, 1 keyboard is player 1 and the first controller player 2, 2 two controllers
    int coopDeath = 0;        // when player 2 dies: 0 they come back beside player 1, 1 game over
    bool coopStorySafety = true; // co-op leaves cutscenes to the game (no leash, no shared camera, no joining)
    bool coopBoss = true;     // in boss fights (the boss is the game's player 2) player 2 plays the boss
    bool coopFriendlyFire = false; // the two players' blows hurt each other (they stay allies)
    int coopCamera = 0;       // 0 a shared camera that keeps both players in view, 1 it follows player 1 alone
    std::string coopPlayer2;  // player 2's class when the mission has no companion (e.g. "IObiwan"); empty: automatic
    // Heroes vs Villains (game/hvv.h)
    int hvvOverheadInfo = 2;  // over the fighters' heads: their name, health and Force: 0 off, 1 allies, 2 everyone
    bool hvvOthersSlowMotion = false; // other fighters' slow motion and camera moves reach the player too
};

const Settings& GetSettings();

// The shape (width / height) of the picture the game is shown at: 4:3, 16:9, or with Widescreen=2 the
// chosen or detected shape (never narrower than 4:3). Fixed once read.
float PictureAspect();
// Widescreen=2 and a picture wider than 4:3: the frame is extended at the sides (see d3d/d3d.h).
bool TrueWidescreen();
Settings& EditSettings();

// Loads `path` (creating it with defaults if missing); later Save() calls write there.
void LoadSettings(const std::wstring& path);
void SaveSettings();

} // namespace swrots
