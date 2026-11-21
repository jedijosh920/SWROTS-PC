#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/fullbody.h"

namespace swrots::game::yodaintro {

// Yoda's Versus intro: the end of his bonus mission's opening cutscene (cinematics\u113\cin_u113_010.cin: he stops,
// reaches out with the Force and stands on guard), as an animation of his own (Yoda_Intro). The cutscene keeps
// each actor's animation as the game's own clips are kept (a .bnm memory image per actor); his is cut from
// kStartTime on, its keys moved to start at 0 (the first one made at the cut), without its "origin" track (the
// cutscene's world position). His root is turned half a turn: the cutscene's faces -z, a duelist's intro +z (the
// round's way, where the intro cameras are).
constexpr const char* kCutscene = "cinematics\\u113\\cin_u113_010.cin";
constexpr const char* kClipName = "Yoda_Intro";
constexpr const char* kClipFile = "animation\\yoda\\yoda_intro.bnm";
constexpr double kLift = 14; // his root raised (game units): the cutscene's stands 14 lower than his own clips
constexpr int kStartTime = 300; // in the clip's time units (1/60 s): 5.0 s into the cutscene, 2.4 s before its end

// His saber, as the cutscene has it: on his belt, pulled to his hand (it leaves the belt at 304 and is in his hand at
// 326: its own actor, R1_LSaberYodaShape), off until 355 and lit then, the blade grown in 0.8 s (cin_u113_010.xml:
// CinEvent_ToggleSaber). His intro starts with it away, brings it back in his hand as he catches it and lights it at
// the cutscene's time.
constexpr int kSaberCaughtTime = 326, kSaberOnTime = 355; // the cutscene's time units
constexpr unsigned kSaberCaughtAfterMs = (kSaberCaughtTime - kStartTime) * 1000 / 60; // into his intro
constexpr unsigned kSaberOnAfterMs = (kSaberOnTime - kStartTime) * 1000 / 60;
constexpr float kSaberGrow = 0.8f;

// His duel cameras (the duel asks for both): the intro a fixed shot of him (MakeIntroCamera); the outro Anakin's
// (whose slot he took), brought down to his size (kCameraScale: his height over Anakin's). Memory images, as the game's (their pointers relocated by the loader through the table
// at their end).
constexpr const char* kCameraFile = "cinematics\\introcamera\\yoda_intro_cam.cin";
constexpr const char* kOutroCameraFile = "cinematics\\outrocamera\\yoda_outro_cam.cin";
constexpr const char* kCameraTemplate = "cinematics\\introcamera\\anakin_intro_cam.cin";
constexpr const char* kOutroCameraTemplate = "cinematics\\outrocamera\\anakin_outro_cam.cin";
constexpr double kCameraScale = 0.45; // his head at 31 units, a man's near 70
constexpr double kShotPosition[3] = { 2, 8, 72 }; // the cutscene's: in front of him, low, 72 units away
constexpr double kShotTarget[3] = { 0, 14, 0 };
constexpr double kShotFov = 50;
constexpr unsigned kShotKeys = 72; // 2.4 s, as his clip

// The clip's memory image; empty when the cutscene or the bone names cannot be read, with the reason in `report`.
std::vector<uint8_t> MakeIntroClip(const fullbody::ReadResource& read, std::string& report);
std::vector<uint8_t> MakeIntroCamera(const fullbody::ReadResource& read, std::string& report);
std::vector<uint8_t> MakeOutroCamera(const fullbody::ReadResource& read, std::string& report);

} // namespace swrots::game::yodaintro
