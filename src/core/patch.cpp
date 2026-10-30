#include "core/patch.h"

#include <windows.h>
#include <cstring>

#include "core/gates.h"
#include "core/log.h"
#include "kernel/sched.h"

#include <map>

namespace swrots {

// The whole game image is mapped read/write/execute by the image mapper, so
// patches are plain writes; only the instruction cache needs flushing.
void PatchBytes(uint32_t address, const void* bytes, uint32_t length)
{
    std::memcpy(reinterpret_cast<void*>(address), bytes, length);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), length);
}

void PatchJump(uint32_t address, const void* target)
{
    uint8_t code[5] = { 0xE9 };
    int32_t rel = int32_t(uintptr_t(target) - (address + 5));
    std::memcpy(code + 1, &rel, 4);
    PatchBytes(address, code, sizeof(code));
}

void PatchHook(uint32_t address, const void* target)
{
    if (kernel::SchedActive()) {
        // A slot per hooked address, in the order they are first hooked (the same on every PC).
        static std::map<uint32_t, uint32_t> slots;
        auto it = slots.find(address);
        if (it == slots.end())
            it = slots.emplace(address, kGateHookFirst + uint32_t(slots.size())).first;
        if (const void* gate = HostCallGate(it->second, target))
            target = gate;
    }
    PatchJump(address, target);
}

void PatchCall(uint32_t address, const void* target, uint32_t length)
{
    uint8_t code[16];
    if (length < 5 || length > sizeof(code))
        Fatal("PatchCall: bad length %u at %08X", length, address);
    code[0] = 0xE8;
    int32_t rel = int32_t(uintptr_t(target) - (address + 5));
    std::memcpy(code + 1, &rel, 4);
    std::memset(code + 5, 0x90, length - 5);
    PatchBytes(address, code, length);
}

void PatchNop(uint32_t address, uint32_t length)
{
    uint8_t code[64];
    std::memset(code, 0x90, length);
    PatchBytes(address, code, length);
}

// Stubs live in the loader's image range, past the game's image and before the PE header copy (0x9F0000-0xF00000,
// executable), so that they are at the same addresses on every PC: the game's libraries are patched with jumps to
// them, and netplay compares that memory. Taken in order, as the port makes them, from the start of the range again
// at every start of the game (ResetStubsForBoot): every start makes the same stubs in the same order, so they do not
// depend on what the process did before (two PCs that played different things before a match agree).
namespace {
constexpr uint32_t kStubStart = 0x009F0000, kStubEnd = 0x00F00000;
uint32_t s_next = kStubStart;
uint32_t s_generation = 1;
} // namespace

void ResetStubsForBoot()
{
    s_next = kStubStart;
    ++s_generation;
}

uint32_t StubGeneration() { return s_generation; }

uint8_t* AllocStub(uint32_t size)
{
    static uint8_t* s_page = nullptr; // beyond the range: from Windows, as before
    static uint32_t s_used = 0;
    size = (size + 15) & ~15u;
    if (s_next + size <= kStubEnd) {
        uint8_t* p = reinterpret_cast<uint8_t*>(uintptr_t(s_next));
        s_next += size;
        return p;
    }
    static const uint32_t kPage = 0x10000;
    if (!s_page || s_used + size > kPage) {
        s_page = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        s_used = 0;
        if (!s_page)
            Fatal("Out of memory allocating code stubs");
    }
    uint8_t* p = s_page + s_used;
    s_used += size;
    return p;
}

} // namespace swrots
