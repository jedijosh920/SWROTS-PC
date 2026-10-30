#include "xapi/xapi.h"

#include <windows.h>

#include <cstring>
#include <intrin.h>
#include <map>
#include <string>

#include "core/crash.h"
#include "core/log.h"
#include "core/patch.h"
#include "game/game.h"
#include "core/gates.h"
#include "kernel/clock.h"
#include "kernel/kernel.h"
#include "kernel/reboot.h"
#include "kernel/sched.h"

namespace swrots::xapi {

static std::map<std::string, const void*>& Replacements()
{
    static std::map<std::string, const void*> map;
    return map;
}

void RegisterSdkReplacement(const char* symbolName, const void* implementation)
{
    Replacements()[symbolName] = implementation;
}

static std::map<std::string, bool>& Passthroughs()
{
    static std::map<std::string, bool> map;
    return map;
}

void RegisterSdkPassthrough(const char* symbolName)
{
    Passthroughs()[symbolName] = true;
}

// ---------------------------------------------------------------------------
// Clock. XAPI reads the TSC and assumes the Xbox's 733 MHz CPU; use the host's
// performance counter instead, always shown at 10 MHz: the game then sees the same
// frequency on every PC, and the fixed clock (kernel/clock.h, netplay) is in those units.
// ---------------------------------------------------------------------------
static LONGLONG HostQpcFrequency()
{
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

// The game's own rdtsc instructions (a profiler's counters, two timing helpers): while the fixed clock is on (netplay),
// the time-stamp counter is the fixed clock's at the Xbox's 733 MHz, not the PC's.
static uint64_t __cdecl GameTsc()
{
    if (kernel::FixedClockOn())
        return kernel::FixedClockNow() * 733 / 10;
    return __rdtsc();
}

// `rdtsc; ret` (0x5081B0).
static __declspec(naked) void TscReturn()
{
    __asm {
        push ecx
        call GameTsc
        pop ecx
        ret
    }
}

// A function returning the counter / 16 in eax and ecx both, keeping edx (0x227170).
static __declspec(naked) void TscBy16()
{
    __asm {
        push edx
        call GameTsc
        shrd eax, edx, 4
        pop edx
        mov ecx, eax
        ret
    }
}

// `rdtsc; mov [esp+8], eax` (0x2420FE) and `rdtsc; mov [esp+0xC], eax` (0x242148), called in their place.
static __declspec(naked) void TscStore8()
{
    __asm {
        push ecx
        call GameTsc
        pop ecx
        mov [esp + 0x0C], eax
        ret
    }
}

static __declspec(naked) void TscStoreC()
{
    __asm {
        push ecx
        call GameTsc
        pop ecx
        mov [esp + 0x10], eax
        ret
    }
}

static void PatchGameTsc()
{
    static const uint8_t kReturn[] = { 0x0F, 0x31, 0xC3, 0xCC, 0xCC };
    static const uint8_t kBy16[] = { 0x51, 0x50, 0x52, 0x0F, 0x31 };
    static const uint8_t kStore8[] = { 0x0F, 0x31, 0x89, 0x44, 0x24, 0x08 };
    static const uint8_t kStoreC[] = { 0x0F, 0x31, 0x89, 0x44, 0x24, 0x0C };
    auto at = [](uint32_t address, const uint8_t* bytes, size_t size) {
        return std::memcmp(reinterpret_cast<const void*>(uintptr_t(address)), bytes, size) == 0;
    };
    int patched = 0;
    if (at(0x005081B0, kReturn, sizeof kReturn)) {
        PatchJump(0x005081B0, reinterpret_cast<const void*>(&TscReturn));
        ++patched;
    }
    if (at(0x00227170, kBy16, sizeof kBy16)) {
        PatchJump(0x00227170, reinterpret_cast<const void*>(&TscBy16));
        ++patched;
    }
    if (at(0x002420FE, kStore8, sizeof kStore8)) {
        PatchCall(0x002420FE, reinterpret_cast<const void*>(&TscStore8), 6);
        ++patched;
    }
    if (at(0x00242148, kStoreC, sizeof kStoreC)) {
        PatchCall(0x00242148, reinterpret_cast<const void*>(&TscStoreC), 6);
        ++patched;
    }
    LOG_INFO("Netplay: %d of the game's 4 rdtsc reads on the fixed clock", patched);
}

static BOOL __stdcall XQueryPerformanceCounter(LARGE_INTEGER* counter)
{
    if (kernel::FixedClockOn()) {
        counter->QuadPart = LONGLONG(kernel::FixedClockNow());
        return TRUE;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG freq = HostQpcFrequency();
    constexpr LONGLONG kRate = LONGLONG(kernel::kClockUnitsPerSecond);
    counter->QuadPart = now.QuadPart / freq * kRate + now.QuadPart % freq * kRate / freq;
    return TRUE;
}

static BOOL __stdcall XQueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    frequency->QuadPart = LONGLONG(kernel::kClockUnitsPerSecond);
    return TRUE;
}

// The engine's own timer reads the TSC directly (see game::kEngineTimerNow);
// return the host clock in the same Xbox units, 733,333,333 / 16 Hz.
static uint32_t EngineTimerTicks()
{
    constexpr LONGLONG kRate = 733333333 / 16;
    if (kernel::FixedClockOn()) { // netplay: the frame's time (kernel/clock.h)
        const uint64_t t = kernel::FixedClockNow();
        constexpr uint64_t kUnits = kernel::kClockUnitsPerSecond;
        return uint32_t(t / kUnits * kRate + t % kUnits * kRate / kUnits);
    }
    const LONGLONG freq = HostQpcFrequency();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return uint32_t(now.QuadPart / freq * kRate + now.QuadPart % freq * kRate / freq);
}

// Preserves ecx/edx like the original.
static __declspec(naked) void EngineTimerNow()
{
    __asm {
        push ecx
        push edx
        call EngineTimerTicks
        pop edx
        pop ecx
        ret
    }
}

// ---------------------------------------------------------------------------
// Utility (cache) drive. On hardware this picks and formats one of the raw
// cache partitions; here Z: is simply a folder.
// ---------------------------------------------------------------------------
static BOOL __stdcall XMountUtilityDrive(BOOL formatClean)
{
    LOG_INFO("XMountUtilityDrive(formatClean=%d)", formatClean);
    kernel::CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition3");
    return TRUE;
}

static BOOL __stdcall XFormatUtilityDrive()
{
    LOG_INFO("XFormatUtilityDrive");
    return TRUE;
}

// ---------------------------------------------------------------------------
// CancelIo: host file handles, host I/O cancellation (same-thread semantics).
// ---------------------------------------------------------------------------
static BOOL __stdcall XCancelIo(HANDLE file)
{
    return CancelIo(kernel::ToHostHandle(file)) || GetLastError() == ERROR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Traps for XDK functions without a native implementation.
// ---------------------------------------------------------------------------
extern "C" void __stdcall SdkTrap(uint32_t symbolIndex, uint32_t returnAddress)
{
    uint32_t count;
    const game::SdkSymbol* syms = game::SdkSymbols(&count);
    Fatal("The game called an XDK function that has no PC implementation yet:\n\n%s (%08X)\ncalled from %08X",
        syms[symbolIndex].name, syms[symbolIndex].address, returnAddress);
}

static void* MakeTrap(uint32_t index)
{
    static uint8_t* s_trampoline = nullptr;
    static uint32_t s_generation = 0;
    if (!s_trampoline || s_generation != StubGeneration()) { // made again at every start (ResetStubsForBoot)
        s_generation = StubGeneration();
        s_trampoline = AllocStub(16);
        uint8_t* p = s_trampoline;
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04; // push [esp+4] (return address)
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04; // push [esp+4] (index)
        *p++ = 0xE8;
        int32_t rel = int32_t(uintptr_t(&SdkTrap) - uintptr_t(p + 4));
        std::memcpy(p, &rel, 4);
    }
    uint8_t* stub = AllocStub(16);
    stub[0] = 0x68;
    std::memcpy(stub + 1, &index, 4);
    stub[5] = 0xE9;
    int32_t rel = int32_t(uintptr_t(s_trampoline) - uintptr_t(stub + 10));
    std::memcpy(stub + 6, &rel, 4);
    return stub;
}

// Optional call tracing ([Debug] TraceSdk=1): logs the first calls of each
// replaced XDK function, then continues into the replacement unchanged.
extern "C" void __stdcall SdkTraceHit(uint32_t symbolIndex, uint32_t returnAddress)
{
    static volatile LONG counts[1024];
    if (symbolIndex < 1024 && InterlockedIncrement(&counts[symbolIndex]) <= 3) {
        uint32_t count;
        const game::SdkSymbol* syms = game::SdkSymbols(&count);
        LOG_INFO("[trace] %s from %08X", syms[symbolIndex].name, returnAddress);
    }
}

static const void* MakeTraceStub(uint32_t index, const void* target)
{
    // pushad; pushfd; push [esp+36] (return address); push index; call SdkTraceHit; popfd; popad; jmp target
    uint8_t* p = AllocStub(32);
    uint8_t* stub = p;
    *p++ = 0x60;                                          // pushad
    *p++ = 0x9C;                                          // pushfd
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x24;   // push dword [esp+36]
    *p++ = 0x68; std::memcpy(p, &index, 4); p += 4;       // push index
    *p++ = 0xE8;
    int32_t rel = int32_t(uintptr_t(&SdkTraceHit) - uintptr_t(p + 4));
    std::memcpy(p, &rel, 4); p += 4;
    *p++ = 0x9D;                                          // popfd
    *p++ = 0x61;                                          // popad
    *p++ = 0xE9;
    rel = int32_t(uintptr_t(target) - uintptr_t(p + 4));
    std::memcpy(p, &rel, 4);
    return stub;
}

static bool g_TraceSdk = false;
void EnableSdkTrace(bool enable) { g_TraceSdk = enable; }

void InstallHooks()
{
    PatchJump(game::kQueryPerformanceCounter, &XQueryPerformanceCounter);
    PatchJump(game::kQueryPerformanceFrequency, &XQueryPerformanceFrequency);
    PatchJump(game::kEngineTimerNow, &EngineTimerNow);
    PatchJump(game::kXMountUtilityDrive, &XMountUtilityDrive);
    PatchJump(game::kXFormatUtilityDrive, &XFormatUtilityDrive);
    PatchJump(game::kXapiCancelIo, &XCancelIo);
    PatchJump(game::kUnhandledExceptionFilter, &GameUnhandledExceptionFilter);
    static const uint8_t kRet = 0xC3;
    PatchBytes(game::kXapiReclaimKernelInit, &kRet, 1);
    if (kernel::SchedActive())
        PatchGameTsc();

    uint32_t count, replaced = 0, trapped = 0, native = 0;
    const game::SdkSymbol* syms = game::SdkSymbols(&count);
    for (uint32_t i = 0; i < count; ++i) {
        const game::SdkSymbol& s = syms[i];
        auto it = Replacements().find(s.name);
        if (it != Replacements().end()) {
            const void* target = it->second;
            // Netplay: behind a gate that cleans the stack after it (core/gates.h); not the frame's end (Swap),
            // whose stack netplay's snapshots start at.
            if (kernel::SchedActive() && std::strcmp(s.name, "D3DDevice_Swap") != 0)
                if (const void* gate = HostCallGate(kGateSdkFirst + i, target))
                    target = gate;
            PatchJump(s.address, g_TraceSdk ? MakeTraceStub(i, target) : target);
            ++replaced;
        } else if (Passthroughs().count(s.name)) {
            ++native;
        } else if (s.kind == game::SymbolKind::Sdk) {
            PatchJump(s.address, MakeTrap(i));
            ++trapped;
        }
    }
    for (const auto& [name, unused] : Passthroughs())
        if (!game::FindSdkSymbol(name.c_str()))
            LOG_WARN("Pass-through for unknown XDK symbol %s", name.c_str());
    LOG_INFO("XDK functions: %u replaced, %u run as-is, %u trapped", replaced, native, trapped);
}

} // namespace swrots::xapi
