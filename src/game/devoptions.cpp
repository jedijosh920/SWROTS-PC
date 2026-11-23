#include "game/devoptions.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "game/game.h"
#include "game/release.h"

namespace swrots::game {

namespace {

// vars_xbox.cfg reader (0x11510): for each "name=value" pair it calls
// console->Set(name, value, 1) at 0x115B7 (push 1 / push ebp / push ebx / mov ecx, eax /
// call [edx + 0x1C], 9 bytes). Values set before a variable is registered wait in the
// console's pending list and apply when it registers (0x12740).
static GAME_VA(kVarsSetSite, 0x000115B7);

// kOptionsHideDebugDisplays (+0xD7) suppresses the engine's debug displays (fps counter, profiler,
// memory display): the options constructor (0x157C0) sets it to 1 and no variable or launch argument
// changes it; the debug display code (e.g. 0xA0D80) draws only while it is 0.

// When a level starts, 0x2AAB00 copies the profile's settings into the options object, including
// god (+0xC9) from the profile's byte +0x2D (mov dl, [edi + 0x2D] / mov [ecx + 0xC9], dl at
// 0x2AABD2, 9 bytes), which would undo god=true from vars_xbox.cfg. The patch keeps either.
static GAME_VA(kProfileGodCopySite, 0x002AABD2);

bool g_DebugDisplays = false;
uint8_t* g_AppliedTo = nullptr;
uint8_t g_ConfigGod = 0;

// The engine's on/off parsing (0x11860): "true", "false", or a number > 0.
bool ParseOnOff(const char* value)
{
    if (_stricmp(value, "true") == 0)
        return true;
    if (_stricmp(value, "false") == 0)
        return false;
    return atoi(value) > 0;
}

void __stdcall LogVarsPair(const char* name, const char* value)
{
    LOG_INFO("vars_xbox.cfg: %s = %s", name ? name : "(null)", value ? value : "(null)");
    if (name && value && _stricmp(name, "god") == 0)
        g_ConfigGod = ParseOnOff(value) ? 1 : 0;
}

__declspec(naked) void ProfileGodStub()
{
    __asm {
        mov dl, byte ptr [edi + 0x2D]
        or dl, g_ConfigGod
        mov byte ptr [ecx + 0xC9], dl
        ret
    }
}

__declspec(naked) void VarsSetStub()
{
    __asm {
        push eax
        push edx
        push ebp
        push ebx
        call LogVarsPair
        pop edx
        pop eax
        push 1
        push ebp
        push ebx
        mov ecx, eax
        call dword ptr [edx + 0x1C]
        ret
    }
}

// The engine's disc error callback (0x6D4F0, xinstall/file layer) shows the "dirty or
// damaged" screen. Logs its arguments, the XAPI last error and the callers, then runs it.
// Entry: push ecx / mov eax, [0x645F7C] (6 bytes), continued at 0x6D4F6.
static GAME_VA(kDiscErrorHandler, 0x0006D4F0);
static GAME_VA(kDiscErrorContinue, 0x0006D4F6);
static GAME_VA(kXapiGetLastError, 0x004A054D);

void __stdcall LogDiscError(const uint32_t* stack)
{
    auto getLastError = reinterpret_cast<uint32_t(__cdecl*)()>(uintptr_t(kXapiGetLastError));
    LOG_WARN("Disc error handler: return %08X, args %08X %08X, last error %u", stack[0], stack[1], stack[2],
        getLastError());
    game::LogGameCallers("disc error", stack);
}

__declspec(naked) void DiscErrorStub()
{
    __asm {
        pushad
        lea eax, [esp + 32]
        push eax
        call LogDiscError
        popad
        push ecx
        mov eax, kEngineServices
        mov eax, [eax]
        push kDiscErrorContinue
        ret
    }
}

// The "You are missing required animations for character %s" report: je over the engine warning.
static GAME_VA(kMissingAnimationsWarnJump, 0x001F8BF0);

} // namespace

void InstallDevOptions(bool debugDisplays)
{
    g_DebugDisplays = debugDisplays;
    g_AppliedTo = nullptr; // a rebooted game's options object may sit at the same address
    g_ConfigGod = 0;
    PatchCall(kVarsSetSite, reinterpret_cast<const void*>(&VarsSetStub), 9);
    PatchCall(kProfileGodCopySite, reinterpret_cast<const void*>(&ProfileGodStub), 9);
    PatchJump(kDiscErrorHandler, reinterpret_cast<const void*>(&DiscErrorStub));
    // Character scripts that lack animations (0x1F8AB0) report them only to a game callback unless
    // asked to warn; make them always warn, so the log and console name the missing animations.
    PatchNop(kMissingAnimationsWarnJump, 2);
}

void ApplyDevOptions(uint8_t* options)
{
    if (!options || options == g_AppliedTo)
        return;
    g_AppliedTo = options;
    if (g_DebugDisplays) {
        options[kOptionsHideDebugDisplays] = 0;
        LOG_INFO("Debug displays enabled");
    }
}

} // namespace swrots::game
