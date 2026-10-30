// Xbox threads on Windows threads.
//
// Every thread that runs game code gets an XboxThread: an ETHREAD image the
// game can inspect, a private KPCR, and a TLS block. The KPCR pointer lives in a
// Windows TLS slot; the game's few FS-relative accesses were rewritten (see
// InstallFsPatches) to fetch it from there.

#include <windows.h>
#include <intrin.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/xbe.h"
#include "game/game.h"
#include "kernel/kernel.h"
#include "kernel/sched.h"

namespace swrots::kernel {

// Offset of our TLS slot inside the 32-bit TEB (TlsSlots at 0xE10). Read by the
// naked FS stubs below, so it must be a plain global.
extern "C" uint32_t g_PcrTebOffset = 0;
static DWORD g_PcrTlsIndex = TLS_OUT_OF_INDEXES;

static uint32_t g_TlsRawStart = 0, g_TlsRawSize = 0, g_TlsZeroFill = 0;

static std::mutex g_ThreadsLock;
static std::unordered_map<DWORD, XboxThread*> g_Threads;

void ThreadingInit(const XbeFile& xbe)
{
    g_PcrTlsIndex = TlsAlloc();
    if (g_PcrTlsIndex >= 64)
        Fatal("Could not allocate a fast TLS slot (%lu)", g_PcrTlsIndex);
    g_PcrTebOffset = 0xE10 + g_PcrTlsIndex * 4;

    const XbeHeader& h = xbe.Header();
    if (h.TlsAddress) {
        auto* tls = reinterpret_cast<const XbeTls*>(uintptr_t(h.TlsAddress));
        g_TlsRawStart = tls->DataStartAddress;
        g_TlsRawSize = tls->DataEndAddress - tls->DataStartAddress;
        g_TlsZeroFill = tls->SizeOfZeroFill;
    }
    LOG_INFO("TLS: raw %u bytes, zero-fill %u bytes, TEB slot offset 0x%X", g_TlsRawSize, g_TlsZeroFill, g_PcrTebOffset);
}

void ResetThreadsForReboot()
{
    std::lock_guard<std::mutex> lock(g_ThreadsLock);
    for (auto& [tid, t] : g_Threads) {
        if (t->hostHandle)
            CloseHandle(t->hostHandle);
        if (!t->fixedMemory) { // fixed memory goes with the reboot's reset of the fixed range
            _aligned_free(t->tlsData);
            _aligned_free(t);
        }
    }
    g_Threads.clear();
}

XboxThread* CurrentThread()
{
    auto* pcr = static_cast<xbox::KPCR*>(TlsGetValue(g_PcrTlsIndex));
    return pcr ? static_cast<XboxThread*>(pcr->CurrentThread) : nullptr;
}

XboxThread* ThreadFromKThread(void* kthread)
{
    return static_cast<XboxThread*>(kthread);
}

XboxThread* ThreadFromHandle(HANDLE handle)
{
    if (handle == GetCurrentThread())
        return CurrentThread();
    DWORD tid = GetThreadId(handle);
    std::lock_guard<std::mutex> lock(g_ThreadsLock);
    auto it = g_Threads.find(tid);
    return it == g_Threads.end() ? nullptr : it->second;
}

void ForEachThread(void (*fn)(XboxThread* t, void* ctx), void* ctx)
{
    std::vector<XboxThread*> threads;
    {
        std::lock_guard<std::mutex> lock(g_ThreadsLock);
        for (auto& [tid, t] : g_Threads)
            threads.push_back(t);
    }
    for (XboxThread* t : threads)
        fn(t, ctx);
}

static XboxThread* AllocThread(uint32_t tlsSize)
{
    if (!tlsSize)
        tlsSize = ((g_TlsRawSize + g_TlsZeroFill + 15) & ~15u) + 4;
    // Deterministic addresses (netplay): the thread object and its TLS in the fixed range, where every PC has them
    // at the same addresses (the game keeps pointers to them).
    const uint32_t objectBytes = (sizeof(XboxThread) + 15) & ~15u;
    uint8_t* fixed = static_cast<uint8_t*>(AllocFixedThreadMemory(objectBytes + ((tlsSize + 15) & ~15u)));
    auto* t = fixed ? reinterpret_cast<XboxThread*>(fixed) : static_cast<XboxThread*>(_aligned_malloc(sizeof(XboxThread), 16));
    std::memset(t, 0, sizeof(XboxThread));
    t->fixedMemory = fixed != nullptr;

    // TLS block: [pointer to data][raw template][zero fill]. The block ends at
    // NtTib.StackBase and XAPI sets _tls_index to -(size / 4), so the game finds
    // the block's first word at fs:[4] + _tls_index * 4. The size must match
    // what XAPI computed ((raw + zero fill + 15) & ~15) + 4.
    t->tlsSize = tlsSize;
    t->tlsData = fixed ? fixed + objectBytes : static_cast<uint8_t*>(_aligned_malloc((tlsSize + 15) & ~15u, 16));
    std::memset(t->tlsData, 0, tlsSize);
    *reinterpret_cast<uint8_t**>(t->tlsData) = t->tlsData + 4;
    if (g_TlsRawSize)
        std::memcpy(t->tlsData + 4, reinterpret_cast<void*>(uintptr_t(g_TlsRawStart)), g_TlsRawSize);

    *reinterpret_cast<uint8_t**>(t->ethread + xbox::KTHREAD_TlsData) = t->tlsData;
    // Dispatcher header of the thread object, so waits on the KTHREAD resolve.
    t->ethread[0] = xbox::ThreadObject;
    return t;
}

// Installs the Xbox per-thread state on the calling host thread.
static void BindThread(XboxThread* t)
{
    NT_TIB* hostTib = reinterpret_cast<NT_TIB*>(__readfsdword(0x18));
    xbox::KPCR& pcr = t->pcr;
    pcr.NtTib = *hostTib;
    pcr.NtTib.StackBase = t->tlsData + t->tlsSize; // Xbox convention: TLS ends at StackBase
    pcr.NtTib.Self = &pcr.NtTib;
    pcr.SelfPcr = &pcr;
    pcr.Prcb = &pcr.CurrentThread;
    pcr.Irql = kPassiveLevel;
    pcr.CurrentThread = t;
    t->threadId = GetCurrentThreadId();
    TlsSetValue(g_PcrTlsIndex, &pcr);

    std::lock_guard<std::mutex> lock(g_ThreadsLock);
    g_Threads[t->threadId] = t;
}

XboxThread* AdoptCurrentThread()
{
    if (XboxThread* t = CurrentThread())
        return t;
    XboxThread* t = AllocThread(0);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->hostHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    BindThread(t);
    return t;
}

static void __cdecl RunXboxThread(XboxThread* t)
{
    BindThread(t);
    SchedThreadStart(t); // the deterministic scheduler: its first turn
    if (t->system)
        t->system(t->start, t->context);
    else
        t->start(t->context);
    SchedThreadEnd();
}

// Calls fn(t) on the stack whose top is `top`, and comes back to the caller's.
static __declspec(naked) void __cdecl RunOnStack(void* top, void(__cdecl* fn)(XboxThread*), XboxThread* t)
{
    __asm {
        push ebp
        mov ebp, esp
        mov eax, [ebp + 8]
        mov ecx, [ebp + 12]
        mov edx, [ebp + 16]
        mov esp, eax
        push edx
        call ecx
        mov esp, ebp
        pop ebp
        ret
    }
}

static DWORD WINAPI XboxThreadProc(void* param)
{
    auto* t = static_cast<XboxThread*>(param);
    if (t->fixedStack) {
        // Deterministic addresses (netplay): the game's code runs on a stack in the fixed range, at the same address
        // on every PC (the game keeps pointers into its stacks). The TEB's stack bounds follow, for exception
        // dispatch (and the Swap's stack scrub).
        auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
        void* const base = tib->StackBase;
        void* const limit = tib->StackLimit;
        tib->StackBase = t->fixedStack + t->fixedStackSize;
        tib->StackLimit = t->fixedStack;
        RunOnStack(t->fixedStack + t->fixedStackSize - 16, &RunXboxThread, t);
        tib->StackBase = base;
        tib->StackLimit = limit;
        return 0;
    }
    RunXboxThread(t);
    return 0;
}

// The Xbox has a single CPU, and the game's threads rely on that: they share
// data without the locking a multi-core machine needs, so running them in
// parallel lets one thread see another's half-written state (e.g. a model's
// transform mid-update, drawn as a giant stretched polygon for a frame). All
// threads that run game code share one host core, as on the console; host-only
// work (the driver, audio mixing, the window) stays free to use the others.
static DWORD_PTR GameCpuMask()
{
    static const DWORD_PTR mask = [] {
        if (GetEnvironmentVariableA("SWROTS_ALL_CORES", nullptr, 0))
            return DWORD_PTR(0);
        DWORD_PTR process = 0, system = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
        DWORD_PTR rest = process & (process - 1); // without the lowest core, often busiest with interrupts
        DWORD_PTR pick = rest ? rest & ~(rest - 1) : process;
        LOG_INFO("Game threads run on CPU mask %08lX (process mask %08lX)", DWORD(pick), DWORD(process));
        return pick;
    }();
    return mask;
}

void PinToGameCpu(HANDLE thread)
{
    if (DWORD_PTR mask = GameCpuMask())
        SetThreadAffinityMask(thread, mask);
}

HANDLE CreateXboxThread(StartRoutine start, void* context, SystemRoutine system, uint32_t stackSize,
    uint32_t tlsSize, bool suspended, DWORD* threadId)
{
    XboxThread* t = AllocThread(tlsSize);
    t->start = start;
    t->context = context;
    t->system = system;

    // Xbox stacks are tiny (64 KiB by default); host code we call into (drivers,
    // audio, file I/O) needs far more headroom.
    SIZE_T reserve = stackSize < 0x100000 ? 0x100000 : stackSize;
    if (uint8_t* stack = static_cast<uint8_t*>(AllocFixedThreadMemory(uint32_t(reserve)))) {
        t->fixedStack = stack;
        t->fixedStackSize = uint32_t(reserve);
        reserve = 0x10000; // the host's own stack only starts the thread
    }
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, reserve, XboxThreadProc, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!h)
        return nullptr;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &t->hostHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    PinToGameCpu(h);
    {
        std::lock_guard<std::mutex> lock(g_ThreadsLock);
        g_Threads[tid] = t;
    }
    if (threadId)
        *threadId = tid;
    t->threadId = tid;
    SchedAddThread(t, suspended); // its turns, in the order threads are made
    if (!suspended)
        ResumeThread(h);
    return h;
}

// ---------------------------------------------------------------------------
// FS stubs. Each replaces one FS-relative instruction with identical register
// results and no flag changes.
// ---------------------------------------------------------------------------
extern "C" __declspec(naked) void FsStub_Fs20Eax()
{
    __asm {
        mov eax, g_PcrTebOffset
        mov eax, fs:[eax]
        mov eax, [eax + 0x20]
        ret
    }
}

extern "C" __declspec(naked) void FsStub_Fs24Eax()
{
    __asm {
        mov eax, g_PcrTebOffset
        mov eax, fs:[eax]
        movzx eax, byte ptr [eax + 0x24]
        ret
    }
}

extern "C" __declspec(naked) void FsStub_Fs28Eax()
{
    __asm {
        mov eax, g_PcrTebOffset
        mov eax, fs:[eax]
        mov eax, [eax + 0x28]
        ret
    }
}

extern "C" __declspec(naked) void FsStub_Fs04Ecx()
{
    __asm {
        mov ecx, g_PcrTebOffset
        mov ecx, fs:[ecx]
        mov ecx, [ecx + 4]
        ret
    }
}

extern "C" __declspec(naked) void FsStub_Fs04Edi()
{
    __asm {
        mov edi, g_PcrTebOffset
        mov edi, fs:[edi]
        mov edi, [edi + 4]
        ret
    }
}

void InstallFsPatches()
{
    using game::FsKind;
    for (const game::FsPatchSite& site : game::kFsPatchSites) {
        const auto* code = reinterpret_cast<const uint8_t*>(uintptr_t(site.address));
        if (code[0] != 0x64)
            Fatal("FS patch site %08X does not hold an FS instruction", site.address);
        const void* stub = nullptr;
        switch (site.kind) {
        case FsKind::Fs20Eax: stub = FsStub_Fs20Eax; break;
        case FsKind::Fs24Eax: stub = FsStub_Fs24Eax; break;
        case FsKind::Fs28Eax: stub = FsStub_Fs28Eax; break;
        case FsKind::Fs04Ecx: stub = FsStub_Fs04Ecx; break;
        case FsKind::Fs04Edi: stub = FsStub_Fs04Edi; break;
        }
        PatchCall(site.address, stub, site.length);
    }
    LOG_INFO("Patched %u FS-relative instructions", uint32_t(std::size(game::kFsPatchSites)));
}

} // namespace swrots::kernel
