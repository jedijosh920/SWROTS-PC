// Co-op (see coop.h). How the game does two players, and what the port does to get them in a story
// mission, is in docs/research/coop.md.

#include "game/coop.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "game/characters.h"
#include "game/freecam.h"
#include "game/menus.h"
#include "game/resources.h"
#include "input/controls.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

// A character's fields.
constexpr uint32_t kInstanceId = 0x4;
constexpr uint32_t kCharacterDead = 0x12C;       // byte: set when its death has played out ("Killed", 0x1507A0)
constexpr uint32_t kCharacterHealth = 0x130;     // float
constexpr uint32_t kCharacterMaxHealth = 0x134;  // float
constexpr uint32_t kCharacterTransform = 0x150;  // rows right, up, forward, position
constexpr uint32_t kCharacterTransformCopy = 0x720; // the same again (its last element at +0x75C)
constexpr uint32_t kCharacterPower = 0xA40;      // float: the Force (Jedi-like characters)
constexpr uint32_t kCharacterMaxPower = 0xA44;   // float
constexpr uint32_t kCharacterCostume = 0x1E8;    // int: the costume (variant)
constexpr uint32_t kCharacterSkin = 0x1EC;       // int: the texture set (0 the plain textures)
constexpr uint32_t kCharacterControl = 0x390;    // 2: a player's; 16: a story companion's
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte: "Invincible?" (story companions have it)
constexpr uint32_t kCharacterAIData = 0xA00;
constexpr uint32_t kAITargetPlayer = 0x50;       // byte: the player's enemy
constexpr int kControlPlayer = 2;
constexpr int kControlCompanion = 16;
constexpr uint32_t kTypeNameSlot = 3;            // vfunc: the class's name
constexpr uint32_t kPlaceSlot = 0x1F4 / 4;       // vfunc, thiscall (const float m[16])
constexpr uint32_t kBindController = 0x00150580; // thiscall (int controller; -1 none)
constexpr uint32_t kCharacterController = 0x43C; // the controller bound to it, -1 none
// The input manager: per controller port (+0x1D8, 4 of them) an entry whose +0x7C is the character it drives.
// 0x150580 with -1 writes -1 to the character before the manager's unbind (0x8ADD0) reads it, so the port
// keeps driving the character: the port's entry is cleared here.
constexpr uint32_t kInputManager = 0x0068D4F4;
constexpr uint32_t kInputPorts = 0x1D8;
constexpr uint32_t kPortCharacter = 0x7C;
constexpr uint32_t kObjectById = 0x000A31F0;     // cdecl (id) -> object or null

// The game manager's players: their number and an instance id per slot (see coop.md).
constexpr uint32_t kGameManager = 0x007EB964;
constexpr uint32_t kManagerPlayerCount = 0x1E4;
constexpr uint32_t kManagerPlayerIds = 0x2A4;
constexpr int kPlayer2Port = 1;
constexpr uint32_t kManagerMissionLost = 0x0027A9F0; // thiscall (): a player died
constexpr uint32_t kManagerLossDelay = 0x0027AB70;   // thiscall ()

// ICharacter's health change (0x151500, thiscall (float change, a, b), slot 0x384 of ICharacter; the Jedi's
// hit 0x269C20 calls it). Its first 9 bytes are moved to a stub.
constexpr uint32_t kChangeHealth = 0x00151500;
constexpr uint8_t kChangeHealthBytes[] = { 0x56, 0x8B, 0xF1, 0x8A, 0x86, 0xD6, 0x05, 0x00, 0x00 };

constexpr ULONGLONG kSettleMs = 1500;      // after a level start, before player 2 joins
constexpr ULONGLONG kRespawnDelayMs = 3000; // a player 2 that died anyway (a fall): a new one after this
constexpr ULONGLONG kShieldMs = 2000;       // a player 2 who came back cannot be hurt for this long
constexpr ULONGLONG kAfterCutsceneMs = 1000; // player 2 takes the companion again this long after a cutscene
constexpr ULONGLONG kDeathPlaysMs = 6000;    // a death's fall, at most (about 4 s)
constexpr float kBesideDistance = 80.0f;    // where player 2 comes back: beside player 1
// The camera: by default ([Coop] Camera=0) a shared camera (below); Camera=1 follows player 1 alone. The
// levels' own cameras are not given player 2 as a target: with player 2 in their focus lists, the first
// mission's open hangar cut to a far, wide shot whatever the players' distance. A character is about 70
// units tall.
// Player 2 is brought back after half a second out of the camera's picture or beyond kLeashDistance, at
// once beyond kFarDistance.
constexpr float kLeashDistance = 600.0f;
constexpr ULONGLONG kLeashMs = 500;
constexpr float kFarDistance = 900.0f;
constexpr ULONGLONG kFarMs = 0;
// The shared camera (Camera=0): the game's camera, aimed at player 1, moved towards the players' middle and
// pulled back until both are in the picture, at most this far, easing at this rate (per second).
constexpr float kMaxPullBack = 450.0f;
constexpr float kMaxShift = 300.0f;
constexpr float kCameraEase = 3.0f;
// Where player 2 is brought back: a spot player 1 stood on a moment ago (on the ground), this far behind.
constexpr float kTrailSpacing = 40.0f;
constexpr int kTrailLength = 32;
constexpr float kBehindDistance = 70.0f;
constexpr float kTrailReach = 400.0f;      // no further from player 1 than this
constexpr float kFallingSpeed = 250.0f;     // units a second downwards: player 1 is in the air
constexpr float kFrameMargin = 0.8f;        // the shared camera keeps both within this much of the picture
constexpr float kOnScreenMargin = 0.9f;   // of the picture's half-size
constexpr float kCharacterMiddle = 40.0f; // above the character's feet

// ICharacter's instant kill (0x152D30, thiscall (a), slot 0xC8; the Jedi's 0x280C30 calls it): falls into
// the void and kill zones. It sets health 0 and calls Killed, past the health change; it does nothing to an
// "Invincible?" character. Its first 9 bytes are moved to a stub.
constexpr uint32_t kInstantKill = 0x00152D30;
constexpr uint8_t kInstantKillBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x58 };
using InstantKillFn = void(__fastcall*)(uint8_t*, void*, uint32_t);
InstantKillFn g_OriginalInstantKill = nullptr;

// Player 1's recent footing (positions while not falling), newest last.
struct TrailPoint { float x, y, z; };
std::vector<TrailPoint> g_Trail;
float g_LastPlayerY = 0;
ULONGLONG g_LastTrailTick = 0;
float g_CameraShift[3] = {}; // the shared camera's current offset (eased)
ULONGLONG g_LastCameraTick = 0;

using ChangeHealthFn = void(__fastcall*)(uint8_t*, void*, float, uint32_t, uint32_t);
ChangeHealthFn g_OriginalChangeHealth = nullptr;

// This boot's (the running level's) state.
struct State {
    ULONGLONG levelSeen = 0;      // when the level's player was first seen
    bool nativeTwoPlayers = false; // the mission has its own player 2 (a co-op bonus mission)
    bool bossMission = false;      // a boss fight: the boss is the game's player 2 (slot 2, its health bar)
    uint8_t* bossObject = nullptr; // the boss
    uint32_t bossId = 0;
    bool boss = false;             // player 2 plays the boss, as in Versus ([Coop] Boss); else an ally of player 1's
    bool bossDone = false;         // the boss fell: nothing more for player 2 in this mission
    bool decided = false;          // nativeTwoPlayers was looked at
    uint8_t* p2 = nullptr;         // the character player 2 plays (or would)
    uint32_t p2Id = 0;
    bool spawned = false;          // co-op spawned it
    std::string p2Class;
    std::string costume, skin;     // its costume and texture set (numbers): a new one wears them too
    bool playing = false;          // registered as player 2
    int savedControl = 0;
    uint8_t savedInvincible = 0;
    int savedCount = 1;
    uint32_t savedSlotId = 0;     // the mission's own player 2 (a co-op bonus mission), or 0
    bool rescuePending = false;    // a fall or kill zone would have killed player 2
    ULONGLONG farSince = 0;        // player 2 beyond the leash since then
    float savedMaxHealth = 0, savedMaxPower = 0; // the character's own (given back when player 2 leaves)
    int controlsLost = 0;         // times player 2's controls were found changed and given back
    bool respawnPending = false;   // the health hook caught a fatal hit
    ULONGLONG shieldUntil = 0;
    ULONGLONG respawnAt = 0;       // a new character for player 2 from then (0: not waiting)
    bool gameOverSent = false;
    ULONGLONG cutsceneSeen = 0;    // a cutscene was playing then
    ULONGLONG downAt = 0;          // when player 2's health ran out (the game over rule)
    std::string reason;
    std::string spawnError;
};
State g_State;

uint8_t* ObjectById(uint32_t id)
{
    return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id);
}

template <typename T> T& Field(uint8_t* object, uint32_t offset)
{
    return *reinterpret_cast<T*>(object + offset);
}

void* VirtualFunction(uint8_t* object, uint32_t slot)
{
    return (*reinterpret_cast<void* const* const*>(object))[slot];
}

const char* TypeName(uint8_t* character)
{
    return reinterpret_cast<const char*(__fastcall*)(uint8_t*, void*)>(VirtualFunction(character, kTypeNameSlot))(
        character, nullptr);
}

uint8_t* Manager()
{
    return *reinterpret_cast<uint8_t**>(uintptr_t(kGameManager));
}

bool Alive(uint8_t* character)
{
    return character && ObjectById(Field<uint32_t>(character, kInstanceId)) == character &&
           !Field<uint8_t>(character, kCharacterDead) && Field<float>(character, kCharacterHealth) > 0.0f;
}

// Still the same character in the level (dead or alive).
bool StillThere()
{
    return g_State.p2 && ObjectById(g_State.p2Id) == g_State.p2;
}

void BindController(uint8_t* character, int controller)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(uintptr_t(kBindController))(character, nullptr, controller);
}

// The input manager's entry for a controller port, or null.
uint8_t* PortEntry(int port)
{
    uint8_t* manager = *reinterpret_cast<uint8_t**>(uintptr_t(kInputManager));
    if (!manager || !(Field<uint32_t>(manager, 0xC) & 0x40000))
        return nullptr;
    return Field<uint8_t*>(manager, kInputPorts + port * 4);
}

void UnbindController(uint8_t* character)
{
    if (uint8_t* entry = PortEntry(kPlayer2Port); entry && Field<uint8_t*>(entry, kPortCharacter) == character)
        Field<uint8_t*>(entry, kPortCharacter) = nullptr;
    BindController(character, -1);
}

// Player 2's controls as Register set them: something else (a cutscene's end, a script) may have changed them.
bool ControlsIntact(uint8_t* character)
{
    const uint8_t* entry = PortEntry(kPlayer2Port);
    return Field<int>(character, kCharacterControl) == kControlPlayer &&
           Field<int>(character, kCharacterController) == kPlayer2Port && entry &&
           Field<uint8_t*>(const_cast<uint8_t*>(entry), kPortCharacter) == character;
}

float Distance(uint8_t* a, uint8_t* b)
{
    const float* p = &Field<float>(a, kCharacterTransform + 48);
    const float* q = &Field<float>(b, kCharacterTransform + 48);
    return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

// --- Player 2's HUD ---

// Player 2's vitals (the HUD's "EnemyVitals" group, whose item names slot 2: +0x84 = 1) fade in while
// slot 2 has a living character (0x25CD10, vtable 0x5A80A0: +4 the item, +0x10 the fade's time; the item's
// alpha is its +0xB) and are never faded out again; the portrait (HudVitals, 0x5A82B0) is picked once
// (+0xC gave up, +0xD picked). When player 2 leaves, the group is hidden and the portrait is to be picked
// again (the group is also the duel opponent's, in missions co-op leaves alone).
constexpr uint32_t kVitalsFadeVtable = 0x005A80A0;
constexpr uint32_t kHudVitalsVtable = 0x005A82B0;
constexpr uint32_t kVitalsItemSlot = 0x84;

size_t ScanForVtables(uintptr_t base, size_t size, uint8_t** found, size_t room)
{
    size_t n = 0;
    __try {
        const uint32_t* words = reinterpret_cast<const uint32_t*>(base);
        for (size_t i = 0; i + 8 <= size / 4 && n < room; ++i)
            if (words[i] == kVitalsFadeVtable || words[i] == kHudVitalsVtable)
                found[n++] = reinterpret_cast<uint8_t*>(base + i * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// A word that only looks like a handler's vtable can be followed by anything: its group must be in the
// game's memory.
bool IsPlayer2Vitals(uint8_t* handler, const std::vector<std::pair<uintptr_t, size_t>>& regions)
{
    const uintptr_t group = uintptr_t(Field<uint8_t*>(handler, 8));
    for (const auto& [base, size] : regions)
        if (group >= base && group + kVitalsItemSlot + 4 <= base + size)
            return *reinterpret_cast<const int*>(group + kVitalsItemSlot) == 1;
    return false;
}

void ResetPlayer2Hud(bool hide)
{
    uint8_t* found[64];
    const std::vector<std::pair<uintptr_t, size_t>> regions = kernel::GameMemoryRegions();
    for (const auto& [base, size] : regions) {
        const size_t n = ScanForVtables(base, size, found, std::size(found));
        for (size_t i = 0; i < n; ++i) {
            uint8_t* handler = found[i];
            if (uintptr_t(handler) + 0x14 > base + size || !IsPlayer2Vitals(handler, regions))
                continue;
            if (Field<uint32_t>(handler, 0) == kHudVitalsVtable) {
                Field<uint8_t>(handler, 0xC) = 0;
                Field<uint8_t>(handler, 0xD) = 0;
            } else if (hide) {
                if (uint8_t* item = Field<uint8_t*>(handler, 4))
                    Field<uint8_t>(item, 0xB) = 0;
                Field<float>(handler, 0x10) = 0;
            }
        }
    }
}

// --- The pause menu ---

// The pause screen (interfc\pausescreen.xbl_xml, asked for as interfc\pausescreen.xml) gets a row
// "Cooperative Mod" under Quit Mission, in the panel's first empty row: a copy of Settings' item (with its
// child, the highlight bar; Settings is a Navigation row, to the screen pause_settings), renamed "coop", one
// row under Quit Mission, with its own text, neighbours and screen ("pause_coop"); Quit Mission's down and
// Continue's up lead to it, and the screen's item count (a u32 after its two names) grows by one.
constexpr char kPauseScreen[] = "interfc\\pausescreen.xml";
constexpr char kCoopScreenName[] = "pause_coop";
constexpr char kCoopScreen[] = "interfc\\pause_coop.xml";
constexpr char kSettingsScreen[] = "interfc\\pause_settings.xml";
constexpr float kPauseRowStep = 24.0f;
constexpr size_t kScreenItemCount = 0x1D; // in the pause screen: after "pausescreen" and "resume"

void PatchPauseScreen(std::vector<uint8_t>& data)
{
    size_t quit, quitEnd, resume, resumeEnd, settings, settingsEnd, existing, existingEnd;
    if (!MenuRowBlock(data, "quit", quit, quitEnd) || !MenuRowBlock(data, "resume", resume, resumeEnd) ||
        !MenuRowBlock(data, "settings", settings, settingsEnd) || MenuRowBlock(data, "coop", existing, existingEnd) ||
        data.size() < kScreenItemCount + 4) {
        LOG_WARN("Co-op: the pause screen is not as expected; no Cooperative Mod entry");
        return;
    }
    // The new row: a copy of Settings (a Navigation row) one row under Quit Mission.
    std::vector<uint8_t> quitRow(data.begin() + quit, data.begin() + quitEnd);
    std::vector<uint8_t> row(data.begin() + settings, data.begin() + settingsEnd);
    float x, y;
    MenuItemPosition(quitRow, &x, &y, false);
    y += kPauseRowStep;
    MenuItemPosition(row, &x, &y, true);
    const size_t nameAt = MenuString("screenItem").size();
    size_t rowEnd = row.size();
    bool ok = MenuReplaceString(row, 0, rowEnd, "settings", "coop") &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "IDS_PAUSE_SETTINGS", "IDS_PORT_COOP_MENU") &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "restart", "resume") &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "combat", "quit") &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "pause_settings", kCoopScreenName);
    if (!ok) {
        LOG_WARN("Co-op: the pause screen's Settings entry is not as expected; no Cooperative Mod entry");
        return;
    }
    // Links into the new row, then the row after Quit Mission's (the later edits first: they move nothing
    // before them).
    if (!MenuReplaceString(data, quit + nameAt + 8, quitEnd, "resume", "coop")) {
        LOG_WARN("Co-op: the pause screen's links are not as expected; no Cooperative Mod entry");
        return;
    }
    data.insert(data.begin() + quitEnd, row.begin(), row.end());
    size_t resumeLinks = resumeEnd;
    if (!MenuReplaceString(data, resume + nameAt + 10, resumeLinks, "quit", "coop")) {
        LOG_WARN("Co-op: the pause screen's links are not as expected; no Cooperative Mod entry");
        return;
    }
    uint32_t count;
    std::memcpy(&count, data.data() + kScreenItemCount, 4);
    ++count;
    std::memcpy(data.data() + kScreenItemCount, &count, 4);
    LOG_INFO("Co-op: the pause screen has a Cooperative Mod entry");
}

// The port's screen "pause_coop": the pause screen's Settings (interfc\pause_settings.xbl_xml) with its rows
// replaced: each made from the Subtitles row (an On/Off control, SettingsScreenOnOffControl) with the
// port's name, text, neighbours and action "CoopOption"; the rows the screen does not need are removed (and
// the screen's item count, a u32 after its name and its first item's, lowered).
struct CoopRow {
    const char* name;
    const char* textId;
    const wchar_t* text;
};
constexpr CoopRow kCoopRows[] = {
    { "coopon", "IDS_PORT_COOP_ON", L"Co-op" },
    { "coopboss", "IDS_PORT_COOP_BOSS", L"Player 2 As Boss" },
    { "coopfriendlyfire", "IDS_PORT_COOP_FRIENDLY_FIRE", L"Friendly Fire" },
    { "cooprespawn", "IDS_PORT_COOP_RESPAWN", L"Respawn Player 2" },
    { "coopcamera", "IDS_PORT_COOP_CAMERA", L"Shared Camera" },
};
constexpr const char* kSettingsRows[] = { "controller", "vibration", "subtitles", "musicvolume", "effectsvolume",
    "dialoguevolume", "difficulty", "restoredefaults" };

bool GenerateCoopScreen(const std::string& lowerName, std::vector<uint8_t>& data)
{
    if (lowerName != kCoopScreen || !ReadDiscResource(kSettingsScreen, data))
        return false;
    size_t templateBegin, templateEnd;
    if (!MenuRowBlock(data, "subtitles", templateBegin, templateEnd)) {
        LOG_WARN("Co-op: the Settings screen is not as expected; no co-op screen");
        return false;
    }
    const std::vector<uint8_t> templateRow(data.begin() + templateBegin, data.begin() + templateEnd);
    const size_t nameAt = MenuString("screenItem").size();
    float x;
    int removed = 0;
    // From the last row back: replacing a row moves only what follows it.
    for (int i = int(std::size(kSettingsRows)) - 1; i >= 0; --i) {
        size_t begin, end;
        if (!MenuRowBlock(data, kSettingsRows[i], begin, end)) {
            LOG_WARN("Co-op: the Settings screen's %s row is missing; no co-op screen", kSettingsRows[i]);
            return false;
        }
        std::vector<uint8_t> original(data.begin() + begin, data.begin() + end);
        float y;
        MenuItemPosition(original, &x, &y, false);
        const int rows = int(std::size(kCoopRows));
        if (i >= rows) { // a row the screen does not need
            data.erase(data.begin() + begin, data.begin() + end);
            ++removed;
            continue;
        }
        std::vector<uint8_t> row = templateRow;
        const std::string name = kCoopRows[i].name;
        const std::string down = kCoopRows[(i + 1) % rows].name;
        const std::string up = kCoopRows[(i + rows - 1) % rows].name;
        size_t rowEnd = row.size();
        bool ok = MenuReplaceString(row, 0, rowEnd, "subtitles", name) &&
                  MenuReplaceString(row, nameAt + 4, rowEnd, "IDS_SHELL_SUBTITLES", kCoopRows[i].textId) &&
                  MenuReplaceString(row, nameAt + 4, rowEnd, "musicvolume", down) &&
                  MenuReplaceString(row, nameAt + 4, rowEnd, "vibration", up) &&
                  MenuReplaceString(row, nameAt + 4, rowEnd, "SettingsScreenOnOffControl", "CoopOption");
        if (!ok) {
            LOG_WARN("Co-op: the Settings screen's Subtitles row is not as expected; no co-op screen");
            return false;
        }
        MenuItemPosition(row, &x, &y, true);
        data.erase(data.begin() + begin, data.begin() + end);
        data.insert(data.begin() + begin, row.begin(), row.end());
    }
    // The screen's name and its first item, then its number of items (after them), then the title.
    size_t end = 64;
    if (!MenuReplaceString(data, 0, end, "pause_settings", kCoopScreenName) ||
        !MenuReplaceString(data, 0, end, "controller", kCoopRows[0].name)) {
        LOG_WARN("Co-op: the Settings screen's header is not as expected; no co-op screen");
        return false;
    }
    const size_t countAt = 4 + MenuString(kCoopScreenName).size() + MenuString(kCoopRows[0].name).size();
    uint32_t count;
    std::memcpy(&count, data.data() + countAt, 4);
    count -= removed;
    std::memcpy(data.data() + countAt, &count, 4);
    end = data.size();
    if (!MenuReplaceString(data, 0, end, "IDS_SETTINGS", "IDS_PORT_COOP_TITLE"))
        LOG_WARN("Co-op: the Settings screen's title is not as expected");
    return true;
}

// The co-op screen's rows: the game's On/Off control (made by 0x2D9690, vtable 0x5D7000; its value a byte at
// +0x23C, its item at +8, the item's name key at +4) with a vtable of the port's own: slot 3 (thiscall (x),
// drawn every frame) shows the port's setting where the control reads its own (it reads the profile's
// subtitles byte, profile +0x2A, for a row made from Subtitles, whose +0x84 is 1); slot 28 (thiscall (),
// called when the value changes) writes the setting; slot 27 (back to the default) sets the setting's default;
// slot 4 (events) goes back without the settings screen's saving; slot 10 makes another one.
constexpr uint32_t kMakeOnOff = 0x002D9690;
constexpr uint32_t kOnOffVtable = 0x005D7000;
constexpr int kOnOffSlots = 32;
constexpr uint32_t kOnOffValue = 0x23C;
constexpr uint32_t kProfiles = 0x007E8F24;     // the profile manager
constexpr uint32_t kCurrentProfile = 0x0024CC50; // thiscall () -> profile or null
constexpr uint32_t kProfileSubtitles = 0x2A;
using DrawFn = char(__fastcall*)(uint8_t*, void*, uint32_t);
uint32_t g_CoopOptionVtable[kOnOffSlots];
DrawFn g_OnOffDraw = nullptr;

int CoopRowOf(uint8_t* handler)
{
    const uint8_t* item = Field<uint8_t*>(handler, 8);
    if (!item)
        return -1;
    const uint32_t key = *reinterpret_cast<const uint32_t*>(item + 4);
    for (int i = 0; i < int(std::size(kCoopRows)); ++i)
        if (key == MenuKey(kCoopRows[i].name))
            return i;
    return -1;
}

bool CoopRowValue(int row)
{
    const Settings& s = GetSettings();
    switch (row) {
    case 0: return s.coop;
    case 1: return s.coopBoss;
    case 2: return s.coopFriendlyFire;
    case 3: return s.coopDeath == 0;
    case 4: return s.coopCamera == 0;
    default: return false;
    }
}

void SetCoopRowValue(int row, bool on)
{
    Settings& s = EditSettings();
    switch (row) {
    case 0: s.coop = on; break;
    case 1: s.coopBoss = on; break;
    case 2: s.coopFriendlyFire = on; break;
    case 3: s.coopDeath = on ? 0 : 1; break;
    case 4: s.coopCamera = on ? 0 : 1; break;
    default: return;
    }
    SaveSettings();
    LOG_INFO("Co-op: %ls turned %s from the pause screen", kCoopRows[row].text, on ? "on" : "off");
}

uint8_t* CurrentProfile()
{
    uint8_t* profiles = *reinterpret_cast<uint8_t**>(uintptr_t(kProfiles));
    return profiles ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(uintptr_t(kCurrentProfile))(profiles, nullptr)
                    : nullptr;
}

char __fastcall CoopOptionDraw(uint8_t* handler, void*, uint32_t value)
{
    const int row = CoopRowOf(handler);
    if (row < 0)
        return g_OnOffDraw(handler, nullptr, value);
    const uint8_t on = CoopRowValue(row) ? 1 : 0;
    Field<uint8_t>(handler, kOnOffValue) = on;
    uint8_t* profile = CurrentProfile();
    const uint8_t saved = profile ? profile[kProfileSubtitles] : 0;
    if (profile)
        profile[kProfileSubtitles] = on;
    const char result = g_OnOffDraw(handler, nullptr, value);
    if (profile)
        profile[kProfileSubtitles] = saved;
    return result;
}

void __fastcall CoopOptionApply(uint8_t* handler, void*)
{
    const int row = CoopRowOf(handler);
    if (row >= 0 && (Field<uint8_t>(handler, kOnOffValue) != 0) != CoopRowValue(row))
        SetCoopRowValue(row, Field<uint8_t>(handler, kOnOffValue) != 0);
}

void __fastcall CoopOptionDefault(uint8_t* handler, void*)
{
    const int row = CoopRowOf(handler);
    const Settings defaults;
    const bool on = row == 0 ? defaults.coop : row == 1 ? defaults.coopBoss : row == 2 ? defaults.coopFriendlyFire
                  : row == 3 ? defaults.coopDeath == 0 : defaults.coopCamera == 0;
    Field<uint8_t>(handler, kOnOffValue) = on ? 1 : 0;
    CoopOptionApply(handler, nullptr);
}

// Back (event 0x26): the settings screen's controls save the game's settings to the profile (or warn that
// they cannot, without a saved game: 0x2D7DF0); the port's are in settings.ini, so the menu just goes back,
// as any item does (0x2C62D0).
constexpr uint32_t kItemEvent = 0x002C62D0;
constexpr int kBack = 0x26;
using EventFn = char(__fastcall*)(uint8_t*, void*, int, int);
EventFn g_OnOffEvent = nullptr;

char __fastcall CoopOptionEvent(uint8_t* handler, void*, int event, int value)
{
    if (event == kBack)
        return reinterpret_cast<EventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
    return g_OnOffEvent(handler, nullptr, event, value);
}

uint8_t* MakeCoopOption()
{
    uint8_t* handler = reinterpret_cast<uint8_t*(__cdecl*)()>(uintptr_t(kMakeOnOff))();
    if (handler)
        *reinterpret_cast<uint32_t**>(handler) = g_CoopOptionVtable;
    return handler;
}

uint8_t* __fastcall MakeCoopOptionSlot(uint8_t*, void*)
{
    return MakeCoopOption();
}

void InstallCoopMenus()
{
    std::memcpy(g_CoopOptionVtable, reinterpret_cast<const void*>(uintptr_t(kOnOffVtable)), sizeof(g_CoopOptionVtable));
    g_OnOffDraw = reinterpret_cast<DrawFn>(uintptr_t(g_CoopOptionVtable[3]));
    g_CoopOptionVtable[3] = uint32_t(reinterpret_cast<uintptr_t>(&CoopOptionDraw));
    g_OnOffEvent = reinterpret_cast<EventFn>(uintptr_t(g_CoopOptionVtable[4]));
    g_CoopOptionVtable[4] = uint32_t(reinterpret_cast<uintptr_t>(&CoopOptionEvent));
    g_CoopOptionVtable[10] = uint32_t(reinterpret_cast<uintptr_t>(&MakeCoopOptionSlot));
    g_CoopOptionVtable[27] = uint32_t(reinterpret_cast<uintptr_t>(&CoopOptionDefault));
    g_CoopOptionVtable[28] = uint32_t(reinterpret_cast<uintptr_t>(&CoopOptionApply));
    SetMenuText("IDS_PORT_COOP_MENU", L"Cooperative Mod");
    SetMenuText("IDS_PORT_COOP_TITLE", L"Cooperative Mod");
    for (const CoopRow& row : kCoopRows)
        SetMenuText(row.textId, row.text);
    AddMenuHandler("CoopOption", &MakeCoopOption);
    AddMenuScreen(kCoopScreen);
    RegisterResourceGenerator(&GenerateCoopScreen);
    RegisterResourcePatch(kPauseScreen, &PatchPauseScreen);
}

// --- Damage ---

// Player 2 does not die under the respawn rule: a hit that would kill them leaves them with a little
// health (so the game starts no death), and they come back beside player 1 on the next frame.
void __fastcall ChangeHealthHook(uint8_t* character, void*, float change, uint32_t a, uint32_t b)
{
    g_OriginalChangeHealth(character, nullptr, change, a, b);
    if (character != g_State.p2 || !g_State.playing || g_State.boss || GetSettings().coopDeath != 0)
        return;
    float& health = Field<float>(character, kCharacterHealth);
    if (health <= 0.0f && !Field<uint8_t>(character, kCharacterDead)) {
        health = 1.0f;
        g_State.respawnPending = true;
    }
}

// A fall into the void or a kill zone would kill player 2 (the story's companion with them, which the
// mission's scripts still need): they are brought back to player 1 instead.
void __fastcall InstantKillHook(uint8_t* character, void*, uint32_t a)
{
    if (character == g_State.p2 && g_State.playing && !g_State.boss) {
        g_State.rescuePending = true;
        return;
    }
    g_OriginalInstantKill(character, nullptr, a);
}

// --- Cutscenes ---

// The game's own "a cutscene is playing" (0xA3720: the letterbox is up (0x12E190), or a cinematic or a
// scripted camera holds the view), asked as the game asks it: only with the letterbox there (it reads it
// unchecked); without, the other two alone.
constexpr uint32_t kLetterBox = 0x0069226C;         // the letterbox object, or null
constexpr uint32_t kInCutscene = 0x000A3720;        // cdecl bool ()
constexpr uint32_t kCinematicPlaying = 0x00695C88;  // non-null while one plays
constexpr uint32_t kScriptedCameras = 0x0068DA44;   // count
constexpr uint32_t kObjectLive = 0x40000;           // object +0xC: set while it is in the level

bool InCutscene()
{
    const uint8_t* letterBox = *reinterpret_cast<uint8_t* const*>(uintptr_t(kLetterBox));
    if (letterBox && (*reinterpret_cast<const uint32_t*>(letterBox + 0xC) & kObjectLive))
        return reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kInCutscene))();
    return *reinterpret_cast<const uint32_t*>(uintptr_t(kCinematicPlaying)) != 0 ||
           *reinterpret_cast<const int*>(uintptr_t(kScriptedCameras)) > 0;
}

// Jedi-like characters (IJedi and those built on it): IsA (vfunc +4) with the type's function as its key.
bool IsJedi(uint8_t* character)
{
    constexpr uint32_t kJediType = 0x00249950;
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>(VirtualFunction(character, 1))(
        character, nullptr, kJediType);
}

// The player's enemies have their AI data's "Target Player" set (the scripted ones, such as the first
// mission's battle droids, have control mode 16 too).
bool TargetsPlayer(uint8_t* character)
{
    const uint8_t* ai = Field<uint8_t*>(character, kCharacterAIData);
    return !ai || ai[kAITargetPlayer] != 0;
}

// --- Player 2 ---

// A maximum changed to `max`, the current value kept in proportion.
void SetMaximum(uint8_t* character, uint32_t current, uint32_t maximum, float max)
{
    float& value = Field<float>(character, current);
    float& old = Field<float>(character, maximum);
    if (max <= 0 || old == max)
        return;
    value = old > 0 ? value / old * max : max;
    old = max;
}

// Player 2 as strong as player 1: the same maximum health and Force.
void MatchPlayer1(uint8_t* player)
{
    SetMaximum(g_State.p2, kCharacterHealth, kCharacterMaxHealth, Field<float>(player, kCharacterMaxHealth));
    if (IsJedi(player) && IsJedi(g_State.p2))
        SetMaximum(g_State.p2, kCharacterPower, kCharacterMaxPower, Field<float>(player, kCharacterMaxPower));
}

// A cutscene can leave the last element of an AI character's transform (+0x18C, and its copy at +0x75C) not
// a number; the AI does not mind, but a player-controlled character cannot move with it (Cin Drallig after
// her bonus mission's opening cutscene).
void RepairTransform(uint8_t* c)
{
    for (uint32_t offset : { kCharacterTransform + 0x3C, kCharacterTransformCopy + 0x3C }) {
        float& w = Field<float>(c, offset);
        if (!std::isfinite(w)) {
            LOG_INFO("Co-op: player 2's transform had a bad element at +0x%X; repaired", offset);
            w = 1.0f;
        }
    }
}

void Register()
{
    uint8_t* c = g_State.p2;
    RepairTransform(c);
    uint8_t* manager = Manager();
    if (!manager)
        return;
    g_State.savedControl = Field<int>(c, kCharacterControl);
    g_State.savedInvincible = Field<uint8_t>(c, kCharacterInvincible);
    g_State.savedCount = Field<int>(manager, kManagerPlayerCount);
    g_State.savedSlotId = Field<uint32_t>(manager, kManagerPlayerIds + 4);
    g_State.savedMaxHealth = Field<float>(c, kCharacterMaxHealth);
    g_State.savedMaxPower = Field<float>(c, kCharacterMaxPower);
    if (g_State.boss) {
        // The boss stays the game's in all but its controls: its health, its health bar, its moments of
        // invincibility, its side.
        Field<int>(c, kCharacterControl) = kControlPlayer;
        BindController(c, kPlayer2Port);
        g_State.playing = true;
        g_State.reason.clear();
        g_State.controlsLost = 0;
        LOG_INFO("Co-op: player 2 plays the boss %s (control was %d)", g_State.p2Class.c_str(), g_State.savedControl);
        return;
    }
    if (uint8_t* player = PlayerObject())
        MatchPlayer1(player);
    Field<int>(c, kCharacterControl) = kControlPlayer;
    BindController(c, kPlayer2Port);
    Field<uint8_t>(c, kCharacterInvincible) = 0;
    // The second HUD (the third HudVitals, which names slot 2) shows whoever is in slot 2 while there are two.
    // In a boss fight slot 2 stays the boss's (its health bar, the duel camera): player 2 has no HUD there.
    if (!g_State.bossMission) {
        Field<uint32_t>(manager, kManagerPlayerIds + 4) = g_State.p2Id;
        Field<int>(manager, kManagerPlayerCount) = 2;
        ResetPlayer2Hud(false); // the portrait is picked for this character
    }
    g_State.playing = true;
    g_State.reason.clear();
    g_State.controlsLost = 0;
    g_State.farSince = 0;
    LOG_INFO("Co-op: player 2 plays %s %s (control was %d)", g_State.spawned ? "the spawned" : "the companion",
        g_State.p2Class.c_str(), g_State.savedControl);
    // A class's sounds are set up only while a level loads (CharSoundManager; +0x444 its sound set, 0 none):
    // a class the level does not have plays its moves without them.
    constexpr uint32_t kCharacterSoundSet = 0x444;
    if (Field<int>(c, kCharacterSoundSet) == 0)
        LOG_INFO("Co-op: %s has no sounds in this level (the level has no character of its own of that class)",
            g_State.p2Class.c_str());
}

// Player 2 stops playing: the character goes back to the game (its AI, its own invincibility).
// `removing`: the character is about to be removed: only its controller is let go (as a live player swap
// does). Given back to the AI in the frame it goes, the game made AI state for it that outlived it (a crash
// two frames later).
void Unregister(const char* why, bool removing = false)
{
    if (!g_State.playing)
        return;
    g_State.playing = false;
    SetFriendlyFire(nullptr, nullptr);
    g_State.respawnPending = false;
    if (g_State.boss) {
        if (StillThere()) {
            UnbindController(g_State.p2);
            Field<int>(g_State.p2, kCharacterControl) = g_State.savedControl;
        }
        LOG_INFO("Co-op: player 2 left the boss %s (%s)", g_State.p2Class.c_str(), why);
        return;
    }
    uint8_t* manager = Manager();
    if (manager && !g_State.bossMission) {
        // As the mission had them: a co-op bonus mission's own player 2 stays in its slot.
        Field<uint32_t>(manager, kManagerPlayerIds + 4) = g_State.savedSlotId;
        Field<int>(manager, kManagerPlayerCount) = g_State.savedCount;
    }
    if (!g_State.savedSlotId && !g_State.bossMission)
        ResetPlayer2Hud(true);
    if (StillThere() && removing) {
        UnbindController(g_State.p2);
    } else if (StillThere()) {
        UnbindController(g_State.p2);
        Field<int>(g_State.p2, kCharacterControl) = g_State.savedControl;
        Field<uint8_t>(g_State.p2, kCharacterInvincible) = g_State.savedInvincible;
        if (!g_State.spawned) { // a companion is as strong as the story made it again
            SetMaximum(g_State.p2, kCharacterHealth, kCharacterMaxHealth, g_State.savedMaxHealth);
            if (IsJedi(g_State.p2))
                SetMaximum(g_State.p2, kCharacterPower, kCharacterMaxPower, g_State.savedMaxPower);
        }
    }
    LOG_INFO("Co-op: player 2 left %s (%s)", g_State.p2Class.c_str(), why);
}


// The mission's companion: a living Jedi beside the player whom the story moves (control mode 16) and
// who is not the player's enemy (Obi-Wan beside Anakin, Cin Drallig beside Serra), the nearest to player
// 1. Allied soldiers (the Jedi Temple's clones), R2-D2 and Palpatine are not companions: without one,
// player 2 gets a character of their own.
uint8_t* FindCompanion(uint8_t* player)
{
    uint8_t* best = nullptr;
    float bestDistance = 0;
    const float* p = &Field<float>(player, kCharacterTransform + 48);
    for (uint8_t* c : LevelCharacters()) {
        if (c == player || !Alive(c) || Field<int>(c, kCharacterControl) != kControlCompanion || TargetsPlayer(c) ||
            !IsJedi(c) || _stricmp(TypeName(c), "IPalpatine") == 0) // Palpatine watching Mace's duel is the story's
            continue;
        const float* q = &Field<float>(c, kCharacterTransform + 48);
        const float d = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
        if (!best || d < bestDistance) {
            best = c;
            bestDistance = d;
        }
    }
    return best;
}

// Player 1 is on the Sith side when the level's clone troopers are their allies (Anakin in the Jedi Temple);
// on the Jedi side the clones, if any, are their enemies (Order 66 on Utapau). The level's clone when they
// are allies (player 2 dresses as it), else null.
uint8_t* SithSideClone(uint8_t* player)
{
    for (uint8_t* c : LevelCharacters())
        if (c != player && c != g_State.p2 && Alive(c) && _strnicmp(TypeName(c), "IClone", 6) == 0)
            return TargetsPlayer(c) ? nullptr : c;
    return nullptr;
}

// A character for player 2 where the mission has no companion: the chosen one ([Coop] Player2), else one
// for player 1's side: a 501st clone trooper beside a Sith, else Obi-Wan (or, beside Obi-Wan, a Jedi Knight).
struct SpawnChoice {
    std::string className;
    std::string costume, skin; // numbers; empty for the class's usual ones
};

SpawnChoice ChooseSpawn(uint8_t* player)
{
    if (!g_State.p2Class.empty() && g_State.spawned)
        return { g_State.p2Class, g_State.costume, g_State.skin }; // the same again after a death
    const std::string& chosen = GetSettings().coopPlayer2;
    if (!chosen.empty())
        return { chosen, "", "" };
    if (g_State.bossMission && g_State.bossObject && ObjectById(g_State.bossId) == g_State.bossObject) {
        // Beside player 1 against the boss: a Jedi against a Sith boss (Dooku, Grievous, Anakin on
        // Mustafar), a 501st clone trooper against a Jedi (Mace Windu, Obi-Wan beside Anakin).
        static const char* const kSithBosses[] = { "IDooku", "IGrievous", "IAnakin", "IVader", "IPalpatine" };
        const char* boss = TypeName(g_State.bossObject);
        const bool sith = std::any_of(std::begin(kSithBosses), std::end(kSithBosses),
            [&](const char* name) { return _strnicmp(boss, name, strlen(name)) == 0; });
        if (!sith)
            return { "ICloneTrooper", "hordeTrooper", "_var01" };
        return { _stricmp(TypeName(player), "IObiwan") == 0 ? "IJediKnight" : "IObiwan", "", "" };
    }
    if (uint8_t* clone = SithSideClone(player)) // dressed as the level's own (the temple's 501st)
        return { TypeName(clone), std::to_string(Field<int>(clone, kCharacterCostume)),
            std::to_string(Field<int>(clone, kCharacterSkin)) };
    return { _stricmp(TypeName(player), "IObiwan") == 0 ? "IJediKnight" : "IObiwan", "", "" };
}

// Picks player 2's character: the companion, or a new one beside player 1.
bool PickCharacter(uint8_t* player)
{
    if (uint8_t* companion = FindCompanion(player)) {
        g_State.p2 = companion;
        g_State.spawned = false;
    } else {
        const SpawnChoice choice = ChooseSpawn(player);
        std::string error;
        if (!SpawnCharacter(choice.className.c_str(), choice.costume, choice.skin, "", SpawnSide::Ally, error) || !LastSpawnedObject()) {
            if (error != g_State.spawnError)
                LOG_WARN("Co-op: no character for player 2: %s", error.c_str());
            g_State.spawnError = error;
            g_State.reason = "no character for player 2: " + error;
            return false;
        }
        g_State.p2 = LastSpawnedObject();
        g_State.spawned = true;
    }
    g_State.p2Id = Field<uint32_t>(g_State.p2, kInstanceId);
    g_State.p2Class = TypeName(g_State.p2);
    g_State.costume = std::to_string(Field<int>(g_State.p2, kCharacterCostume));
    g_State.skin = std::to_string(Field<int>(g_State.p2, kCharacterSkin));
    return true;
}

// Player 1's footing: where they stood a moment ago, while not falling (a spot to put player 2 on).
void FollowPlayer1(uint8_t* player, ULONGLONG now)
{
    const float* p = &Field<float>(player, kCharacterTransform + 48);
    const float dt = g_LastTrailTick ? float(now - g_LastTrailTick) / 1000.0f : 0.0f;
    const bool falling = dt > 0 && (g_LastPlayerY - p[1]) / dt > kFallingSpeed;
    g_LastTrailTick = now;
    g_LastPlayerY = p[1];
    if (falling)
        return;
    if (!g_Trail.empty()) {
        const TrailPoint& last = g_Trail.back();
        const float dx = p[0] - last.x, dy = p[1] - last.y, dz = p[2] - last.z;
        if (dx * dx + dy * dy + dz * dz < kTrailSpacing * kTrailSpacing)
            return;
    }
    g_Trail.push_back({ p[0], p[1], p[2] });
    if (int(g_Trail.size()) > kTrailLength)
        g_Trail.erase(g_Trail.begin());
}

// Player 2 placed where player 1 stood a moment ago (a little behind them, on ground they walked on), facing
// as player 1 does; just behind player 1 when they have not moved yet.
void PlaceSafely(uint8_t* player)
{
    float m[16];
    std::memcpy(m, &Field<float>(player, kCharacterTransform), sizeof(m));
    const float px = m[12], py = m[13], pz = m[14];
    m[12] -= m[8] * kBehindDistance;
    m[14] -= m[10] * kBehindDistance;
    for (auto it = g_Trail.rbegin(); it != g_Trail.rend(); ++it) {
        const float dx = it->x - px, dy = it->y - py, dz = it->z - pz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= kBehindDistance && d <= kTrailReach) {
            m[12] = it->x;
            m[13] = it->y;
            m[14] = it->z;
            break;
        }
    }
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(VirtualFunction(g_State.p2, kPlaceSlot))(
        g_State.p2, nullptr, m);
}

// Whether player 2 is in the game camera's picture (their middle, with a small margin), from the camera's
// placement and field of view (radians, up and down; degrees taken as such). Unknown counts as in view.
bool OnScreen(uint8_t* character)
{
    float m[16], fov = 0;
    if (!GameCameraPlacement(m, fov) || fov <= 0)
        return true;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_INFO("Co-op: the game camera's field of view is %.3f", fov);
    }
    if (fov > 3.2f)
        fov *= 3.14159265f / 180.0f;
    const float* p = &Field<float>(character, kCharacterTransform + 48);
    const float d[3] = { p[0] - m[12], p[1] + kCharacterMiddle - m[13], p[2] - m[14] };
    const float x = d[0] * m[0] + d[1] * m[1] + d[2] * m[2];
    const float y = d[0] * m[4] + d[1] * m[5] + d[2] * m[6];
    const float z = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
    const float tanUp = std::tan(fov * 0.5f) * kOnScreenMargin;
    const float tanSide = tanUp * (GetSettings().widescreen ? 16.0f / 9.0f : 4.0f / 3.0f);
    return z > 0 && std::fabs(x) < z * tanSide && std::fabs(y) < z * tanUp;
}

// The leash: a player 2 out of the picture (the shared camera could not fit both) for a moment, or too far
// away, is brought back to player 1.
void Leash(uint8_t* player, ULONGLONG now)
{
    const float distance = Distance(player, g_State.p2);
    const bool onScreen = OnScreen(g_State.p2);
    if (onScreen && distance <= kLeashDistance) {
        g_State.farSince = 0;
        return;
    }
    if (!g_State.farSince)
        g_State.farSince = now;
    const ULONGLONG wait = distance > kFarDistance ? kFarMs : kLeashMs;
    if (now - g_State.farSince >= wait) {
        PlaceSafely(player);
        g_State.farSince = 0;
        LOG_INFO("Co-op: player 2 was %.0f away%s and came back to player 1", distance, onScreen ? "" : ", out of the picture");
    }
}

// The shared camera: the game's camera (which follows player 1) moved towards the players' middle and pulled
// back along its view until both players' middles are well inside the picture, within limits, eased. The
// view's direction stays the game's. Off (eased back to the game's) with Camera=1, in cutscenes, without a
// player 2.
bool SharedCamera(float m[16], float fov)
{
    const ULONGLONG now = GetTickCount64();
    const float dt = g_LastCameraTick ? std::min(float(now - g_LastCameraTick) / 1000.0f, 0.1f) : 0.0f;
    g_LastCameraTick = now;
    float target[3] = {};
    uint8_t* player = PlayerObject();
    if (GetSettings().coopCamera == 0 && player && g_State.playing && !g_State.boss && StillThere() && Alive(g_State.p2) && fov > 0 &&
        !InCutscene()) {
        if (fov > 3.2f)
            fov *= 3.14159265f / 180.0f;
        const float* r = m;
        const float* u = m + 4;
        const float* f = m + 8;
        const float* c = m + 12;
        const float* a0 = &Field<float>(player, kCharacterTransform + 48);
        const float* b0 = &Field<float>(g_State.p2, kCharacterTransform + 48);
        const float a[3] = { a0[0], a0[1] + kCharacterMiddle, a0[2] };
        const float b[3] = { b0[0], b0[1] + kCharacterMiddle, b0[2] };
        // Sideways and up or down towards the middle (along the view's right and up), at most kMaxShift.
        const float half[3] = { (b[0] - a[0]) * 0.5f, (b[1] - a[1]) * 0.5f, (b[2] - a[2]) * 0.5f };
        const float sr = half[0] * r[0] + half[1] * r[1] + half[2] * r[2];
        const float su = half[0] * u[0] + half[1] * u[1] + half[2] * u[2];
        float shift[3] = { r[0] * sr + u[0] * su, r[1] * sr + u[1] * su, r[2] * sr + u[2] * su };
        const float length = std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
        if (length > kMaxShift)
            for (float& v : shift)
                v *= kMaxShift / length;
        // Back along the view until both fit.
        const float tanUp = std::tan(fov * 0.5f) * kFrameMargin;
        const float tanSide = tanUp * (GetSettings().widescreen ? 16.0f / 9.0f : 4.0f / 3.0f);
        float back = 0;
        for (const float* q : { a, b }) {
            const float d[3] = { q[0] - c[0] - shift[0], q[1] - c[1] - shift[1], q[2] - c[2] - shift[2] };
            const float x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
            const float y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
            const float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
            back = std::max({ back, std::fabs(x) / tanSide - z, std::fabs(y) / tanUp - z });
        }
        back = std::min(back, kMaxPullBack);
        for (int i = 0; i < 3; ++i)
            target[i] = shift[i] - f[i] * back;
    }
    const float ease = std::min(1.0f, dt * kCameraEase);
    bool moved = false;
    for (int i = 0; i < 3; ++i) {
        g_CameraShift[i] += (target[i] - g_CameraShift[i]) * ease;
        moved |= std::fabs(g_CameraShift[i]) > 0.5f;
    }
    if (!moved) {
        g_CameraShift[0] = g_CameraShift[1] = g_CameraShift[2] = 0;
        return false;
    }
    for (int i = 0; i < 3; ++i)
        m[12 + i] += g_CameraShift[i];
    return true;
}

// Player 2 back on their feet by player 1, with full health and a moment's shield.
void Respawn(uint8_t* player)
{
    PlaceSafely(player);
    Field<float>(g_State.p2, kCharacterHealth) = Field<float>(g_State.p2, kCharacterMaxHealth);
    Field<uint8_t>(g_State.p2, kCharacterInvincible) = 1;
    g_State.shieldUntil = GetTickCount64() + kShieldMs;
    LOG_INFO("Co-op: player 2 came back beside player 1");
}

// The mission is lost as when player 1 dies: their death (the Jedi's vfunc +0x430, 0x267B30) has the game
// manager start the loss (0x27A9F0: the Game Over screen, outside Versus) and set its delay (0x27AB70).
void GameOver()
{
    if (g_State.gameOverSent)
        return;
    g_State.gameOverSent = true;
    LOG_INFO("Co-op: player 2 died: the mission is lost");
    if (uint8_t* manager = Manager()) {
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kManagerMissionLost))(manager, nullptr);
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kManagerLossDelay))(manager, nullptr);
    }
}

} // namespace

// --- Player 2's clone trooper shoots ---
// A clone's moves are its script class's sequences (CloneTrooper.cpp, script vtable 0x5E53D0), thiscall on
// the script object with two stack arguments. Under player controls the attack button plays the rifle butt
// (0x356B80); the AI shoots with CloneBlastAttack (0x3562B0). Player 2's heavy attack (which a clone does
// not have) is pressed as the attack button (input.cpp) and plays CloneBlastAttack instead.
constexpr uint32_t kRifleButt = 0x00356B80;
constexpr uint8_t kRifleButtBytes[] = { 0x56, 0x68, 0x60, 0xFB, 0x55, 0x00 }; // push esi; push 0x55FB60
constexpr uint32_t kBlastAttack = 0x003562B0;
constexpr uint8_t kBlastAttackBytes[] = { 0x56, 0x8B, 0x35, 0xC0, 0x64, 0x69, 0x00 }; // push esi; mov esi, [0x6964C0]
uint8_t* g_RifleButtStub = nullptr;

// The sequence runs for the character whose script instance is the current context ([0x6964C0]: a
// character's +0x434 holds its script, whose +0x1C is that instance).
constexpr uint32_t kScriptContext = 0x006964C0;
constexpr uint32_t kCharacterScript = 0x434;
constexpr uint32_t kScriptInstance = 0x1C;

bool __fastcall RifleButtIsPlayer2(uint8_t*)
{
    if (!g_State.playing || !g_State.p2 || g_State.boss)
        return false;
    const uint8_t* context = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptContext));
    const uint8_t* script = Field<uint8_t*>(g_State.p2, kCharacterScript);
    if (!context || !script || *reinterpret_cast<uint8_t* const*>(script + kScriptInstance) != context)
        return false;
    // The sequence is entered again every frame while it plays: the choice is made when it starts (no
    // call for a moment before) and kept, so one attack is a shot or a rifle butt, never both.
    static ULONGLONG lastCall = 0;
    static bool shot = false;
    const ULONGLONG now = GetTickCount64();
    if (now - lastCall > 150)
        shot = input::Player2Shooting();
    lastCall = now;
    static uint8_t* noted = nullptr;
    if (shot && noted != g_State.p2) {
        noted = g_State.p2;
        LOG_INFO("Co-op: player 2's clone fires its blaster (heavy attack: the AI's shot instead of the rifle butt)");
    }
    return shot;
}

__declspec(naked) void RifleButtHook()
{
    __asm {
        push ecx
        call RifleButtIsPlayer2
        pop ecx
        test al, al
        jz original
        mov eax, kBlastAttack
        jmp eax
    original:
        jmp [g_RifleButtStub]
    }
}

// --- Player 2 keeps their limbs ---
// A saber cut takes a limb (TDismembermentManager): the manager tells the character's script
// (0x2DB950, stdcall (character, kind, value, part)), whose callback hides the limb and, for a clone,
// takes away its blaster (the arm is gone). A clone dies of the hit as a rule; player 2, as strong as
// player 1, lived on armless. Player 2's characters are not told.
constexpr uint32_t kDismemberNotify = 0x002DB950;
constexpr uint8_t kDismemberNotifyBytes[] = { 0x8B, 0x0D, 0x98, 0x32, 0x7F, 0x00 }; // mov ecx, [0x7F3298]
using DismemberNotifyFn = void(__stdcall*)(uint8_t*, uint32_t, uint32_t, void*);
DismemberNotifyFn g_OriginalDismemberNotify = nullptr;

// The cut itself: the character's dismemberment binding (vtable 0x5D7D24, its character at +4) slot 2
// (0x2DBEA0, thiscall (part, value)) tells the script, then takes the limb off the body. Skipped whole.
constexpr uint32_t kDismemberCut = 0x002DBEA0;
constexpr uint8_t kDismemberCutBytes[] = { 0x8B, 0x44, 0x24, 0x08, 0x56 }; // mov eax, [esp + 8]; push esi
using DismemberCutFn = uint32_t(__fastcall*)(uint8_t*, void*, void*, uint32_t);
DismemberCutFn g_OriginalDismemberCut = nullptr;

uint32_t __fastcall DismemberCutHook(uint8_t* binding, void*, void* part, uint32_t value)
{
    uint8_t* character = binding ? Field<uint8_t*>(binding, 4) : nullptr;
    if (character && character == g_State.p2 && g_State.playing && !g_State.boss) {
        static uint8_t* noted = nullptr;
        if (noted != character) {
            noted = character;
            LOG_INFO("Co-op: player 2 keeps a limb a hit would have cut off");
        }
        return 0;
    }
    return g_OriginalDismemberCut(binding, nullptr, part, value);
}

void __stdcall DismemberNotifyHook(uint8_t* character, uint32_t kind, uint32_t value, void* part)
{
    if (character && character == g_State.p2 && g_State.playing && !g_State.boss)
        return;
    g_OriginalDismemberNotify(character, kind, value, part);
}

void InstallCoop()
{
    g_State = State{};
    SetFriendlyFire(nullptr, nullptr);
    g_Trail.clear();
    g_LastTrailTick = g_LastCameraTick = 0;
    g_CameraShift[0] = g_CameraShift[1] = g_CameraShift[2] = 0;
    input::SetCoopInput(false);
    // The stubs live in the port's memory and survive reboots; the jumps are patched into every new image.
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kChangeHealth)), kChangeHealthBytes, sizeof(kChangeHealthBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kInstantKill)), kInstantKillBytes, sizeof(kInstantKillBytes)) != 0) {
        LOG_WARN("Co-op: unexpected code at the health change or the instant kill; co-op is off");
        return;
    }
    auto trampoline = [](uint32_t address, const uint8_t* bytes, uint32_t length) {
        uint8_t* stub = AllocStub(length + 5);
        std::memcpy(stub, bytes, length);
        stub[length] = 0xE9;
        const int32_t rel = int32_t(address + length) - int32_t(uintptr_t(stub) + length + 5);
        std::memcpy(stub + length + 1, &rel, 4);
        return stub;
    };
    static uint8_t* changeHealthStub = trampoline(kChangeHealth, kChangeHealthBytes, sizeof(kChangeHealthBytes));
    static uint8_t* instantKillStub = trampoline(kInstantKill, kInstantKillBytes, sizeof(kInstantKillBytes));
    g_OriginalChangeHealth = reinterpret_cast<ChangeHealthFn>(changeHealthStub);
    g_OriginalInstantKill = reinterpret_cast<InstantKillFn>(instantKillStub);
    PatchJump(kChangeHealth, reinterpret_cast<const void*>(&ChangeHealthHook));
    // The pause screen's Cooperative Mod entry and its screen.
    InstallCoopMenus();
    PatchJump(kInstantKill, reinterpret_cast<const void*>(&InstantKillHook));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kRifleButt)), kRifleButtBytes, sizeof(kRifleButtBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBlastAttack)), kBlastAttackBytes, sizeof(kBlastAttackBytes)) == 0) {
        static uint8_t* rifleButtStub = trampoline(kRifleButt, kRifleButtBytes, sizeof(kRifleButtBytes));
        g_RifleButtStub = rifleButtStub;
        PatchJump(kRifleButt, reinterpret_cast<const void*>(&RifleButtHook));
    } else {
        LOG_WARN("Co-op: unexpected code at the clone trooper's attacks; player 2's clone uses its rifle butt");
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kDismemberNotify)), kDismemberNotifyBytes, sizeof(kDismemberNotifyBytes)) == 0) {
        static uint8_t* dismemberStub = trampoline(kDismemberNotify, kDismemberNotifyBytes, sizeof(kDismemberNotifyBytes));
        g_OriginalDismemberNotify = reinterpret_cast<DismemberNotifyFn>(dismemberStub);
        PatchJump(kDismemberNotify, reinterpret_cast<const void*>(&DismemberNotifyHook));
    } else {
        LOG_WARN("Co-op: unexpected code at the dismemberment notice; player 2 can lose limbs");
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kDismemberCut)), kDismemberCutBytes, sizeof(kDismemberCutBytes)) == 0) {
        static uint8_t* cutStub = trampoline(kDismemberCut, kDismemberCutBytes, sizeof(kDismemberCutBytes));
        g_OriginalDismemberCut = reinterpret_cast<DismemberCutFn>(cutStub);
        PatchJump(kDismemberCut, reinterpret_cast<const void*>(&DismemberCutHook));
    } else {
        LOG_WARN("Co-op: unexpected code at the dismemberment notice; player 2 can lose limbs");
    }
    SetCameraAdjuster(&SharedCamera);
}

void CoopFrame()
{
    if (!g_OriginalChangeHealth)
        return;
    const Settings& settings = GetSettings();
    uint8_t* player = PlayerObject();
    if (!player) { // menus, Versus, or between levels
        input::SetCoopInput(false);
        g_State.reason = "no story mission is running";
        return;
    }
    const ULONGLONG now = GetTickCount64();
    FollowPlayer1(player, now);
    if (!g_State.levelSeen)
        g_State.levelSeen = now;
    if (now - g_State.levelSeen < kSettleMs) {
        g_State.reason = "the mission is starting";
        return;
    }
    if (!g_State.decided) {
        // A mission whose slot 2 is taken: a co-op bonus mission's player 2 left to the AI (Cin Drallig
        // beside Serra, without a second controller) is a companion like any other; a boss fight's boss
        // (slot 2 shows its health: Dooku, Grievous, the Mustafar duels) is player 2's to play, as in Versus;
        // a player 2 already playing is left alone.
        uint8_t* manager = Manager();
        g_State.decided = true;
        if (manager && Field<int>(manager, kManagerPlayerCount) >= 2) {
            uint8_t* other = ObjectById(Field<uint32_t>(manager, kManagerPlayerIds + 4));
            const bool usable = other && other != player && Alive(other) && Field<int>(other, kCharacterControl) != kControlPlayer;
            const bool companion = usable && Field<int>(other, kCharacterControl) == kControlCompanion && !TargetsPlayer(other) &&
                IsJedi(other);
            const bool boss = usable && TargetsPlayer(other);
            g_State.nativeTwoPlayers = !companion && !boss;
            if (boss) {
                g_State.bossMission = true;
                g_State.bossObject = other;
                g_State.bossId = Field<uint32_t>(other, kInstanceId);
            }
            LOG_INFO("Co-op: this mission has a player 2 of its own (%s)%s", other ? TypeName(other) : "none",
                companion ? ", left to the AI: co-op plays it" : boss ? ", the boss" : ": left alone");
        }
    }
    if (g_State.gameOverSent) // the mission is lost: nothing more until it starts again
        return;
    if (g_State.nativeTwoPlayers || g_State.bossDone) {
        Unregister(g_State.bossDone ? "the boss fell" : "this mission has a player 2 of its own");
        input::SetCoopInput(false);
        input::SetPlayer2ShootButton(false);
        g_State.reason = g_State.bossDone ? "the boss fell" : "this mission has a player 2 of its own";
        return;
    }
    if (g_State.bossMission) {
        // Player 2 As Boss: the boss; else a character of player 1's side beside them (the boss keeps the
        // second slot and its health bar). Changed while playing, player 2 changes over.
        const bool wantBoss = settings.coopBoss;
        if (wantBoss != g_State.boss || (wantBoss && !g_State.p2)) {
            Unregister(wantBoss ? "player 2 plays the boss now" : "Player 2 As Boss was turned off");
            if (!g_State.boss && g_State.spawned && StillThere()) {
                RemoveSpawnedCharacter(g_State.p2);
                LOG_INFO("Co-op: the character spawned for player 2 was removed");
            }
            g_State.boss = wantBoss;
            g_State.spawned = false;
            g_State.p2 = nullptr;
            g_State.p2Class.clear();
            g_State.costume.clear();
            g_State.skin.clear();
            g_State.respawnAt = 0;
            if (wantBoss && ObjectById(g_State.bossId) == g_State.bossObject) {
                g_State.p2 = g_State.bossObject;
                g_State.p2Id = g_State.bossId;
                g_State.p2Class = TypeName(g_State.bossObject);
            } else if (wantBoss) {
                g_State.bossDone = true; // the boss is gone
                return;
            }
        }
    }
    input::SetCoopInput(settings.coop);
    if (!settings.coop) {
        Unregister("co-op was turned off");
        if (g_State.spawned && StillThere()) { // co-op's own character goes with it
            RemoveSpawnedCharacter(g_State.p2);
            LOG_INFO("Co-op: the character spawned for player 2 was removed");
        }
        if (g_State.spawned) {
            g_State.p2 = nullptr;
            g_State.spawned = false;
            g_State.p2Class.clear();
        }
        g_State.reason = "co-op is off";
        return;
    }
    const bool controller = input::Player2HasController();
    input::SetPlayer2ShootButton(g_State.playing && !g_State.boss && _strnicmp(g_State.p2Class.c_str(), "IClone", 6) == 0);

    // Story safety: in a cutscene (and a moment after) co-op leaves the characters to it: no leash, no
    // shared camera, no joining. Player 2 keeps the character: the cutscene plays it all the same, and a
    // character let go of for a cutscene and taken again stood still afterwards (Cin Drallig in her bonus
    // mission, Obi-Wan in the first mission), whatever its controls said.
    if (InCutscene())
        g_State.cutsceneSeen = now;
    const bool cutscene = settings.coopStorySafety && g_State.cutsceneSeen && now - g_State.cutsceneSeen < kAfterCutsceneMs;
    if (cutscene) {
        g_State.farSince = 0;
        g_State.reason = "a cutscene is playing";
        if (!g_State.playing)
            return;
    }

    if (g_State.playing && g_State.boss) {
        // As in Versus: the game's rules for the boss (its health, its death), its camera.
        if (!StillThere() || !Alive(g_State.p2)) {
            Unregister("the boss fell");
            g_State.bossDone = true;
        } else if (!ControlsIntact(g_State.p2)) {
            LOG_INFO("Co-op: the boss's controls were changed (control %d); given back", Field<int>(g_State.p2, kCharacterControl));
            Field<int>(g_State.p2, kCharacterControl) = kControlPlayer;
            BindController(g_State.p2, kPlayer2Port);
        }
        if (g_State.playing && !controller) {
            Unregister("the controller went");
            g_State.reason = "waiting for a controller for player 2";
        }
        return;
    }
    if (g_State.playing) {
        if (!StillThere()) {
            Unregister("the character is gone");
            g_State.p2 = nullptr;
            g_State.respawnAt = now + kRespawnDelayMs;
        } else if (g_State.respawnPending) {
            g_State.respawnPending = false;
            Respawn(player);
        } else if (g_State.rescuePending) {
            g_State.rescuePending = false;
            PlaceSafely(player);
            Field<uint8_t>(g_State.p2, kCharacterInvincible) = 1;
            g_State.shieldUntil = now + kShieldMs;
            LOG_INFO("Co-op: player 2 would have fallen to their death: brought back to player 1");
        } else if (!Alive(g_State.p2)) {
            // Died all the same (a fall, a scripted death), or under the game over rule.
            // The game over comes once their death has played out (as player 1's does).
            if (settings.coopDeath == 1) {
                if (!g_State.downAt)
                    g_State.downAt = now;
                if (Field<uint8_t>(g_State.p2, kCharacterDead) || now - g_State.downAt >= kDeathPlaysMs)
                    GameOver();
                return;
            }
            Unregister("player 2's character died");
            g_State.p2 = nullptr;
            g_State.respawnAt = now + kRespawnDelayMs;
        }
        if (g_State.playing && StillThere() && Alive(g_State.p2) && !ControlsIntact(g_State.p2)) {
            const uint8_t* entry = PortEntry(kPlayer2Port);
            LOG_INFO("Co-op: player 2's controls were changed (control %d, controller %d, port 1 drives %p); given back",
                Field<int>(g_State.p2, kCharacterControl), Field<int>(g_State.p2, kCharacterController),
                entry ? Field<uint8_t*>(const_cast<uint8_t*>(entry), kPortCharacter) : nullptr);
            Field<int>(g_State.p2, kCharacterControl) = kControlPlayer;
            BindController(g_State.p2, kPlayer2Port);
            ++g_State.controlsLost;
        }
        // Friendly fire: the two players' blows hurt each other while they stay allies (characters.cpp).
        if (settings.coopFriendlyFire && g_State.playing && StillThere())
            SetFriendlyFire(player, g_State.p2);
        else
            SetFriendlyFire(nullptr, nullptr);
        if (g_State.playing && StillThere() && Alive(g_State.p2) && !cutscene) {
            Leash(player, now);
            MatchPlayer1(player); // player 1's maximums can grow (upgrades)
        }
        if (g_State.shieldUntil && now >= g_State.shieldUntil && StillThere()) {
            g_State.shieldUntil = 0;
            Field<uint8_t>(g_State.p2, kCharacterInvincible) = 0;
        }
        if (g_State.playing && !controller) {
            Unregister("the controller went");
            g_State.reason = "waiting for a controller for player 2";
        }
        return;
    }

    if (!controller) {
        g_State.reason = "waiting for a controller for player 2";
        return;
    }
    if (g_State.respawnAt && now < g_State.respawnAt) {
        g_State.reason = "player 2 comes back in a moment";
        return;
    }
    if (g_State.boss) {
        if (!Alive(g_State.p2)) {
            g_State.bossDone = true;
            return;
        }
        Register();
        return;
    }
    if (!Alive(g_State.p2)) {
        // Every second at most while no character can be had.
        static ULONGLONG lastTry = 0;
        if (now - lastTry < 1000)
            return;
        lastTry = now;
        if (!PickCharacter(player))
            return;
        if (g_State.respawnAt && g_State.spawned)
            Respawn(player);
    }
    g_State.respawnAt = 0;
    Register();
}

void CoopCharacterRemoving(uint8_t* character)
{
    if (!character || character != g_State.p2)
        return;
    Unregister("the character is removed", true);
    g_State.p2 = nullptr;
    g_State.respawnAt = GetTickCount64() + kRespawnDelayMs;
}

uint8_t* CoopPlayer2()
{
    return g_State.playing && StillThere() ? g_State.p2 : nullptr;
}

CoopState GetCoopState()
{
    CoopState s;
    const Settings& settings = GetSettings();
    s.enabled = settings.coop;
    s.levelAllows = PlayerObject() && !g_State.nativeTwoPlayers;
    s.boss = g_State.boss;
    s.controller = input::Player2HasController();
    s.playing = g_State.playing;
    s.player2 = g_State.p2Class;
    s.spawned = g_State.spawned;
    if (g_State.playing && StillThere()) {
        s.health = Field<float>(g_State.p2, kCharacterHealth);
        s.maxHealth = Field<float>(g_State.p2, kCharacterMaxHealth);
        if (uint8_t* player = PlayerObject())
            s.distance = Distance(player, g_State.p2);
    }
    s.cutscene = s.levelAllows && InCutscene();
    s.reason = g_State.reason;
    return s;
}

} // namespace swrots::game
