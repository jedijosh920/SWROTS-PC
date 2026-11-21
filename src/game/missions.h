#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Missions and modes of mods' own (docs/modding/missions.md): a folder mods\missions\<id>\ with mission.ini (its
// name, map, player and cast), mission.lua (its script) and optionally lines.ini (its dialogue). A mission runs on the
// developer map or a custom map; its script (Lua 5.4) drives it through the port's functions: characters, moves,
// cameras, dialogue, objectives, timers, endings.

struct MissionInfo {
    std::string id;          // its folder's name
    std::string name;        // as shown
    std::string description;
    std::string map;         // a custom map's id; empty: the developer map's floor
    std::string player;      // the player's class (Player=)
    std::string folder;      // its folder, mods-relative ("missions\<id>")
};

// Every start of the game image: reads mods\missions\*\mission.ini; when this start is a mission's (launch argument
// `mission:<id>`), lays out its level (its cast placed in it) and loads its script. After InstallHvv.
void InstallMissions(const std::wstring& mods);

const std::vector<MissionInfo>& Missions();
const MissionInfo* FindMission(const std::string& id);

// Starts a mission: the game restarts into its map (any thread). False with `error` when it cannot.
bool StartMission(const std::string& id, std::string& error);

// Whether this start is a mission's.
bool MissionActive();

// Once a frame on the game thread: runs the script.
void MissionFrame();

// For testing a mission (console `mission next`, F8 with the debug menu on): a cutscene is skipped at once; otherwise
// the script's current wait (a fight's wait_until, a wait) ends now, so the next part begins. Game thread.
void AdvanceMission();

// The player's character is defeated at once, as by a killing blow (console `kill`: to test a defeat). Game thread.
bool KillPlayer();

// What the mission's screen shows (debug/missionhud.cpp), read on the game thread.
struct MissionHud {
    bool active = false;
    bool paused = false;
    float letterbox = 0;       // 0..1 of the bars' full height
    float fade = 0;            // 0 clear .. 1 black
    std::string objective;     // the current objective (empty: none)
    float objectiveFlash = 0;  // 1 when just set, down to 0
    std::string speaker, line; // the subtitle (empty: none)
    std::string title, subtitle;
    float titleAlpha = 0;
    std::string notice;
    float noticeAlpha = 0;
    bool timerShown = false;
    float timerSeconds = 0;    // what the timer shows
    std::string timerLabel;
    std::vector<std::pair<std::string, std::string>> counters; // label, value (top right)
    std::string ending;        // "MISSION COMPLETE" / "MISSION FAILED" while the end shows
    std::string endingDetail;
    float endingAlpha = 0;
    float endingFade = 0; // the end's fade to black, after its words have been up a while
    std::string error;         // the script's error, shown so a modder sees it
};
MissionHud GetMissionHud();

} // namespace swrots::game
