#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Heroes vs Villains (docs/research/heroes-vs-villains.md): four heroes against four villains on the developer map.
// The fighters are placed in the level (so they load with it, with their sounds) and wait until the match starts;
// the player is one of them. Each side has a number of lives: a fighter that falls comes back after its death has
// played out, near the fight, while its side has lives left; a side with none left and no one standing loses.
// Points for damage, defeats, assists and streaks; a kill feed; the top five at the end. Asked for by the launch
// arguments `hvv:<8 classes>` (heroes, then villains), `hvvplayer:<0-7>` (the player's place in the line-up, 0 by
// default), `hvvlives:<n>`, `hvvhealth:<n>` and `hvvintro:0` (no intro), next to the dev map's.

// Each start of the game image, after InstallDevMap: lays out the dev map for a match when this start asks for one.
void InstallHvv();

// True when this start of the game is a match.
bool HvvActive();
// True while its fight is on (after the intro, before the end): its guard on the player's falls is in place.
bool HvvFighting();

// Whether nothing of the match's map (its collision boxes) stands between two points of the world.
bool HvvLineOfSight(const float a[3], const float b[3]);
// Whether two characters are fighters of the same side in the running match (false when either is none).
bool HvvSameSide(const uint8_t* a, const uint8_t* b);
// Whether the game is paused (its pause menu).
bool GamePaused();
// The game's own time in milliseconds: it stands still while the game is paused (the port's timers in matches, so
// a pause holds them: reinforcements, specials, respawns, the stuck fighters' rescue). GameTimeFrame each frame.
uint64_t GameTime();
void GameTimeFrame();

// The match, for the overlay (scoreboard, titles, markers, kill feed, leaderboard, end) and the debug menu.
struct HvvState {
    bool active = false;
    bool started = false;  // the fighters are there (the intro, the fight or the end)
    bool intro = false;    // the intro is on
    bool fighting = false; // the fight is on
    int winner = -1;       // 0 the heroes, 1 the villains; -1 none yet
    int player = 0;        // the player's place in the line-up
    int startLives = 0;
    int lives[2] = {};     // left, heroes and villains
    int standing[2] = {};  // fighters standing
    // the intro's titles, then "FIGHT!"
    std::string caption, subcaption;
    int captionSide = -1;  // the side a title is of
    float captionFade = 0; // 0 to 1 through the title's time
    int heroes = 0;        // fighters on the heroes' side: the first ones in `fighters`, the villains after
    bool playerDown = false; // the player fell (the kill camera) or is out
    bool paused = false;     // the game is paused (its pause menu): the match's screen hides
    struct Fighter {
        std::string className, name; // name: as shown ("YOU" for the player)
        int side = 0;
        bool standing = false, out = false, isPlayer = false;
        bool placed = false;      // in the level now (`head` valid)
        float head[3] = {};       // above its head, in the world
        float feet[3] = {};       // its feet (its position)
        float health = 0;         // 0 to 1, while placed
        float force = -1;         // 0 to 1; negative: none
        int score = 0, defeats = 0, falls = 0, assists = 0, damage = 0, streak = 0;
    };
    std::vector<Fighter> fighters;
    struct FeedLine {
        std::string killer, victim; // killer empty: it fell by itself
        int killerSide = -1, victimSide = 0;
        bool mine = false;          // the player is in it
        float age = 0;              // seconds
    };
    std::vector<FeedLine> feed;     // newest first
    // the player's own notices: "DEFEATED <NAME>" with a streak, "ASSIST <NAME>", "DEFEATED BY <NAME>"
    std::string notice, noticeSub;
    int noticePoints = 0;
    int noticeSide = 0;
    float noticeAge = -1;           // seconds; negative: none
    bool board = false;             // the leaderboard is pinned (`hvv board`)
    bool ending = false;            // the end: the top five
    float endingAge = 0;
    std::vector<int> top;           // fighters, best first (up to 5)
    // Battlefront: the sides are the clones (0) and the droids (1), lives are tickets
    bool conquest = false;
    struct PostState {
        float at[3] = {};
        int owner = -1;             // 0, 1, or -1 neutral
        float hold = 0;             // -1 the clones' to +1 the droids'
        std::string name;           // A, B, ...
    };
    std::vector<PostState> posts;
    bool heroReady = false;         // the player can call a hero (B)
    int heroPoints = 0, heroCost = 0;
};
HvvState GetHvvState();

// The default line-up: heroes, then villains.
std::vector<std::string> HvvDefaultLineup();

// Starts a match (the game restarts into it). `lineup`: 2 to 16 classes, heroes then villains; `heroes`: how many
// of them are heroes (0: half; up to 8 a side); `player`: the player's place in it; `lives`: each side's;
// `health`: every fighter's (0: the defaults, 35 and 250); `intro`: the lines shown first; `map`: a custom map's id
// (custommaps.h), empty for the dev map's floor. False with `error` set for a line-up the game cannot play.
bool StartHvv(const std::vector<std::string>& lineup, int heroes, int player, int lives, int health, bool intro,
    const std::string& map, std::string& error);

// Fighters for the match panel: the Versus fighters and the other saber duelists (registered classes only).
std::vector<std::string> HvvFighterClasses();
// A fighter's name as shown (e.g. "OBI-WAN KENOBI").
std::string HvvDisplayName(const std::string& className);

// Ends the running match at once: the side with more lives left wins (on a tie, the higher total score). False
// without a match in its fight.
bool EndHvvNow();

// The launch arguments that carry the player's profile into the next start (saving it first): the main menu after a
// match keeps it. Empty without one (a start from Default_Xbox.cfg).
std::string ProfileSessionArguments();

// Battlefront (conquest): clones against battle droids on the dev map or a custom map with command posts (its
// [Battlefront] section; the dev map's floor has its own), tickets, heroes for points. Starts it (the game restarts).
struct BattlefrontSetup {
    std::string map;                // a custom map's id, empty for the dev map's floor
    int side = 0;                   // the player's: 0 the clones, 1 the droids
    int troops = 6;                 // troopers a side (2 to 8), the player one of them
    int tickets = 100;
    bool heroes = true;
    std::vector<std::string> republicHeroes, separatistHeroes; // classes (those the game has are kept)
};
bool StartBattlefront(const BattlefrontSetup& setup, std::string& error);
// Whether this start is a Battlefront match.
bool BattlefrontActive();
// The player asks for its hero (the troopers' B): taken when its points are enough and its side has none.
void BattlefrontHeroRequest();

// Pins the leaderboard (as holding Tab shows it), or unpins it.
void SetHvvBoardPinned(bool pinned);

// Once a frame on the game thread: the match's progress.
void HvvFrame();

} // namespace swrots::game
