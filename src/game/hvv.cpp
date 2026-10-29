// Heroes vs Villains on the developer map (see hvv.h).

#include "game/hvv.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "game/characters.h"
#include "game/cody.h"
#include "game/coop.h"
#include "game/customchars.h"
#include "game/custommaps.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/game.h"
#include "input/controls.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

constexpr int kMaxSide = 8;         // fighters a side, at most (the default line-up has four)
constexpr int kMaxFighters = 2 * kMaxSide;
constexpr float kLineGap = 150.0f;  // between fighters in a line (the dev map's floor; a map has its own)
constexpr float kLinesApart = 700.0f;
constexpr float kPi = 3.14159265f;
constexpr ULONGLONG kWakeAfter = 500;     // ms after the player is in the level: the placed fighters join
constexpr int kDefaultLives = 35;         // a side's
constexpr int kDefaultHealth = 250;       // every fighter's (a Jedi has 1000 of its own: long duels, not a battle)
// Every fighter keeps a health of kHealthScale; the mode's health is made by scaling the damage taken (250: four
// times). The HUD's bar and its frame are as long as the maximum (x 1/1500, 0x25D1A0): a Jedi's own 1000 matches the
// Force bar under it (1000 too); a low maximum would show a stub.
constexpr float kHealthScale = 1000.0f;
constexpr ULONGLONG kRespawnAfter = 4000; // ms after a fighter fell (its death plays out first)
constexpr ULONGLONG kShieldFor = 2000;    // ms a fighter that came back cannot be hurt
// The player's death (as Battlefront's): its body dies where it fell while the camera goes from it to look at its
// killer; then it comes back.
constexpr ULONGLONG kPlayerDown = 4500;   // ms from the fatal hit to coming back
constexpr ULONGLONG kKillCamIn = 1000;    // ms: the camera from the game's to the kill camera
constexpr ULONGLONG kKillCamBack = 500;   // ms: back to the game's camera, behind the fighter that came back
constexpr ULONGLONG kEngageEvery = 1000;  // ms: fighters without an enemy to fight are given one
// The AI walks straight at its target (the dev map has no paths): a wall or the bar between them stops it. A fighter
// that stays within kStuckMove units for kStuckFor while its target is further than kStuckReach is brought near it.
constexpr ULONGLONG kStuckFor = 5000;
constexpr float kStuckMove = 40.0f, kStuckReach = 260.0f;
constexpr ULONGLONG kIdleFor = 12000; // beside its target, no blow either way this long: its target is given again
// Where a fighter comes back: on the floor, just outside the fight, facing it.
constexpr float kFloorX0 = 1500.0f, kFloorX1 = 24000.0f, kFloorZ0 = 2700.0f, kFloorZ1 = 13500.0f;
constexpr float kFloorMargin = 600.0f;
constexpr float kRespawnReach = 950.0f;  // from the fight's middle, at most
constexpr float kRespawnClear = 380.0f;  // from every fighter, at least (when it can)
constexpr int kRespawnTries = 32;
constexpr float kSpawnNear = 350.0f; // a custom map's own clear places this near a post stand for it
constexpr float kSpawnLift = 25.0f;  // above a custom map's floor (under its low ceilings)
// The intro (ms): for each side a frontal shot of its line playing its Versus intros, then a close shot along the
// line; the camera back behind the player; "FIGHT!" shows a while after.
constexpr ULONGLONG kFrontShot = 2800, kCloseShot = 2200, kToPlayer = 900, kFightBanner = 1500;
constexpr ULONGLONG kSideShots = kFrontShot + kCloseShot;
constexpr ULONGLONG kShotsLength = 2 * kSideShots;
// Defeats, assists and points.
constexpr ULONGLONG kKillerWithin = 10000; // ms: the last enemy hit this recent made the defeat
constexpr float kLikelyReach = 450.0f;     // a hit naming no attacker: an enemy this near dealt it
constexpr ULONGLONG kAssistWithin = 15000; // ms: other enemies' hits this recent are assists
constexpr int kDefeatPoints = 100, kAssistPoints = 50, kStreakPoints = 25;
constexpr ULONGLONG kNoticeFor = 3500, kFeedFor = 7000;
constexpr size_t kFeedLines = 6;
// The end: the top five, lined up, after the winner is known for a moment; then back to the main menu.
constexpr ULONGLONG kEndAfter = 2500;
constexpr ULONGLONG kMenuAfter = 12000; // ms of the top five

constexpr const char* kDefaultLineup[] = { "IObiwan", "IYoda", "IMace", "ISerra",
    "IVader", "IDooku", "IGrievous", "IAnakin" };

// Battlefront (conquest, `bf:1`): the same match with troopers (clones against battle droids) and tickets for lives,
// command posts to take and hold (a side holding more of them drains the other's tickets; fighters come back at their
// side's posts) and heroes: points buy one (the player's, a press of B) and the AI's sides call theirs too, one hero a
// side at a time.
constexpr float kPostRadius = 240.0f;      // a fighter this near a post counts for it
constexpr float kCaptureMs = 7000.0f;      // one fighter alone turns a post (neutral to its side) in this long
constexpr int kMostCapturers = 3;          // more fighters turn it faster, up to this many
constexpr ULONGLONG kBleedMs = 5000;       // a side holding fewer posts loses a ticket this often
constexpr ULONGLONG kBleedAllMs = 2000;    // ... holding none while the other holds all
constexpr int kHeroPoints = 500;           // the player's points for a hero
constexpr int kAiHeroPoints = 800;         // a side's AI's points for its hero
constexpr int kCapturePoints = 50;
constexpr int kDefaultTickets = 100;
constexpr int kDefaultTroopHealth = 150, kDefaultHeroHealth = 800;
constexpr float kDevPosts[][2] = { { 12750, 8100 }, { 10500, 8100 }, { 15000, 8100 }, { 12750, 6400 }, { 12750, 9800 } };
constexpr float kDevStarts[2][2] = { { 8600, 8100 }, { 16900, 8100 } };
constexpr uint32_t kLoaderIds = 0x40; // the hero classes' placed characters (they load the classes, sounds and all)

// The game's character fields and calls the match uses.
constexpr uint32_t kActivate = 0xB4;           // object vfunc, thiscall (): a placed character joins the level
constexpr uint32_t kPlace = 0x1F4;             // object vfunc, thiscall (const float m[16])
constexpr uintptr_t kPlaySequence = 0x150E30;  // ICharacter, thiscall (sequence, 0, 0, 0, speed): what Versus' intro calls
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uint32_t kChangeHealth = 0x00151500; // character, thiscall (float change, a, attacker): co-op hooks it
constexpr uint32_t kCharacterHealth = 0x130;
constexpr uint32_t kCharacterMaxHealth = 0x134;
constexpr uint32_t kCharacterPower = 0xA40;    // Force power, and its maximum (0 for those without)
constexpr uint32_t kCharacterMaxPower = 0xA44;
constexpr uint32_t kCharacterDead = 0x12C;     // byte: its death played out
constexpr uint32_t kCharacterTransform = 0x150; // rows right, up, forward, position
constexpr uint32_t kCharacterScript = 0x434;   // its script instance (a hit can name it as the attacker)
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte, "Invincible?"
constexpr uint32_t kCharacterAI = 0x9FC;       // its AI controller
constexpr uint32_t kAITarget = 0x41C;          // the controller's target
constexpr uint32_t kAISetTarget = 0x00193D90;  // thiscall on the controller (target)
constexpr uint32_t kAIOff = 0x28;              // byte on the controller: its AI switched off
// Battlefront's ways to the posts: the game's own goto (ICharacter::Goto, thiscall (character, marker, may fight):
// a goto controller walks it there, fighting what it meets) to the level's goto markers (ICharacterGoto, never a
// character), and whether one is still on its way (ICharacter::ExecutingGoto, thiscall (character, marker)).
constexpr uint32_t kCharacterGoto = 0x00151400;
constexpr uint32_t kExecutingGoto = 0x001513E0;
constexpr uint32_t kGotoMarkerVtable = 0x00598970;
constexpr float kFightRange = 800.0f;          // Battlefront: an enemy this near is fought, the post waits
constexpr float kMarkerReach = 150.0f;         // a marker counts as reached this near (well inside kPostRadius)
// A hit's attacker (the health change's last argument) is the character, its script instance, one of its
// weapons (+0x1080.. +0x10A0: its sabers, ILightSaber) or a projectile of its (IScriptProjectile), which points
// back at it.
constexpr uint32_t kWeaponsFrom = 0x1080, kWeaponsTo = 0x10A4;
constexpr uint32_t kOwnerSearch = 0x300;

struct Hit {
    int by;
    ULONGLONG at;
};

struct Fighter {
    std::string className;
    uint32_t placedId = 0;   // the placed object's id (none for the player)
    uint8_t* object = nullptr;
    uint32_t instance = 0;   // its instance id (+4), to tell it is still there
    ULONGLONG fellAt = 0;    // when it fell (0: fighting)
    ULONGLONG shieldUntil = 0;
    bool out = false;        // fell with its side out of lives
    std::deque<Hit> hits;    // enemy hits taken, newest last
    int score = 0, defeats = 0, falls = 0, assists = 0, streak = 0;
    float damage = 0;
    float stillAt[3] = {};   // where it was when it last moved on (stuck: see Unstick)
    int post = -1;           // Battlefront: the post it walks to (-1: none, fighting or there)
    ULONGLONG stillSince = 0;
    ULONGLONG lastBlow = 0;  // when it last dealt or took an enemy's hit (idle beside its target: see Unstick)
    std::string troopClass;  // Battlefront: its trooper's class (it is a hero while `hero`)
    bool hero = false;
};

bool g_Active = false;
Fighter g_Fighters[kMaxFighters];
int g_Count = 0;  // fighters in the match
int g_Heroes = 0; // of which heroes (the first ones; the villains after)
int g_Player = 0;
int g_StartLives = kDefaultLives;
int g_Health = kDefaultHealth;
int g_Lives[2] = {};
int g_Winner = -1;
ULONGLONG g_InLevelAt = 0, g_WinnerAt = 0, g_LastEngage = 0;
bool g_Woken = false;
bool g_PlayerFell = false; // a fatal hit on the player: its old body dies, then it comes back
ULONGLONG g_PlayerDownAt = 0;
int g_PlayerKiller = -1;      // who defeated the player last (the kill camera looks at it), or -1
bool g_Swapped = false;       // the controls went to a fresh fighter; the old body died
uint8_t* g_Corpse = nullptr;  // the player's old body
uint32_t g_CorpseId = 0;
float g_CorpseAt[3] = {};     // where it fell
float g_DeathView[16] = {};   // the game's camera when the player fell (the kill camera starts there)
bool g_DeathViewSet = false;
float g_KillView[16] = {};    // the kill camera's last placement (the way back starts there)
ULONGLONG g_BackAt = 0;       // when the player came back (the camera returns)
ULONGLONG g_RefocusAt = 0;    // when the camera's focus is put on the player once more after coming back
bool g_IntroWanted = true;
enum class Phase { Waiting, Intro, Fight, End } g_Phase = Phase::Waiting;
ULONGLONG g_IntroAt = 0, g_FightAt = 0, g_EndAt = 0;
bool g_MenuAsked = false;   // the way back to the menus is on its way
uint8_t* g_Podium[5] = {};  // the end's top five: fresh characters on the podium
bool g_PodiumIntros = false; // their intros are still to start
int g_ShotPlayed = -1;      // the last intro shot whose line played its intros
float g_LastShot[16] = {};  // the intro camera's last placement, for the way back to the game's
bool g_BoardPinned = false;
std::vector<int> g_Top;     // the end's top five
// the arena: the floor's, or a custom map's (its lines' gap and distance, where fighters can come back)
float g_LineGap = kLineGap, g_LinesApart = kLinesApart;
float g_Bounds[4] = { kFloorX0, kFloorZ0, kFloorX1, kFloorZ1 };
float g_Margin = kFloorMargin;
std::vector<std::pair<float, float>> g_Spawns; // a map's own places to come back (the dev map's coordinates)
std::string g_MapName;
int g_KillEye = -1; // the kill camera's place last chosen (kept while it sees)

// Battlefront
bool g_Conquest = false;
struct Post {
    float x = 0, z = 0, y = 0;
    int owner = -1;     // 0 the clones, 1 the droids, -1 neutral
    float hold = 0;     // -1 the clones' to +1 the droids'
};
std::vector<Post> g_Posts;
std::vector<uint8_t*> g_PostMarkers; // each post's goto marker (a level object: lives as long as the level)
float g_TeamStart[2][2] = {};
std::vector<std::string> g_HeroPool[2];
int g_HeroOf[2] = { -1, -1 }; // the fighter that is its side's hero, or -1
int g_SidePoints[2] = {};     // the AI's points towards its side's next hero
int g_TroopHealth = kDefaultTroopHealth, g_HeroHealth = kDefaultHeroHealth;
int g_PlayerHeroSpent = 0;
int g_HeroCost = kHeroPoints; // the player's (bfherocost:)
bool g_PlayerHeroAsked = false;
ULONGLONG g_LastBleed = 0, g_LastPosts = 0;

struct FeedEntry {
    int killer, victim;
    ULONGLONG at;
};
std::deque<FeedEntry> g_Feed;
struct Notice {
    std::string text, sub;
    int points = 0, side = 0;
    ULONGLONG at = 0;
} g_Notice;

// The fighters' names on screen.
struct DisplayName {
    const char* className;
    const char* name;
};
constexpr DisplayName kNames[] = { { "IAnakin", "ANAKIN SKYWALKER" }, { "IObiwan", "OBI-WAN KENOBI" },
    { "IOldObiwan", "OBI-WAN KENOBI" }, { "IYoda", "YODA" }, { "IMace", "MACE WINDU" },
    { "ICinDrallig", "CIN DRALLIG" }, { "IVader", "DARTH VADER" }, { "IDooku", "COUNT DOOKU" },
    { "IGrievous", "GENERAL GRIEVOUS" }, { "ISerra", "SERRA KETO" }, { "IPalpatine", "DARTH SIDIOUS" } };

// Just above a fighter's head, from its feet (the humans' bodies are 73-75 units tall).
float MarkerHeight(const std::string& className)
{
    std::string base = className;
    if (const CustomCharacter* custom = FindCustomCharacter(className.c_str()))
        base = custom->base;
    if (_stricmp(base.c_str(), "IYoda") == 0)
        return 42.0f;
    if (_stricmp(base.c_str(), "IGrievous") == 0)
        return 102.0f;
    return 86.0f;
}

std::string DisplayNameOf(const std::string& className)
{
    if (const CustomCharacter* custom = FindCustomCharacter(className.c_str()))
        return custom->name;
    for (const DisplayName& n : kNames)
        if (_stricmp(n.className, className.c_str()) == 0)
            return n.name;
    if (_stricmp(className.c_str(), "ICloneTrooper") == 0) // Battlefront's troopers
        return "CLONE TROOPER";
    if (_stricmp(className.c_str(), "IBattleDroid") == 0)
        return "BATTLE DROID";
    std::string name = className.size() > 1 && className[0] == 'I' ? className.substr(1) : className;
    for (char& c : name)
        c = char(toupper(static_cast<unsigned char>(c)));
    return name;
}

// A fighter's name as shown: its character's, the player's too (its lines are marked as its own).
std::string ShownName(int i)
{
    return DisplayNameOf(g_Fighters[i].className);
}

// The player's own choice of character, kept while a match plays its fighter, given back after.
struct PlayerChoice {
    bool saved = false;
    std::string className, variant, mesh, skin;
} g_SavedChoice;

void SavePlayerChoice()
{
    if (g_SavedChoice.saved)
        return;
    g_SavedChoice.saved = true;
    g_SavedChoice.className = PlayerClass() ? PlayerClass() : "";
    g_SavedChoice.variant = PlayerVariantChoice();
    g_SavedChoice.mesh = PlayerMesh();
    g_SavedChoice.skin = PlayerSkin();
}

void RestorePlayerChoice()
{
    if (!g_SavedChoice.saved)
        return;
    SetPlayerClass(nullptr);
    if (!g_SavedChoice.className.empty()) {
        SetPlayerClass(g_SavedChoice.className.c_str(), false);
        SetPlayerVariant(g_SavedChoice.variant);
        SetPlayerMesh(g_SavedChoice.mesh);
        SetPlayerSkin(g_SavedChoice.skin);
    }
    g_SavedChoice = {};
}

int SideOf(int slot) { return slot < g_Heroes ? 0 : 1; }
int SideStart(int side) { return side == 0 ? 0 : g_Heroes; }
int SideEnd(int side) { return side == 0 ? g_Heroes : g_Count; }
int SideSize(int side) { return SideEnd(side) - SideStart(side); }

// A place in the line-up: the heroes' line faces the villains' across the middle of the floor.
void LinePlace(int slot, float& x, float& z, float& yaw)
{
    const int k = slot - SideStart(SideOf(slot));
    x = kDevMapMiddleX + (k - (SideSize(SideOf(slot)) - 1) / 2.0f) * g_LineGap;
    z = kDevMapMiddleZ + (SideOf(slot) == 0 ? -g_LinesApart / 2 : g_LinesApart / 2);
    yaw = SideOf(slot) == 0 ? 0.0f : kPi; // a character faces +z at yaw 0
}

// A placement on the floor facing `yaw`, `lift` above it (60 for one that may fall onto it: a spawn's; a fighter
// set down on the floor itself keeps playing what it plays, one that falls is stopped by its fall).
void Transform(float x, float z, float yaw, float (&m)[16], float lift = 60.0f)
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float rows[16] = { c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, kDevMapFloorY + lift, z, 1 };
    std::copy(std::begin(rows), std::end(rows), m);
}

std::vector<std::string> Split(const std::string& text, char separator)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, separator))
        out.push_back(part);
    return out;
}

uint8_t* ObjectById(uint32_t id)
{
    return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id);
}

template <typename T>
T& Field(uint8_t* object, uint32_t offset) { return *reinterpret_cast<T*>(object + offset); }

template <typename F>
F VirtualOf(uint8_t* object, uint32_t offset)
{
    return reinterpret_cast<F>((*reinterpret_cast<void* const* const*>(object))[offset / 4]);
}

void Track(Fighter& f, uint8_t* object)
{
    f.object = object;
    f.instance = object ? Field<uint32_t>(object, 4) : 0;
    f.fellAt = 0;
    f.hits.clear();
}

void FullHealth(uint8_t* character)
{
    Field<float>(character, kCharacterMaxHealth) = kHealthScale;
    Field<float>(character, kCharacterHealth) = kHealthScale;
}

// Still in the level (`placed`), and standing.
bool Placed(Fighter& f)
{
    if (!f.object || ObjectById(f.instance) != f.object) {
        f.object = nullptr;
        return false;
    }
    return true;
}

bool Standing(Fighter& f)
{
    return Placed(f) && Field<float>(f.object, kCharacterHealth) > 0 && !Field<uint8_t>(f.object, kCharacterDead);
}

const float* Position(const uint8_t* character)
{
    return reinterpret_cast<const float*>(character + kCharacterTransform + 48);
}

int FighterOf(const uint8_t* object)
{
    for (int i = 0; i < g_Count; ++i)
        if (object && g_Fighters[i].object == object)
            return i;
    return -1;
}

// The fighter behind a hit's attacker object (see kWeaponsFrom), or -1.
int AttackerOf(uint8_t* attacker)
{
    if (!attacker)
        return -1;
    if (uint8_t* owner = BlastOwner(attacker); owner && FighterOf(attacker) < 0) // a blast of a special's (Cody's shells, Force Storm)
        attacker = owner;
    else if (uint8_t* master = HelperMasterOf(attacker)) // a reinforcement's blow counts for its master
        attacker = master;
    else if (uint8_t* shooter = FighterOf(attacker) < 0 ? ShooterOf(attacker) : nullptr) // a kit fighter's bolt
        attacker = shooter;
    for (int i = 0; i < g_Count; ++i) {
        if (!Placed(g_Fighters[i])) // a body taken away (a fighter's old one after a respawn)
            continue;
        uint8_t* f = g_Fighters[i].object;
        if (f == attacker || Field<uint8_t*>(f, kCharacterScript) == attacker)
            return i;
        // its sabers: only a Jedi has the weapon slots (IsA, vfunc +4, the IJedi type 0x249950); a clone's or a battle
        // droid's object ends before them (0xEE0 bytes)
        const auto isA = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>((*reinterpret_cast<void* const* const*>(f))[1]);
        if (!isA(f, nullptr, 0x00249950))
            continue;
        for (uint32_t at = kWeaponsFrom; at < kWeaponsTo; at += 4)
            if (Field<uint8_t*>(f, at) == attacker)
                return i;
    }
    __try { // a projectile: its own pointer back to its owner
        for (uint32_t at = 4; at < kOwnerSearch; at += 4) {
            uint8_t* p = Field<uint8_t*>(attacker, at);
            for (int i = 0; i < g_Count; ++i)
                if (p && Placed(g_Fighters[i]) && (p == g_Fighters[i].object ||
                        p == Field<uint8_t*>(g_Fighters[i].object, kCharacterScript)))
                    return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

// Somewhere to come back: just outside the fight (near its middle, as far from every standing fighter as the tries
// find, at least kRespawnClear when one does), facing the middle.
void ConquestPlace(int side, float (&m)[16]);

void SafePlace(int except, float (&m)[16])
{
    if (g_Conquest) { // at one of its side's posts
        ConquestPlace(SideOf(except), m);
        return;
    }
    float cx = 0, cz = 0;
    int n = 0;
    std::vector<std::pair<float, float>> others;
    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        if (i == except || !Standing(f))
            continue;
        const float* p = Position(f.object);
        others.push_back({ p[0], p[2] });
        cx += p[0];
        cz += p[2];
        ++n;
    }
    if (n) {
        cx /= n;
        cz /= n;
    } else {
        cx = kDevMapMiddleX;
        cz = kDevMapMiddleZ;
    }
    float bestX = cx, bestZ = cz, bestScore = -1e9f;
    const int tries = g_Spawns.empty() ? kRespawnTries : int(g_Spawns.size());
    for (int t = 0; t < tries; ++t) {
        float x, z;
        if (!g_Spawns.empty()) { // a map's own places
            x = g_Spawns[t].first;
            z = g_Spawns[t].second;
        } else {
            const float angle = float(std::rand()) / RAND_MAX * 2 * kPi;
            const float reach = kRespawnClear + float(std::rand()) / RAND_MAX * (kRespawnReach - kRespawnClear);
            x = std::clamp(cx + std::sin(angle) * reach, g_Bounds[0] + g_Margin, g_Bounds[2] - g_Margin);
            z = std::clamp(cz + std::cos(angle) * reach, g_Bounds[1] + g_Margin, g_Bounds[3] - g_Margin);
        }
        float clear = 1e9f;
        for (const auto& o : others)
            clear = std::min(clear, std::hypot(x - o.first, z - o.second));
        // clear enough, then as near the middle as can be
        const float score = std::min(clear, kRespawnClear) * 4 - std::hypot(x - cx, z - cz);
        if (score > bestScore) {
            bestScore = score;
            bestX = x;
            bestZ = z;
        }
    }
    Transform(bestX, bestZ, std::atan2(cx - bestX, cz - bestZ), m, DevMapCustomMap() ? kSpawnLift : 60.0f);
}

// Where the player waits out of the fight (between its fall and its comeback, or out of lives): far off on the dev
// map's endless floor; on a custom map, which has no floor there (the body fell all the while, and came back still
// in its fall, stuck in it), at the map's own clear place furthest from every standing fighter.
void WaitingPlace(float x, float z, float (&m)[16])
{
    if (!DevMapCustomMap() || g_Spawns.empty()) {
        Transform(x, z, 0, m);
        return;
    }
    float best = -1;
    for (const auto& s : g_Spawns) {
        float nearest = 1e9f;
        for (int i = 0; i < g_Count; ++i) {
            if (i == g_Player || !Placed(g_Fighters[i]) || !Standing(g_Fighters[i]))
                continue;
            const float* p = Position(g_Fighters[i].object);
            nearest = std::min(nearest, std::hypot(p[0] - s.first, p[2] - s.second));
        }
        if (nearest > best) {
            best = nearest;
            x = s.first;
            z = s.second;
        }
    }
    Transform(x, z, 0, m, kSpawnLift);
}

void Shield(Fighter& f, ULONGLONG now)
{
    Field<uint8_t>(f.object, kCharacterInvincible) = 1;
    f.shieldUntil = now + kShieldFor;
}

// --- Fighting ----------------------------------------------------------------------------------------------------

// Battlefront's troopers have a post to see to: one in three of a side holds one of its posts (a defender), the rest
// go for the nearest post their side does not hold (attackers; defending when it holds them all, attacking when it
// holds none). An enemy within kFightRange is fought first; with none, the fighter walks to its post (the game's
// goto, which fights what it meets on the way) and waits there. Heroes hunt (Engage's way). False: not for this one.
bool SeeToPost(int i)
{
    Fighter& f = g_Fighters[i];
    if (!g_Conquest || g_PostMarkers.size() != g_Posts.size() || f.hero)
        return false;
    uint8_t* brain = Field<uint8_t*>(f.object, kCharacterAI);
    f.post = -1;
    if (!brain || Field<uint8_t>(brain, kAIOff))
        return true;
    const int side = SideOf(i);
    const float* p = Position(f.object);
    // an enemy near: fought (the one it fights already, while it stays near)
    const int current = FighterOf(Field<uint8_t*>(brain, kAITarget));
    int nearest = -1;
    float nearestDistance = kFightRange;
    for (int k = 0; k < g_Count; ++k) {
        if (SideOf(k) == side || !Standing(g_Fighters[k]) || (k == g_Player && (g_PlayerFell || g_Fighters[k].out)))
            continue;
        const float* q = Position(g_Fighters[k].object);
        const float d = std::hypot(p[0] - q[0], p[2] - q[2]) * (k == current ? 0.75f : 1.0f);
        if (d < nearestDistance) {
            nearestDistance = d;
            nearest = k;
        }
    }
    if (nearest >= 0) {
        if (nearest != current)
            reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(
                brain, nullptr, g_Fighters[nearest].object);
        return true;
    }
    // its post
    const bool defender = (i - SideStart(side)) % 3 == 2;
    std::vector<size_t> owned, others;
    for (size_t k = 0; k < g_Posts.size(); ++k)
        (g_Posts[k].owner == side ? owned : others).push_back(k);
    size_t goal = 0;
    if ((defender && !owned.empty()) || others.empty()) {
        goal = owned[size_t(i) % owned.size()];
    } else {
        goal = others[0];
        for (size_t k : others)
            if (std::hypot(g_Posts[k].x - p[0], g_Posts[k].z - p[2]) < std::hypot(g_Posts[goal].x - p[0], g_Posts[goal].z - p[2]))
                goal = k;
    }
    if (std::hypot(g_Posts[goal].x - p[0], g_Posts[goal].z - p[2]) <= kMarkerReach)
        return true; // there: holds it
    f.post = int(goal);
    uint8_t* marker = g_PostMarkers[goal];
    if (!reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kExecutingGoto))(f.object, nullptr, marker))
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*, bool)>(uintptr_t(kCharacterGoto))(
            f.object, nullptr, marker, true);
    return true;
}

// A fighter of the AI's that has no enemy to fight (none, or one that fell) is set on the nearest standing enemy,
// as the AI's own target choice would once provoked: a fighter that came back, or one that stood waiting.
void Engage(int i)
{
    Fighter& f = g_Fighters[i];
    if (i == g_Player || !Standing(f))
        return;
    if (SeeToPost(i))
        return;
    uint8_t* brain = Field<uint8_t*>(f.object, kCharacterAI);
    if (!brain)
        return;
    uint8_t* target = Field<uint8_t*>(brain, kAITarget);
    const int current = FighterOf(target);
    if (current >= 0 && SideOf(current) != SideOf(i) && Standing(g_Fighters[current]))
        return;
    const float* p = Position(f.object);
    // Battlefront: the post it goes for (the nearest its side does not hold): the enemy nearest it
    const Post* goal = nullptr;
    for (const Post& post : g_Posts)
        if (post.owner != SideOf(i) && (!goal || std::hypot(post.x - p[0], post.z - p[2]) < std::hypot(goal->x - p[0], goal->z - p[2])))
            goal = &post;
    int best = -1;
    float bestDistance = 1e9f;
    for (int k = 0; k < g_Count; ++k) {
        if (SideOf(k) == SideOf(i) || !Standing(g_Fighters[k]) ||
            (k == g_Player && (g_PlayerFell || g_Fighters[k].out))) // the player's fresh fighter, waiting
            continue;
        const float* q = Position(g_Fighters[k].object);
        const float d = std::hypot(p[0] - q[0], p[2] - q[2]) * (goal ? 0.5f : 1.0f) +
            (goal ? std::hypot(goal->x - q[0], goal->z - q[2]) : 0.0f);
        if (d < bestDistance) {
            bestDistance = d;
            best = k;
        }
    }
    if (best >= 0)
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(
            brain, nullptr, g_Fighters[best].object);
}

bool LineOfSight(const float a[3], const float b[3]); // below, with the cameras
std::string PostName(size_t k);

// A fighter of the AI's that cannot reach its target (see kStuckFor): moved to a place near the target that sees it
// and is clear of the map's boxes.
void Unstick(int i, ULONGLONG now)
{
    Fighter& f = g_Fighters[i];
    if (i == g_Player || !Standing(f) || !DevMapCustomMap())
        return;
    const float* p = Position(f.object);
    if (!f.stillSince || std::hypot(p[0] - f.stillAt[0], p[2] - f.stillAt[2]) > kStuckMove) {
        std::memcpy(f.stillAt, p, sizeof f.stillAt);
        f.stillSince = now;
        return;
    }
    if (now - f.stillSince < kStuckFor)
        return;
    uint8_t* brain = Field<uint8_t*>(f.object, kCharacterAI);
    // Battlefront: one walking to its post (with no path there: a wall or a rock between) is brought near the post
    const bool toPost = f.post >= 0 && size_t(f.post) < g_Posts.size();
    const int target = toPost || !brain ? -1 : FighterOf(Field<uint8_t*>(brain, kAITarget));
    if (!toPost && (target < 0 || !Standing(g_Fighters[target])))
        return;
    float t[3];
    if (toPost) {
        t[0] = g_Posts[f.post].x;
        t[1] = p[1];
        t[2] = g_Posts[f.post].z;
    } else {
        std::memcpy(t, Position(g_Fighters[target].object), sizeof t);
    }
    const std::string goal = toPost ? "command post " + PostName(size_t(f.post)) : ShownName(target);
    if (!g_PostMarkers.empty() && !toPost && std::hypot(p[0] - t[0], p[2] - t[2]) > kFightRange) { // waiting at its post
        f.stillSince = now;
        return;
    }
    auto setTarget = [&] {
        if (!toPost) // (one going to a post: its way is given anew at the next Engage)
            reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(
                brain, nullptr, g_Fighters[target].object);
    };
    if (toPost && std::hypot(p[0] - t[0], p[2] - t[2]) < kStuckReach) {
        f.stillSince = now;
        return;
    }
    // near its target and not fighting: out of its reach (on a platform the AI cannot climb), brought beside it; in
    // Battlefront at any distance (troopers stand still to shoot: still is stuck only when no blow lands either way)
    if (std::hypot(p[0] - t[0], p[2] - t[2]) < kStuckReach || !g_PostMarkers.empty()) {
        const ULONGLONG last = std::max(f.lastBlow, g_Fighters[target].lastBlow);
        if (now - std::max(last, f.stillSince) < kIdleFor)
            return;
        f.lastBlow = now;
    }
    f.stillSince = now;
    // a place around the target (its own side first, then all round, nearer to further) that sees the target and is
    // clear (nothing from the floor up to above a head there); else the map's spawn nearest the target that sees it
    const float head[3] = { t[0], t[1] + 40, t[2] };
    auto clear = [&](float x, float z) {
        const float low[3] = { x, t[1] + 5, z }, high[3] = { x, t[1] + 110, z }, chest[3] = { x, t[1] + 40, z };
        return LineOfSight(low, high) && LineOfSight(head, chest);
    };
    float bestX = 0, bestZ = 0;
    bool found = false;
    for (float radius : { 90.0f, 150.0f, 220.0f, 300.0f }) {
        for (int k = 0; k < 12 && !found; ++k) {
            const float angle = std::atan2(p[0] - t[0], p[2] - t[2]) + float(k) * kPi / 6;
            const float x = t[0] + std::sin(angle) * radius, z = t[2] + std::cos(angle) * radius;
            if (clear(x, z)) {
                bestX = x;
                bestZ = z;
                found = true;
            }
        }
        if (found)
            break;
    }
    float nearest = 1e9f;
    for (size_t k = 0; !found && k <= g_Spawns.size(); ++k) {
        if (k == g_Spawns.size()) {
            found = nearest < 1e9f;
            break;
        }
        const float d = std::hypot(g_Spawns[k].first - t[0], g_Spawns[k].second - t[2]);
        if (d < nearest && clear(g_Spawns[k].first, g_Spawns[k].second)) {
            nearest = d;
            bestX = g_Spawns[k].first;
            bestZ = g_Spawns[k].second;
        }
    }
    if (!found) {
        LOG_INFO("HvV: %s cannot reach %s, and no place near it is clear", ShownName(i).c_str(), goal.c_str());
        return;
    }
    float m[16];
    Transform(bestX, bestZ, std::atan2(t[0] - bestX, t[2] - bestZ), m, DevMapCustomMap() ? kSpawnLift : 60.0f);
    VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(f.object, kPlace)(f.object, nullptr, m);
    setTarget(); // its walk was cut short: set on the target anew
    LOG_INFO("HvV: %s could not reach %s; brought nearer", ShownName(i).c_str(), goal.c_str());
}

void EngageAll()
{
    for (int i = 0; i < g_Count; ++i)
        Engage(i);
}

void NoticeFor(const std::string& text, const std::string& sub, int points, int side, ULONGLONG now)
{
    g_Notice = { text, sub, points, side, now };
}

// A fighter fell: who defeated it (the last enemy that hit it, lately), who assisted (other enemies that hit it),
// the points, the streaks, the feed and the player's notices.
void Defeated(int victim, ULONGLONG now)
{
    Fighter& v = g_Fighters[victim];
    int killer = -1;
    for (auto it = v.hits.rbegin(); it != v.hits.rend(); ++it) {
        if (now - it->at <= kKillerWithin) {
            killer = it->by;
            break;
        }
    }
    std::vector<int> assists;
    for (const Hit& h : v.hits)
        if (h.by != killer && now - h.at <= kAssistWithin &&
            std::find(assists.begin(), assists.end(), h.by) == assists.end())
            assists.push_back(h.by);
    v.hits.clear();
    ++v.falls;
    v.streak = 0;
    if (killer >= 0 && g_Conquest && killer != g_Player)
        g_SidePoints[SideOf(killer)] += kDefeatPoints;
    if (killer >= 0) {
        Fighter& k = g_Fighters[killer];
        ++k.defeats;
        ++k.streak;
        const int points = kDefeatPoints + kStreakPoints * std::min(k.streak - 1, 8);
        k.score += points;
        if (killer == g_Player)
            NoticeFor("DEFEATED " + DisplayNameOf(v.className),
                k.streak >= 2 ? std::to_string(k.streak) + " KILLSTREAK" : "", points, SideOf(victim), now);
    }
    for (int a : assists) {
        ++g_Fighters[a].assists;
        g_Fighters[a].score += kAssistPoints;
        if (a == g_Player && killer != g_Player)
            NoticeFor("ASSIST " + DisplayNameOf(v.className), "", kAssistPoints, SideOf(victim), now);
    }
    if (victim == g_Player)
        g_PlayerKiller = killer;
    if (victim == g_Player)
        NoticeFor(killer >= 0 ? "DEFEATED BY " + DisplayNameOf(g_Fighters[killer].className) : "YOU FELL", "", 0,
            killer >= 0 ? SideOf(killer) : SideOf(victim), now);
    g_Feed.push_front({ killer, victim, now });
    while (g_Feed.size() > kFeedLines)
        g_Feed.pop_back();
    LOG_INFO("HvV: %s %s%s", ShownName(victim).c_str(), killer >= 0 ? "defeated by " : "fell",
        killer >= 0 ? ShownName(killer).c_str() : "");
}

// The enemy most likely behind a hit that names no attacker (Force lightning's steady damage, some finishing moves:
// the health change's attacker is null): the nearest standing enemy within reach, one whose AI is set on the
// victim first (the player, which has no AI, counts as one); -1 when none is near.
int LikelyAttacker(int victim)
{
    Fighter& v = g_Fighters[victim];
    if (!Placed(v))
        return -1;
    const float* at = Position(v.object);
    int best = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        if (SideOf(i) == SideOf(victim) || !Standing(f))
            continue;
        const float* p = Position(f.object);
        const float d = std::hypot(p[0] - at[0], p[2] - at[2]);
        if (d > kLikelyReach)
            continue;
        uint8_t* brain = i == g_Player ? nullptr : Field<uint8_t*>(f.object, kCharacterAI);
        const bool onIt = i == g_Player || (brain && Field<uint8_t*>(brain, kAITarget) == v.object);
        const float score = d - (onIt ? kLikelyReach : 0);
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

// The mode's health of a fighter: the match's, or in Battlefront a trooper's or a hero's.
int HealthOf(int i)
{
    if (!g_Conquest || i < 0)
        return g_Health;
    return g_Fighters[i].hero ? g_HeroHealth : g_TroopHealth;
}

// Every hit on a fighter: the enemy that dealt it scores its damage and is remembered for the defeat.
void Damaged(uint8_t* character, float change, uint32_t, uint8_t* attacker)
{
    if (!g_Active || g_Phase != Phase::Fight || change >= 0)
        return;
    const int victim = FighterOf(character);
    int by = AttackerOf(attacker);
    if (victim >= 0 && by < 0 && !attacker) // a hit that names no one (Force lightning, some finishing moves)
        by = LikelyAttacker(victim);
    if (victim < 0 || by < 0 || SideOf(by) == SideOf(victim))
        return;
    Fighter& v = g_Fighters[victim];
    Fighter& k = g_Fighters[by];
    k.damage += -change * float(HealthOf(victim)) / kHealthScale; // in the mode's health
    k.score = int(k.damage) + k.defeats * kDefeatPoints + k.assists * kAssistPoints; // damage counts as it comes
    v.hits.push_back({ by, GameTime() });
    v.lastBlow = k.lastBlow = GameTime();
    while (v.hits.size() > 32)
        v.hits.pop_front();
}

// Damage to a fighter, scaled to the mode's health (see kHealthScale).
float ScaleDamage(uint8_t* character, float change)
{
    const int i = FighterOf(character);
    if (!g_Active || change >= 0 || i < 0)
        return change;
    return change * kHealthScale / float(HealthOf(i));
}

// A fatal hit on the player: the game must never see it die (that ends the mission with its Game Over screen).
// While its side has lives it comes back; with none left it stays down, out, until the match ends.
bool PlayerFatalHit(uint8_t* character)
{
    if (!g_Active || !g_Woken || character != PlayerObject())
        return false;
    if (g_Winner >= 0 || g_Fighters[g_Player].out)
        return true;
    if (!g_PlayerFell) {
        g_PlayerFell = true;
        g_PlayerDownAt = GameTime();
        g_Swapped = false;
        g_KillEye = -1;
        float fov = 0; // the camera as the hit lands (the kill camera starts there)
        g_DeathViewSet = GameCameraPlacement(g_DeathView, fov);
        g_BackAt = 0;
        std::memcpy(g_CorpseAt, Position(character), sizeof g_CorpseAt);
        Field<uint8_t>(character, kCharacterInvincible) = 1; // no more hits until its body is let die
        Defeated(g_Player, g_PlayerDownAt);
    }
    return true;
}

void CheckWinner(ULONGLONG now)
{
    if (g_Winner >= 0)
        return;
    if (g_Conquest) { // a side out of tickets loses at once
        for (int side = 0; side < 2; ++side) {
            if (g_Lives[side] > 0)
                continue;
            g_Winner = 1 - side;
            g_WinnerAt = now;
            LOG_INFO("Battlefront: the %s win", g_Winner == 0 ? "clones" : "droids");
            return;
        }
        return;
    }
    for (int side = 0; side < 2; ++side) {
        if (g_Lives[side] > 0)
            continue;
        bool anyone = false;
        for (int i = SideStart(side); i < SideEnd(side); ++i)
            anyone |= (Standing(g_Fighters[i]) && !g_Fighters[i].out) || (g_Fighters[i].fellAt && !g_Fighters[i].out) ||
                (i == g_Player && g_PlayerFell);
        if (!anyone) {
            g_Winner = 1 - side;
            g_WinnerAt = now;
            LOG_INFO("HvV: the %s win", g_Winner == 0 ? "heroes" : "villains");
            return;
        }
    }
}

// --- Other fighters' slow motion and camera ------------------------------------------------------------------------
// The game was made for duels with the player in them: a move's slow motion and the saber lock's camera are global.
// In a match they reach the player only from moves it is part of (settings.ini [HvV] OthersSlowMotion=1: all).
//
// - Slow motion: the moves' scripts call 0x176BF0 (cdecl (float scale, int rampIn, int hold, int rampOut); it
//   skips when the script effects are off, byte 0x6943D0, else jumps to 0xA20E0, which sets the time manager's ramp:
//   the scale multiplies every frame's time). A slowing (scale < 1) holds for 9999 frames until the move's script
//   restores 1; there is no stack, so a restore from anyone ends anyone's.
// - The saber lock's camera ("combateffect", 0x2BB250, cdecl, 9 arguments, the first: on) frames the player and
//   the instigator's target; the shared saber-lock sequences call it from six places.
// - The instigator is the running script's character ([[0x6943CC] +0x68]); a kill move, grab or lock links the two
//   fighters ([0x6943CC] +0x23C: their instance ids at +0xC and +0x10).
constexpr uint32_t kScriptSlowMotion = 0x00176BF0;
constexpr uint8_t kScriptSlowMotionBytes[] = { 0xE8, 0xEB, 0x57, 0xFF, 0xFF, 0x80, 0x38, 0x00, 0x75, 0x05 };
constexpr uint32_t kSetSlowMotion = 0x000A20E0;    // cdecl (scale, rampIn, hold, rampOut)
constexpr uint32_t kCancelSlowMotion = 0x000A2110; // cdecl (): back to 1
constexpr uint32_t kScriptEffectsOff = 0x006943D0;
constexpr uint32_t kScriptContext = 0x006943CC;
constexpr uint32_t kCombatCamera = 0x002BB250;
constexpr uint32_t kCombatCameraCalls[] = { 0x00408B52, 0x00408BAE, 0x00408DE1, 0x00408E27, 0x0040907B, 0x004090C1 };

uint8_t* g_SlowOwner = nullptr;       // whose slowing is running (its restore must get through)
uint8_t* g_LockCameraOwner = nullptr; // whose lock camera is on

// The running script's character and whether the player is part of its move: the character itself, linked to it
// (a kill move, grab or lock), or its AI's target.
bool PlayerInMove(uint8_t** instigator)
{
    uint8_t* player = PlayerObject();
    *instigator = nullptr;
    if (!player)
        return true;
    __try {
        uint8_t* context = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptContext));
        uint8_t* c = context ? *reinterpret_cast<uint8_t**>(context + 0x68) : nullptr;
        *instigator = c;
        if (!c || c == player)
            return true;
        const uint32_t playerId = *reinterpret_cast<uint32_t*>(player + 4);
        if (uint8_t* link = *reinterpret_cast<uint8_t**>(context + 0x23C))
            if (*reinterpret_cast<uint32_t*>(link + 0xC) == playerId || *reinterpret_cast<uint32_t*>(link + 0x10) == playerId)
                return true;
        uint8_t* brain = *reinterpret_cast<uint8_t**>(c + kCharacterAI);
        return brain && *reinterpret_cast<uint8_t**>(brain + kAITarget) == player;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// Whether an effect (on: begun; off: ended) passes, keeping its owner so that its end gets through.
bool EffectPasses(bool on, uint8_t*& owner)
{
    if (!g_Active || GetSettings().hvvOthersSlowMotion)
        return true;
    uint8_t* by = nullptr;
    const bool mine = PlayerInMove(&by);
    if (on) {
        if (!mine)
            return false;
        owner = by;
        return true;
    }
    if (by == owner) {
        owner = nullptr;
        return true;
    }
    return mine;
}

void __cdecl ScriptSlowMotion(float scale, int rampIn, int hold, int rampOut)
{
    if (*reinterpret_cast<volatile uint8_t*>(uintptr_t(kScriptEffectsOff)))
        return;
    if (!EffectPasses(scale < 1.0f, g_SlowOwner))
        return;
    reinterpret_cast<void(__cdecl*)(float, int, int, int)>(uintptr_t(kSetSlowMotion))(scale, rampIn, hold, rampOut);
}

void __cdecl LockCamera(uint32_t on, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
    uint32_t a8, uint32_t a9)
{
    if (!EffectPasses((on & 0xFF) != 0, g_LockCameraOwner))
        return;
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        uint32_t)>(uintptr_t(kCombatCamera))(on, a2, a3, a4, a5, a6, a7, a8, a9);
}

// Each start of a match (the game image is fresh): the hooks, the code checked first.
void InstallEffectHooks()
{
    g_SlowOwner = g_LockCameraOwner = nullptr;
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kScriptSlowMotion)), kScriptSlowMotionBytes,
            sizeof kScriptSlowMotionBytes) == 0)
        PatchJump(kScriptSlowMotion, reinterpret_cast<const void*>(&ScriptSlowMotion));
    else
        LOG_WARN("HvV: the slow-motion call is not as expected; other fighters' slow motion reaches the player");
    int hooked = 0;
    for (uint32_t site : kCombatCameraCalls) {
        const uint8_t* at = reinterpret_cast<const uint8_t*>(uintptr_t(site));
        int32_t rel;
        std::memcpy(&rel, at + 1, 4);
        if (at[0] != 0xE8 || site + 5 + uint32_t(rel) != kCombatCamera)
            continue;
        PatchCall(site, reinterpret_cast<const void*>(&LockCamera), 5);
        ++hooked;
    }
    if (hooked != int(std::size(kCombatCameraCalls)))
        LOG_WARN("HvV: %d of %zu saber-lock camera calls hooked", hooked, std::size(kCombatCameraCalls));
}

// A slowing whose owner fell or went (its restore will never come): time runs again.
void CheckSlowMotion()
{
    if (!g_SlowOwner)
        return;
    const int i = FighterOf(g_SlowOwner);
    if (i >= 0 && Standing(g_Fighters[i]))
        return;
    g_SlowOwner = nullptr;
    reinterpret_cast<void(__cdecl*)()>(uintptr_t(kCancelSlowMotion))();
}

// --- The profile across the match ---------------------------------------------------------------------------------
// A level change is a reboot; the game keeps the player's profile across one in `Z:\persist.tmp` (cache\persist.tmp):
// saved before it reboots (0x24D300, thiscall on the profile [0x7E8F24], which skips the write without a profile's
// data at +0x3C) and read by the next start only when its arguments say v_sessionCreated:1, which also lands the
// menus on the main menu (0x2FAFA0) instead of the title. A match started from the menus brings them along, with the
// controller and memory unit in use (the session settings [[0x66F7A4] +4]: +0x84, +0x80), and goes back to the main
// menu the same way.
constexpr uint32_t kProfile = 0x007E8F24;
constexpr uint32_t kSaveProfile = 0x0024D300;
constexpr uint32_t kSessionSettings = 0x0066F7A4;

// Saves the profile for the next start; false without one (a start from Default_Xbox.cfg, before the menus).
bool SaveProfile(int& controller, int& memoryUnit)
{
    __try {
        uint8_t* profile = *reinterpret_cast<uint8_t**>(uintptr_t(kProfile));
        if (!profile || !*reinterpret_cast<void**>(profile + 0x3C))
            return false;
        if (!reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(kSaveProfile))(profile, nullptr))
            return false;
        uint8_t* settings = *reinterpret_cast<uint8_t**>(*reinterpret_cast<uintptr_t*>(uintptr_t(kSessionSettings)) + 4);
        controller = settings ? *reinterpret_cast<int*>(settings + 0x84) : 0;
        memoryUnit = settings ? *reinterpret_cast<int*>(settings + 0x80) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The arguments that carry the profile into the next start (empty without one).
std::string SessionArguments()
{
    int controller = 0, memoryUnit = 0;
    if (!SaveProfile(controller, memoryUnit))
        return {};
    return " v_sessionCreated:1 playlicensemovie:0 title:0 controller:" + std::to_string(controller) +
        " memoryunit:" + std::to_string(memoryUnit);
}

// After the end: back to the main menu with the profile, for a match started from the menus; one started from
// Default_Xbox.cfg (whose map: the game would read again) plays again.
void BackToMenu()
{
    std::string value;
    const std::string session = BootArgument("v_sessionCreated", value) && value == "1" ? SessionArguments() : "";
    if (session.empty()) {
        LOG_INFO("HvV: the match again (it was not started from the menus)");
        kernel::RestartMission();
        return;
    }
    LOG_INFO("HvV: back to the main menu");
    kernel::LaunchGame("map: v_shellBetweenLevels:0 v_versusMode:0 v_bonusMissionMode:0 "
        "v_challengeMissionMode:0 v_checkpointIndex:-1" + session);
}

// --- The intro and the end ---------------------------------------------------------------------------------------

uint8_t* GameOptions()
{
    auto* services = *reinterpret_cast<uint8_t**>(uintptr_t(kEngineServices));
    auto* core = services ? *reinterpret_cast<uint8_t**>(services + 0x38) : nullptr;
    return core ? *reinterpret_cast<uint8_t**>(core + 8) : nullptr;
}

// Everyone holds still (the game's aiDisabled; the player's controls held back), or acts again.
void HoldEveryone(bool hold)
{
    if (uint8_t* options = GameOptions())
        options[kOptionsAiDisabled] = hold ? 1 : 0;
    input::HoldPlayerInput(hold);
}

// A character plays its Versus intro the way the duel starts one (0x27B550): its sequence named <...>_Intro, found
// in its script's list ([char +0x434] +4: +8 count, +0xC items, each named at +0xC; the duel's lookup 0x150D60),
// started with ICharacter's play (0x150E30, the Jedi's vfunc +0x314; its +0x410 is a reaction handler that
// ignored the intro). Yoda has none: one of his "flavour" idles instead.
void PlayIntro(uint8_t* character)
{
    uint8_t* script = Field<uint8_t*>(character, kCharacterScript);
    uint8_t* list = script ? Field<uint8_t*>(script, 4) : nullptr;
    if (!list)
        return;
    const int count = Field<int>(list, 8);
    uint8_t** items = Field<uint8_t**>(list, 0xC);
    uint8_t* flavour = nullptr; // for one without an intro (Yoda): its first "flavour" idle (Idle_Flavor_v1...)
    // a custom character's own first (<costume>_Intro): its base's class may hold other fighters' intros too
    const CustomCharacter* custom = CustomCharacterOf(character);
    const std::string own = custom ? custom->costume + "_Intro" : std::string();
    for (int i = 0; i < count && !own.empty(); ++i) {
        const char* name = items[i] ? Field<const char*>(items[i], 0xC) : nullptr;
        if (name && _stricmp(name, own.c_str()) == 0) {
            reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
                character, nullptr, items[i], 0.0f, 0, 0, 1.0f);
            return;
        }
    }
    for (int i = 0; i < count; ++i) {
        const char* name = items[i] ? Field<const char*>(items[i], 0xC) : nullptr;
        const size_t length = name ? strlen(name) : 0;
        if (length > 6 && _stricmp(name + length - 6, "_Intro") == 0) {
            flavour = items[i];
            break;
        }
        if (!flavour && length >= 11 && _strnicmp(name, "Idle_Flavor", 11) == 0)
            flavour = items[i];
    }
    if (flavour)
        reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
            character, nullptr, flavour, 0.0f, 0, 0, 1.0f);
}

// A camera placement (rows right, up, forward, position) at `eye` looking at `target`, up +Y.
void LookAt(const float eye[3], const float target[3], float m[16])
{
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f)
        v /= fl > 0 ? fl : 1;
    float r[3] = { f[2], 0, -f[0] }; // up x forward
    const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    for (float& v : r)
        v /= rl > 0 ? rl : 1;
    const float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    const float rows[16] = { r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, eye[0], eye[1], eye[2], 1 };
    std::copy(std::begin(rows), std::end(rows), m);
}

float Smooth(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

ULONGLONG IntroLength() { return kShotsLength + kToPlayer; }

// The intro's shot `t` ms in: 0 the heroes' line from the front, 1 along it, 2 and 3 the villains'; -1 past them.
// `a`: how far through the shot (0 to 1).
int IntroShotAt(ULONGLONG t, float* a = nullptr)
{
    if (t >= kShotsLength)
        return -1;
    const ULONGLONG in = t % kSideShots;
    const bool close = in >= kFrontShot;
    if (a)
        *a = close ? float(in - kFrontShot) / kCloseShot : float(in) / kFrontShot;
    return int(t / kSideShots) * 2 + (close ? 1 : 0);
}

// The intro's camera `t` ms in; false past its shots (the way back blends towards the game's own).
bool IntroShot(ULONGLONG t, float m[16])
{
    float along = 0;
    const int shot = IntroShotAt(t, &along);
    if (shot < 0)
        return false;
    const int side = shot / 2;
    const float a = Smooth(along);
    const float floorY = kDevMapFloorY;
    const float lineZ = kDevMapMiddleZ + (side == 0 ? -g_LinesApart / 2 : g_LinesApart / 2);
    const float facing = side == 0 ? 1.0f : -1.0f; // the line faces +z (heroes) or -z
    const float x = kDevMapMiddleX;
    if (shot % 2 == 0) { // from the front, near, a little low, panning along the line (each fighter's intro in turn)
        const float half = (SideSize(side) - 1) / 2.0f * g_LineGap;
        const float along = (side == 0 ? -1.0f : 1.0f) * half * (0.75f - 1.5f * a);
        const float eye[3] = { x + along * 0.8f, floorY + 68, lineZ + facing * 230 };
        const float target[3] = { x + along, floorY + 52, lineZ };
        LookAt(eye, target, m);
    } else { // close, from one end of the line, along it at chest height
        const float end = (side == 0 ? 1.0f : -1.0f) * ((SideSize(side) - 1) / 2.0f * g_LineGap + 110);
        const float eye[3] = { x + end - (side == 0 ? 35 : -35) * a, floorY + 72, lineZ + facing * (85 + 15 * a) };
        const float target[3] = { x - end * 0.5f, floorY + 58, lineZ - facing * 10 };
        LookAt(eye, target, m);
    }
    return true;
}

// Between two camera placements (`a` 0 to 1): positions and aims blended. `out` may be `to`.
void Blend(const float from[16], const float to[16], float a, float out[16])
{
    float eye[3], target[3];
    for (int k = 0; k < 3; ++k) {
        eye[k] = from[12 + k] + (to[12 + k] - from[12 + k]) * a;
        const float f = from[12 + k] + from[8 + k] * 200, t = to[12 + k] + to[8 + k] * 200;
        target[k] = f + (t - f) * a;
    }
    LookAt(eye, target, out);
}

// The end: the top five lined up facing the camera, first in the middle (as Battlefront's end screen); the top three
// up front, fourth and fifth well behind them and to the sides (smaller in the picture).
constexpr float kPodium[5][2] = { { 0, 0 }, { -95, 30 }, { 95, 30 }, { -170, 110 }, { 170, 110 } }; // x, back
constexpr float kPodiumZ = 0; // relative to the floor's middle

// The podium stands just in front of where the heroes' line stood, its back row on the line (open floor on any map:
// behind it can be a wall), facing the villains' side, where the camera is.
float PodiumZ() { return kDevMapMiddleZ + kPodiumZ - g_LinesApart / 2 + kPodium[4][1]; }

void PodiumPlace(int rank, float (&m)[16])
{
    // facing +z (the camera): its left is -x on screen's right, so the podium's x is turned over (second on the left);
    // on the floor itself: a fall would stop the intro each plays there
    Transform(kDevMapMiddleX - kPodium[rank][0], PodiumZ() - kPodium[rank][1], 0, m, 1.0f);
}

void EndCamera(float m[16])
{
    const float back = std::min(290.0f, g_LinesApart - kPodium[4][1] - 30); // short of the villains' line
    const float a = Smooth(float(GameTime() - g_EndAt) / 4000.0f);
    const float eye[3] = { kDevMapMiddleX, kDevMapFloorY + 75 - 10 * a, PodiumZ() + back - 40 * a };
    const float target[3] = { kDevMapMiddleX, kDevMapFloorY + 55, PodiumZ() - 30 };
    LookAt(eye, target, m);
}

// Whether the line from `a` to `b` passes no collision box of the map (always on the floor, which has none).
bool LineOfSight(const float a[3], const float b[3])
{
    const CustomMap* map = DevMapCustomMap();
    return !map || CustomMapLineOfSight(*map, a, b);
}

// The kill camera (as Battlefront's): from the side of the player's body the game's camera was on when it fell
// (a place the camera could be: no wall), nearer the body and a little higher, turned to look at its killer (the
// nearest enemy when no one was credited; the body when none is near). While the player is out of lives it follows
// a teammate still fighting, from the side it is on.
int NearestEnemy(const float at[3], float within)
{
    int best = -1;
    for (int i = 0; i < g_Count; ++i) {
        if (SideOf(i) == SideOf(g_Player) || !Standing(g_Fighters[i]))
            continue;
        const float* p = Position(g_Fighters[i].object);
        const float d = std::hypot(p[0] - at[0], p[2] - at[2]);
        if (d < within) {
            within = d;
            best = i;
        }
    }
    return best;
}

void KillView(float m[16])
{
    if (g_Corpse && ObjectById(g_CorpseId) == g_Corpse)
        std::memcpy(g_CorpseAt, Position(g_Corpse), sizeof g_CorpseAt);
    float target[3] = { g_CorpseAt[0], g_CorpseAt[1] + 20, g_CorpseAt[2] };
    float eye[3];
    if (g_Fighters[g_Player].out && !g_PlayerFell) { // out: a teammate's fight
        const float* from = g_KillView + 12;
        for (int i = SideStart(SideOf(g_Player)); i < SideEnd(SideOf(g_Player)); ++i) {
            if (i == g_Player || !Standing(g_Fighters[i]) || g_Fighters[i].out)
                continue;
            const float* p = Position(g_Fighters[i].object);
            const float dx = from[0] - p[0], dz = from[2] - p[2], d = std::max(1.0f, std::hypot(dx, dz));
            const float want[3] = { p[0] + dx / d * 220, p[1] + 95, p[2] + dz / d * 220 };
            for (int k = 0; k < 3; ++k)
                eye[k] = from[k] + (want[k] - from[k]) * 0.05f; // eased, frame by frame
            const float look[3] = { p[0], p[1] + 45, p[2] };
            LookAt(eye, look, m);
            return;
        }
        LookAt(from, target, m);
        return;
    }
    int look = g_PlayerKiller >= 0 && Standing(g_Fighters[g_PlayerKiller]) ? g_PlayerKiller : -1;
    if (look < 0)
        look = NearestEnemy(g_CorpseAt, 900);
    if (look >= 0) {
        const float* k = Position(g_Fighters[look].object);
        target[0] = k[0];
        target[1] = k[1] + 50;
        target[2] = k[2];
    }
    // the places tried: the first (on the game camera's side), then around the body, then above it; the one last
    // chosen is kept while it still sees both the body and the target (no jumping between places)
    const float body[3] = { g_CorpseAt[0], g_CorpseAt[1] + 30, g_CorpseAt[2] };
    auto place = [&](int i, float (&out)[3]) {
        if (i == 0) {
            for (int k = 0; k < 3; ++k)
                out[k] = g_DeathView[12 + k] + (g_CorpseAt[k] - g_DeathView[12 + k]) * 0.35f;
            out[1] = std::max(out[1], g_CorpseAt[1] + 70) + 25;
        } else if (i <= 8) {
            const float angle = float(i - 1) * kPi / 4;
            out[0] = g_CorpseAt[0] + std::sin(angle) * 150;
            out[1] = g_CorpseAt[1] + 95;
            out[2] = g_CorpseAt[2] + std::cos(angle) * 150;
        } else {
            out[0] = g_CorpseAt[0] + 20;
            out[1] = g_CorpseAt[1] + 140;
            out[2] = g_CorpseAt[2] - 20;
        }
    };
    auto sees = [&](const float (&from)[3]) { return LineOfSight(body, from) && LineOfSight(from, target); };
    if (g_KillEye >= 0) {
        place(g_KillEye, eye);
        if (!sees(eye))
            g_KillEye = -1;
    }
    for (int i = 0; g_KillEye < 0 && i <= 9; ++i) {
        place(i, eye);
        if (sees(eye) || i == 9)
            g_KillEye = i;
    }
    place(g_KillEye, eye);
    LookAt(eye, target, m);
}

// The camera during the intro, on the way back to the game's, at the player's death and at the end.
bool HvvCamera(float m[16], float)
{
    if (!g_Active)
        return false;
    if (g_Phase == Phase::End) {
        EndCamera(m);
        return true;
    }
    if (g_Phase == Phase::Fight) {
        const ULONGLONG now = GameTime();
        if (g_PlayerFell || g_Fighters[g_Player].out) {
            if (!g_DeathViewSet) {
                std::copy(m, m + 16, g_DeathView);
                g_DeathViewSet = true;
            }
            float view[16];
            KillView(view);
            const float a = g_PlayerFell ? Smooth(float(now - g_PlayerDownAt) / kKillCamIn) : 1.0f;
            Blend(g_DeathView, view, a, m);
            std::copy(m, m + 16, g_KillView);
            return true;
        }
        if (g_BackAt && now - g_BackAt < kKillCamBack) { // back to the game's camera
            Blend(g_KillView, m, Smooth(float(now - g_BackAt) / kKillCamBack), m);
            return true;
        }
        return false;
    }
    if (g_Phase != Phase::Intro)
        return false;
    const ULONGLONG t = GameTime() - g_IntroAt;
    if (IntroShot(t, m)) {
        std::copy(m, m + 16, g_LastShot);
        return true;
    }
    const ULONGLONG back = kShotsLength;
    Blend(g_LastShot, m, Smooth(float(t - back) / kToPlayer), m); // back to the game's own camera
    return true;
}

std::vector<int> Ranking()
{
    std::vector<int> order(g_Count);
    for (int i = 0; i < g_Count; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [](int a, int b) { return g_Fighters[a].score > g_Fighters[b].score; });
    return order;
}

// The end begins: everyone holds still, the top five come to the podium (one that is down comes back there), the
// others go far away, and the five play their intros.
// The end: every character of the match (fighters, the player, the fallen) goes out of sight, and the top five
// stand on the podium as fresh characters of their classes: the fighters themselves would bring their fight along
// (lying from a blow, turned their own way, mid-move), and a character placed where it falls stops its intro.
void StartEnd(ULONGLONG now)
{
    g_Phase = Phase::End;
    g_EndAt = now;
    HoldEveryone(true);
    std::vector<int> order = Ranking();
    g_Top.assign(order.begin(), order.begin() + std::min(5, g_Count));
    int away = 0;
    for (uint8_t* c : LevelCharacters()) {
        float m[16];
        Transform(kDevMapMiddleX + 5000 + 250 * float(away % 20), kDevMapMiddleZ - 4000 - 250 * float(away / 20), 0, m);
        VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(c, kPlace)(c, nullptr, m);
        Field<uint8_t>(c, kCharacterInvincible) = 1;
        ++away;
    }
    for (size_t rank = 0; rank < 5; ++rank) {
        g_Podium[rank] = nullptr;
        if (rank >= g_Top.size())
            continue;
        float m[16];
        PodiumPlace(int(rank), m);
        std::string error;
        uint8_t* c = SpawnCharacterAt(g_Fighters[g_Top[rank]].className.c_str(), "", "", "", SpawnSide::Default, m, error);
        if (!c) {
            LOG_WARN("HvV: %s is not on the podium (%s)", ShownName(g_Top[rank]).c_str(), error.c_str());
            continue;
        }
        FullHealth(c);
        Field<uint8_t>(c, kCharacterInvincible) = 1;
        g_Podium[rank] = c;
    }
    g_PodiumIntros = true; // a moment after they stand (see HvvFrame)
    LOG_INFO("HvV: the end; first %s with %d", ShownName(g_Top[0]).c_str(), g_Fighters[g_Top[0]].score);
}


// --- Battlefront ---------------------------------------------------------------------------------------------------

// Where a fighter of `side` comes back: one of its side's posts or its start, picked at random among those with no
// enemy near (kSpawnClear: nobody comes back into a fight, or onto a camp); with none clear, the one furthest from the
// enemies. A step off it, facing the middle of the posts.
constexpr float kSpawnClear = 650.0f;

float NearestEnemyTo(int side, float x, float z)
{
    float nearest = 1e9f;
    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        if (SideOf(i) == side || f.out || !Standing(f))
            continue;
        const float* p = Position(f.object);
        nearest = std::min(nearest, std::hypot(p[0] - x, p[2] - z));
    }
    return nearest;
}

void ConquestPlace(int side, float (&m)[16])
{
    std::vector<std::pair<float, float>> places;
    for (const Post& post : g_Posts)
        if (post.owner == side)
            places.push_back({ post.x, post.z });
    places.push_back({ g_TeamStart[side][0], g_TeamStart[side][1] });
    std::vector<size_t> clear;
    size_t furthest = 0;
    float furthestDistance = -1;
    for (size_t k = 0; k < places.size(); ++k) {
        const float d = NearestEnemyTo(side, places[k].first, places[k].second);
        if (d >= kSpawnClear)
            clear.push_back(k);
        if (d > furthestDistance) {
            furthestDistance = d;
            furthest = k;
        }
    }
    const size_t pick = clear.empty() ? furthest : clear[size_t(std::rand()) % clear.size()];
    float x = places[pick].first, z = places[pick].second;
    float lift = 60.0f;
    if (DevMapCustomMap()) {
        // a custom map's ground is not flat around a post (walls, steps, low ceilings): one of the map's own clear
        // places near it (Spawns=, open floor checked by its converter), else the post itself; set down from lower
        // (a low ceiling over the floor: a body dropped from 60 above it met the ceiling and stayed stuck)
        std::vector<size_t> picks;
        size_t nearest = g_Spawns.size();
        float nearestDistance = 1e9f;
        for (size_t k = 0; k < g_Spawns.size(); ++k) {
            const float d = std::hypot(g_Spawns[k].first - x, g_Spawns[k].second - z);
            if (d < kSpawnNear)
                picks.push_back(k);
            if (d < nearestDistance) {
                nearestDistance = d;
                nearest = k;
            }
        }
        if (picks.empty() && nearest < g_Spawns.size())
            picks.push_back(nearest); // none that near: the nearest
        if (!picks.empty()) {
            const auto& s = g_Spawns[picks[size_t(std::rand()) % picks.size()]];
            x = s.first;
            z = s.second;
        }
        lift = kSpawnLift;
    } else {
        const float angle = float(std::rand()) / RAND_MAX * 2 * kPi, reach = 60.0f + float(std::rand()) / RAND_MAX * 140.0f;
        x = std::clamp(x + std::sin(angle) * reach, g_Bounds[0] + g_Margin, g_Bounds[2] - g_Margin);
        z = std::clamp(z + std::cos(angle) * reach, g_Bounds[1] + g_Margin, g_Bounds[3] - g_Margin);
    }
    float cx = 0, cz = 0;
    for (const Post& post : g_Posts) {
        cx += post.x / float(g_Posts.size());
        cz += post.z / float(g_Posts.size());
    }
    Transform(x, z, std::atan2(cx - x, cz - z), m, lift);
}

// A post's letter (A, B, C...).
std::string PostName(size_t k)
{
    return std::string(1, char('A' + k % 26));
}

// The posts: the fighters near each turn it towards their side (when only one side is there), a side's own once
// turned all the way, neutral on the way between. Points for the fighters who took it.
void UpdatePosts(ULONGLONG now)
{
    const float dt = g_LastPosts ? float(std::min<ULONGLONG>(now - g_LastPosts, 500)) : 0.0f;
    g_LastPosts = now;
    for (size_t k = 0; k < g_Posts.size(); ++k) {
        Post& post = g_Posts[k];
        int count[2] = {};
        std::vector<int> inside[2];
        for (int i = 0; i < g_Count; ++i) {
            Fighter& f = g_Fighters[i];
            if (f.out || !Standing(f) || (i == g_Player && g_PlayerFell))
                continue;
            const float* p = Position(f.object);
            if (std::hypot(p[0] - post.x, p[2] - post.z) > kPostRadius)
                continue;
            ++count[SideOf(i)];
            inside[SideOf(i)].push_back(i);
        }
        if ((count[0] > 0) == (count[1] > 0)) // nobody, or contested
            continue;
        const int side = count[0] > 0 ? 0 : 1;
        const float step = dt / kCaptureMs * float(std::min(count[side], kMostCapturers)) * (side == 0 ? -1.0f : 1.0f);
        const int before = post.owner;
        post.hold = std::clamp(post.hold + step, -1.0f, 1.0f);
        if (post.owner == 1 - side && (side == 0 ? post.hold < 0.0f : post.hold > 0.0f))
            post.owner = -1; // neutralised
        if (post.owner == -1 && std::abs(post.hold) >= 1.0f)
            post.owner = side;
        if (post.owner == before || post.owner != side)
            continue;
        for (int i : inside[side]) {
            g_Fighters[i].score += kCapturePoints;
            if (i == g_Player)
                NoticeFor("COMMAND POST " + PostName(k) + " CAPTURED", "", kCapturePoints, side, now);
        }
        if (std::find(inside[side].begin(), inside[side].end(), g_Player) == inside[side].end())
            g_SidePoints[side] += kCapturePoints;
        LOG_INFO("Battlefront: the %s take command post %s", side == 0 ? "clones" : "droids", PostName(k).c_str());
    }
    // a side holding fewer posts loses tickets: faster when it holds none and the other all
    int held[2] = {};
    for (const Post& post : g_Posts)
        if (post.owner >= 0)
            ++held[post.owner];
    if (held[0] == held[1]) {
        g_LastBleed = now;
        return;
    }
    const int losing = held[0] < held[1] ? 0 : 1;
    const ULONGLONG every = held[losing] == 0 && held[1 - losing] == int(g_Posts.size()) ? kBleedAllMs : kBleedMs;
    if (!g_LastBleed)
        g_LastBleed = now;
    if (now - g_LastBleed >= every && g_Lives[losing] > 0) {
        g_LastBleed = now;
        --g_Lives[losing];
    }
}

// The player's points buy a hero (B, as the troopers' kit asks: BattlefrontHeroRequest) while its side has none.
bool PlayerHeroReady()
{
    const Fighter& me = g_Fighters[g_Player];
    return g_Conquest && !me.hero && !me.out && !g_PlayerFell && g_HeroOf[SideOf(g_Player)] < 0 &&
        !g_HeroPool[SideOf(g_Player)].empty() && me.score - g_PlayerHeroSpent >= g_HeroCost;
}

std::string PickHero(int side)
{
    const std::vector<std::string>& pool = g_HeroPool[side];
    return pool.empty() ? std::string() : pool[size_t(std::rand()) % pool.size()];
}

// The player becomes its side's hero where it stands (the live player change).
void BecomeHero(ULONGLONG now)
{
    Fighter& me = g_Fighters[g_Player];
    const std::string hero = PickHero(SideOf(g_Player));
    SetPlayerClass(nullptr);
    SetPlayerClass(hero.c_str(), false);
    std::string error;
    if (!ReplacePlayer(error)) {
        LOG_WARN("Battlefront: no hero for you (%s)", error.c_str());
        SetPlayerClass(nullptr);
        SetPlayerClass(me.className.c_str(), false);
        return;
    }
    uint8_t* player = PlayerObject();
    Track(me, player);
    FullHealth(player);
    me.className = hero;
    me.hero = true;
    g_HeroOf[SideOf(g_Player)] = g_Player;
    g_PlayerHeroSpent += g_HeroCost;
    Shield(me, now);
    RefocusCameraOnPlayer();
    NoticeFor("YOU ARE " + DisplayNameOf(hero), "", 0, SideOf(g_Player), now);
    LOG_INFO("Battlefront: you are %s", hero.c_str());
}

// A fighter that falls as a hero comes back as its trooper; its side may call another.
void HeroFell(int i)
{
    Fighter& f = g_Fighters[i];
    if (!f.hero)
        return;
    f.hero = false;
    f.className = f.troopClass;
    if (g_HeroOf[SideOf(i)] == i)
        g_HeroOf[SideOf(i)] = -1;
}

} // namespace

void InstallHvv()
{
    g_Active = false;
    g_InLevelAt = g_WinnerAt = g_LastEngage = 0;
    g_Woken = false;
    g_PlayerFell = false;
    g_Winner = -1;
    g_Phase = Phase::Waiting;
    g_IntroAt = g_FightAt = g_EndAt = 0;
    g_MenuAsked = false;
    g_ShotPlayed = -1;
    g_Feed.clear();
    g_Notice = {};
    g_Top.clear();
    for (Fighter& f : g_Fighters)
        f = {};
    g_Conquest = false;
    g_Posts.clear();
    g_PostMarkers.clear();
    g_HeroPool[0].clear();
    g_HeroPool[1].clear();
    g_HeroOf[0] = g_HeroOf[1] = -1;
    g_SidePoints[0] = g_SidePoints[1] = 0;
    g_PlayerHeroSpent = 0;
    g_PlayerHeroAsked = false;
    g_LastBleed = g_LastPosts = 0;
    SetFatalHitGuard(nullptr);
    SetDamageObserver(nullptr);
    SetDamageFilter(nullptr);
    std::string lineup, value;
    if (!DevMapActive() || !BootArgument("hvv", lineup)) {
        RestorePlayerChoice();
        return;
    }
    const std::vector<std::string> classes = Split(lineup, ',');
    if (classes.size() < 2 || classes.size() > size_t(kMaxFighters)) {
        LOG_WARN("HvV: the line-up needs 2 to %d classes, not %zu ('%s')", kMaxFighters, classes.size(), lineup.c_str());
        RestorePlayerChoice();
        return;
    }
    g_Count = int(classes.size());
    g_Heroes = BootArgument("hvvheroes", value) ? std::atoi(value.c_str()) : g_Count / 2;
    g_Heroes = std::clamp(g_Heroes, std::max(1, g_Count - kMaxSide), std::min(kMaxSide, g_Count - 1));
    g_Player = BootArgument("hvvplayer", value) ? std::atoi(value.c_str()) : 0;
    if (g_Player < 0 || g_Player >= g_Count)
        g_Player = 0;
    g_StartLives = BootArgument("hvvlives", value) ? std::max(1, std::atoi(value.c_str())) : kDefaultLives;
    g_Lives[0] = g_Lives[1] = g_StartLives;
    g_Health = BootArgument("hvvhealth", value) ? std::max(1, std::atoi(value.c_str())) : kDefaultHealth;
    g_IntroWanted = !BootArgument("hvvintro", value) || value != "0";
    g_LineGap = kLineGap;
    g_LinesApart = kLinesApart;
    const float floorBounds[4] = { kFloorX0, kFloorZ0, kFloorX1, kFloorZ1 };
    std::copy(std::begin(floorBounds), std::end(floorBounds), g_Bounds);
    g_Margin = kFloorMargin;
    g_Spawns.clear();
    g_MapName.clear();
    if (const CustomMap* map = DevMapCustomMap()) { // its lines (turned to face along z), its spawns and bounds
        g_LineGap = map->lineGap;
        g_LinesApart = std::hypot(map->villains[0] - map->heroes[0], map->villains[1] - map->heroes[1]);
        CustomMapBounds(*map, g_Bounds);
        g_Margin = map->hasBounds ? 0.0f : kFloorMargin;
        for (const auto& s : map->spawns) {
            float x, z;
            CustomMapToWorld(*map, s.first, s.second, x, z);
            g_Spawns.push_back({ x, z });
        }
        g_MapName = map->name;
    }
    g_Conquest = BootArgument("bf", value) && value == "1";
    if (g_Conquest) {
        g_IntroWanted = false;
        if (!BootArgument("hvvlives", value))
            g_Lives[0] = g_Lives[1] = g_StartLives = kDefaultTickets;
        g_HeroCost = BootArgument("bfherocost", value) ? std::max(0, std::atoi(value.c_str())) : kHeroPoints;
        if (BootArgument("bfhealth", value)) {
            const std::vector<std::string> h = Split(value, ',');
            if (h.size() >= 2) {
                g_TroopHealth = std::max(1, std::atoi(h[0].c_str()));
                g_HeroHealth = std::max(1, std::atoi(h[1].c_str()));
            }
        }
        for (int side = 0; side < 2; ++side) { // its heroes: the classes this game has
            if (!BootArgument(side == 0 ? "bfheroes0" : "bfheroes1", value))
                continue;
            for (const std::string& c : Split(value, ','))
                if (const char* name = RegisteredClassName(c.c_str()); name && ClassHasBody(name))
                    g_HeroPool[side].push_back(name);
        }
        const CustomMap* map = DevMapCustomMap();
        if (map && !map->commandPosts.empty()) {
            for (const auto& c : map->commandPosts) {
                Post post;
                CustomMapToWorld(*map, c.first, c.second, post.x, post.z);
                g_Posts.push_back(post);
            }
        } else {
            for (const auto& c : kDevPosts) {
                Post post;
                post.x = c[0];
                post.z = c[1];
                g_Posts.push_back(post);
            }
        }
        if (map && map->hasStarts) {
            CustomMapToWorld(*map, map->cloneStart[0], map->cloneStart[1], g_TeamStart[0][0], g_TeamStart[0][1]);
            CustomMapToWorld(*map, map->droidStart[0], map->droidStart[1], g_TeamStart[1][0], g_TeamStart[1][1]);
        } else if (map) { // its lines
            CustomMapToWorld(*map, map->heroes[0], map->heroes[1], g_TeamStart[0][0], g_TeamStart[0][1]);
            CustomMapToWorld(*map, map->villains[0], map->villains[1], g_TeamStart[1][0], g_TeamStart[1][1]);
        } else {
            std::memcpy(g_TeamStart, kDevStarts, sizeof g_TeamStart);
        }
        for (Post& post : g_Posts)
            post.y = kDevMapFloorY;
        // each side starts with the post nearest its start
        for (int side = 0; side < 2; ++side) {
            Post* nearest = nullptr;
            for (Post& post : g_Posts)
                if (post.owner < 0 && (!nearest || std::hypot(post.x - g_TeamStart[side][0], post.z - g_TeamStart[side][1]) <
                        std::hypot(nearest->x - g_TeamStart[side][0], nearest->z - g_TeamStart[side][1])))
                    nearest = &post;
            if (nearest) {
                nearest->owner = side;
                nearest->hold = side == 0 ? -1.0f : 1.0f;
            }
        }
    }

    DevMapLayout layout;
    for (int i = 0; i < g_Count; ++i) {
        g_Fighters[i].className = classes[i];
        g_Fighters[i].troopClass = classes[i];
        float x, z, yaw;
        LinePlace(i, x, z, yaw);
        if (g_Conquest) { // a group at its side's start
            const int side = SideOf(i), k = i - SideStart(side);
            const float sx = g_TeamStart[side][0], sz = g_TeamStart[side][1];
            const float tx = kDevMapMiddleX, tz = kDevMapMiddleZ;
            yaw = std::atan2(tx - sx, tz - sz);
            x = sx + std::cos(yaw) * (float(k % 4) - 1.5f) * 70.0f - std::sin(yaw) * float(k / 4) * 80.0f;
            z = sz - std::sin(yaw) * (float(k % 4) - 1.5f) * 70.0f - std::cos(yaw) * float(k / 4) * 80.0f;
        }
        if (i == g_Player) {
            layout.start = true;
            layout.startX = x;
            layout.startZ = z;
            layout.startYaw = yaw;
            continue;
        }
        g_Fighters[i].placedId = kDevMapFirstId + uint32_t(i);
        // a battle droid's first costume has no body: the plain droid's
        layout.characters.push_back({ classes[i], g_Fighters[i].placedId, x, z, yaw,
            _stricmp(classes[i].c_str(), "IBattleDroid") == 0 ? "hordeBattleDroid" : "" });
    }
    if (g_Conquest) { // the heroes' classes load with the level (their sounds), placed far off and never woken
        uint32_t loader = 0;
        for (int side = 0; side < 2; ++side)
            for (const std::string& hero : g_HeroPool[side])
                layout.characters.push_back({ hero, kDevMapFirstId + kLoaderIds + loader++,
                    kDevMapMiddleX + 7000.0f + 200.0f * float(loader), kDevMapMiddleZ - 4500.0f, 0.0f });
        for (const Post& post : g_Posts) // the posts' goto markers
            layout.markers.push_back({ post.x, post.z });
        layout.markerReach = kMarkerReach;
    }
    SetDevMapLayout(layout);
    SavePlayerChoice();
    SetPlayerClass(nullptr);
    SetPlayerClass(classes[g_Player].c_str(), false);
    SetFatalHitGuard(&PlayerFatalHit);
    SetDamageObserver(&Damaged);
    SetDamageFilter(&ScaleDamage);
    SetCameraAdjuster(&HvvCamera); // in place of co-op's for this start
    InstallEffectHooks();
    g_Active = true;
    std::srand(unsigned(GetTickCount()));
    std::string heroes, villains;
    for (int i = 0; i < g_Count; ++i)
        (i < g_Heroes ? heroes : villains) += (i == 0 || i == g_Heroes ? "" : ", ") + classes[i];
    LOG_INFO("HvV: %s against %s; the player is %s; %d lives a side, %d health; on %s", heroes.c_str(),
        villains.c_str(), classes[g_Player].c_str(), g_StartLives, g_Health,
        g_MapName.empty() ? "the dev map" : g_MapName.c_str());
}

bool HvvActive() { return g_Active; }

bool HvvFighting() { return g_Active && g_Phase == Phase::Fight; }

bool HvvSameSide(const uint8_t* a, const uint8_t* b)
{
    const int i = FighterOf(a), k = FighterOf(b);
    return g_Active && i >= 0 && k >= 0 && SideOf(i) == SideOf(k);
}

// The game's pause: the time manager (TManager_Time, [[0x68D944] +0x5C]) has a frozen state on its stack (+0x14 set,
// +0x38 its entries: only the pause pushes one, 0xA7A30; see fixes.cpp).
bool GamePaused()
{
    __try {
        uint8_t* services = *reinterpret_cast<uint8_t**>(uintptr_t(0x0068D944));
        uint8_t* time = services ? *reinterpret_cast<uint8_t**>(services + 0x5C) : nullptr;
        return time && time[0x14] && *reinterpret_cast<uint32_t*>(time + 0x38) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

ULONGLONG g_RealTick = 0, g_GameTime = 100000;

void GameTimeFrame()
{
    const ULONGLONG now = GetTickCount64();
    if (g_RealTick && !GamePaused())
        g_GameTime += std::min<ULONGLONG>(now - g_RealTick, 250); // a long frame (a load) counts as a short one
    g_RealTick = now;
}

ULONGLONG GameTime()
{
    return g_GameTime;
}

bool HvvLineOfSight(const float a[3], const float b[3]) { return !g_Active || LineOfSight(a, b); }

HvvState GetHvvState()
{
    HvvState s;
    s.active = g_Active;
    if (!g_Active)
        return s;
    const ULONGLONG now = GameTime();
    s.started = g_Woken;
    s.intro = g_Active && g_Phase == Phase::Intro;
    s.fighting = g_Phase == Phase::Fight;
    s.winner = g_Winner;
    s.player = g_Player;
    s.startLives = g_StartLives;
    s.heroes = g_Heroes;
    s.paused = GamePaused();
    s.playerDown = g_PlayerFell || (g_Player < g_Count && g_Fighters[g_Player].out);
    s.conquest = g_Conquest;
    for (size_t k = 0; k < g_Posts.size(); ++k) {
        HvvState::PostState post;
        post.at[0] = g_Posts[k].x;
        post.at[1] = g_Posts[k].y;
        post.at[2] = g_Posts[k].z;
        post.owner = g_Posts[k].owner;
        post.hold = g_Posts[k].hold;
        post.name = PostName(k);
        s.posts.push_back(post);
    }
    s.heroReady = PlayerHeroReady();
    s.heroCost = g_HeroCost;
    if (g_Player < g_Count)
        s.heroPoints = std::max(0, g_Fighters[g_Player].score - g_PlayerHeroSpent);
    s.board = g_BoardPinned;
    s.fighters.resize(size_t(g_Count));
    for (int side = 0; side < 2; ++side)
        s.lives[side] = g_Lives[side];
    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        auto& o = s.fighters[i];
        o.className = f.className;
        o.name = ShownName(i);
        o.side = SideOf(i);
        o.isPlayer = i == g_Player;
        o.standing = g_Woken && Standing(f) && !f.out;
        o.out = f.out;
        o.placed = g_Woken && Placed(f);
        if (o.placed) {
            const float* p = Position(f.object);
            o.head[0] = p[0];
            o.head[1] = p[1] + MarkerHeight(f.className);
            o.head[2] = p[2];
            std::copy(p, p + 3, o.feet);
            o.health = std::clamp(Field<float>(f.object, kCharacterHealth) / kHealthScale, 0.0f, 1.0f);
            const float maxPower = Field<float>(f.object, kCharacterMaxPower);
            o.force = maxPower > 0 ? std::clamp(Field<float>(f.object, kCharacterPower) / maxPower, 0.0f, 1.0f) : -1.0f;
        }
        o.score = f.score;
        o.defeats = f.defeats;
        o.falls = f.falls;
        o.assists = f.assists;
        o.damage = int(f.damage);
        o.streak = f.streak;
        if (o.standing)
            ++s.standing[SideOf(i)];
    }
    for (const FeedEntry& e : g_Feed) {
        if (now - e.at > kFeedFor)
            continue;
        HvvState::FeedLine line;
        line.killer = e.killer >= 0 ? ShownName(e.killer) : "";
        line.killerSide = e.killer >= 0 ? SideOf(e.killer) : -1;
        line.victim = ShownName(e.victim);
        line.victimSide = SideOf(e.victim);
        line.mine = e.killer == g_Player || e.victim == g_Player;
        line.age = float(now - e.at) / 1000.0f;
        s.feed.push_back(line);
    }
    if (g_Notice.at && now - g_Notice.at < kNoticeFor) {
        s.notice = g_Notice.text;
        s.noticeSub = g_Notice.sub;
        s.noticePoints = g_Notice.points;
        s.noticeSide = g_Notice.side;
        s.noticeAge = float(now - g_Notice.at) / 1000.0f;
    }
    if (g_Phase == Phase::Intro) {
        const ULONGLONG t = now - g_IntroAt;
        if (t < kShotsLength) {
            const int side = int(t / kSideShots);
            s.caption = side == 0 ? "HEROES" : "VILLAINS";
            for (int i = SideStart(side); i < SideEnd(side); ++i)
                s.subcaption += (s.subcaption.empty() ? "" : "   ") + DisplayNameOf(g_Fighters[i].className);
            s.captionSide = side;
            s.captionFade = float(t % kSideShots) / kSideShots;
        }
    } else if (g_Phase == Phase::Fight && g_FightAt && now - g_FightAt < kFightBanner) {
        s.caption = "FIGHT!";
        s.captionFade = float(now - g_FightAt) / kFightBanner;
    }
    if (g_Phase == Phase::End) {
        s.ending = true;
        s.endingAge = float(now - g_EndAt) / 1000.0f;
        s.top = g_Top;
        for (size_t rank = 0; rank < g_Top.size() && rank < 5; ++rank) { // where the podium's characters stand
            auto& o = s.fighters[size_t(g_Top[rank])];
            o.placed = g_Podium[rank] != nullptr;
            if (g_Podium[rank])
                std::copy(Position(g_Podium[rank]), Position(g_Podium[rank]) + 3, o.feet);
        }
    }
    return s;
}

std::vector<std::string> HvvDefaultLineup()
{
    return std::vector<std::string>(std::begin(kDefaultLineup), std::end(kDefaultLineup));
}

std::vector<std::string> HvvFighterClasses()
{
    std::vector<std::string> out;
    for (const DisplayName& n : kNames)
        if (const char* name = RegisteredClassName(n.className); name && ClassHasBody(name))
            out.push_back(name);
    for (const CustomCharacter& custom : CustomCharacters())
        out.push_back(custom.className);
    return out;
}

std::string HvvDisplayName(const std::string& className)
{
    return DisplayNameOf(className);
}

std::string ProfileSessionArguments()
{
    return SessionArguments();
}

bool EndHvvNow()
{
    if (!g_Active || g_Phase != Phase::Fight || g_Winner >= 0)
        return false;
    int score[2] = {};
    for (int i = 0; i < g_Count; ++i)
        score[SideOf(i)] += g_Fighters[i].score;
    g_Winner = g_Lives[0] != g_Lives[1] ? (g_Lives[0] > g_Lives[1] ? 0 : 1) : (score[0] >= score[1] ? 0 : 1);
    g_WinnerAt = GameTime();
    LOG_INFO("HvV: ended from the console; the %s win", g_Winner == 0 ? "heroes" : "villains");
    return true;
}

void SetHvvBoardPinned(bool pinned)
{
    g_BoardPinned = pinned;
}

bool StartHvv(const std::vector<std::string>& lineup, int heroes, int player, int lives, int health, bool intro,
    const std::string& map, std::string& error)
{
    if (!map.empty() && !FindCustomMap(map)) {
        error = "no map " + map + " under mods/maps";
        return false;
    }
    if (lineup.size() < 2 || lineup.size() > size_t(kMaxFighters)) {
        error = "a match needs 2 to " + std::to_string(kMaxFighters) + " fighters";
        return false;
    }
    const int count = int(lineup.size());
    if (heroes <= 0)
        heroes = count / 2;
    if (heroes >= count || heroes > kMaxSide || count - heroes > kMaxSide) {
        error = "each side needs 1 to " + std::to_string(kMaxSide) + " fighters";
        return false;
    }
    std::string text;
    for (const std::string& c : lineup) {
        const char* name = RegisteredClassName(c.c_str());
        if (!name) {
            error = c + ": not a class the game knows";
            return false;
        }
        if (!ClassHasBody(name)) {
            error = std::string(name) + " was cut from the game";
            return false;
        }
        text += (text.empty() ? "" : ",") + std::string(name);
    }
    if (player < 0 || player >= count)
        player = 0;
    StartDevMap("hvv:" + text + " hvvheroes:" + std::to_string(heroes) + " hvvplayer:" + std::to_string(player) +
        " hvvlives:" + std::to_string(lives > 0 ? lives : kDefaultLives) +
        " hvvhealth:" + std::to_string(health > 0 ? health : kDefaultHealth) + (intro ? "" : " hvvintro:0") +
        (map.empty() ? "" : " custommap:" + map) + SessionArguments());
    return true;
}

void HvvFrame()
{
    if (!g_Active)
        return;
    uint8_t* player = PlayerObject();
    if (!player)
        return;
    const ULONGLONG now = GameTime();
    if (!g_InLevelAt)
        g_InLevelAt = now;
    if (!g_Woken) {
        if (now - g_InLevelAt < kWakeAfter)
            return;
        g_Woken = true;
        Track(g_Fighters[g_Player], player);
        FullHealth(player);
        for (size_t k = 0; g_Conquest && k < g_Posts.size(); ++k) { // the posts' goto markers (all, or none used)
            uint8_t* marker = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(
                kDevMapFirstMarkerId + uint32_t(k));
            if (!marker || Field<uint32_t>(marker, 0) != kGotoMarkerVtable) {
                LOG_WARN("HvV: post %zu has no goto marker; the AI hunts instead", k);
                g_PostMarkers.clear();
                break;
            }
            g_PostMarkers.push_back(marker);
        }
        for (int i = 0; i < g_Count; ++i) {
            Fighter& f = g_Fighters[i];
            if (i == g_Player)
                continue;
            uint8_t* placed = nullptr;
            for (uint8_t* c : LevelCharacters())
                if (Field<uint32_t>(c, 4) == f.placedId)
                    placed = c;
            if (!placed) {
                LOG_WARN("HvV: %s (id 0x%X) is not in the level", f.className.c_str(), f.placedId);
                continue;
            }
            Track(f, placed);
            if (Field<float>(placed, kCharacterHealth) <= 0) {
                VirtualOf<void(__fastcall*)(uint8_t*, void*)>(placed, kActivate)(placed, nullptr);
                SettleActivated(placed);
            }
            SetCharacterSide(placed, SideOf(i) == SideOf(g_Player) ? SpawnSide::Ally : SpawnSide::Enemy);
            FullHealth(placed);
        }
        if (g_IntroWanted) {
            g_Phase = Phase::Intro;
            g_IntroAt = now;
            HoldEveryone(true);
            input::TakeHeldPress(); // a press from before (the menu's) does not skip it
            LOG_INFO("HvV: the intro");
        } else {
            g_Phase = Phase::Fight;
            g_FightAt = now;
            EngageAll();
            LOG_INFO("HvV: the fight is on");
        }
        return;
    }
    if (g_Phase == Phase::Intro) {
        if (input::TakeHeldPress() && now - g_IntroAt > 1000) // skipped (A or Start), as the game's Versus intros
            g_IntroAt = now - IntroLength();
        const ULONGLONG t = now - g_IntroAt;
        if (t < kShotsLength) {
            const int shot = IntroShotAt(t);
            if (shot % 2 == 0 && shot != g_ShotPlayed) { // a line's first shot: its fighters play their intros
                g_ShotPlayed = shot;
                for (int i = SideStart(shot / 2); i < SideEnd(shot / 2); ++i)
                    if (Standing(g_Fighters[i]))
                        PlayIntro(g_Fighters[i].object);
            }
        }
        if (t < IntroLength())
            return;
        HoldEveryone(false);
        g_Phase = Phase::Fight;
        g_FightAt = now;
        g_LastEngage = now;
        EngageAll();
        LOG_INFO("HvV: the fight is on");
    }
    if (g_Phase == Phase::End) {
        if (g_PodiumIntros && now - g_EndAt >= 200) { // they stand: their intros
            g_PodiumIntros = false;
            for (uint8_t* c : g_Podium)
                if (c)
                    PlayIntro(c);
        }
        if (!g_MenuAsked && now - g_EndAt >= kMenuAfter) {
            g_MenuAsked = true;
            BackToMenu();
        }
        return;
    }
    CheckSlowMotion();
    if (g_Winner >= 0 && now - g_WinnerAt >= kEndAfter && !g_PlayerFell) {
        StartEnd(now);
        return;
    }

    // The player's fatal hit (kept from the game, whose player must never die: that ends the mission): a fresh
    // fighter of its class takes over the controls at once (the live player change, which removes the old body),
    // held, shielded, out of the AI's sight, where it will come back; where the old body fell, a double of it (an
    // ordinary character) is dealt the death by its killer and dies there (its death plays, as any fighter's). The
    // kill camera meanwhile; then the fighter comes back, a life of its side gone. (Keeping the player's own body to
    // die froze the game once: an object list of its went round in a circle, 0x1F3DB0.)
    Fighter& me = g_Fighters[g_Player];
    if (g_PlayerFell && !g_Swapped) {
        g_Swapped = true;
        g_Corpse = nullptr;
        float fell[16];
        std::memcpy(fell, player + kCharacterTransform, sizeof fell);
        // the old body out of sight first: its removal can take a moment, and it played its own fall beside the
        // double's
        float away[16];
        Transform(kDevMapMiddleX + 9000, kDevMapMiddleZ + 4500, 0, away);
        VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, away);
        std::string error;
        const std::string fallen = me.className; // the body that falls: as it fought (a hero's)
        if (g_Conquest && me.hero) { // it comes back as its trooper
            HeroFell(g_Player);
            SetPlayerClass(nullptr);
            SetPlayerClass(me.className.c_str(), false);
        }
        if (ReplacePlayer(error)) {
            player = PlayerObject();
            Track(me, player);
            // it waits far from the fight (out of the kill camera's picture and the AI's way) until it comes back
            float m[16];
            WaitingPlace(kDevMapMiddleX - 9000, kDevMapMiddleZ + 4500, m);
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
            FullHealth(player);
            Field<uint8_t>(player, kCharacterInvincible) = 1;
            SetIgnoredByAI(player, true);
            input::HoldPlayerInput(true);
            if (uint8_t* body = SpawnCharacterAt(fallen.c_str(), "", "", "", SpawnSide::Default, fell, error)) {
                g_Corpse = body;
                g_CorpseId = Field<uint32_t>(body, 4);
                uint8_t* by = g_PlayerKiller >= 0 && Placed(g_Fighters[g_PlayerKiller])
                    ? g_Fighters[g_PlayerKiller].object : nullptr;
                reinterpret_cast<void(__fastcall*)(uint8_t*, void*, float, uint32_t, uint32_t)>(uintptr_t(kChangeHealth))(
                    body, nullptr, -10 * kHealthScale, 0, uint32_t(reinterpret_cast<uintptr_t>(by)));
            } else {
                LOG_WARN("HvV: no body to fall (%s)", error.c_str());
            }
        } else {
            LOG_WARN("HvV: a fresh fighter could not take over (%s); you get up where you are", error.c_str());
        }
    }
    if (g_PlayerFell && now - g_PlayerDownAt >= kPlayerDown) {
        g_PlayerFell = false;
        Field<uint8_t>(player, kCharacterInvincible) = 0;
        if (g_Lives[SideOf(g_Player)] <= 0) { // out: held, out of the way, until the end
            me.out = true;
            float m[16];
            WaitingPlace(g_Bounds[0] + 100, g_Bounds[1] + 100, m);
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
            Field<uint8_t>(player, kCharacterInvincible) = 1;
            LOG_INFO("HvV: you are out");
        } else {
            --g_Lives[SideOf(g_Player)];
            float m[16];
            SafePlace(g_Player, m);
            VirtualOf<void(__fastcall*)(uint8_t*, void*, const float*)>(player, kPlace)(player, nullptr, m);
            FullHealth(player);
            Shield(me, now);
            SetIgnoredByAI(player, false);
            input::HoldPlayerInput(false);
            // the camera on the new body (a change made during a grapple's camera left it on the old one)
            const int lists = RefocusCameraOnPlayer();
            g_BackAt = now;
            LOG_INFO("HvV: you came back (%d lives left; %d camera lists on you)", g_Lives[SideOf(g_Player)], lists);
            g_RefocusAt = now + 500;
        }
    }
    if (g_RefocusAt && now >= g_RefocusAt) { // once more, after a camera change the game makes late (a grapple's end)
        g_RefocusAt = 0;
        RefocusCameraOnPlayer();
    }

    for (int i = 0; i < g_Count; ++i) {
        Fighter& f = g_Fighters[i];
        if (f.shieldUntil && now >= f.shieldUntil) {
            f.shieldUntil = 0;
            if (Placed(f))
                Field<uint8_t>(f.object, kCharacterInvincible) = 0;
        }
        if (i == g_Player || f.out || g_Winner >= 0)
            continue;
        if (!f.fellAt) {
            if (!Standing(f)) {
                f.fellAt = now;
                Defeated(i, now);
                HeroFell(i);
            }
            continue;
        }
        if (now - f.fellAt < kRespawnAfter)
            continue;
        // Its death has played out: a new one of its class comes back, while its side has lives.
        const int side = SideOf(i);
        if (g_Lives[side] <= 0) {
            f.out = true;
            f.fellAt = 0;
            LOG_INFO("HvV: %s is out", f.className.c_str());
            continue;
        }
        if (g_Conquest && g_HeroOf[side] < 0 && g_SidePoints[side] >= kAiHeroPoints && !g_HeroPool[side].empty()) {
            f.className = PickHero(side);
            f.hero = true;
            g_HeroOf[side] = i;
            g_SidePoints[side] -= kAiHeroPoints;
            NoticeFor(DisplayNameOf(f.className) + " JOINS THE BATTLE", side == 0 ? "REPUBLIC HERO" : "SEPARATIST HERO", 0,
                side, now);
            LOG_INFO("Battlefront: %s comes as the %s' hero", f.className.c_str(), side == 0 ? "clones" : "droids");
        }
        float m[16];
        SafePlace(i, m);
        std::string error;
        uint8_t* back = SpawnCharacterAt(f.className.c_str(), "", "", "",
            side == SideOf(g_Player) ? SpawnSide::Ally : SpawnSide::Enemy, m, error);
        if (!back) {
            LOG_WARN("HvV: %s could not come back: %s", f.className.c_str(), error.c_str());
            f.out = true;
            continue;
        }
        --g_Lives[side];
        Track(f, back);
        FullHealth(back);
        Shield(f, now);
        Engage(i);
        LOG_INFO("HvV: %s came back (%d lives left)", f.className.c_str(), g_Lives[side]);
    }
    if (g_Conquest && g_Phase == Phase::Fight && g_Winner < 0) {
        UpdatePosts(now);
        if (g_PlayerHeroAsked) {
            g_PlayerHeroAsked = false;
            if (PlayerHeroReady())
                BecomeHero(now);
        }
    }
    if (now - g_LastEngage >= kEngageEvery) {
        g_LastEngage = now;
        EngageAll();
        for (int i = 0; i < g_Count; ++i)
            Unstick(i, now);
    }
    CheckWinner(now);
}

bool BattlefrontActive()
{
    return g_Active && g_Conquest;
}

void BattlefrontHeroRequest()
{
    if (BattlefrontActive())
        g_PlayerHeroAsked = true;
}

bool StartBattlefront(const BattlefrontSetup& setup, std::string& error)
{
    if (!setup.map.empty() && !FindCustomMap(setup.map)) {
        error = "no map " + setup.map + " under mods/maps";
        return false;
    }
    const int troops = std::clamp(setup.troops, 2, kMaxSide);
    const char* clone = RegisteredClassName("ICloneTrooper");
    const char* droid = RegisteredClassName("IBattleDroid");
    if (!clone || !droid) {
        error = "the game has no clone troopers or battle droids";
        return false;
    }
    // the player's place: the first of its side
    std::string lineup;
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < troops; ++k)
            lineup += std::string(lineup.empty() ? "" : ",") + (side == 0 ? clone : droid);
    const int player = setup.side == 1 ? troops : 0;
    auto heroes = [](const std::vector<std::string>& pool) {
        std::string text;
        for (const std::string& c : pool)
            if (const char* name = RegisteredClassName(c.c_str()); name && ClassHasBody(name))
                text += (text.empty() ? "" : ",") + std::string(name);
        return text;
    };
    StartDevMap("hvv:" + lineup + " hvvheroes:" + std::to_string(troops) + " hvvplayer:" + std::to_string(player) +
        " hvvlives:" + std::to_string(setup.tickets > 0 ? setup.tickets : kDefaultTickets) + " hvvintro:0 bf:1" +
        " bfhealth:" + std::to_string(kDefaultTroopHealth) + "," + std::to_string(kDefaultHeroHealth) +
        (setup.heroes ? " bfheroes0:" + heroes(setup.republicHeroes) + " bfheroes1:" + heroes(setup.separatistHeroes) : "") +
        (setup.map.empty() ? "" : " custommap:" + setup.map) + SessionArguments());
    return true;
}

} // namespace swrots::game
