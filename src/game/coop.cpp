// Co-op (see coop.h). How the game does two players, and what the port does to get them in a story
// mission, is in docs/research/coop.md.

#include "game/coop.h"

#include <windows.h>

#include <algorithm>
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
constexpr uint32_t kCharacterControl = 0x390;    // 2: a player's; 16: a story companion's
constexpr uint32_t kCharacterInvincible = 0x5D6; // byte: "Invincible?" (story companions have it)
constexpr uint32_t kCharacterAIData = 0xA00;
constexpr uint32_t kAITargetPlayer = 0x50;       // byte: the player's enemy
constexpr int kControlPlayer = 2;
constexpr int kControlCompanion = 16;
constexpr uint32_t kTypeNameSlot = 3;            // vfunc: the class's name
constexpr uint32_t kPlaceSlot = 0x1F4 / 4;       // vfunc, thiscall (const float m[16])
constexpr uint32_t kBindController = 0x00150580; // thiscall (int controller; -1 none)
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
    if (!g_InResolve && g_State.playing && StillThere())
        AddToFocusList(list);
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

// --- Player 2 ---

void Register()
{
    uint8_t* c = g_State.p2;
    uint8_t* manager = Manager();
    if (!manager)
        return;
    g_State.savedControl = Field<int>(c, kCharacterControl);
    g_State.savedInvincible = Field<uint8_t>(c, kCharacterInvincible);
    g_State.savedCount = Field<int>(manager, kManagerPlayerCount);
    Field<int>(c, kCharacterControl) = kControlPlayer;
    BindController(c, kPlayer2Port);
    Field<uint8_t>(c, kCharacterInvincible) = 0;
    // The second HUD (the third HudVitals, which names slot 2) shows whoever is in slot 2 while there are two.
    Field<uint32_t>(manager, kManagerPlayerIds + 4) = g_State.p2Id;
    Field<int>(manager, kManagerPlayerCount) = 2;
    g_State.playing = true;
    const ULONGLONG started = GetTickCount64();
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
        if (Field<uint32_t>(manager, kManagerPlayerIds + 4) == g_State.p2Id)
            Field<uint32_t>(manager, kManagerPlayerIds + 4) = 0;
        Field<int>(manager, kManagerPlayerCount) = std::min(g_State.savedCount, 1);
    }
    if (StillThere()) {
        RemoveFromFocusLists();
        BindController(g_State.p2, -1);
        Field<int>(g_State.p2, kCharacterControl) = g_State.savedControl;
        Field<uint8_t>(g_State.p2, kCharacterInvincible) = g_State.savedInvincible;
    } else {
        g_FocusLists.clear();
    }
    LOG_INFO("Co-op: player 2 left %s (%s)", g_State.p2Class.c_str(), why);
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

// Player 2 back on their feet beside player 1, with full health and a moment's shield.
void Respawn(uint8_t* player)
{
    float m[16];
    std::memcpy(m, &Field<float>(player, kCharacterTransform), sizeof(m));
    for (int i = 0; i < 3; ++i)
        m[12 + i] += m[i] * kBesideDistance; // to player 1's right
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(VirtualFunction(g_State.p2, kPlaceSlot))(
        g_State.p2, nullptr, m);
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
        // A mission with two players of its own (the co-op bonus missions, from their menu) is left alone.
        uint8_t* manager = Manager();
        g_State.nativeTwoPlayers = manager && Field<int>(manager, kManagerPlayerCount) >= 2;
        g_State.decided = true;
        if (g_State.nativeTwoPlayers)
            LOG_INFO("Co-op: this mission has two players of its own");
    }
    if (g_State.gameOverSent) // the mission is lost: nothing more until it starts again
        return;
    if (g_State.nativeTwoPlayers) {
        input::SetCoopInput(false);
        g_State.reason = "this mission has two players of its own";
        return;
    }
    input::SetCoopInput(settings.coop);
    if (!settings.coop) {
        Unregister("co-op was turned off");
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
    }
    s.reason = g_State.reason;
    return s;
}

} // namespace swrots::game
