// Versus mode: which characters the select screen offers, and what a character that was never made
// for versus needs to take part.

#include "game/versus.h"

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "game/characters.h"
#include "game/customchars.h"
#include "game/game.h"
#include "game/resources.h"
#include "game/roster.h"

namespace swrots::game {

namespace {

// Classes put in the slots: the game's own name strings, which stay at the same address across
// reboots (the registry is rebuilt identically). The duel reads the table again after the reboot
// that starts it, so changes are re-applied at every boot.
const char* g_Duelists[kDuelistCount] = {};
// The slots as the game has them, read at every boot before changes.
const char* g_OriginalDuelists[kDuelistCount] = {};

bool Replaced(int slot)
{
    return slot >= 0 && slot < kDuelistCount && g_Duelists[slot] && g_OriginalDuelists[slot] &&
        _stricmp(g_Duelists[slot], g_OriginalDuelists[slot]) != 0;
}

void ApplyDuelists()
{
    // Development aid: SWROTS_DUELISTS="<slot>=<class>[,...]" applies changes from the start, for
    // unattended tests.
    static std::string fromEnvironment[kDuelistCount];
    char spec[256] = {};
    if (GetEnvironmentVariableA("SWROTS_DUELISTS", spec, sizeof(spec))) {
        // Kept for this process (fromEnvironment), not passed on to a relaunched one, as SWROTS_COMMANDS.
        SetEnvironmentVariableA("SWROTS_DUELISTS", nullptr);
        char* context = nullptr;
        for (char* item = strtok_s(spec, ",", &context); item; item = strtok_s(nullptr, ",", &context)) {
            char* eq = strchr(item, '=');
            int slot = eq ? atoi(item) : -1;
            if (slot >= 0 && slot < kDuelistCount) {
                fromEnvironment[slot] = eq + 1;
                g_Duelists[slot] = fromEnvironment[slot].c_str();
            }
        }
    }
    for (int i = 0; i < kDuelistCount; ++i) {
        if (g_Duelists[i])
            PatchBytes(kDuelistClasses + i * 4, &g_Duelists[i], 4);
    }
    SyncRosterClasses();
}

// Fighter variants. The duel gives each fighter a variant of its class from a table per slot and
// player (kDuelistVariants: Anakin's and Obi-Wan's duel variants, 0 for the rest). A class in another
// class's slot gets the first of its own variants whose model is on the disc instead: the first is
// not always shipped (the battle droid's plain "BattleDroid" model is not; its "hordeBattleDroid" is).
// The fighter's variant list is at +0x1E0: records of 20 bytes from +0x2C, {name, mesh path, 0,
// code, 0}. At the site
//   mov edx, [ecx * 4 + kDuelistVariants] / mov [edi + 0x1E8], edx   (13 bytes, 0x27B87E)
// ecx is slot * 2 + player, ebx the slot and edi the fighter; the flags are the caller's (a jne
// follows).
constexpr uint32_t kVariantSite = 0x0027B87E;
constexpr uint32_t kVariantContinue = 0x0027B88B;
constexpr uint8_t kVariantBytes[] = { 0x8B, 0x14, 0x8D, 0xC0, 0xC1, 0x5C, 0x00, 0x89, 0x97, 0xE8, 0x01, 0x00, 0x00 };

int32_t ChosenVariant(int tableIndex, int slot, const uint8_t* fighter)
{
    if (slot >= kDuelistCount) // an extra fighter (roster.cpp); the table has the nine
        return VariantOnDisc(fighter, 0);
    int32_t variant = reinterpret_cast<const int32_t*>(uintptr_t(kDuelistVariants))[tableIndex];
    if (!Replaced(slot) || !fighter)
        return variant;
    return VariantOnDisc(fighter, 0);
}

int32_t __stdcall FighterVariant(int tableIndex, int slot, const uint8_t* fighter)
{
    int32_t variant = ChosenVariant(tableIndex, slot, fighter);
    // player 2 (tableIndex slot * 2 + player) of a mirror duel: a custom character's body for player 2 (Mesh2=)
    const CustomCharacter* custom = fighter ? CustomCharacterOf(fighter) : nullptr;
    if (custom && !custom->mesh2.empty()) {
        const bool mirror = BothPlayersChose(custom->className.c_str());
        LOG_INFO("Versus: %s is player %d%s", custom->className.c_str(), (tableIndex & 1) + 1, mirror ? " of a mirror" : "");
        if ((tableIndex & 1) && mirror)
            variant = DressMirrorPlayer2(const_cast<uint8_t*>(fighter), variant, custom->mesh2);
    }
    return variant;
}

__declspec(naked) void FighterVariantStub()
{
    __asm {
        pushfd
        pushad
        push edi
        push ebx
        push ecx
        call FighterVariant
        mov [esp + 20], eax // becomes edx
        popad
        popfd
        mov dword ptr [edi + 0x1E8], edx
        push kVariantContinue
        ret
    }
}

// Duel cameras. For every fighter the duel loads cinematics\introcamera\<name>_Intro_Cam.cin and
// cinematics\outrocamera\<name>_Outro_Cam.cin, <name> being the character's name (OldObiwan as
// "obiold"); without them there is no intro, and a win freezes on the missing outro. The disc has
// cameras only for the nine duelists, so a character without its own uses the cameras of the
// duelist whose slot it took. At the site
//   push ecx / mov ecx, esp / push 0x1D12E0   (8 bytes, 0x27BAAD)
// ebp holds the name and ebx the slot.
constexpr uint32_t kCameraNameSite = 0x0027BAAD;
constexpr uint32_t kCameraNameContinue = 0x0027BAB5;
constexpr uint8_t kCameraNameBytes[] = { 0x51, 0x8B, 0xCC, 0x68, 0xE0, 0x12, 0x1D, 0x00 };

bool HasCamera(const char* name, const char* kind)
{
    std::string path = std::string("cinematics\\") + kind + "camera\\" + name + "_" + kind + "_cam.cin";
    for (char& c : path)
        c = char(tolower(static_cast<unsigned char>(c)));
    return DiscHasResource(path);
}

const char* __stdcall CameraName(const char* name, int slot)
{
    if (!name || slot < 0 || (HasCamera(name, "intro") && HasCamera(name, "outro")))
        return name;
    if (slot >= kDuelistCount) {
        const char* standIn = ExtraFighterCameras(slot);
        // a base that was never a duelist (a custom character's, e.g. the Jedi Brute) has none either: Vader's (a
        // missing outro camera crashes the round's end, 0x27D64F, and a missing intro one stops the duel's start)
        if (standIn && !(HasCamera(standIn, "intro") && HasCamera(standIn, "outro")))
            standIn = "Vader";
        if (standIn)
            LOG_INFO("Versus: %s has no duel cameras; using %s's", name, standIn);
        return standIn ? standIn : name;
    }
    const char* original = g_OriginalDuelists[slot];
    if (!original || _stricmp(original, "IOldObiwan") == 0)
        return "obiold";
    static std::string names[kDuelistCount];
    names[slot] = original + 1; // IAnakin -> Anakin
    LOG_INFO("Versus: %s has no duel cameras; using %s's", name, names[slot].c_str());
    return names[slot].c_str();
}

__declspec(naked) void CameraNameStub()
{
    __asm {
        pushad
        push ebx
        push ebp
        call CameraName
        mov [esp + 8], eax // becomes ebp
        popad
        push ecx
        mov ecx, esp
        push 0x001D12E0
        push kCameraNameContinue
        ret
    }
}

} // namespace

void InstallVersus()
{
    std::memcpy(g_OriginalDuelists, reinterpret_cast<const void*>(uintptr_t(kDuelistClasses)), sizeof(g_OriginalDuelists));
    ApplyDuelists();
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kVariantSite)), kVariantBytes, sizeof(kVariantBytes)) == 0) {
        PatchJump(kVariantSite, reinterpret_cast<const void*>(&FighterVariantStub));
        PatchNop(kVariantSite + 5, sizeof(kVariantBytes) - 5);
    } else {
        LOG_WARN("Versus: fighter variant site does not match; not patched");
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kCameraNameSite)), kCameraNameBytes,
            sizeof(kCameraNameBytes)) == 0) {
        PatchJump(kCameraNameSite, reinterpret_cast<const void*>(&CameraNameStub));
        PatchNop(kCameraNameSite + 5, sizeof(kCameraNameBytes) - 5);
    } else {
        LOG_WARN("Versus: duel camera site does not match; not patched");
    }
}

bool SetDuelist(int slot, const char* className)
{
    if (slot < 0 || slot >= kDuelistCount)
        return false;
    const char* name = RegisteredClassName(className);
    if (!name)
        return false;
    g_Duelists[slot] = name;
    ApplyDuelists();
    return true;
}

const char* Duelist(int slot)
{
    if (slot < 0 || slot >= kDuelistCount)
        return nullptr;
    return reinterpret_cast<const char* const*>(uintptr_t(kDuelistClasses))[slot];
}

} // namespace swrots::game
