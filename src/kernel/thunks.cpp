// Resolves the game's kernel import table (the XBE "kernel thunk" table) to
// native implementations. Imports without an implementation resolve to a stub
// that reports the export by name and stops, so gaps show up immediately
// instead of as memory corruption.

#include <windows.h>

#include <cstring>

#include "core/gates.h"
#include "core/log.h"
#include "core/patch.h"
#include "kernel/exports.h"
#include "kernel/kernel.h"
#include "kernel/sched.h"

namespace swrots::kernel {

static const char* kKernelNames[400] = {};
static void* g_Exports[400] = {};

static int InitNames()
{
#define KERNEL_NAME(ord, name) kKernelNames[ord] = name;
#include "kernel/kernel_names.inc"
#undef KERNEL_NAME
    return 0;
}

bool RegisterExport(uint32_t ordinal, const char* name, void* address)
{
    if (ordinal < 400)
        g_Exports[ordinal] = address;
    (void)name;
    return true;
}

extern "C" void __stdcall UnimplementedKernelCall(uint32_t ordinal, uint32_t returnAddress)
{
    static int once = InitNames();
    (void)once;
    const char* name = ordinal < 400 && kKernelNames[ordinal] ? kKernelNames[ordinal] : "?";
    Fatal("The game called an unimplemented Xbox kernel function:\n\n%s (ordinal %u)\ncalled from %08X",
        name, ordinal, returnAddress);
}

// push <ordinal>; jmp UnimplementedTrampoline
static void* MakeUnimplementedStub(uint32_t ordinal)
{
    static uint8_t* s_trampoline = nullptr;
    if (!s_trampoline) {
        // [esp] = ordinal, [esp+4] = caller's return address
        s_trampoline = AllocStub(16);
        uint8_t* p = s_trampoline;
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04; // push dword [esp+4]
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04; // push dword [esp+4]  (ordinal)
        *p++ = 0xE8;                                        // call UnimplementedKernelCall
        int32_t rel = int32_t(uintptr_t(&UnimplementedKernelCall) - uintptr_t(p + 4));
        std::memcpy(p, &rel, 4);
    }
    uint8_t* stub = AllocStub(16);
    stub[0] = 0x68; // push imm32
    std::memcpy(stub + 1, &ordinal, 4);
    stub[5] = 0xE9; // jmp trampoline
    int32_t rel = int32_t(uintptr_t(s_trampoline) - uintptr_t(stub + 10));
    std::memcpy(stub + 6, &rel, 4);
    return stub;
}

void InstallThunks(uint32_t thunkTableAddress)
{
    static int once = InitNames();
    (void)once;

    auto* thunk = reinterpret_cast<uint32_t*>(uintptr_t(thunkTableAddress));
    uint32_t resolved = 0, missing = 0, gated = 0;
    // Netplay: the functions behind gates that clean the stack after them (core/gates.h), but those that leave
    // by unwinding frames.
    const bool gates = SchedActive();
    for (; *thunk; ++thunk) {
        uint32_t ordinal = *thunk & 0x7FFFFFFF;
        void* target = ordinal < 400 ? g_Exports[ordinal] : nullptr;
        if (target) {
            ++resolved;
            const char* name = ordinal < 400 ? kKernelNames[ordinal] : nullptr;
            if (gates && IsCodeAddress(target) && !(name && (!std::strcmp(name, "RtlUnwind") ||
                                                             !std::strcmp(name, "RtlRaiseException")))) {
                if (const void* gate = HostCallGate(ordinal, target)) {
                    target = const_cast<void*>(gate);
                    ++gated;
                }
            }
        } else {
            ++missing;
            target = MakeUnimplementedStub(ordinal);
            LOG_DEBUG("Kernel import %u (%s) has no implementation", ordinal,
                ordinal < 400 && kKernelNames[ordinal] ? kKernelNames[ordinal] : "?");
        }
        *thunk = uint32_t(uintptr_t(target));
    }
    LOG_INFO("Kernel imports: %u native, %u unimplemented (trap if called)%s", resolved, missing,
        gated ? " (behind netplay's gates)" : "");
}

} // namespace swrots::kernel
