// Characters: playing a level as another character class, in any of its costumes or any mesh.

#include "game/characters.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "game/coop.h"
#include "game/game.h"
#include "game/resources.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

// The chosen class, in the port's own memory: the game's copy of the name is gone after a reboot
// (every level change or restart), and the choice lasts until the game is closed.
std::string g_PlayerClassName;
const char* g_PlayerClass = nullptr;
std::string g_PlayerVariant; // a costume: its name, a part of it or its number; empty for the usual one
std::string g_PlayerMesh;    // a mesh under meshes/chars ("folder\\file"); empty for the costume's own
std::string g_PlayerSkin;    // a texture set: its number or name; empty for the costume's usual one
bool g_SaberColorSet = false; // the player's own saber colour (see ApplySaberColor)
float g_SaberColor[3] = {};

std::string Lower(std::string text)
{
    for (char& c : text)
        c = char(tolower(static_cast<unsigned char>(c)));
    return text;
}

bool MeshOnDisc(const char* mesh)
{
    return mesh && DiscHasResource(Lower(std::string("meshes\\chars\\") + mesh + ".msh"));
}

// A costume list's records, up to the first null name.
int VariantCount(const uint8_t* list)
{
    int count = 0;
    while (list && count < 32 && *reinterpret_cast<const char* const*>(list + kVariantRecords + count * kVariantRecordSize))
        ++count;
    return count;
}

const char* VariantName(const uint8_t* list, int i)
{
    return *reinterpret_cast<const char* const*>(list + kVariantRecords + i * kVariantRecordSize);
}

const char* VariantMesh(const uint8_t* list, int i)
{
    return *reinterpret_cast<const char* const*>(list + kVariantRecords + i * kVariantRecordSize + 4);
}

// A costume by its number, its name or a part of its name (the shortest name containing it: "duel"
// is Anakin_Duel); -1 when none match or two equally short ones do.
int FindVariant(const uint8_t* list, const std::string& spec)
{
    const int count = VariantCount(list);
    if (spec.empty() || !count)
        return -1;
    if (std::all_of(spec.begin(), spec.end(), [](char c) { return isdigit(static_cast<unsigned char>(c)) != 0; })) {
        int i = atoi(spec.c_str());
        return i < count ? i : -1;
    }
    for (int i = 0; i < count; ++i)
        if (_stricmp(VariantName(list, i), spec.c_str()) == 0)
            return i;
    // The shortest name containing it: "duel" is Anakin_Duel rather than Anakin_NPC_Duel.
    int found = -1;
    size_t shortest = 0;
    bool tie = false;
    for (int i = 0; i < count; ++i) {
        const std::string name = Lower(VariantName(list, i));
        if (name.find(Lower(spec)) == std::string::npos)
            continue;
        if (found < 0 || name.size() < shortest) {
            found = i;
            shortest = name.size();
            tie = false;
        } else if (name.size() == shortest) {
            tie = true;
        }
    }
    return tie ? -1 : found;
}

// A pointer to a short printable string in .rdata (where the costume tables' texts are).
bool IsRDataText(uint32_t p)
{
    if (p < kRDataStart || p >= kRDataEnd)
        return false;
    const char* t = reinterpret_cast<const char*>(uintptr_t(p));
    for (int i = 0; i < 80 && p + i < kRDataEnd; ++i) {
        if (!t[i])
            return i > 0;
        if (t[i] < 0x20 || t[i] > 0x7E)
            return false;
    }
    return false;
}

// A costume list's texture sets ("Starting texture set" in the game's level data): after the header's
// name and four numbers, up to six texture name suffixes, e.g. the clone trooper's "_var01" and
// "_var02". A character's +0x1EC picks one (1 the first; 0 the plain textures): when its mesh loads
// (0x155763), each texture is looked for as <name><suffix> first (0x14F7E0), where the level has it.
constexpr uint32_t kTextureSets = 0x14;
constexpr int kMaxTextureSets = 6;
constexpr uint32_t kCharacterTextureSet = 0x1EC;

std::vector<const char*> TextureSets(const uint8_t* list)
{
    std::vector<const char*> sets;
    for (int i = 0; list && i < kMaxTextureSets; ++i) {
        const uint32_t p = *reinterpret_cast<const uint32_t*>(list + kTextureSets + i * 4);
        if (!IsRDataText(p))
            break;
        sets.push_back(reinterpret_cast<const char*>(uintptr_t(p)));
    }
    return sets;
}

// A texture set by number (0 the plain textures) or name, with or without its "_" ("var01"); -1 when
// there is none.
int FindTextureSet(const uint8_t* list, const std::string& spec)
{
    const std::vector<const char*> sets = TextureSets(list);
    if (spec.empty())
        return -1;
    if (std::all_of(spec.begin(), spec.end(), [](char c) { return isdigit(static_cast<unsigned char>(c)) != 0; })) {
        const int i = atoi(spec.c_str());
        return i <= int(sets.size()) ? i : -1;
    }
    const std::string want = Lower(spec[0] == '_' ? spec.substr(1) : spec);
    if (want == "default" || want == "plain")
        return 0;
    for (size_t i = 0; i < sets.size(); ++i) {
        const std::string name = Lower(sets[i][0] == '_' ? sets[i] + 1 : sets[i]);
        if (name == want)
            return int(i) + 1;
    }
    return -1;
}

// When a level starts, 0x2AB660 creates the player through kSpawnPlayer with the launch settings'
// player (+0xAC, a string: the mission list's or the versus select's class, e.g. IAnakin); the
// character factory creates it by class name. The chosen class replaces that string there, after
// the mission list had its say. Entry: mov eax, [kEngineServices] (5 bytes).
using SpawnPlayerFn = void(__cdecl*)(void* playerClass, int variant);
SpawnPlayerFn g_OriginalSpawnPlayer = nullptr;

bool g_PlayerSpawned = false;  // a level started this boot (its player was created)

// A restart that falls back to a new game process (core/window.h) would lose the choice, which lives
// in this process: it is handed over as SWROTS_PLAYER, which the new process reads at its start.
void HandChoiceToRelaunch()
{
    char saber[64] = {};
    if (g_SaberColorSet)
        sprintf_s(saber, "%d %d %d", int(g_SaberColor[0] * 255 + 0.5f), int(g_SaberColor[1] * 255 + 0.5f),
            int(g_SaberColor[2] * 255 + 0.5f));
    SetEnvironmentVariableA("SWROTS_SABER", g_SaberColorSet ? saber : nullptr);
    if (!g_PlayerClass && g_PlayerVariant.empty() && g_PlayerSkin.empty() && g_PlayerMesh.empty()) {
        SetEnvironmentVariableA("SWROTS_PLAYER", nullptr);
        return;
    }
    std::string spec = g_PlayerClass ? g_PlayerClass : "-";
    if (g_PlayerClass && !g_PlayerVariant.empty())
        spec += " " + g_PlayerVariant;
    if (g_PlayerClass && !g_PlayerSkin.empty())
        spec += " skin " + g_PlayerSkin;
    if (!g_PlayerMesh.empty())
        spec += " mesh " + g_PlayerMesh;
    SetEnvironmentVariableA("SWROTS_PLAYER", spec.c_str());
}
bool g_RestartOnChange = true; // a change restarts the mission ([Debug] AutoRestart)

// The level's own player, as it asked for it (before any choice replaces it): what "each level's own"
// means for a live change.
std::string g_LevelClassName;
int g_LevelCostume = -1;
// The player the level created, and whether in its own class (no choice): its maximum health is the
// level's own (see ReplacePlayer).
const uint8_t* g_LevelPlayerObject = nullptr;
bool g_LevelPlayerIsOwn = false;

void __cdecl SpawnPlayerHook(void* playerClass, int variant)
{
    g_PlayerSpawned = true;
    const char* levelClass = playerClass ? *reinterpret_cast<const char* const*>(playerClass) : nullptr;
    g_LevelClassName = levelClass ? levelClass : "";
    LOG_INFO("Characters: the level's player is %s", g_LevelClassName.c_str());
    if (g_PlayerClass && playerClass) {
        auto assign = reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(uintptr_t(kTStringAssign));
        assign(playerClass, nullptr, g_PlayerClass);
        LOG_INFO("Characters: the player is %s", g_PlayerClass);
    }
    g_OriginalSpawnPlayer(playerClass, variant);
}

// The player's variant: -1 above means the profile's costume, an index into the level's own class's
// variants. The created player gets it through its vfunc +0x2C8 (0xB1D60); at
//   mov eax, [esp + 0xC] / mov edx, [ecx] / push eax   (7 bytes, 0xB1DE1)
// ecx is the new player and [esp + 0xC] the variant. A chosen class gets one whose model is on disc.
constexpr uint32_t kPlayerVariantSite = 0x000B1DE1;
constexpr uint32_t kPlayerVariantContinue = 0x000B1DE8;
constexpr uint8_t kPlayerVariantBytes[] = { 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x11, 0x50 };

// The player's saber colour. A character's +0xF20 points at a colour (float r, g, b, 0-1) that overrides
// its sabers' own: the equip (0x280480, at 0x28054E) gives each saber that instead of its default, and
// the power-up switch (0x286B90, saber vfunc +0x614) leaves the colour alone while it is set (as Versus
// does for its fighters, from 0x650C98). The game's own `sabercolor` instead remaps a colour for every
// saber that has it. A saber's colour is set with its vfunc +0x604 (thiscall (const float rgb[3]));
// the pure colours (1,0,0), (0,1,0), (0,0,1), (1,0,1) are drawn as the game's tuned red, green, blue,
// purple, any other as it is (0x2E1600).
constexpr uint32_t kCharacterSaberColor = 0xF20;
constexpr uint32_t kCharacterWeapons = 0x1080; // 4 slots, 0x20 apart: the weapon object, or null
constexpr int kWeaponSlots = 4;
constexpr uint32_t kSaberSetColor = 0x604;
constexpr uint32_t kSaberVtables[] = { 0x005B1FF0, 0x005B2620, 0x005B2C90, 0x005B32F8, 0x005B3958 };

uint8_t* g_Player = nullptr;      // the level's player, from its creation (this boot)
uint32_t g_PlayerVtable = 0;      // its vtable then: the player is still there while it matches

bool PlayerAlive()
{
    return g_Player && *reinterpret_cast<uint32_t*>(g_Player) == g_PlayerVtable;
}

// Only the Jedi-like characters (IJedi and those built on it) have the colour override and the weapon
// slots; asked as the power-up switch asks (0x286BB2): IsA, vfunc +4, with the type's function as its
// key (a type reference is just that address, 0x23C820).
constexpr uint32_t kJediType = 0x00249950;

bool HasSaberColor(uint8_t* character)
{
    const auto isA = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>(
        (*reinterpret_cast<void* const* const*>(character))[1]);
    return isA(character, nullptr, kJediType);
}

// Gives the player's sabers the chosen colour (or, with none, nothing: they keep theirs until the
// next level start).
// `onlyChanged`: only sabers whose colour (saber +0x814, three floats, as SetColor stores it) is not the
// chosen one. Scripted parts of a level (the Mustafar duel's cutscenes and saber locks) colour the
// sabers themselves, so the player's are checked every frame.
constexpr uint32_t kSaberColor = 0x814;

void ApplySaberColor(bool onlyChanged = false)
{
    if (!PlayerAlive() || !HasSaberColor(g_Player))
        return;
    *reinterpret_cast<const float**>(g_Player + kCharacterSaberColor) = g_SaberColorSet ? g_SaberColor : nullptr;
    if (!g_SaberColorSet)
        return;
    for (int i = 0; i < kWeaponSlots; ++i) {
        uint8_t* weapon = *reinterpret_cast<uint8_t**>(g_Player + kCharacterWeapons + i * 0x20);
        if (!weapon)
            continue;
        const uint32_t vtable = *reinterpret_cast<uint32_t*>(weapon);
        if (std::find(std::begin(kSaberVtables), std::end(kSaberVtables), vtable) == std::end(kSaberVtables))
            continue;
        if (onlyChanged && std::memcmp(weapon + kSaberColor, g_SaberColor, sizeof(g_SaberColor)) == 0)
            continue;
        const auto setColor = reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(
            reinterpret_cast<void* const*>(uintptr_t(vtable))[kSaberSetColor / 4]);
        setColor(weapon, nullptr, g_SaberColor);
    }
}

// A character's own copy of its class's costume list, with another body in its costume's record:
// others of the class (which share the game's list) keep theirs. Kept for as long as the character
// may load its body (the level; cleared at every boot).
struct OwnedList {
    std::string mesh;
    std::vector<uint8_t> list;
};

// Dresses a character being created (before its ICharacter::Init loads the body): the costume
// `costume` (a name, a part of one or a number; empty for `chosen`), texture set `skin` and body
// `mesh` (resolved in place: emptied when not usable). `who` names it in the log. Returns the costume
// index to use.
int Dress(uint8_t* character, int chosen, const std::string& costume, const std::string& skin, std::string& mesh,
    OwnedList& owned, const char* who)
{
    uint8_t*& list = *reinterpret_cast<uint8_t**>(character + kCharacterVariants);
    if (!costume.empty() && list) {
        const int wanted = FindVariant(list, costume);
        if (wanted < 0)
            LOG_WARN("Characters: no costume '%s'; the usual one instead", costume.c_str());
        else if (!mesh.empty() || MeshOnDisc(VariantMesh(list, wanted)))
            chosen = wanted;
        else
            LOG_WARN("Characters: costume %s's model (%s) is not on the disc; the usual one instead",
                VariantName(list, wanted), VariantMesh(list, wanted));
    }
    if (!mesh.empty()) {
        // A name as given (SWROTS_PLAYER takes any): "folder\file" as the disc or mods\ has it.
        std::string error;
        const std::string resolved = ResolveMesh(mesh, error);
        const std::string file = Lower("meshes\\chars\\" + resolved + ".msh");
        if (resolved.empty() || (!DiscHasResource(file) && !HasLooseResource(file))) {
            LOG_WARN("Characters: mesh '%s' not used (%s); the costume's own instead", mesh.c_str(),
                error.empty() ? "not on the disc or under mods" : error.c_str());
            mesh.clear();
        } else {
            mesh = resolved;
        }
    }
    if (!mesh.empty() && list) {
        const int count = VariantCount(list);
        const int slot = chosen >= 0 && chosen < count ? chosen : 0;
        const size_t size = kVariantRecords + (count + 1) * kVariantRecordSize;
        // Another class's body (none of this class's costumes) is worn as a private copy; a body only
        // under mods\ is no one else's.
        bool own = false;
        for (int i = 0; i < count && !own; ++i)
            own = Lower(VariantMesh(list, i)) == Lower(mesh);
        const char* className = *reinterpret_cast<const char* const*>(list);
        owned.mesh = own || !DiscHasResource(Lower("meshes\\chars\\" + mesh + ".msh")) || !className
            ? mesh : PrivateBodyName(mesh, className);
        owned.list.assign(list, list + size - kVariantRecordSize);
        owned.list.resize(size, 0); // the null record that ends it
        *reinterpret_cast<const char**>(owned.list.data() + kVariantRecords + slot * kVariantRecordSize + 4) =
            owned.mesh.c_str();
        list = owned.list.data();
        chosen = slot;
        LOG_INFO("Characters: %s's mesh is %s%s", who, mesh.c_str(), owned.mesh != mesh ? " (its own copy)" : "");
    }
    if (list && chosen >= 0 && chosen < VariantCount(list))
        LOG_INFO("Characters: %s's costume %s (%d)", who, VariantName(list, chosen), chosen);
    if (!skin.empty() && list) {
        const int set = FindTextureSet(list, skin);
        if (set < 0) {
            LOG_WARN("Characters: no texture set '%s'; the usual textures instead", skin.c_str());
        } else {
            *reinterpret_cast<int*>(character + kCharacterTextureSet) = set;
            if (set > 0) {
                // The set's textures from wherever the disc has them: those in the costume mesh's folder
                // ending with the suffix (e.g. meshes\chars\clonetrooper\hordetrooper_var01.stx).
                const std::string suffix = Lower(TextureSets(list)[set - 1]) + ".stx";
                const int slot = chosen >= 0 && chosen < VariantCount(list) ? chosen : 0;
                const std::string body = Lower(VariantMesh(list, slot));
                const std::string folder = "meshes\\chars\\" + body.substr(0, body.find('\\') + 1);
                int declared = 0;
                for (const std::string& name : DiscResourceNames(folder))
                    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                        DeclareDiscResource(name))
                        ++declared;
                LOG_INFO("Characters: %s's texture set %s (%d)%s", who, TextureSets(list)[set - 1], set,
                    declared ? ", textures from other levels" : "");
            }
        }
    }
    return chosen;
}

OwnedList g_PlayerList;

// Spawning a character into the running level, the way the game's own AI respawner does (AI.cpp,
// 0x19C1E0): create the class's object (the class registry's lookup 0xE9B60, then its create 0xE2350:
// class info vfunc +0x30), clear +0xC's 0x08000000, set the costume (+0x1E8), and hand it to the spawn
// (0xA2DE0: cdecl bool (object, owner or null, const float transform[16], 1)), which places it
// (object vfunc +0x1F4) and adds it to the level's instance manager, which initializes it (loading
// what the level lacks through the port's resource hooks). A character's transform is at +0x150:
// rows right, up, forward, position (as the dev item spawner at 0x2DF600 builds one in front of the
// player). The spawned character takes its class's own AI and teams.
constexpr uint32_t kClassLookup = 0x000E9B60;    // cdecl (const char* class) -> class info
constexpr uint32_t kCreateInstance = 0x000E2350; // cdecl (class info) -> object
constexpr uint32_t kSpawnInstance = 0x000A2DE0;  // cdecl bool (object, owner, const float m[16], int 1)
constexpr uint32_t kInstanceFlags = 0x0C;
constexpr uint32_t kInstanceInactive = 0x08000000;
constexpr uint32_t kCharacterCostume = 0x1E8;
constexpr uint32_t kCharacterTransform = 0x150;
constexpr uint32_t kActivate = 0xB4;
constexpr uint32_t kPortPlayerTeam = 0x8000; // a team bit of the port's: the player and its allies
constexpr uint32_t kPortEnemyTeam = 0x4000;  // and its spawned enemies
constexpr uint32_t kPortNeutralTeam = 0x2000; // a neutral spawn (see IsEnemyHook)
constexpr uint32_t kPortRiotTeam = 0x1000;    // a rioting one           // object vfunc, thiscall ()
constexpr float kSpawnDistance = 120.0f; // in front of the player (a character is about 70 tall)

std::list<OwnedList> g_SpawnLists; // the spawned characters' own costume lists (this boot)
int g_Spawned = 0;          // numbers the spawns in the log (this session)
int g_SpawnedInLevel = 0;   // spawned in the running level
// The maximum health chosen on the Game tab (0: the class's), kept through live changes.
float g_ChosenMaxHealth = 0;
// The level's own player's maximum health (levels set their own: Obi-Wan 1000 in Utapau, his class 1500),
// taken at its first live change and given back when its class is again ("player off").
float g_LevelMaxHealth = 0;
std::unordered_map<std::string, std::string> g_BodyClasses; // body -> class (see RecordBody)
uint8_t* g_LastSpawned = nullptr;
uint8_t* g_ReplacedPlayer = nullptr; // the body the last live change replaced (research: findrefs old)
// What the port spawned in the running level, to remove them again (with their vtables then: an
// object the game destroyed meanwhile no longer has it).
struct SpawnedCharacter {
    uint8_t* object;
    uint32_t id; // its instance id (+4): still in the level while the object manager finds it by it
};
std::vector<SpawnedCharacter> g_SpawnedCharacters;

// The game's own way to remove an object (0xA2FE0, cdecl (object): its tear-down, vfunc +0x1CC, then
// the object manager's Delete, 0xB5840, which waits for the end of an update when one is running), as
// some fifty places in the game use it.
constexpr uint32_t kDestroyObject = 0x000A2FE0;

// Deactivated first: vfunc +0xB0 undoes the activation (+0xB4) a spawn ends with, taking the
// character out of what it joined then (e.g. a Jedi's place in the list of characters blaster bolts
// may be deflected by, 0x7EAAF0: IJedi slot 44 removes it, slot 45 adds it). Without it a bolt in
// flight still finds the removed character (0x26F14A).
constexpr uint32_t kDeactivate = 0xB0;

// The object manager's Delete (0xB5840) refuses an object marked protected (+0x11D: bosses such as
// Dooku are spawned with it). The port removes only what it created or replaced, so it clears it.
constexpr uint32_t kObjectProtected = 0x11D;

// The lightsaber users that can deflect bolts ([0x7EAAF0], an array of characters; 0x26F360 adds one,
// 0x26F350 removes one, both cdecl (character)). Only some classes' deactivation takes theirs out
// (Dooku's and a spawned Jedi knight's did not), and a bolt picking a removed one crashed (0x26F14A).
constexpr uint32_t kRemoveDeflector = 0x0026F350;

void DestroyObject(uint8_t* object)
{
    reinterpret_cast<void(__cdecl*)(uint8_t*)>(uintptr_t(kRemoveDeflector))(object);
    object[kObjectProtected] = 0;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>((*reinterpret_cast<void* const* const*>(object))[kDeactivate / 4])(
        object, nullptr);
    reinterpret_cast<void(__cdecl*)(uint8_t*)>(uintptr_t(kDestroyObject))(object);
}

uint8_t* CreateCharacter(const char* name, const std::string& costume, const std::string& skin,
    const std::string& mesh, const char* who, std::string& error)
{
    void* info = reinterpret_cast<void*(__cdecl*)(const char*)>(uintptr_t(kClassLookup))(name);
    auto* object = info ? reinterpret_cast<uint8_t*(__cdecl*)(void*)>(uintptr_t(kCreateInstance))(info) : nullptr;
    if (!object) {
        error = std::string(name) + ": the game could not create it";
        return nullptr;
    }
    *reinterpret_cast<uint32_t*>(object + kInstanceFlags) &= ~kInstanceInactive;
    g_SpawnLists.emplace_back();
    std::string body = mesh;
    const int chosen = Dress(object, VariantOnDisc(object, 0), costume, skin, body, g_SpawnLists.back(), who);
    *reinterpret_cast<int*>(object + kCharacterCostume) = chosen;
    return object;
}

bool PlaceCharacter(uint8_t* object, const float* m, const char* name, std::string& error)
{
    const bool spawned = reinterpret_cast<bool(__cdecl*)(uint8_t*, void*, const float*, int)>(
        uintptr_t(kSpawnInstance))(object, nullptr, m, 1);
    if (!spawned) {
        error = std::string(name) + ": the game refused to spawn it";
        return false;
    }
    // Then activated, as the dev item spawner (0x2DF849) and the player's creation (0xB1D60) do.
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>((*reinterpret_cast<void* const* const*>(object))[kActivate / 4])(
        object, nullptr);
    return true;
}

int __stdcall PlayerVariant(void* player, int variant)
{
    if (!player)
        return variant;
    g_Player = static_cast<uint8_t*>(player);
    g_PlayerVtable = *reinterpret_cast<uint32_t*>(player);
    if (g_SaberColorSet && HasSaberColor(g_Player)) // before it equips its saber (ICharacter::Init)
        *reinterpret_cast<const float**>(g_Player + kCharacterSaberColor) = g_SaberColor;
    if (!g_PlayerClass)
        g_LevelCostume = variant; // the level's costume for its own class
    g_LevelPlayerObject = g_Player;
    g_LevelPlayerIsOwn = !g_PlayerClass;
    const int chosen = g_PlayerClass ? VariantOnDisc(player, variant) : variant;
    return Dress(g_Player, chosen, g_PlayerVariant, g_PlayerSkin, g_PlayerMesh, g_PlayerList, "the player");
}

__declspec(naked) void PlayerVariantStub()
{
    __asm {
        push ecx
        push dword ptr [esp + 0x10] // the variant ([esp + 0xC] at the site)
        push ecx
        call PlayerVariant
        pop ecx
        mov edx, [ecx]
        push eax
        push kPlayerVariantContinue
        ret
    }
}

// HUD portraits. The game manager ([0x7EB964]) keeps a list of character portraits, loaded at the
// level start for the characters the level expects (0x27BE30): an array at +0x260 {capacity, count,
// data} of {face texture, select-screen head texture, face name}, added to with 0x1F8A00. The HUD
// finds the player's face in it by name (0x27B740: thiscall (name, head?)), the name from the
// game's table of portraits per class (kPortraits: 12 faces, then 0; the heads 0x34 bytes on). A
// class the level did not expect is not in the list -- the HUD then shows the missing-texture
// pattern -- so a portrait of the table missing from the list is loaded when asked for, the way the
// level start loads them, and added. Its textures come from another level's PAK if need be.
constexpr uint32_t kPortraitLookup = 0x0027B740;
constexpr uint32_t kPortraitListAdd = 0x001F8A00; // thiscall (const Record*)
constexpr uint32_t kPortraits = 0x00650C30;
constexpr uint32_t kPortraitHeads = kPortraits + 0x34;
constexpr int kPortraitCount = 12;
constexpr uint8_t kPortraitLookupPrologue[] = { 0x55, 0x8B, 0x6C, 0x24, 0x08, 0x85, 0xED }; // push ebp; mov ebp, [esp+8]; test ebp, ebp

struct PortraitRecord {
    void* face;
    void* head;
    const char* name;
};

using PortraitLookupFn = void*(__fastcall*)(uint8_t* manager, void* edx, const char* name, int head);
PortraitLookupFn g_OriginalPortraitLookup = nullptr;

// Loads an interface texture by path through the engine's texture manager, as 0x27C1D1 does.
void* LoadInterfaceTexture(const char* path)
{
    auto* services = *reinterpret_cast<uint8_t**>(uintptr_t(kEngineServices));
    auto* textures = *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(services + 0x38) + 0x4C);
    alignas(4) uint8_t enginePath[0x100] = {};
    auto pathFromText = reinterpret_cast<void*(__fastcall*)(void*, void*, const char*, int)>(uintptr_t(kEnginePathFromText));
    void* filePath = pathFromText(enginePath, nullptr, path, 1);
    int first = 0, second = 0;
    using LoadFn = void*(__fastcall*)(void*, void*, void*, int*, int*, int, int, int);
    auto load = (*reinterpret_cast<LoadFn* const*>(textures))[1];
    return load(textures, nullptr, filePath, &second, &first, 1, 0, 1);
}

void* __fastcall PortraitLookupHook(uint8_t* manager, void* edx, const char* name, int head)
{
    void* found = g_OriginalPortraitLookup(manager, edx, name, head);
    if (found || !manager || !name)
        return found;
    auto* faces = reinterpret_cast<const char* const*>(uintptr_t(kPortraits));
    auto* heads = reinterpret_cast<const char* const*>(uintptr_t(kPortraitHeads));
    for (int i = 0; i < kPortraitCount; ++i) {
        if (!faces[i] || _stricmp(faces[i], name) != 0)
            continue;
        static const uint8_t* failedFor = nullptr; // the manager (level) a portrait could not load in
        static uint32_t failed = 0;
        if (failedFor != manager) {
            failedFor = manager;
            failed = 0;
        }
        if (failed & (1u << i))
            return nullptr;
        PortraitRecord record = { LoadInterfaceTexture(faces[i]), heads[i] ? LoadInterfaceTexture(heads[i]) : nullptr,
            faces[i] };
        if (!record.face) {
            failed |= 1u << i;
            LOG_WARN("Characters: HUD portrait %s did not load", faces[i]);
            return nullptr;
        }
        // Added even without a head, so it is loaded once per level.
        reinterpret_cast<void(__fastcall*)(void*, void*, const PortraitRecord*)>(uintptr_t(kPortraitListAdd))(
            manager + 0x260, nullptr, &record);
        LOG_INFO("Characters: HUD portrait %s loaded", faces[i]);
        return head ? record.head : record.face;
    }
    return nullptr;
}

} // namespace

int VariantOnDisc(const void* character, int preferred)
{
    if (!character)
        return preferred;
    const uint8_t* list = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(character) + 0x1E0);
    int first = -1;
    for (int i = 0; list && i < 32; ++i) {
        const uint8_t* record = list + 0x2C + i * 20;
        const char* name = *reinterpret_cast<const char* const*>(record);
        const char* mesh = *reinterpret_cast<const char* const*>(record + 4);
        if (!name)
            break;
        if (!mesh)
            continue;
        std::string path = std::string("meshes\\chars\\") + mesh + ".msh";
        for (char& c : path)
            c = char(tolower(static_cast<unsigned char>(c)));
        if (!DiscHasResource(path))
            continue;
        if (i == preferred)
            return i;
        if (first < 0)
            first = i;
    }
    if (first < 0 || first == preferred)
        return preferred;
    const char* name = *reinterpret_cast<const char* const*>(list + 0x2C + first * 20);
    if (preferred >= VariantCount(list))
        LOG_INFO("Characters: costume %s (%d): the class has no costume %d (the level's)", name, first, preferred);
    else
        LOG_INFO("Characters: costume %s (%d) instead of %d, whose model is not on the disc", name, first, preferred);
    return first;
}

bool ClassHasBody(const char* className)
{
    const uint8_t* list = FindVariantList(className);
    if (!list)
        return true; // no costume list: nothing known against it
    // A body: a mesh shipped with its animation binding (Poggle's static model has none).
    for (int i = 0, n = VariantCount(list); i < n; ++i)
        if (MeshOnDisc(VariantMesh(list, i)) &&
            DiscHasResource(Lower(std::string("meshes\\chars\\") + VariantMesh(list, i) + ".ban")))
            return true;
    return false;
}

const char* RegisteredClassName(const char* name)
{
    auto* registry = *reinterpret_cast<uint8_t**>(uintptr_t(kClassRegistry));
    if (!registry || !name)
        return nullptr;
    auto** buckets = reinterpret_cast<uint8_t**>(registry + kClassRegistryBuckets);
    for (int b = 0; b < kClassRegistryBucketCount; ++b) {
        for (uint8_t* node = buckets[b]; node; node = *reinterpret_cast<uint8_t**>(node + 8)) {
            const char* className = *reinterpret_cast<const char**>(node + 4);
            if (className && _stricmp(className, name) == 0)
                return className;
        }
    }
    return nullptr;
}

// Optional moves. A character's behaviour sequences marked optional (the duel moves: blocks against a
// saber, Block2_*; shunts and traps, the way into a saber lock; some specials) are loaded only in
// levels whose characters use them; elsewhere a character in a level not made for it misses them
// (the branch to one is refused, see fixes.cpp). The launch settings' developer switch
// `optionalanims` (+0xE4, read once by the level loader, 0xB12E9: mov dl, [ecx + 0xE4]) has every
// level load all of them, with their animations (from other levels' PAKs where needed: resources.cpp).
// The port turns it on by changing that read to `mov dl, 1`.
constexpr uint32_t kOptionalMovesRead = 0x000B12E9;
constexpr uint8_t kOptionalMovesReadBytes[] = { 0x8A, 0x91, 0xE4, 0x00, 0x00, 0x00 };
bool g_OptionalMoves = false;

void EnableOptionalMoves(bool enabled)
{
    g_OptionalMoves = enabled;
}

// "Is that character my enemy?" (0x18F3F0, thiscall on a character's AI controller (other, flag), its
// character at +0x10): a rioting character is everyone's enemy and everyone is its; a neutral one is
// no one's and no one is its. Otherwise the game decides.
constexpr uint32_t kIsEnemy = 0x0018F3F0;
constexpr uint8_t kIsEnemyPrologue[] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14 };
using IsEnemyFn = bool(__fastcall*)(uint8_t*, void*, uint8_t*, int);
IsEnemyFn g_OriginalIsEnemy = nullptr;

uint32_t TeamsOf(const uint8_t* character)
{
    const uint8_t* ai = character ? *reinterpret_cast<uint8_t* const*>(character + 0xA00) : nullptr;
    return ai ? *reinterpret_cast<const uint32_t*>(ai + 0x214) : 0;
}

// The side a character fights on, for one the port gave a side (or against one): the player's (the
// player, its allies, a level character not targeting the player) or the enemies' (the port's enemies, a
// level character targeting the player). Team bits alone do not do: fighting a character with teams
// gives the player every team that one lacks (0x18FA8C), the port's among them.
enum class Side { Player, Enemies, Neutral, Riot };

Side SideOf(const uint8_t* character, uint32_t teams)
{
    if (character == g_Player)
        return Side::Player;
    if (teams & kPortRiotTeam)
        return Side::Riot;
    if (teams & kPortNeutralTeam)
        return Side::Neutral;
    if (teams & kPortPlayerTeam)
        return Side::Player;
    if (teams & kPortEnemyTeam)
        return Side::Enemies;
    const uint8_t* ai = *reinterpret_cast<uint8_t* const*>(character + 0xA00);
    return ai && ai[0x50] ? Side::Enemies : Side::Player;
}

bool __fastcall IsEnemyHook(uint8_t* brain, void* edx, uint8_t* other, int flag)
{
    const uint8_t* self = brain ? *reinterpret_cast<uint8_t* const*>(brain + 0x10) : nullptr;
    if (self && other && self != other) {
        constexpr uint32_t kPortSides = kPortPlayerTeam | kPortEnemyTeam | kPortNeutralTeam | kPortRiotTeam;
        const uint32_t mine = self == g_Player ? 0 : TeamsOf(self), theirs = other == g_Player ? 0 : TeamsOf(other);
        if ((mine | theirs) & kPortSides) {
            const Side a = SideOf(self, mine), b = SideOf(other, theirs);
            if (a == Side::Riot || b == Side::Riot)
                return true;
            if (a == Side::Neutral || b == Side::Neutral)
                return false;
            return a != b;
        }
    }
    return g_OriginalIsEnemy(brain, edx, other, flag);
}

// Neutral spawns and the health they had: one that loses health was attacked, and riots.
struct NeutralSpawn {
    uint8_t* object;
    uint32_t id;
    float health;
};
std::vector<NeutralSpawn> g_Neutrals;

void InstallCharacters()
{
    g_Neutrals.clear();
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kIsEnemy)), kIsEnemyPrologue, sizeof(kIsEnemyPrologue)) == 0) {
        uint8_t* stub = AllocStub(16);
        std::memcpy(stub, kIsEnemyPrologue, sizeof(kIsEnemyPrologue));
        stub[8] = 0xE9;
        const int32_t back = int32_t(kIsEnemy + 8) - int32_t(uintptr_t(stub) + 13);
        std::memcpy(stub + 9, &back, 4);
        g_OriginalIsEnemy = reinterpret_cast<IsEnemyFn>(stub);
        PatchJump(kIsEnemy, reinterpret_cast<const void*>(&IsEnemyHook));
    } else {
        LOG_WARN("Characters: unexpected code at the enemy check; neutral and riot sides unavailable");
    }
    if (g_OptionalMoves) {
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kOptionalMovesRead)), kOptionalMovesReadBytes,
                sizeof(kOptionalMovesReadBytes)) == 0) {
            static const uint8_t kMoveOne[] = { 0xB2, 0x01, 0x90, 0x90, 0x90, 0x90 }; // mov dl, 1
            PatchBytes(kOptionalMovesRead, kMoveOne, sizeof(kMoveOne));
        } else {
            LOG_WARN("Characters: unexpected code at the optional moves read; levels load their usual moves");
        }
    }
    // Development aid: SWROTS_PLAYER=<class> from the start, for unattended tests; read at the first
    // boot only, so the console's choice survives reboots. The class name is checked when the
    // registry exists (it does not yet at boot), so it is used as given.
    static bool environmentRead = false;
    char spec[128] = {};
    if (!environmentRead && GetEnvironmentVariableA("SWROTS_PLAYER", spec, sizeof(spec))) {
        // "<class>[ <costume>][ mesh <mesh>]", as the console's player command; "-" keeps the level's class.
        std::vector<std::string> words;
        char* context = nullptr;
        for (char* w = strtok_s(spec, " ", &context); w; w = strtok_s(nullptr, " ", &context))
            words.push_back(w);
        for (size_t i = 0; i < words.size(); ++i) {
            if (i == 0) {
                if (words[0] != "-") {
                    g_PlayerClassName = words[0];
                    g_PlayerClass = g_PlayerClassName.c_str();
                }
            } else if (_stricmp(words[i].c_str(), "mesh") == 0 && i + 1 < words.size()) {
                g_PlayerMesh = words[++i]; // as given; resolved at the spawn (PlayerVariant)
            } else if (_stricmp(words[i].c_str(), "skin") == 0 && i + 1 < words.size()) {
                g_PlayerSkin = words[++i];
            } else {
                g_PlayerVariant = words[i];
            }
        }
    }
    char saber[64] = {};
    if (!environmentRead && GetEnvironmentVariableA("SWROTS_SABER", saber, sizeof(saber))) {
        float rgb[3];
        g_SaberColorSet = ParseSaberColor(saber, rgb);
        if (g_SaberColorSet)
            std::copy(rgb, rgb + 3, g_SaberColor);
    }
    environmentRead = true;
    g_PlayerSpawned = false;
    g_Player = nullptr;
    g_SpawnLists.clear(); // the level's characters went with the reboot
    g_SpawnedInLevel = 0;
    g_LastSpawned = nullptr;
    g_ReplacedPlayer = nullptr;
    g_SpawnedCharacters.clear();
    g_BodyClasses.clear();
    g_LevelMaxHealth = 0;
    g_ChosenMaxHealth = 0;
    g_LevelCostume = -1; // taken again when the level's own player is created
    g_LevelPlayerObject = nullptr;
    g_LevelPlayerIsOwn = false;
    kernel::SetBeforeRelaunch(&HandChoiceToRelaunch);
    uint8_t* stub = AllocStub(16);
    std::memcpy(stub, reinterpret_cast<const void*>(uintptr_t(kSpawnPlayer)), 5);
    stub[5] = 0xE9;
    int32_t rel = int32_t(kSpawnPlayer + 5) - int32_t(uintptr_t(stub) + 10);
    std::memcpy(stub + 6, &rel, 4);
    g_OriginalSpawnPlayer = reinterpret_cast<SpawnPlayerFn>(stub);
    PatchJump(kSpawnPlayer, reinterpret_cast<const void*>(&SpawnPlayerHook));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kPlayerVariantSite)), kPlayerVariantBytes,
            sizeof(kPlayerVariantBytes)) == 0) {
        PatchJump(kPlayerVariantSite, reinterpret_cast<const void*>(&PlayerVariantStub));
        PatchNop(kPlayerVariantSite + 5, sizeof(kPlayerVariantBytes) - 5);
    } else {
        LOG_WARN("Characters: unexpected code at the player variant site; variants unchanged");
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kPortraitLookup)), kPortraitLookupPrologue,
            sizeof(kPortraitLookupPrologue)) == 0) {
        uint8_t* portraitStub = AllocStub(16);
        std::memcpy(portraitStub, kPortraitLookupPrologue, sizeof(kPortraitLookupPrologue));
        portraitStub[7] = 0xE9;
        int32_t back = int32_t(kPortraitLookup + 7) - int32_t(uintptr_t(portraitStub) + 12);
        std::memcpy(portraitStub + 8, &back, 4);
        g_OriginalPortraitLookup = reinterpret_cast<PortraitLookupFn>(portraitStub);
        PatchJump(kPortraitLookup, reinterpret_cast<const void*>(&PortraitLookupHook));
    } else {
        LOG_WARN("Characters: unexpected code at the HUD portrait lookup; portraits unchanged");
    }
}

// Every costume list in the image, by the name its header gives (lower case), found once: the lists
// are static data, the same at every boot.
const std::unordered_map<std::string, const uint8_t*>& VariantLists()
{
    static std::unordered_map<std::string, const uint8_t*> lists;
    static bool scanned = false;
    if (scanned)
        return lists;
    scanned = true;
    const auto isText = IsRDataText;
    // A header's name and four small numbers (the weapon, font and skeleton tables built alike have
    // pointers there), then (at +0x2C) a first record: a costume name without a backslash and its mesh
    // ("Folder\File") with one.
    for (uint32_t a = kDataStart; a + kVariantRecords + kVariantRecordSize <= kDataEnd; a += 4) {
        const uint32_t name = *reinterpret_cast<const uint32_t*>(uintptr_t(a));
        if (!isText(name))
            continue;
        const auto* numbers = reinterpret_cast<const uint32_t*>(uintptr_t(a + 4));
        if (numbers[0] == 0 || numbers[0] >= 0x10000 || numbers[1] >= 0x10000 || numbers[2] >= 0x10000 ||
            numbers[3] >= 0x10000)
            continue;
        const uint32_t first = *reinterpret_cast<const uint32_t*>(uintptr_t(a + kVariantRecords));
        const uint32_t mesh = *reinterpret_cast<const uint32_t*>(uintptr_t(a + kVariantRecords + 4));
        if (isText(first) && isText(mesh) && !std::strchr(reinterpret_cast<const char*>(uintptr_t(first)), '\\') &&
            std::strchr(reinterpret_cast<const char*>(uintptr_t(mesh)), '\\'))
            lists.emplace(Lower(reinterpret_cast<const char*>(uintptr_t(name))), reinterpret_cast<const uint8_t*>(uintptr_t(a)));
    }
    return lists;
}

const uint8_t* FindVariantList(const char* className)
{
    if (!className)
        return nullptr;
    // The list names the class without its "I" (IAnakin -> Anakin).
    const char* wanted = (className[0] == 'I' || className[0] == 'i') && className[1] ? className + 1 : className;
    const auto& lists = VariantLists();
    const auto it = lists.find(Lower(wanted));
    return it != lists.end() ? it->second : nullptr;
}

std::vector<std::string> CharacterClasses(bool all)
{
    std::vector<std::string> out;
    auto* registry = *reinterpret_cast<uint8_t**>(uintptr_t(kClassRegistry));
    if (!registry)
        return out;
    auto** buckets = reinterpret_cast<uint8_t**>(registry + kClassRegistryBuckets);
    for (int b = 0; b < kClassRegistryBucketCount; ++b) {
        for (uint8_t* node = buckets[b]; node; node = *reinterpret_cast<uint8_t**>(node + 8)) {
            const char* className = *reinterpret_cast<const char**>(node + 4);
            if (className && (all || FindVariantList(className)))
                out.push_back(className);
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) { return _stricmp(x.c_str(), y.c_str()) < 0; });
    return out;
}

bool PlayerInLevel()
{
    return g_PlayerSpawned;
}

void SetRestartOnChange(bool enabled)
{
    g_RestartOnChange = enabled;
}

bool RestartOnChange()
{
    return g_RestartOnChange;
}

int ClassVariantIndex(const char* className, const std::string& spec)
{
    return FindVariant(FindVariantList(className), spec);
}

std::vector<Variant> ClassVariants(const char* className)
{
    std::vector<Variant> out;
    const uint8_t* list = FindVariantList(className);
    for (int i = 0, n = VariantCount(list); i < n; ++i)
        out.push_back({ VariantName(list, i), VariantMesh(list, i), MeshOnDisc(VariantMesh(list, i)) });
    return out;
}

std::vector<std::string> CharacterMeshes(const std::string& filter)
{
    std::vector<std::string> out;
    const std::string want = Lower(filter);
    const std::string prefix = "meshes\\chars\\";
    // The disc's character bodies: the meshes shipped with an animation binding (.ban). The others are
    // limbs, debris, vehicles and effects (skeleton\skeleton_lightningfx), which crash the game as a
    // character (0x611B4). A mod's own meshes count: the engine makes their binding.
    const std::vector<std::string> disc = DiscResourceNames(prefix);
    std::vector<std::string> names;
    for (const std::string& name : disc) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".msh") == 0 &&
            std::find(disc.begin(), disc.end(), name.substr(0, name.size() - 4) + ".ban") != disc.end())
            names.push_back(name);
    }
    for (const std::string& name : LooseResourceNames(prefix))
        names.push_back(name);
    for (const std::string& name : names) {
        if (name.size() < prefix.size() + 4 || name.compare(name.size() - 4, 4, ".msh") != 0)
            continue;
        std::string mesh = name.substr(prefix.size(), name.size() - prefix.size() - 4);
        if (mesh.find('\\') == std::string::npos || (!want.empty() && mesh.find(want) == std::string::npos))
            continue;
        out.push_back(mesh);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string ResolveMesh(const std::string& text, std::string& error)
{
    std::string want = Lower(text);
    std::replace(want.begin(), want.end(), '/', '\\');
    const std::string prefix = "meshes\\chars\\";
    if (want.rfind(prefix, 0) == 0)
        want = want.substr(prefix.size());
    if (want.size() > 4 && want.compare(want.size() - 4, 4, ".msh") == 0)
        want.resize(want.size() - 4);
    std::vector<std::string> matches;
    for (const std::string& mesh : CharacterMeshes("")) {
        const size_t slash = mesh.find('\\');
        if (mesh == want || mesh.substr(0, slash) == want || mesh.substr(slash + 1) == want)
            matches.push_back(mesh);
    }
    if (matches.size() == 1)
        return matches[0];
    if (matches.size() > 1) {
        error = "several meshes match: " + matches[0] + ", " + matches[1] + (matches.size() > 2 ? ", ..." : "");
        return "";
    }
    if (want.find('\\') != std::string::npos && DiscHasResource(prefix + want + ".msh")) {
        error = want + " is not a character body (limbs, debris, a vehicle or an effect; see meshes)";
        return "";
    }
    if (want.find('\\') != std::string::npos)
        return want; // not on the disc: a loose copy under mods may provide it
    error = "no character mesh '" + text + "' on the disc (see meshes)";
    return "";
}

void SetPlayerVariant(const std::string& variant)
{
    g_PlayerVariant = variant;
}

void SetPlayerMesh(const std::string& mesh)
{
    g_PlayerMesh = mesh;
}

std::string PlayerVariantChoice()
{
    return g_PlayerVariant;
}

std::string PlayerMesh()
{
    return g_PlayerMesh;
}

void SetPlayerSkin(const std::string& skin)
{
    g_PlayerSkin = skin;
}

std::string PlayerSkin()
{
    return g_PlayerSkin;
}

std::vector<std::string> ClassTextureSets(const char* className)
{
    std::vector<std::string> out;
    for (const char* set : TextureSets(FindVariantList(className)))
        out.push_back(set);
    return out;
}

int ClassTextureSetIndex(const char* className, const std::string& spec)
{
    const uint8_t* list = FindVariantList(className);
    return list ? FindTextureSet(list, spec) : -1;
}

// --- Changing the player live ---------------------------------------------------------------------
// The new character (the current choice of class, costume, texture set and body) is spawned where the
// player stands and takes over:
// - "the player" (0xA30F0, some 400 callers) is the primary character record's +0xC (record
//   [0x68DCEC], 0xB1C60); controller 0 is bound with 0x150580 (thiscall (int id), rebinding the input
//   manager's slot, 0x8ADD0); +0x390 = 2 marks the player-controlled character; 0x68EF70 points at the
//   player's transform (+0x150);
// - the level's own references to the player move to it (found by a scan, as `findrefs` lists them):
//   the camera's focus lists (TCamComPrimaryFocusList +0x44) and controls (ICameraControl), the level's
//   triggers and points (ICharacterGoto +0x1F0, IPointSpawnPlayer, ICinematicsPlayer, IGuardPoint,
//   IInvisibleBrush), and other characters' references (their targets). The old body's own parts (its
//   saber, effect groups, mesh instances, animation commits) keep pointing at it;
// - the old player is marked inactive (+0xC 0x08000000) and moved far below the level.
constexpr uint32_t kBindController = 0x00150580;    // thiscall (int id)
constexpr uint32_t kPrimaryCharacter = 0x0068DCEC;  // the player's creation record: +0xC the player
constexpr uint32_t kPlayerTransformRef = 0x0068EF70; // const float* (the player's +0x150)
constexpr uint32_t kCharacterControl = 0x390;       // 2: the player's
constexpr uint32_t kPlace = 0x1F4;                   // object vfunc, thiscall (const float m[16])

// The level's references to the player, by the object type's vtable and the field (as `findrefs`
// found them in the Jedi Temple): only a dword in such a field of such an object moves.
struct LevelReference {
    uint32_t vtable;
    uint32_t field;
    uint32_t into; // where in the player it points: 0 the player, 0x150 its transform
};
constexpr uint32_t kGameManager = 0x007EB964;      // the game manager (pointer)
constexpr uint32_t kManagerPlayerSlots = 0x1E4;    // its player slots
constexpr uint32_t kManagerPlayerIds = 0x2A4;      // a player's instance id per slot
constexpr uint32_t kFocusListVtable = 0x0056CB0C; // TCamComPrimaryFocusList
constexpr uint32_t kHudVitalsVtable = 0x005A82B0;  // the HUD's portrait (HudVitals.cpp)
constexpr LevelReference kLevelReferences[] = {
    { 0x0056CB0C, 0x044, 0 },     // TCamComPrimaryFocusList: the camera's focus
    { 0x0056D028, 0x274, 0 },     // ICameraControl
    { 0x0056D028, 0x20C, 0 },
    { 0x0056D028, 0x21C, 0 },
    { 0x00598970, 0x1F0, 0 },     // ICharacterGoto: a trigger moving the player
    { 0x00580AF8, 0x21C, 0 },     // IPointSpawnPlayer
    { 0x00599128, 0x1E0, 0 },     // ICinematicsPlayer
    { 0x00592F30, 0x1F0, 0 },     // IGuardPoint
    { 0x0057E168, 0x05C, 0 },     // IInvisibleBrush
};

// Pages known readable or not during a scan (RepointPlayerReferences), so that following a pointer
// into memory that is not there does not fault.
std::unordered_map<uintptr_t, bool>* g_ReadablePages = nullptr;

bool PageReadable(uintptr_t a)
{
    const uintptr_t page = a & ~uintptr_t(0xFFF);
    if (g_ReadablePages) {
        const auto it = g_ReadablePages->find(page);
        if (it != g_ReadablePages->end())
            return it->second;
    }
    MEMORY_BASIC_INFORMATION info{};
    const bool readable = VirtualQuery(reinterpret_cast<const void*>(page), &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (g_ReadablePages)
        (*g_ReadablePages)[page] = readable;
    return readable;
}

bool ReadGameDword(uintptr_t a, uint32_t& v)
{
    if (!PageReadable(a) || !PageReadable(a + 3))
        return false;
    __try {
        v = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The level reference at `a` pointing `into` the player, if it is one.
bool IsLevelReference(uintptr_t a, uintptr_t base, uint32_t into)
{
    for (const LevelReference& r : kLevelReferences) {
        uint32_t vtable;
        if (r.into == into && a >= base + r.field && ReadGameDword(a - r.field, vtable) && vtable == r.vtable)
            return true;
    }
    return false;
}

// What `a` lies in: a character or one of its two AI objects, as each pair shows. A character points at
// its AI controller (+0x9FC, 0x591910-based, which points back at it at +0x10 and holds the AI's target,
// +0x41C, with the target's instance id at +0x418: 0x193D90) and at its AI data (+0xA00, 0x591C38-based,
// back at +0x29C). Another character's pointers to the player (its target, an opponent) are the level's.
constexpr uint32_t kCharacterAIData = 0xA00;
constexpr uint32_t kAIHasSide = 0x1C;     // AI data: set where "Target Player" counts as a side
constexpr uint32_t kAITargetPlayer = 0x50; // AI data "Target Player": the player's enemy
constexpr uint32_t kAIIgnoredByAI = 0x52;  // AI data "Ignored By AI"
struct AIPart { uint32_t inCharacter, owner; };
constexpr AIPart kAIParts[] = { { 0x9FC, 0x10 }, { kCharacterAIData, 0x29C } };
constexpr uint32_t kDuelMasterCameraVtable = 0x005B4DC0; // IMasterCameraVader
constexpr uint32_t kMasterCameraTargetId = 0x23C;
// An instance id (object +4): a kind in the top four bits (0x4: made while playing, 0x7: the level's own
// objects, the level's player among them) and an index in the low 16.
static bool IsInstanceId(uint32_t id) { return (id >> 28) != 0 && (id & 0x0FFF0000u) == 0; }

// The game memory being scanned (RepointPlayerReferences), to follow only pointers into it.
const std::vector<std::pair<uintptr_t, size_t>>* g_ScanRegions = nullptr;

bool InScanRegions(uintptr_t a, size_t size)
{
    // The regions are sorted by start: the last one starting at or below `a`.
    const auto& regions = *g_ScanRegions;
    auto it = std::upper_bound(regions.begin(), regions.end(), a,
        [](uintptr_t value, const std::pair<uintptr_t, size_t>& r) { return value < r.first; });
    if (it == regions.begin())
        return false;
    --it;
    return a + size <= it->first + it->second;
}

bool PointsTo(uintptr_t a, uintptr_t value)
{
    uint32_t v = 0;
    return InScanRegions(a, 4) && ReadGameDword(a, v) && v == value;
}

// The character whose AI object `ai` is, or 0.
uintptr_t AIDataOwner(uintptr_t ai)
{
    for (const AIPart& part : kAIParts) {
        uint32_t owner = 0;
        if (InScanRegions(ai + part.owner, 4) && ReadGameDword(ai + part.owner, owner) && owner > 0x10000 &&
            PointsTo(owner + part.inCharacter, ai))
            return owner;
    }
    return 0;
}

enum class Holder { None, Character, AIData };

// Whether `a` lies in a character other than `self` and `fresh` (the old and the new player) or in such a
// character's AI objects (`start`: where it begins). The new player's own are not the level's: its AI
// pointing at the old player is not to become pointing at itself. In a character, only its own fields
// count (from kCharacterOwnFields on: an opponent at +0x3B8): below are the object's links in the
// engine's lists (+0x40, +0x4C: the objects of a sector, drawn through them), which the engine keeps
// as objects come and go; one moved to the new player broke its sector's list, and the new player was
// not drawn (only its saber was) until moving put it in a sector again.
constexpr uintptr_t kCharacterOwnFields = 0x200;
// The nearest object start below `a` decides.
Holder OtherCharacterHolding(uintptr_t a, uintptr_t base, const uint8_t* self, const uint8_t* fresh, uintptr_t& start)
{
    for (uintptr_t o = 0; o < 0x1200 && o <= a - base; o += 4) {
        start = (a & ~uintptr_t(3)) - o;
        uint32_t ai = 0;
        if (InScanRegions(start + kCharacterAIData, 4) && ReadGameDword(start + kCharacterAIData, ai) && ai > 0x10000 &&
            AIDataOwner(ai) == start)
            return start != uintptr_t(self) && start != uintptr_t(fresh) && o >= kCharacterOwnFields ? Holder::Character
                                                                                                   : Holder::None;
        if (o < 0x800) {
            if (const uintptr_t owner = AIDataOwner(start))
                return owner != uintptr_t(self) && owner != uintptr_t(fresh) ? Holder::AIData : Holder::None;
        }
    }
    return Holder::None;
}

// The addresses in [base, base + size) holding a value in [lo, lo + span), `id` or `vtable`, up to
// `capacity`: plain reads page by page (a page not readable is skipped), the first pass of
// RepointPlayerReferences; the candidates are checked one by one afterwards. No C++ objects (SEH).
size_t CollectCandidates(uintptr_t base, size_t size, uint32_t lo, uint32_t span, uint32_t id, uint32_t vtable,
    uintptr_t* out, size_t capacity)
{
    size_t count = 0;
    const uintptr_t end = base + size;
    for (uintptr_t at = base; at < end && count < capacity;) {
        // One run of pages with the same state at a time.
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info))
            break;
        const uintptr_t runEnd = uintptr_t(info.BaseAddress) + info.RegionSize;
        const uintptr_t to = runEnd < end ? runEnd : end;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            __try {
                for (uintptr_t a = (at + 3) & ~uintptr_t(3); a + 4 <= to; a += 4) {
                    const uint32_t v = *reinterpret_cast<const uint32_t*>(a);
                    if ((v - lo < span || v == id || v == vtable) && count < capacity)
                        out[count++] = a;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        at = to;
    }
    return count;
}

// Every character in the level: found through its AI data, which starts with kAIDataMarker and points
// back at its character (+0x29C), which points at it (+0xA00).
constexpr uint32_t kAIDataMarker = 0x00591C38;
constexpr uint32_t kObjectById = 0x000A31F0;

std::vector<uint8_t*> LevelCharacters()
{
    std::vector<uint8_t*> characters;
    std::vector<uintptr_t> found(4096);
    for (const auto& [base, size] : kernel::GameMemoryRegions()) {
        const size_t n = CollectCandidates(base, size, kAIDataMarker, 1, kAIDataMarker, kAIDataMarker, found.data(), found.size());
        for (size_t i = 0; i < n; ++i) {
            uint32_t owner = 0, back = 0;
            uint32_t id = 0;
            // Still in the level: the object manager finds it by its instance id (ObjectById, 0xA31F0,
            // cdecl (id)); a removed character's memory can stay as it was.
            if (ReadGameDword(found[i] + 0x29C, owner) && owner > 0x10000 && ReadGameDword(owner + kCharacterAIData, back) &&
                back == found[i] && ReadGameDword(owner + 4, id) &&
                reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id) == reinterpret_cast<uint8_t*>(uintptr_t(owner)))
                characters.push_back(reinterpret_cast<uint8_t*>(uintptr_t(owner)));
        }
    }
    std::sort(characters.begin(), characters.end());
    characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
    return characters;
}

// IObject's "Uniform scale" (+0x194, IObject::Prop_Serialize 0xF5420), read where objects are drawn
// and moved (e.g. 0x2971DA).
constexpr uint32_t kObjectUniformScale = 0x194;

bool CharacterScale(const uint8_t* character, float& scale)
{
    if (!character)
        return false;
    scale = *reinterpret_cast<const float*>(character + kObjectUniformScale);
    return true;
}

bool SetCharacterScale(uint8_t* character, float scale)
{
    if (!character || !(scale > 0.0f))
        return false;
    *reinterpret_cast<float*>(character + kObjectUniformScale) = scale;
    return true;
}

// Projectiles in flight (blaster bolts, deflected bolts, rockets) keep characters they involve, and a
// deflected one read a character the port had just removed (0x26E6AD, from IDeflectionProjectile's
// update): before the port removes characters (a live change, despawn), the bolts in the air go too.
constexpr uint32_t kBlasterProjectileVtable = 0x005AAB90;
constexpr uint32_t kDeflectionProjectileVtable = 0x005AC4B8;
constexpr uint32_t kRocketProjectileVtable = 0x005B7B90;

int RemoveProjectiles()
{
    std::vector<uintptr_t> found(4096);
    std::vector<uint8_t*> projectiles;
    for (const auto& [base, size] : kernel::GameMemoryRegions()) {
        const size_t n = CollectCandidates(base, size, kBlasterProjectileVtable, 1, kDeflectionProjectileVtable,
            kRocketProjectileVtable, found.data(), found.size());
        for (size_t i = 0; i < n; ++i) {
            uint32_t id = 0;
            // A live object: the object manager finds it by its instance id.
            if (ReadGameDword(found[i] + 4, id) &&
                reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id) == reinterpret_cast<uint8_t*>(found[i]))
                projectiles.push_back(reinterpret_cast<uint8_t*>(found[i]));
        }
    }
    for (uint8_t* projectile : projectiles)
        DestroyObject(projectile);
    if (!projectiles.empty())
        LOG_INFO("Characters: %zu projectile(s) in flight removed", projectiles.size());
    return int(projectiles.size());
}

// Characters targeting one about to be removed (despawn) let it go, through the AI's own SetTarget
// (0x193D90, thiscall on the AI controller (target), null clearing it): a target left behind was read
// once removed (a bolt fired at it, 0x26E6AD).
constexpr uint32_t kAISetTarget = 0x00193D90;
constexpr uint32_t kAIControllerInCharacter = 0x9FC;
constexpr uint32_t kAITarget = 0x41C;

int ClearTargetsOn(const std::vector<uint8_t*>& removed, const std::vector<uint8_t*>& characters)
{
    int cleared = 0;
    for (uint8_t* c : characters) {
        uint8_t* brain = *reinterpret_cast<uint8_t**>(c + kAIControllerInCharacter);
        if (!brain)
            continue;
        uint8_t* target = *reinterpret_cast<uint8_t**>(brain + kAITarget);
        if (!target || std::find(removed.begin(), removed.end(), target) == removed.end())
            continue;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(brain, nullptr, nullptr);
        ++cleared;
    }
    return cleared;
}

// Other characters' own pointers to characters about to be removed (an opponent, an aim), and their AI
// objects', set to none, by the rules a live change moves them by (OtherCharacterHolding).
int ClearReferencesTo(const std::vector<uint8_t*>& removed)
{
    std::vector<std::pair<uintptr_t, size_t>> regions = kernel::GameMemoryRegions();
    std::sort(regions.begin(), regions.end());
    g_ScanRegions = &regions;
    std::unordered_map<uintptr_t, bool> readable;
    g_ReadablePages = &readable;
    std::vector<uintptr_t> candidates(1 << 14);
    int cleared = 0;
    for (uint8_t* object : removed) {
        const uint32_t value = uint32_t(uintptr_t(object));
        for (const auto& [base, size] : regions) {
            const size_t found = CollectCandidates(base, size, value, 1, value, value, candidates.data(), candidates.size());
            for (size_t c = 0; c < found; ++c) {
                const uintptr_t a = candidates[c];
                if (a >= uintptr_t(object) && a < uintptr_t(object) + 0x1200)
                    continue;
                uintptr_t holder = 0;
                if (OtherCharacterHolding(a, base, object, nullptr, holder) == Holder::None)
                    continue;
                const uint8_t* other = reinterpret_cast<const uint8_t*>(holder);
                if (std::find(removed.begin(), removed.end(), other) != removed.end())
                    continue;
                *reinterpret_cast<uint32_t*>(a) = 0;
                ++cleared;
            }
        }
    }
    g_ScanRegions = nullptr;
    g_ReadablePages = nullptr;
    return cleared;
}

int RepointPlayerReferences(uint8_t* from, uint8_t* to)
{
    const ULONGLONG started = GetTickCount64();
    std::vector<std::pair<uintptr_t, size_t>> regions = kernel::GameMemoryRegions();
    regions.push_back({ 0x00612BE0, 0x0096E000 - 0x00612BE0 }); // the image's .data and .bss
    std::sort(regions.begin(), regions.end());
    g_ScanRegions = &regions;
    std::unordered_map<uintptr_t, bool> readable;
    g_ReadablePages = &readable;
    const uint32_t old = uint32_t(uintptr_t(from)), fresh = uint32_t(uintptr_t(to));
    const uint32_t oldId = *reinterpret_cast<const uint32_t*>(from + 4), freshId = *reinterpret_cast<const uint32_t*>(to + 4);
    int ids = 0;
    const uintptr_t skipLo = uintptr_t(from), skipHi = skipLo + 0x1200;
    const uintptr_t newLo = uintptr_t(to), newHi = newLo + 0x1200;
    int moved = 0, kept = 0;
    std::vector<uintptr_t> focusLists;
    int portraits = 0;
    std::vector<uintptr_t> candidates(1 << 16);
    size_t checked = 0;
    for (const auto& [base, size] : regions) {
        const size_t found = CollectCandidates(base, size, old, 0x1200, oldId, kHudVitalsVtable, candidates.data(),
            candidates.size());
        checked += found;
        for (size_t c = 0; c < found; ++c) {
            const uintptr_t a = candidates[c];
            uint32_t v;
            if (!ReadGameDword(a, v))
                continue;
            // The HUD's portrait (HudVitals, 0x25E960, vtable slot 3) is picked once (+0xD set; +0xC when
            // there was none) from the player's class, into +0x10: picked again, it is the new player's.
            if (v == kHudVitalsVtable && a + 0x14 <= base + size) {
                *reinterpret_cast<uint8_t*>(a + 0xC) = 0; // gave up (no face)
                *reinterpret_cast<uint8_t*>(a + 0xD) = 0; // picked
                ++portraits;
                continue;
            }
            // The player's instance id (object +4), which the level's objects keep beside their pointer
            // to it (an object reference: pointer, then id) and look the player up by: in the level's
            // references (ICharacterGoto +0x1F4, the focus lists' +0x48, ICameraControl +0x278), the
            // duel's master camera (IMasterCameraVader +0x23C: it follows the id; with the old one it
            // stayed where it was until the next cutscene), and other characters and their AI (an
            // opponent's targets; the AI's at +0x418).
            if (v == oldId && IsInstanceId(oldId)) {
                uintptr_t holder = 0;
                uint32_t vtable = 0;
                if (IsLevelReference(a - 4, base, 0) ||
                    (a >= base + kMasterCameraTargetId && ReadGameDword(a - kMasterCameraTargetId, vtable) &&
                        vtable == kDuelMasterCameraVtable) ||
                    OtherCharacterHolding(a, base, from, to, holder) != Holder::None) {
                    *reinterpret_cast<uint32_t*>(a) = freshId;
                    ++ids;
                }
                continue;
            }
            if (v < old || v >= old + 0x1200 || (a >= skipLo && a < skipHi) || (a >= newLo && a < newHi))
                continue;
            const uint32_t into = v - old;
            if (into != 0 && into != kCharacterTransform)
                continue;
            uintptr_t holder = 0;
            const Holder held = IsLevelReference(a, base, into) || into != 0 ? Holder::None
                                                                              : OtherCharacterHolding(a, base, from, to, holder);
            if (!IsLevelReference(a, base, into) && held == Holder::None) {
                ++kept;
                continue;
            }
            uint32_t vtable = 0;
            if (ReadGameDword(a - 0x44, vtable) && vtable == kFocusListVtable)
                focusLists.push_back(a - 0x44);
            *reinterpret_cast<uint32_t*>(a) = fresh + into;
            ++moved;
        }
    }
    // The camera's focus lists cache a node of their target's (entry +8, from its vfunc +0x5C) when
    // resolved; resolving them again (slot 5, 0xC4650: thiscall (owner), the owner kept at +4) takes
    // the new player's, for the entries marked as the player's.
    std::sort(focusLists.begin(), focusLists.end());
    focusLists.erase(std::unique(focusLists.begin(), focusLists.end()), focusLists.end());
    for (uintptr_t list : focusLists) {
        auto* object = reinterpret_cast<uint8_t*>(list);
        const auto resolve = reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint32_t)>(
            (*reinterpret_cast<void* const* const*>(object))[5]);
        resolve(object, nullptr, *reinterpret_cast<uint32_t*>(object + 4));
    }
    LOG_INFO("Characters: %zu camera focus list(s) resolved again, %d HUD portrait(s) to pick again, %d "
        "instance id(s) moved, %d other pointer(s) to the old player left (its own parts); %zu candidates, %llu ms",
        focusLists.size(), portraits, ids, kept, checked, GetTickCount64() - started);
    g_ScanRegions = nullptr;
    g_ReadablePages = nullptr;
    return moved;
}

// Whether controller 0's entry in the input manager ([0x68D4F4] +0x1D8[0], its character at +0x7C)
// holds this character.
bool ControllerBoundTo(const uint8_t* character)
{
    const uint8_t* input = *reinterpret_cast<uint8_t* const*>(uintptr_t(0x0068D4F4));
    const uint8_t* entry = input ? *reinterpret_cast<uint8_t* const*>(input + 0x1D8) : nullptr;
    return !entry || *reinterpret_cast<const uint8_t* const*>(entry + 0x7C) == character;
}

constexpr uint32_t kCharacterHealth = 0x130;
constexpr uint32_t kCharacterMaxHealth = 0x134;

// The class each body (mesh) was bound to in this level, by the characters the port placed and the
// players it replaced. A body's animation binding (its .ban) is built for the first class wearing it
// and stays while the mesh is loaded, which the port keeps for the level (see resources.cpp): another
// class in it would animate with the wrong binding (0x663C5: an animation index it does not have).
std::string CharacterBody(const uint8_t* character)
{
    const uint8_t* list = *reinterpret_cast<uint8_t* const*>(character + kCharacterVariants);
    const int costume = *reinterpret_cast<const int*>(character + kCharacterCostume);
    return list && costume >= 0 && costume < VariantCount(list) ? Lower(VariantMesh(list, costume)) : std::string();
}

std::string CharacterType(uint8_t* character)
{
    const auto typeName = reinterpret_cast<const char*(__fastcall*)(uint8_t*, void*)>(
        (*reinterpret_cast<void* const* const*>(character))[3]);
    return Lower(typeName(character, nullptr));
}

void RecordBody(uint8_t* character)
{
    const std::string body = CharacterBody(character);
    if (!body.empty())
        g_BodyClasses.emplace(body, CharacterType(character));
}

bool BodyBoundToAnotherClass(uint8_t* character)
{
    const auto it = g_BodyClasses.find(CharacterBody(character));
    return it != g_BodyClasses.end() && it->second != CharacterType(character);
}

bool ReplacePlayer(std::string& error)
{
    if (!PlayerAlive()) {
        error = g_PlayerSpawned ? "only in story levels, not in Versus" : "no mission is running";
        return false;
    }
    // The class: the chosen one, else the level's own (in its own costume unless one is chosen).
    const char* name = g_PlayerClass ? g_PlayerClass : RegisteredClassName(g_LevelClassName.c_str());
    if (!name && !g_PlayerClass) // the mission list names it without its "I" (Player:Anakin)
        name = RegisteredClassName(("I" + g_LevelClassName).c_str());
    std::string costume = g_PlayerVariant;
    if (!g_PlayerClass && costume.empty() && g_LevelCostume >= 0)
        costume = std::to_string(g_LevelCostume);
    if (!name || !ClassHasBody(name)) {
        error = "the player's class cannot be changed live";
        return false;
    }
    uint8_t* old = g_Player;
    float m[16];
    std::memcpy(m, old + kCharacterTransform, sizeof(m));
    uint8_t* fresh = CreateCharacter(name, costume, g_PlayerSkin, g_PlayerMesh, "the player", error);
    if (!fresh)
        return false;
    // Two classes in one body share its animation binding: a body another class wore in this level is
    // left to a restart (the object was never placed; the game does not see it).
    RecordBody(old);
    if (BodyBoundToAnotherClass(fresh)) {
        error = "another class wore the same body in this level";
        return false;
    }
    if (g_SaberColorSet && HasSaberColor(fresh))
        *reinterpret_cast<const float**>(fresh + kCharacterSaberColor) = g_SaberColor;
    if (!PlaceCharacter(fresh, m, name, error))
        return false;
    RecordBody(fresh);
    *reinterpret_cast<int*>(fresh + kCharacterControl) = 2;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(uintptr_t(kBindController))(fresh, nullptr, 0);
    *reinterpret_cast<const float**>(uintptr_t(kPlayerTransformRef)) = reinterpret_cast<const float*>(fresh + kCharacterTransform);
    if (auto* record = *reinterpret_cast<uint8_t**>(uintptr_t(kPrimaryCharacter))) {
        if (*reinterpret_cast<uint8_t**>(record + 0xC) == old)
            *reinterpret_cast<uint8_t**>(record + 0xC) = fresh;
    }
    const int moved = RepointPlayerReferences(old, fresh);
    // The game manager knows the players by instance id (+4), per slot (+0x2A4[slot], +0x1E4 slots),
    // and finds them with ObjectById (0x27AB30: the HUD's health and Force bars, among others).
    if (auto* manager = *reinterpret_cast<uint8_t**>(uintptr_t(kGameManager))) {
        const int32_t slots = *reinterpret_cast<int32_t*>(manager + kManagerPlayerSlots);
        const uint32_t oldId = *reinterpret_cast<uint32_t*>(old + 4), freshId = *reinterpret_cast<uint32_t*>(fresh + 4);
        for (int32_t i = 0; i < slots && i < 8; ++i) {
            auto& id = *reinterpret_cast<uint32_t*>(manager + kManagerPlayerIds + i * 4);
            if (id == oldId)
                id = freshId;
        }
    }
    // Its teams carry over (the port's own bit, which the player's allies share: see SpawnCharacter),
    // and its size.
    uint32_t teams = 0;
    if (CharacterTeams(old, teams) && teams)
        SetCharacterTeams(fresh, teams);
    float scale = 1.0f;
    if (CharacterScale(old, scale) && scale != 1.0f)
        SetCharacterScale(fresh, scale);
    // Health carries over as a share of the maximum (the new class's, or the one chosen on the Game tab):
    // a change is not a heal.
    const float oldHealth = *reinterpret_cast<const float*>(old + kCharacterHealth);
    const float oldMax = *reinterpret_cast<const float*>(old + kCharacterMaxHealth);
    float& freshMax = *reinterpret_cast<float*>(fresh + kCharacterMaxHealth);
    if (g_LevelMaxHealth <= 0 && old == g_LevelPlayerObject && g_LevelPlayerIsOwn)
        g_LevelMaxHealth = oldMax; // the level's own player's, not a chosen class's
    if (g_ChosenMaxHealth > 0)
        freshMax = g_ChosenMaxHealth;
    else if (!g_PlayerClass && g_LevelMaxHealth > 0)
        freshMax = g_LevelMaxHealth;
    if (oldMax > 0 && freshMax > 0)
        *reinterpret_cast<float*>(fresh + kCharacterHealth) = std::max(1.0f, freshMax * std::clamp(oldHealth / oldMax, 0.0f, 1.0f));
    // The old player goes, as the game removes its own objects, with the projectiles in flight. Its
    // controller slot (+0x43C) is let go first: removing a character unbinds its slot (0x8ADD0 with -1), which would clear the controller
    // entry's character (input manager [0x68D4F4] +0x1D8[slot] +0x7C), now the new player's; moves
    // that read the stick's direction then crashed on it (0x89329).
    *reinterpret_cast<int*>(old + kCharacterControl) = 0;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(uintptr_t(kBindController))(old, nullptr, -1);
    RemoveProjectiles();
    DestroyObject(old);
    if (!ControllerBoundTo(fresh)) {
        LOG_WARN("Characters: controller 0 lost the new player; bound again");
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(uintptr_t(kBindController))(fresh, nullptr, 0);
    }
    g_ReplacedPlayer = old;
    g_Player = fresh;
    g_PlayerVtable = *reinterpret_cast<uint32_t*>(fresh);
    LOG_INFO("Characters: the player changed live to %s (%d references moved)", name, moved);
    return true;
}

bool SpawnCharacter(const char* className, const std::string& costume, const std::string& skin,
    const std::string& mesh, SpawnSide side, std::string& error)
{
    if (!PlayerAlive()) {
        error = g_PlayerSpawned ? "only in story levels, not in Versus" : "no mission is running";
        return false;
    }
    const char* name = RegisteredClassName(className);
    if (!name) {
        error = std::string(className) + ": not a class the game knows";
        return false;
    }
    if (!ClassHasBody(name)) {
        error = std::string(name) + " was cut from the game (none of its costumes is on the disc)";
        return false;
    }
    char who[96];
    sprintf_s(who, "spawned %s #%d", name, ++g_Spawned);
    uint8_t* object = CreateCharacter(name, costume, skin, mesh, who, error);
    if (!object)
        return false;
    RecordBody(g_Player);
    if (BodyBoundToAnotherClass(object)) {
        error = std::string(name) + " wears " + CharacterBody(object) + ", a body another class wore in this level "
            "(pick another costume, or restart the mission)";
        return false;
    }
    // In front of the player, facing it: right and forward turned round.
    float m[16];
    std::memcpy(m, g_Player + kCharacterTransform, sizeof(m));
    for (int i = 0; i < 3; ++i)
        m[12 + i] += m[8 + i] * kSpawnDistance;
    for (int i = 0; i < 3; ++i) {
        m[i] = -m[i];
        m[8 + i] = -m[8 + i];
    }
    if (!PlaceCharacter(object, m, name, error))
        return false;
    RecordBody(object);
    LOG_INFO("Characters: %s at %.0f %.0f %.0f", who, m[12], m[13], m[14]);
    if (side != SpawnSide::Default) {
        // A character's side is its AI data's "Target Player" (+0x50, set with "+0x1C"): the level's
        // enemies have it, the player and its allies not, and two characters whose differ are enemies
        // (0x18F3F0, either way round). A spawn has its class's default (a hero's: the player's enemy),
        // so an ally gets it cleared. Teams decide too when both have some (a shared bit is a friend;
        // with the player, 0x18F300): the player and its allies get the port's own bit, outside the
        // level designers' teams A-H, and the port's enemies another.
        uint32_t playerTeams = 0;
        CharacterTeams(g_Player, playerTeams);
        SetCharacterTeams(g_Player, playerTeams | kPortPlayerTeam);
        static const struct { uint32_t team; uint8_t targetsPlayer; uint8_t ignored; const char* name; } kSides[] = {
            {}, { kPortPlayerTeam, 0, 0, "an ally" }, { kPortEnemyTeam, 1, 0, "an enemy" },
            { kPortNeutralTeam, 0, 1, "neutral" }, { kPortRiotTeam, 1, 0, "rioting" } };
        const auto& sideData = kSides[int(side)];
        SetCharacterTeams(object, sideData.team);
        if (uint8_t* ai = *reinterpret_cast<uint8_t**>(object + kCharacterAIData)) {
            ai[kAIHasSide] = 1;
            ai[kAITargetPlayer] = sideData.targetsPlayer;
            ai[kAIIgnoredByAI] = sideData.ignored; // "Ignored By AI": no one sets out for it
        }
        if (side == SpawnSide::Neutral)
            g_Neutrals.push_back({ object, *reinterpret_cast<uint32_t*>(object + 4),
                *reinterpret_cast<const float*>(object + kCharacterHealth) });
        LOG_INFO("Characters: %s is %s", who, sideData.name);
    }
    ++g_SpawnedInLevel;
    g_LastSpawned = object;
    g_SpawnedCharacters.push_back({ object, *reinterpret_cast<uint32_t*>(object + 4) });
    return true;
}

constexpr uint32_t kCharacterPower = 0xA40;    // Force power, as `power` sets it (0x150480)
constexpr uint32_t kCharacterMaxPower = 0xA44; // its maximum (1000 for Anakin)
bool g_InfiniteForce = false;

void SetInfiniteForce(bool on)
{
    g_InfiniteForce = on;
}

bool InfiniteForce()
{
    return g_InfiniteForce;
}

void PlayerFrame()
{
    // A neutral spawn that lost health was attacked: it riots.
    for (auto it = g_Neutrals.begin(); it != g_Neutrals.end();) {
        if (reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(it->id) != it->object) {
            it = g_Neutrals.erase(it);
            continue;
        }
        const float health = *reinterpret_cast<const float*>(it->object + kCharacterHealth);
        if (health < it->health) {
            SetCharacterTeams(it->object, kPortRiotTeam);
            if (uint8_t* ai = *reinterpret_cast<uint8_t**>(it->object + kCharacterAIData)) {
                ai[kAITargetPlayer] = 1;
                ai[kAIIgnoredByAI] = 0;
            }
            LOG_INFO("Characters: a neutral character was attacked and riots");
            it = g_Neutrals.erase(it);
            continue;
        }
        ++it;
    }

    if (g_SaberColorSet)
        ApplySaberColor(true);

    // Infinite Force: the player's Force kept at its maximum (Jedi-like characters have it).
    if (g_InfiniteForce && PlayerAlive() && HasSaberColor(g_Player))
        *reinterpret_cast<float*>(g_Player + kCharacterPower) = *reinterpret_cast<const float*>(g_Player + kCharacterMaxPower);
}

uint8_t* PlayerObject()
{
    return PlayerAlive() ? g_Player : nullptr;
}

uint8_t* LastSpawnedObject()
{
    // Still in the level (the game removes dead characters' bodies in time).
    if (!g_LastSpawned || g_SpawnedCharacters.empty() || g_SpawnedCharacters.back().object != g_LastSpawned)
        return nullptr;
    return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(g_SpawnedCharacters.back().id) == g_LastSpawned
        ? g_LastSpawned : nullptr;
}

uint8_t* ReplacedPlayerObject()
{
    return g_ReplacedPlayer;
}

// A character's teams: its AI data (character +0xA00) holds "Team Setting" (TAIData +0x214, a bit per
// team A-H), which the AI reads to tell friend from foe (e.g. 0x18FA8C, which makes the player and
// another character enemies by giving the player every team the other is not in).
constexpr uint32_t kAITeams = 0x214;

bool CharacterTeams(const uint8_t* character, uint32_t& teams)
{
    if (!character)
        return false;
    __try {
        const uint8_t* ai = *reinterpret_cast<uint8_t* const*>(character + kCharacterAIData);
        if (!ai)
            return false;
        teams = *reinterpret_cast<const uint32_t*>(ai + kAITeams);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetCharacterTeams(uint8_t* character, uint32_t teams)
{
    if (!character)
        return false;
    __try {
        uint8_t* ai = *reinterpret_cast<uint8_t**>(character + kCharacterAIData);
        if (!ai)
            return false;
        *reinterpret_cast<uint32_t*>(ai + kAITeams) = teams;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetPlayerMaxHealth(float health)
{
    if (!PlayerAlive() || health <= 0)
        return;
    g_ChosenMaxHealth = health;
    *reinterpret_cast<float*>(g_Player + kCharacterMaxHealth) = health;
    *reinterpret_cast<float*>(g_Player + kCharacterHealth) = health;
}

void RefillPlayerHealth()
{
    if (PlayerAlive())
        *reinterpret_cast<float*>(g_Player + kCharacterHealth) = *reinterpret_cast<const float*>(g_Player + kCharacterMaxHealth);
}

PlayerInfo CurrentPlayer()
{
    PlayerInfo info;
    if (!PlayerAlive())
        return info;
    info.valid = true;
    const auto typeName = reinterpret_cast<const char*(__fastcall*)(uint8_t*, void*)>(
        (*reinterpret_cast<void* const* const*>(g_Player))[3]);
    const char* name = typeName(g_Player, nullptr);
    info.className = name ? name : "?";
    const uint8_t* list = *reinterpret_cast<uint8_t**>(g_Player + kCharacterVariants);
    const int costume = *reinterpret_cast<int*>(g_Player + kCharacterCostume);
    if (list && costume >= 0 && costume < VariantCount(list))
        info.costume = VariantName(list, costume);
    const float* m = reinterpret_cast<const float*>(g_Player + kCharacterTransform);
    std::copy(m + 12, m + 15, info.position);
    info.facing = std::atan2(m[8], m[10]) * 57.29578f; // the forward row's heading, degrees
    // Health and its maximum (floats), as the `health` / `maxhealth` variables set them (0x150420,
    // 0x150450); Force power as `power` does (0x150480), on the Jedi-like characters that have it.
    info.health = *reinterpret_cast<const float*>(g_Player + kCharacterHealth);
    info.maxHealth = *reinterpret_cast<const float*>(g_Player + kCharacterMaxHealth);
    info.hasPower = HasSaberColor(g_Player);
    if (info.hasPower) {
        info.power = *reinterpret_cast<const float*>(g_Player + kCharacterPower);
        info.maxPower = *reinterpret_cast<const float*>(g_Player + kCharacterMaxPower);
    }
    return info;
}

int SpawnedCount()
{
    return g_SpawnedInLevel;
}

// Removes spawned characters (those of `which` still there; all with null), as the game removes its own.
int RemoveSpawnedCharacters(const uint8_t* which)
{
    std::vector<uint8_t*> live;
    for (const SpawnedCharacter& spawned : g_SpawnedCharacters) {
        // Still there: the object manager finds it by its id (ObjectById).
        if ((!which || spawned.object == which) &&
            reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(spawned.id) == spawned.object)
            live.push_back(spawned.object);
    }
    for (uint8_t* object : live)
        CoopCharacterRemoving(object); // player 2 lets go of theirs first
    if (!live.empty()) {
        // Nothing keeps aiming at them, and no bolt in the air involves them.
        std::vector<uint8_t*> others;
        for (uint8_t* c : LevelCharacters())
            if (std::find(live.begin(), live.end(), c) == live.end())
                others.push_back(c);
        const int cleared = ClearTargetsOn(live, others);
        const int references = ClearReferencesTo(live);
        if (cleared || references)
            LOG_INFO("Characters: %d character(s) stopped targeting the spawned ones, %d other reference(s) cleared",
                cleared, references);
        RemoveProjectiles();
    }
    int removed = 0;
    for (uint8_t* object : live) {
        DestroyObject(object);
        ++removed;
    }
    if (which) {
        g_SpawnedCharacters.erase(std::remove_if(g_SpawnedCharacters.begin(), g_SpawnedCharacters.end(),
            [&](const SpawnedCharacter& c) { return c.object == which; }), g_SpawnedCharacters.end());
        g_SpawnedInLevel = std::max(0, g_SpawnedInLevel - removed);
        if (g_LastSpawned == which)
            g_LastSpawned = nullptr;
    } else {
        g_SpawnedCharacters.clear();
        g_SpawnedInLevel = 0;
        g_LastSpawned = nullptr;
    }
    LOG_INFO("Characters: %d spawned character(s) removed", removed);
    return removed;
}

int RemoveSpawned()
{
    return RemoveSpawnedCharacters(nullptr);
}

bool RemoveSpawnedCharacter(uint8_t* character)
{
    return character && RemoveSpawnedCharacters(character) > 0;
}

void SetPlayerSaberColor(const float* rgb)
{
    g_SaberColorSet = rgb != nullptr;
    if (rgb)
        std::copy(rgb, rgb + 3, g_SaberColor);
    if (rgb)
        ApplySaberColor(); // live: the player's sabers change now
    else if (PlayerAlive() && HasSaberColor(g_Player))
        *reinterpret_cast<const float**>(g_Player + kCharacterSaberColor) = nullptr;
}

bool ParseSaberColor(const std::string& spec, float* rgb)
{
    static const struct { const char* name; float rgb[3]; } kNamed[] = {
        { "red", { 1, 0, 0 } }, { "green", { 0, 1, 0 } }, { "blue", { 0, 0, 1 } }, { "purple", { 1, 0, 1 } } };
    for (const auto& named : kNamed) {
        if (_stricmp(spec.c_str(), named.name) == 0) {
            std::copy(named.rgb, named.rgb + 3, rgb);
            return true;
        }
    }
    int r, g, b;
    char end;
    if (sscanf_s(spec.c_str(), "%d %d %d %c", &r, &g, &b, &end, 1) != 3 || r < 0 || g < 0 || b < 0 || r > 255 ||
        g > 255 || b > 255)
        return false;
    rgb[0] = r / 255.0f;
    rgb[1] = g / 255.0f;
    rgb[2] = b / 255.0f;
    return true;
}

bool PlayerSaberColor(float* rgb)
{
    if (g_SaberColorSet && rgb)
        std::copy(g_SaberColor, g_SaberColor + 3, rgb);
    return g_SaberColorSet;
}

bool SetPlayerClass(const char* className)
{
    if (!className) {
        g_PlayerClass = nullptr;
        g_PlayerVariant.clear();
        g_PlayerMesh.clear();
        g_PlayerSkin.clear();
        return true;
    }
    const char* name = RegisteredClassName(className);
    if (!name)
        return false;
    g_PlayerClassName = name;
    g_PlayerClass = g_PlayerClassName.c_str();
    return true;
}

const char* PlayerClass()
{
    return g_PlayerClass;
}

} // namespace swrots::game
