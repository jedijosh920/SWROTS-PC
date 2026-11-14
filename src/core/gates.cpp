// Gates for the game's calls into the port (see gates.h).

#include "core/gates.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "core/log.h"
#include "kernel/sched.h"

namespace swrots {

namespace {

constexpr uint32_t kGateBytes = 16; // push target (5), jmp GateCommon (5), padding

alignas(4096) uint8_t g_GateCode[kGateSlots * kGateBytes];
bool g_GateCodeReady = false;

// The calls in progress on a thread, innermost last: where each returns to, and the stack pointer at its arguments.
struct GateFrame {
    uint32_t ret;
    uint32_t args;
};
struct GateStack {
    uint32_t depth;
    GateFrame frames[256];
};
thread_local GateStack t_Gates;

uint32_t g_GateReturnAddress = 0;

extern "C" void __cdecl GateEnter(uint32_t ret, uint32_t args)
{
    GateStack& s = t_Gates;
    if (s.depth >= 256) {
        LOG_ERROR("Gates: calls nested too deep");
        LogFlush();
        TerminateProcess(GetCurrentProcess(), 3);
    }
    s.frames[s.depth++] = { ret, args };
}

extern "C" uint32_t __cdecl GateLeave(uint32_t esp)
{
    GateStack& s = t_Gates;
    // Frames left by an exception that unwound past their gates (deeper than this call's arguments) are dropped.
    while (s.depth > 1 && s.frames[s.depth - 1].args + 0x400 < esp)
        --s.depth;
    if (!s.depth) {
        LOG_ERROR("Gates: a return with no call");
        LogFlush();
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return s.frames[--s.depth].ret;
}

void GateReturn();

// Entered from a gate: [esp] the target, [esp + 4] the game's return address, its arguments above.
__declspec(naked) void GateCommon()
{
    __asm {
        pushfd
        pushad
        lea eax, [esp + 44]           // the arguments
        push eax
        push dword ptr [esp + 44]     // the game's return address
        call GateEnter
        add esp, 8
        popad
        popfd
        push dword ptr [g_GateReturnAddress]
        pop dword ptr [esp + 4]       // the target returns to GateReturn
        ret                           // to the target
    }
}

// The target returned (its arguments popped or not, as its convention says). Zeroes the stack below, back to
// where 8 KiB of zeros show that nothing deeper was dirtied, then returns to the game with every register as the
// target left it.
__declspec(naked) void GateReturn()
{
    __asm {
        push 0                        // the game's return address, below
        pushfd
        pushad
        lea eax, [esp + 40]
        push eax
        call GateLeave
        add esp, 4
        mov [esp + 36], eax
        mov ecx, esp
        mov edx, dword ptr fs:[8]     // the stack's limit
        xor ebx, ebx
    scrub:
        sub ecx, 4
        cmp ecx, edx
        jb scrubbed
        cmp dword ptr [ecx], 0
        jne dirty
        inc ebx
        cmp ebx, 2048
        jb scrub
        jmp scrubbed
    dirty:
        mov dword ptr [ecx], 0
        xor ebx, ebx
        jmp scrub
    scrubbed:
        popad
        popfd
        mov dword ptr [esp - 4], 0    // the copies of the registers just restored
        mov dword ptr [esp - 8], 0
        mov dword ptr [esp - 12], 0
        mov dword ptr [esp - 16], 0
        mov dword ptr [esp - 20], 0
        mov dword ptr [esp - 24], 0
        mov dword ptr [esp - 28], 0
        mov dword ptr [esp - 32], 0
        mov dword ptr [esp - 36], 0
        ret
    }
}

} // namespace

namespace {

alignas(16) uint8_t g_StringPool[1 << 20];
uint32_t g_StringPoolUsed = 0;
std::mutex g_StringLock;
std::map<std::string, const char*> g_Strings;
std::map<std::wstring, const wchar_t*> g_TextsPooled;

void* PoolTake(uint32_t bytes)
{
    bytes = (bytes + 3) & ~3u;
    if (g_StringPoolUsed + bytes > sizeof(g_StringPool)) {
        static bool logged = false;
        if (!logged)
            LOG_WARN("Gates: the pool of the game's strings is full (not deterministic from here)");
        logged = true;
        return nullptr;
    }
    void* p = g_StringPool + g_StringPoolUsed;
    g_StringPoolUsed += bytes;
    return p;
}

} // namespace

const char* GameString(const char* text)
{
    if (!text)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_StringLock);
    auto it = g_Strings.find(text);
    if (it != g_Strings.end())
        return it->second;
    const size_t bytes = std::strlen(text) + 1;
    auto* p = static_cast<char*>(PoolTake(uint32_t(bytes)));
    if (!p)
        return text;
    std::memcpy(p, text, bytes);
    g_Strings.emplace(text, p);
    return p;
}

const wchar_t* GameText(const wchar_t* text)
{
    if (!text)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_StringLock);
    auto it = g_TextsPooled.find(text);
    if (it != g_TextsPooled.end())
        return it->second;
    const size_t bytes = (std::wcslen(text) + 1) * sizeof(wchar_t);
    auto* p = static_cast<wchar_t*>(PoolTake(uint32_t(bytes)));
    if (!p)
        return text;
    std::memcpy(p, text, bytes);
    g_TextsPooled.emplace(text, p);
    return p;
}

namespace {

__declspec(naked) void ScrubDirtyBelow()
{
    __asm {
        push ebx
        mov ecx, esp
        mov edx, dword ptr fs:[8]
        xor ebx, ebx
    scrub:
        sub ecx, 4
        cmp ecx, edx
        jb scrubbed
        cmp dword ptr [ecx], 0
        jne dirty
        inc ebx
        cmp ebx, 2048
        jb scrub
        jmp scrubbed
    dirty:
        mov dword ptr [ecx], 0
        xor ebx, ebx
        jmp scrub
    scrubbed:
        pop ebx
        ret
    }
}

} // namespace

void ScrubStack()
{
    if (g_GateCodeReady && kernel::SchedActive())
        ScrubDirtyBelow();
}

bool IsCodeAddress(const void* address)
{
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

const void* HostCallGate(uint32_t slot, const void* target)
{
    if (slot >= kGateSlots || !target)
        return nullptr;
    if (!g_GateCodeReady) {
        DWORD old;
        if (!VirtualProtect(g_GateCode, sizeof(g_GateCode), PAGE_EXECUTE_READWRITE, &old)) {
            LOG_ERROR("Gates: no executable room (%lu)", GetLastError());
            return nullptr;
        }
        g_GateReturnAddress = uint32_t(uintptr_t(&GateReturn));
        g_GateCodeReady = true;
    }
    uint8_t* p = g_GateCode + slot * kGateBytes;
    p[0] = 0x68; // push target
    const uint32_t t = uint32_t(uintptr_t(target));
    std::memcpy(p + 1, &t, 4);
    p[5] = 0xE9; // jmp GateCommon
    const int32_t rel = int32_t(uintptr_t(&GateCommon) - uintptr_t(p + 10));
    std::memcpy(p + 6, &rel, 4);
    std::memset(p + 10, 0xCC, kGateBytes - 10);
    FlushInstructionCache(GetCurrentProcess(), p, kGateBytes);
    return p;
}

} // namespace swrots
