// Animation aliases: which of its own animations a character plays for a sequence it inherits.

#include "game/aliases.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "game/release.h"

namespace swrots::game {

namespace {

// A character's script class (GScript) inherits the human base class's sequences, each of which plays
// a default animation: R2_Hit_Front plays Anakin_HitRe2_F_hi, Block1_React_High Anakin_BloRe1_Hi, and
// so on. A class renames them for its own animations through its alias list, vtable slot 6
// (thiscall, 2 arguments: the animation and sequence name lists). Each class's method fills a
// null-terminated array of "animation\0sequence" strings and hands it to kAddAliases, which appends
// the animation to the first list and the sequence name to the second.
//
// Yoda's class was made for fighting clones and droids: his list (41 entries) covers their reactions
// but none of a saber duelist's. Those sequences play the base class's Anakin animations, which the
// port loads for him from other levels. For hits, grapples, throws, force powers and falls these look
// right on him (better than any of his own few animations would); Anakin's blocks do not: he floats
// in them. The port maps the blocks to his own block reaction.
static GAME_VA(kAddAliases, 0x001F6460); // cdecl (animations, sequences, const char** pairs)
static GAME_VA(kYodaVtable, 0x00608C10);
static GAME_VA(kYodaAliases, 0x00494190);
constexpr int kAliasSlot = 6;

struct AliasGroup {
    const char* animation;
    std::vector<const char*> sequences;
};

// The block sequences Yoda's class inherits that a duelist's reaction tables (r_*.csv, b_*.csv) or
// the base class can play on him, and that his own list lacks.
const AliasGroup kYodaGroups[] = {
    { "Yoda_BlockRe1", {
        "Block1_High", "Block1_React_High", "Block1_React_High_NoShunt", "Block1_High_Alt",
        "Block1_React_High_Alt", "Block1_React_High_Alt_NoShunt", "Block1_Left", "Block1_React_Left",
        "Block1_React_Left_NoShunt", "Block1_Left_Alt", "Block1_React_Left_Alt", "Block1_React_Left_Alt_NoShunt",
        "Block1_LowLeft", "Block1_React_LowLeft", "Block1_LowLeft_Alt", "Block1_React_LowLeft_Alt", "Block1_Right",
        "Block1_React_Right", "Block1_React_Right_NoShunt", "Block1_Right_Alt", "Block1_React_Right_Alt",
        "Block1_React_Right_Alt_NoShunt", "Block1_LowRight", "Block1_React_LowRight", "Block1_LowRight_Alt",
        "Block1_React_LowRight_Alt", "Block1_React_Gen_Alt", "Block2_High", "Block2_React_High", "Block2_High_Alt",
        "Block2_React_High_Alt", "Block2_Left", "Block2_React_Left", "Block2_Left_Alt", "Block2_React_Left_Alt",
        "Block2_LowLeft", "Block2_React_LowLeft", "Block2_LowLeft_Alt", "Block2_React_LowLeft_Alt",
        "Block2_SpinLeft", "Block2_React_SpinLeft", "Block2_SpinLeft_Alt", "Block2_React_SpinLeft_Alt",
        "Block2_Right", "Block2_React_Right", "Block2_Right_Alt", "Block2_React_Right_Alt", "Block2_LowRight",
        "Block2_React_LowRight", "Block2_LowRight_Alt", "Block2_React_LowRight_Alt", "Block2_SpinRight",
        "Block2_React_SpinRight", "Block2_SpinRight_Alt", "Block2_React_SpinRight_Alt",
    } },
};

using AliasesFn = void(__fastcall*)(void* self, void* edx, void* animations, void* sequences);
AliasesFn g_OriginalYodaAliases = nullptr;
std::vector<std::string> g_YodaPairs;
std::vector<const char*> g_YodaPairList;

void __fastcall YodaAliasesHook(void* self, void* edx, void* animations, void* sequences)
{
    g_OriginalYodaAliases(self, edx, animations, sequences);
    auto add = reinterpret_cast<void(__cdecl*)(void*, void*, const char* const*)>(uintptr_t(kAddAliases));
    add(animations, sequences, g_YodaPairList.data());
}

} // namespace

void InstallAnimationAliases()
{
    auto slot = reinterpret_cast<uint32_t*>(uintptr_t(kYodaVtable + kAliasSlot * 4));
    if (*slot != kYodaAliases) {
        LOG_WARN("Aliases: Yoda's class is not as expected; his blocks are not added");
        return;
    }
    // Every reboot reinstalls the patches on a fresh image; the pairs are built once.
    if (g_YodaPairList.empty()) {
        for (const AliasGroup& group : kYodaGroups) {
            for (const char* sequence : group.sequences) {
                std::string pair = group.animation;
                pair += '\0';
                pair += sequence;
                g_YodaPairs.push_back(std::move(pair));
            }
        }
        for (const std::string& pair : g_YodaPairs)
            g_YodaPairList.push_back(pair.c_str());
        g_YodaPairList.push_back(nullptr);
    }

    g_OriginalYodaAliases = reinterpret_cast<AliasesFn>(uintptr_t(kYodaAliases));
    const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&YodaAliasesHook));
    PatchBytes(kYodaVtable + kAliasSlot * 4, &hook, 4);
    LOG_INFO("Aliases: %zu block reactions added to Yoda", g_YodaPairs.size());
}

} // namespace swrots::game
