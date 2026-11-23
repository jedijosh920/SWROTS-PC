// More sequence functions for a class's script (see classsetup.h).
//
// The class setup driver (0x1F66A0, on the setup object 0x6965D8) walks the class's script vtable from slot 7: it
// loads a function (0x1F67A0: mov eax, [edi]; mov ecx, [ebp-0x18]; mov edi, [ecx+eax]), calls it with ecx = the class
// object ([ebp+8]) in setup mode, and stops when the function set the end marker [setup+7] (0x1F685F; that function
// itself gets no record), else makes a record of what it declared and goes on. When the class's own end marker is
// reached, the port clears it and hands the driver its functions instead of the vtable's, then a last one that ends
// the list as the class's own did.

#include "game/classsetup.h"

#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "game/release.h"

namespace swrots::game {

namespace {

static GAME_VA(kSetup, 0x006965D8);
constexpr int kMaxAdded = 32;

std::vector<ClassSequenceProvider> g_Providers;
ClassSequenceFn g_Added[kMaxAdded + 1] = {};
int g_AddedCount = 0;
int g_AddedIndex = 0;
bool g_Adding = false;
uint8_t* g_SetupClass = nullptr; // the class object whose setup runs (each class's setup is asked once)
bool g_AskedThisSetup = false;

bool __fastcall EndOfList(uint8_t*, void*, uint32_t, uint32_t)
{
    reinterpret_cast<uint8_t*>(uintptr_t(kSetup))[7] = 1;
    return false;
}

template <typename T> T Field(const uint8_t* p, uint32_t offset)
{
    return *reinterpret_cast<const T*>(p + offset);
}

void __stdcall EndCheck(uint8_t* classObject, uint8_t* setup)
{
    if (classObject != g_SetupClass) {
        g_SetupClass = classObject;
        g_AskedThisSetup = false;
        g_Adding = false;
    }
    if (!setup[7])
        return;
    if (g_Adding) { // ours ended it
        g_Adding = false;
        return;
    }
    if (g_AskedThisSetup || g_Providers.empty())
        return;
    g_AskedThisSetup = true;
    const char* shortName = Field<const char*>(setup, 0xC);
    if (!shortName)
        return;
    int count = 0;
    for (ClassSequenceProvider provider : g_Providers)
        count += provider(shortName, g_Added + count, kMaxAdded - count);
    if (count <= 0)
        return;
    g_Added[count] = &EndOfList;
    g_AddedCount = count + 1;
    g_AddedIndex = 0;
    g_Adding = true;
    setup[7] = 0;
    LOG_INFO("Class setup: %s gets %d more sequences", shortName, count);
}

uint32_t __stdcall PickFunction(uint32_t original)
{
    if (g_Adding && g_AddedIndex < g_AddedCount)
        return uint32_t(uintptr_t(g_Added[g_AddedIndex++]));
    return original;
}

static GAME_VA(kSetupEndCheck, 0x001F685F);
constexpr uint8_t kSetupEndCheckBytes[] = { 0x38, 0x5E, 0x07, 0x0F, 0x85 }; // cmp [esi + 7], bl; jne
static GAME_VA(kSetupLoad, 0x001F67A0);
constexpr uint8_t kSetupLoadBytes[] = { 0x8B, 0x07, 0x8B, 0x4D, 0xE8, 0x8B, 0x3C, 0x01 };
static GAME_VA(g_SetupEndContinue, 0x001F6868);
static GAME_VA(g_SetupEndStop, 0x001F6A95);
static GAME_VA(g_SetupLoadContinue, 0x001F67A8);

__declspec(naked) void SetupEndEntry()
{
    __asm {
        pushad
        push esi
        push dword ptr [ebp + 8]
        call EndCheck
        popad
        cmp byte ptr [esi + 7], bl
        jne stop
        jmp [g_SetupEndContinue]
    stop:
        jmp [g_SetupEndStop]
    }
}

__declspec(naked) void SetupLoadEntry()
{
    __asm {
        mov eax, [edi]
        mov ecx, [ebp - 0x18]
        mov edi, [ecx + eax]
        push eax
        push ecx
        push edx
        push edi
        call PickFunction
        mov edi, eax
        pop edx
        pop ecx
        pop eax
        jmp [g_SetupLoadContinue]
    }
}

bool g_Patched = false;

} // namespace

void InstallClassSetup()
{
    g_Providers.clear();
    g_Adding = false;
    g_SetupClass = nullptr;
    g_AskedThisSetup = false;
    g_Patched = false;
    if (!MatchesCode(kSetupEndCheck, kSetupEndCheckBytes, sizeof kSetupEndCheckBytes) ||
        !MatchesCode(kSetupLoad, kSetupLoadBytes, sizeof kSetupLoadBytes)) {
        LOG_WARN("Class setup: the setup driver is not as expected; classes get no added sequences");
        return;
    }
    PatchJump(kSetupEndCheck, reinterpret_cast<const void*>(&SetupEndEntry));
    PatchJump(kSetupLoad, reinterpret_cast<const void*>(&SetupLoadEntry));
    PatchNop(kSetupLoad + 5, sizeof kSetupLoadBytes - 5);
    g_Patched = true;
}

void AddClassSequences(ClassSequenceProvider provider)
{
    if (!g_Patched || !provider)
        return;
    for (ClassSequenceProvider p : g_Providers)
        if (p == provider)
            return;
    g_Providers.push_back(provider);
}

} // namespace swrots::game
