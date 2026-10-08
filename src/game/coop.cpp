// Co-op (see coop.h). How the game does two players, and what the port does to get them in a story
// mission, is in docs/research/coop.md.

#include "game/coop.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "game/characters.h"
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
constexpr uint32_t kCharacterPower = 0xA40;      // float: the Force (Jedi-like characters)
constexpr uint32_t kCharacterMaxPower = 0xA44;   // float
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

// The camera's focus lists (TCamComPrimaryFocusList): the targets it keeps in view, an array at +0x18
// (capacity), +0x1C (count), +0x20 (entries, 0x70 bytes: +0 "the player", +4 the target, +8 its node);
// grown with 0xC4C10 (thiscall on the array: count, const entry* fill), resolved with vtable slot 5
// (0xC4650, thiscall (owner), the owner kept at +4), which drops entries without a target.
constexpr uint32_t kFocusListVtable = 0x0056CB0C;
constexpr uint32_t kFocusListResolve = 0x000C4650;
constexpr uint8_t kFocusListResolveBytes[] = { 0x8B, 0x44, 0x24, 0x04, 0x55 };
constexpr uint32_t kFocusArrayGrow = 0x000C4C10;
constexpr uint32_t kFocusCapacity = 0x18, kFocusCount = 0x1C, kFocusEntries = 0x20;
constexpr uint32_t kFocusEntrySize = 0x70;

constexpr ULONGLONG kSettleMs = 1500;      // after a level start, before player 2 joins
constexpr ULONGLONG kRespawnDelayMs = 3000; // a player 2 that died anyway (a fall): a new one after this
constexpr ULONGLONG kShieldMs = 2000;       // a player 2 who came back cannot be hurt for this long
constexpr ULONGLONG kAfterCutsceneMs = 1000; // player 2 takes the companion again this long after a cutscene
constexpr ULONGLONG kDeathPlaysMs = 6000;    // a death's fall, at most (about 4 s)
constexpr float kBesideDistance = 80.0f;    // where player 2 comes back: beside player 1
// The leash: the camera keeps both players in view only while they are this close (beyond, it frames player 1
// alone, as without co-op: in open places such as the first mission's hangar, framing two players a few
// hundred units apart pulls it far out into space), and a player 2 left behind (or gone ahead, or fallen)
// is brought back beside player 1. A character is about 70 units tall.
constexpr float kCameraDropDistance = 300.0f;
constexpr float kCameraTakeDistance = 220.0f;
constexpr float kLeashDistance = 500.0f;
constexpr ULONGLONG kLeashMs = 2000;
constexpr float kFarDistance = 900.0f;
constexpr ULONGLONG kFarMs = 500;

using ChangeHealthFn = void(__fastcall*)(uint8_t*, void*, float, uint32_t, uint32_t);
using ResolveFn = void(__fastcall*)(uint8_t*, void*, uint32_t);
ChangeHealthFn g_OriginalChangeHealth = nullptr;
ResolveFn g_OriginalResolve = nullptr;

// This boot's (the running level's) state.
struct State {
    ULONGLONG levelSeen = 0;      // when the level's player was first seen
    bool nativeTwoPlayers = false; // the mission has its own player 2 (a co-op bonus mission)
    bool decided = false;          // nativeTwoPlayers was looked at
    uint8_t* p2 = nullptr;         // the character player 2 plays (or would)
    uint32_t p2Id = 0;
    bool spawned = false;          // co-op spawned it
    std::string p2Class;
    bool playing = false;          // registered as player 2
    int savedControl = 0;
    uint8_t savedInvincible = 0;
    int savedCount = 1;
    uint32_t savedSlotId = 0;     // the mission's own player 2 (a co-op bonus mission), or 0
    bool cameraDropped = false;    // player 2 is out of the camera's lists (too far)
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
std::set<uint8_t*> g_FocusLists; // the lists player 2 was added to
bool g_InResolve = false;

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

// --- The camera ---

bool ReadsAsFocusList(uint8_t* object)
{
    __try {
        return *reinterpret_cast<uint32_t*>(object) == kFocusListVtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Adds player 2 to a focus list that keeps player 1 in view (its first entry is "the player").
void AddToFocusList(uint8_t* list)
{
    const int count = Field<int>(list, kFocusCount);
    uint8_t* entries = Field<uint8_t*>(list, kFocusEntries);
    if (count < 1 || !entries || entries[0] != 1)
        return;
    for (int i = 0; i < count; ++i)
        if (Field<uint8_t*>(entries + i * kFocusEntrySize, 4) == g_State.p2)
            return;
    alignas(16) uint8_t fill[kFocusEntrySize];
    std::memcpy(fill, entries, sizeof(fill)); // player 1's entry, as the template
    fill[0] = 0;
    Field<uint8_t*>(fill, 4) = g_State.p2;
    Field<uint32_t>(fill, 8) = 0;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int, const uint8_t*)>(uintptr_t(kFocusArrayGrow))(
        list + kFocusCapacity, nullptr, count + 1, fill);
    g_InResolve = true;
    g_OriginalResolve(list, nullptr, Field<uint32_t>(list, 4));
    g_InResolve = false;
    g_FocusLists.insert(list);
}

void RemoveFromFocusLists()
{
    for (uint8_t* list : g_FocusLists) {
        if (!ReadsAsFocusList(list))
            continue;
        const int count = Field<int>(list, kFocusCount);
        uint8_t* entries = Field<uint8_t*>(list, kFocusEntries);
        bool found = false;
        for (int i = 0; entries && i < count; ++i) {
            uint8_t* entry = entries + i * kFocusEntrySize;
            if (Field<uint8_t*>(entry, 4) == g_State.p2) {
                entry[0] = 0;
                Field<uint8_t*>(entry, 4) = nullptr;
                Field<uint32_t>(entry, 8) = 0; // resolving drops it
                found = true;
            }
        }
        if (found) {
            g_InResolve = true;
            g_OriginalResolve(list, nullptr, Field<uint32_t>(list, 4));
            g_InResolve = false;
        }
    }
    g_FocusLists.clear();
}

// The focus lists in a block of the game's memory, found by their vtable; the number found (at most `room`).
size_t ScanForFocusLists(uintptr_t base, size_t size, uint8_t** found, size_t room)
{
    size_t n = 0;
    __try {
        const uint32_t* words = reinterpret_cast<const uint32_t*>(base);
        for (size_t i = 0; i + 16 <= size / 4 && n < room; ++i)
            if (words[i] == kFocusListVtable)
                found[n++] = reinterpret_cast<uint8_t*>(base + i * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// The focus lists there already.
std::vector<uint8_t*> FindFocusLists()
{
    std::vector<uint8_t*> lists;
    uint8_t* found[256];
    for (const auto& [base, size] : kernel::GameMemoryRegions())
        lists.insert(lists.end(), found, found + ScanForFocusLists(base, size, found, std::size(found)));
    return lists;
}

// Lists resolved later (a new camera after a cutscene) get player 2 too.
void __fastcall ResolveHook(uint8_t* list, void*, uint32_t owner)
{
    g_OriginalResolve(list, nullptr, owner);
    if (!g_InResolve && g_State.playing && !g_State.cameraDropped && StillThere())
        AddToFocusList(list);
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

// --- Damage ---

// Player 2 does not die under the respawn rule: a hit that would kill them leaves them with a little
// health (so the game starts no death), and they come back beside player 1 on the next frame.
void __fastcall ChangeHealthHook(uint8_t* character, void*, float change, uint32_t a, uint32_t b)
{
    g_OriginalChangeHealth(character, nullptr, change, a, b);
    if (character != g_State.p2 || !g_State.playing || GetSettings().coopDeath != 0)
        return;
    float& health = Field<float>(character, kCharacterHealth);
    if (health <= 0.0f && !Field<uint8_t>(character, kCharacterDead)) {
        health = 1.0f;
        g_State.respawnPending = true;
    }
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

void Register()
{
    uint8_t* c = g_State.p2;
    uint8_t* manager = Manager();
    if (!manager)
        return;
    g_State.savedControl = Field<int>(c, kCharacterControl);
    g_State.savedInvincible = Field<uint8_t>(c, kCharacterInvincible);
    g_State.savedCount = Field<int>(manager, kManagerPlayerCount);
    g_State.savedSlotId = Field<uint32_t>(manager, kManagerPlayerIds + 4);
    g_State.savedMaxHealth = Field<float>(c, kCharacterMaxHealth);
    g_State.savedMaxPower = Field<float>(c, kCharacterMaxPower);
    if (uint8_t* player = PlayerObject())
        MatchPlayer1(player);
    Field<int>(c, kCharacterControl) = kControlPlayer;
    BindController(c, kPlayer2Port);
    Field<uint8_t>(c, kCharacterInvincible) = 0;
    // The second HUD (the third HudVitals, which names slot 2) shows whoever is in slot 2 while there are two.
    Field<uint32_t>(manager, kManagerPlayerIds + 4) = g_State.p2Id;
    Field<int>(manager, kManagerPlayerCount) = 2;
    ResetPlayer2Hud(false); // the portrait is picked for this character
    g_State.playing = true;
    g_State.controlsLost = 0;
    const ULONGLONG started = GetTickCount64();
    g_State.cameraDropped = false;
    g_State.farSince = 0;
    for (uint8_t* list : FindFocusLists())
        AddToFocusList(list);
    LOG_INFO("Co-op: player 2 plays %s %s (control was %d); %zu camera list(s), %llu ms", g_State.spawned ? "the spawned" : "the companion",
        g_State.p2Class.c_str(), g_State.savedControl, g_FocusLists.size(), GetTickCount64() - started);
}

// Player 2 stops playing: the character goes back to the game (its AI, its own invincibility).
void Unregister(const char* why)
{
    if (!g_State.playing)
        return;
    g_State.playing = false;
    g_State.respawnPending = false;
    if (uint8_t* manager = Manager()) {
        // As the mission had them: a co-op bonus mission's own player 2 stays in its slot.
        Field<uint32_t>(manager, kManagerPlayerIds + 4) = g_State.savedSlotId;
        Field<int>(manager, kManagerPlayerCount) = g_State.savedCount;
    }
    if (!g_State.savedSlotId)
        ResetPlayer2Hud(true);
    if (StillThere()) {
        RemoveFromFocusLists();
        UnbindController(g_State.p2);
        Field<int>(g_State.p2, kCharacterControl) = g_State.savedControl;
        Field<uint8_t>(g_State.p2, kCharacterInvincible) = g_State.savedInvincible;
        if (!g_State.spawned) { // a companion is as strong as the story made it again
            SetMaximum(g_State.p2, kCharacterHealth, kCharacterMaxHealth, g_State.savedMaxHealth);
            if (IsJedi(g_State.p2))
                SetMaximum(g_State.p2, kCharacterPower, kCharacterMaxPower, g_State.savedMaxPower);
        }
    } else {
        RemoveFromFocusLists(); // the entries still name the character (by pointer only)
    }
    LOG_INFO("Co-op: player 2 left %s (%s)", g_State.p2Class.c_str(), why);
}


// The mission's companion: a living Jedi beside the player whom the story moves (control mode 16) and
// who is not the player's enemy (Obi-Wan beside Anakin, Cin Drallig beside Serra), the nearest to player
// 1. Allied soldiers (the Jedi Temple's clones) and R2-D2 are not companions: without one, player 2 gets
// a character of their own.
uint8_t* FindCompanion(uint8_t* player)
{
    uint8_t* best = nullptr;
    float bestDistance = 0;
    const float* p = &Field<float>(player, kCharacterTransform + 48);
    for (uint8_t* c : LevelCharacters()) {
        if (c == player || !Alive(c) || Field<int>(c, kCharacterControl) != kControlCompanion || TargetsPlayer(c) ||
            !IsJedi(c))
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

std::string SpawnClass(uint8_t* player)
{
    if (!g_State.p2Class.empty() && g_State.spawned)
        return g_State.p2Class; // the same again after a death
    const std::string& chosen = GetSettings().coopPlayer2;
    if (!chosen.empty())
        return chosen;
    return _stricmp(TypeName(player), "IObiwan") == 0 ? "IAnakin" : "IObiwan";
}

// Picks player 2's character: the companion, or a new one beside player 1.
bool PickCharacter(uint8_t* player)
{
    if (uint8_t* companion = FindCompanion(player)) {
        g_State.p2 = companion;
        g_State.spawned = false;
    } else {
        const std::string name = SpawnClass(player);
        std::string error;
        if (!SpawnCharacter(name.c_str(), "", "", "", SpawnSide::Ally, error) || !LastSpawnedObject()) {
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
    return true;
}

// Player 2 placed beside player 1.
void PlaceBeside(uint8_t* player)
{
    float m[16];
    std::memcpy(m, &Field<float>(player, kCharacterTransform), sizeof(m));
    for (int i = 0; i < 3; ++i)
        m[12 + i] += m[i] * kBesideDistance; // to player 1's right
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(VirtualFunction(g_State.p2, kPlaceSlot))(
        g_State.p2, nullptr, m);
}

// The camera keeps player 2 in view while the players are close enough; the leash brings them back.
void Leash(uint8_t* player, ULONGLONG now)
{
    const float distance = Distance(player, g_State.p2);
    if (!g_State.cameraDropped && distance > kCameraDropDistance) {
        RemoveFromFocusLists();
        g_State.cameraDropped = true;
        LOG_INFO("Co-op: %.0f apart: the camera follows player 1 alone", distance);
    } else if (g_State.cameraDropped && distance < kCameraTakeDistance) {
        g_State.cameraDropped = false;
        for (uint8_t* list : FindFocusLists())
            AddToFocusList(list);
        LOG_INFO("Co-op: %.0f apart: the camera keeps both players in view", distance);
    }
    if (distance <= kLeashDistance) {
        g_State.farSince = 0;
        return;
    }
    if (!g_State.farSince)
        g_State.farSince = now;
    if (now - g_State.farSince >= kLeashMs || (distance > kFarDistance && now - g_State.farSince >= kFarMs)) {
        PlaceBeside(player);
        g_State.farSince = 0;
        LOG_INFO("Co-op: player 2 was %.0f away and came back beside player 1", distance);
    }
}

// Player 2 back on their feet beside player 1, with full health and a moment's shield.
void Respawn(uint8_t* player)
{
    PlaceBeside(player);
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

void InstallCoop()
{
    g_State = State{};
    g_FocusLists.clear();
    g_InResolve = false;
    input::SetCoopInput(false);
    // The stubs live in the port's memory and survive reboots; the jumps are patched into every new image.
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kChangeHealth)), kChangeHealthBytes, sizeof(kChangeHealthBytes)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kFocusListResolve)), kFocusListResolveBytes,
            sizeof(kFocusListResolveBytes)) != 0) {
        LOG_WARN("Co-op: unexpected code at the health change or the camera list; co-op is off");
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
    static uint8_t* resolveStub = trampoline(kFocusListResolve, kFocusListResolveBytes, sizeof(kFocusListResolveBytes));
    g_OriginalChangeHealth = reinterpret_cast<ChangeHealthFn>(changeHealthStub);
    g_OriginalResolve = reinterpret_cast<ResolveFn>(resolveStub);
    PatchJump(kChangeHealth, reinterpret_cast<const void*>(&ChangeHealthHook));
    PatchJump(kFocusListResolve, reinterpret_cast<const void*>(&ResolveHook));
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
    if (!g_State.levelSeen)
        g_State.levelSeen = now;
    if (now - g_State.levelSeen < kSettleMs) {
        g_State.reason = "the mission is starting";
        return;
    }
    if (!g_State.decided) {
        // A mission whose slot 2 is taken: a co-op bonus mission's player 2 left to the AI (Cin Drallig
        // beside Serra, without a second controller) is a companion like any other; a story duel's
        // opponent (slot 2 shows the opponent's health) or a player 2 already playing is left alone.
        uint8_t* manager = Manager();
        g_State.decided = true;
        if (manager && Field<int>(manager, kManagerPlayerCount) >= 2) {
            uint8_t* other = ObjectById(Field<uint32_t>(manager, kManagerPlayerIds + 4));
            const bool companion = other && other != player && Alive(other) &&
                Field<int>(other, kCharacterControl) == kControlCompanion && !TargetsPlayer(other) && IsJedi(other);
            g_State.nativeTwoPlayers = !companion;
            LOG_INFO("Co-op: this mission has a player 2 of its own (%s)%s", other ? TypeName(other) : "none",
                companion ? ", left to the AI: co-op plays it" : ": left alone");
        }
    }
    if (g_State.gameOverSent) // the mission is lost: nothing more until it starts again
        return;
    if (g_State.nativeTwoPlayers) {
        input::SetCoopInput(false);
        g_State.reason = "this mission has a player 2 of its own";
        return;
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

    // Story safety: in a cutscene the companion is the game's (scripts move it, the cutscene keeps it
    // alive); player 2 takes it again a moment after.
    if (settings.coopStorySafety && InCutscene()) {
        g_State.cutsceneSeen = now;
        Unregister("a cutscene");
        g_State.reason = "a cutscene is playing";
        return;
    }
    if (g_State.cutsceneSeen && now - g_State.cutsceneSeen < kAfterCutsceneMs) {
        g_State.reason = "a cutscene is playing";
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
        if (g_State.playing && StillThere() && Alive(g_State.p2)) {
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
    g_State.reason.clear();
    Register();
}

void CoopCharacterRemoving(uint8_t* character)
{
    if (!character || character != g_State.p2)
        return;
    Unregister("the character is removed");
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
    s.reason = g_State.reason;
    return s;
}

} // namespace swrots::game
