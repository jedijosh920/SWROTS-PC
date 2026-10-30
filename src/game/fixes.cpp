// Guards against bugs in the game's own code, found through the port's developer tools.

#include "game/fixes.h"

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"

namespace swrots::game {

namespace {

// The time manager (TManager_Time, 0xB7490) keeps a stack of time-scale states for slow-motion
// effects. When an effect's timer runs out it pops the stack (0xA64E0) without checking that
// the stack has an entry. The timeScale variable's change callback (0x98940, run when it is set
// through the console) sets the manager's "effect active" flag (+0x14) without pushing one, so
// the pop read before the stack's storage and crashed (writing the float in memory skips the
// callback, which is why that never crashed). Guarded: an empty stack counts as no
// effect active (the function then restores the normal scale). The site
//   mov al, [esi + 0x14] / xor ebx, ebx / cmp al, bl   (7 bytes; je 0xA6555 follows)
// becomes a call that sets ZF for "inactive or empty" and ebx = 0.
constexpr uint32_t kTimeStackPopSite = 0x000A64E5;
constexpr uint8_t kTimeStackPopBytes[] = { 0x8A, 0x46, 0x14, 0x33, 0xDB, 0x3A, 0xC3, 0x74 };

__declspec(naked) void TimeStackPopGuard()
{
    __asm {
        xor ebx, ebx
        mov al, byte ptr [esi + 0x14]
        test al, al
        jz done
        cmp dword ptr [esi + 0x38], 0
    done:
        ret
    }
}

// A character mesh's animation binding (.ban) is rebuilt when the mesh is used with animations its
// shipped binding lacks (a mesh on another character class). The rebuild (0x6890D) reports the
// animations it cannot find and then throws the whole binding away (from 0x689F8: frees it and
// returns 0), so the character has none and the game crashes using it (0x68F37). Some animations the
// classes ask for exist nowhere in the game (Anakin's Anakin_Frc_Jump_C1/C2, cut during development),
// so a borrowed mesh could never get a binding. Kept instead: after the report, the success path
// (0x68A33, the same stack) takes the binding, with a neighbouring animation in each missing one's
// place (resources.cpp lays the found list out for it). The game's own bindings never fail this way.
constexpr uint32_t kBindingFailSite = 0x000689F8;
constexpr uint32_t kBindingKeep = 0x00068A33;
constexpr uint8_t kBindingFailBytes[] = { 0x8B, 0x44, 0x24, 0x3C, 0x8B, 0x0D, 0x7C, 0x5F, 0x64, 0x00 };

// A shipped binding is checked against the animations asked for (0x686F4): a different count means
// a rebuild -- unless the packer says so otherwise (its vfunc +0xF4, kept at [esp+0x13]), when the
// names are compared one by one, as far as the request goes, past the end of a shorter binding, which
// can then pass (a mesh on another class: Obi-Wan's 787 animations against Anakin's binding) and be
// filled past its end (0x68F71). A count that differs now always rebuilds:
//   mov cl, [esp+0x13] / test cl, cl / je 0x68804   (12 bytes) -> jmp 0x68804
constexpr uint32_t kBindingCountSite = 0x00068703;
constexpr uint32_t kBindingRebuild = 0x00068804;
constexpr uint8_t kBindingCountBytes[] = { 0x8A, 0x4C, 0x24, 0x13, 0x84, 0xC9, 0x0F, 0x84, 0xF5, 0x00, 0x00, 0x00 };

// StdHashString (StdHashString.h) tables with a node pool take new entries from the pool's free list
// (0x62640) without checking that it has one; the pools are sized for exactly what a level's PAK
// holds, so a level that loads more than that (a character added by the port's resource loading)
// ran out and dereferenced null. Tables without a pool allocate a 20-byte node instead (0x6267A),
// which is what an empty pool now does. The site
//   mov ecx, [eax + 0xC] / mov edi, [eax + 4] / inc ecx / mov [eax + 0xC], ecx   (10 bytes)
// becomes a jump to the guard, which continues at 0x62668 or takes the allocating path.
constexpr uint32_t kHashPoolTakeSite = 0x0006265E;
constexpr uint32_t kHashPoolTakeContinue = 0x00062668;
constexpr uint32_t kHashPoolAllocate = 0x0006267A;
constexpr uint8_t kHashPoolTakeBytes[] = { 0x8B, 0x48, 0x0C, 0x8B, 0x78, 0x04, 0x41, 0x89, 0x48, 0x0C };

__declspec(naked) void HashPoolTakeGuard()
{
    __asm {
        mov edi, dword ptr [eax + 4]
        test edi, edi
        jz allocate
        mov ecx, dword ptr [eax + 0xC]
        inc ecx
        mov dword ptr [eax + 0xC], ecx
        push kHashPoolTakeContinue
        ret
    allocate:
        push kHashPoolAllocate
        ret
    }
}

// A character's behaviour (GBehavior CharState) branches to a sequence by name (0x16C940, vtable
// 0x58EFB8 slot 7). A level loads only the optional sequences its own characters use; the others are
// there with no steps and marked (+1). The game reports a branch to one ("... is marked as optional but
// not loaded for this level!") and branches anyway. Its step walk (0x49CAD0) then stops only on the
// last step ([+0x44] + ([+0x40] - 1) * 0x14), which for no steps lies before the first: it walks on
// through whatever memory follows until a step kind beyond its four handlers (0x49B7E0) crashes. Seen
// with a character in a level that does not have it (Obi-Wan in the Jedi Temple: GSO_Launcher_Light).
// The branch is refused instead, after the report: the character does not do that move. The site,
// where the report's paths meet,
//   lea ecx, [esp + 0xC] / call 0x225230   (9 bytes: the report's string goes; 0x16CA24 follows)
// becomes a jump to the guard, edi holding the sequence.
constexpr uint32_t kSequenceBranchSite = 0x0016CA1B;
constexpr uint32_t kSequenceBranchContinue = 0x0016CA24;
constexpr uint32_t kStringRelease = 0x00225230;

void __declspec(naked) EmptySequenceGuard()
{
    __asm {
        lea ecx, [esp + 0xC]
        mov eax, kStringRelease
        call eax
        cmp dword ptr [edi + 0x40], 0
        je refuse
        push kSequenceBranchContinue
        ret
    refuse:
        pop edi
        pop esi
        xor al, al
        pop ebx
        pop ecx
        ret 0x1C
    }
}

// A character answers whether it goes by a name (vfunc +0x460, 0x14FE90, shared by 42 classes) by comparing it with
// the two names of its class's script (`[[character+0x434]+4]` +0x5C and +0x60) through 0x2233F0, which does not
// take a null string. Palpatine's class has no second name, so asking a Palpatine (as the game's logic does about
// characters in a fight) crashed (0x4EBEB4); the game never has one fight, but a spawned one does. The function is
// replaced by the same comparison, a missing name matching nothing.
constexpr uint32_t kCharacterNamed = 0x0014FE90;
constexpr uint8_t kCharacterNamedBytes[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x34, 0x04, 0x00, 0x00 };
constexpr uint32_t kCompareNames = 0x002233F0; // cdecl (a, b) -> 0 when equal

bool __fastcall CharacterNamed(uint8_t* character, void*, const char* name)
{
    uint8_t* script = *reinterpret_cast<uint8_t**>(character + 0x434);
    uint8_t* info = script ? *reinterpret_cast<uint8_t**>(script + 4) : nullptr;
    if (!info || !name)
        return false;
    const auto compare = reinterpret_cast<int(__cdecl*)(const char*, const char*)>(uintptr_t(kCompareNames));
    static const uint32_t kNames[] = { 0x5C, 0x60 };
    for (uint32_t at : kNames) {
        const char* own = *reinterpret_cast<const char* const*>(info + at);
        if (own && compare(name, own) == 0)
            return true;
    }
    return false;
}

} // namespace

void InstallGameFixes()
{
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kCharacterNamed)), kCharacterNamedBytes,
            sizeof(kCharacterNamedBytes)) == 0)
        PatchHook(kCharacterNamed, reinterpret_cast<const void*>(&CharacterNamed));
    else
        LOG_WARN("Game fix: character name check does not match; not patched");
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kHashPoolTakeSite)), kHashPoolTakeBytes,
            sizeof(kHashPoolTakeBytes)) == 0) {
        PatchJump(kHashPoolTakeSite, reinterpret_cast<const void*>(&HashPoolTakeGuard));
        PatchNop(kHashPoolTakeSite + 5, sizeof(kHashPoolTakeBytes) - 5);
    } else {
        LOG_WARN("Game fix: hash string pool site does not match; not patched");
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kTimeStackPopSite)), kTimeStackPopBytes,
            sizeof(kTimeStackPopBytes)) == 0)
        PatchCall(kTimeStackPopSite, reinterpret_cast<const void*>(&TimeStackPopGuard), 7);
    else
        LOG_WARN("Game fix: time-scale stack site does not match; not patched");
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBindingFailSite)), kBindingFailBytes,
            sizeof(kBindingFailBytes)) == 0)
        PatchJump(kBindingFailSite, reinterpret_cast<const void*>(uintptr_t(kBindingKeep)));
    else
        LOG_WARN("Game fix: animation binding site does not match; not patched");
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBindingCountSite)), kBindingCountBytes,
            sizeof(kBindingCountBytes)) == 0)
        PatchJump(kBindingCountSite, reinterpret_cast<const void*>(uintptr_t(kBindingRebuild)));
    else
        LOG_WARN("Game fix: animation binding count site does not match; not patched");
    uint8_t branchBytes[9] = { 0x8D, 0x4C, 0x24, 0x0C, 0xE8 };
    const int32_t release = int32_t(kStringRelease) - int32_t(kSequenceBranchSite + 9);
    std::memcpy(branchBytes + 5, &release, 4);
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kSequenceBranchSite)), branchBytes, sizeof(branchBytes)) == 0) {
        PatchJump(kSequenceBranchSite, reinterpret_cast<const void*>(&EmptySequenceGuard));
        PatchNop(kSequenceBranchSite + 5, sizeof(branchBytes) - 5);
    } else {
        LOG_WARN("Game fix: sequence branch site does not match; not patched");
    }
}

} // namespace swrots::game
