// Memory: virtual memory, contiguous ("physical") memory and pool.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_set>

#include "core/log.h"
#include "kernel/exports.h"
#include "kernel/host_nt.h"
#include "kernel/kernel.h"
#include "kernel/mm.h"

namespace swrots::kernel {

// Xbox-only protection modifiers. Uncached/write-combined host memory would be
// extremely slow for the CPU-side access the game does, so they are dropped.
static const ULONG kXboxOnlyProtect = PAGE_NOCACHE | PAGE_WRITECOMBINE;
static const ULONG kMemNoZero = 0x00800000;

static ULONG HostProtect(ULONG protect)
{
    protect &= ~kXboxOnlyProtect;
    return protect ? protect : PAGE_READWRITE;
}

// ---------------------------------------------------------------------------
// Contiguous memory: a first-fit page allocator over the region reserved by the
// loader, so allocations have stable "physical" addresses (va - base).
// ---------------------------------------------------------------------------
static uint32_t g_ContigBase = 0, g_ContigSize = 0;
static std::mutex g_ContigLock;
static std::map<uint32_t, uint32_t> g_ContigFree; // offset -> size
static std::map<uint32_t, uint32_t> g_ContigUsed; // offset -> size

// What the game allocated outside contiguous memory, released when it reboots.
static std::mutex g_TrackLock;
static std::unordered_set<void*> g_VirtualBases; // NtAllocateVirtualMemory / MmAllocateSystemMemory
static std::unordered_set<void*> g_PoolBlocks;   // ExAllocatePoolWithTag
static std::mutex g_PoolLock; // the netplay pool
// Moves on whenever the game allocates or frees memory (MemoryLayoutGeneration).
static std::atomic<uint32_t> g_LayoutGeneration{ 0 };

void ContiguousInit(void* base, uint32_t size)
{
    g_ContigBase = uint32_t(uintptr_t(base));
    g_ContigSize = size;
    if (size)
        g_ContigFree[0] = size;
    LOG_INFO("Contiguous memory: %08X, %u MiB", g_ContigBase, size >> 20);
}

bool IsContiguous(const void* p)
{
    uint32_t a = uint32_t(uintptr_t(p));
    return g_ContigSize && a >= g_ContigBase && a < g_ContigBase + g_ContigSize;
}

uint32_t ContiguousToPhysical(const void* p)
{
    return uint32_t(uintptr_t(p)) - g_ContigBase;
}

void* PhysicalToContiguous(uint32_t physical)
{
    return reinterpret_cast<void*>(uintptr_t(g_ContigBase + (physical & 0x0FFFFFFF)));
}

bool ConsumeWrites(const void* p, uint32_t size)
{
    if (!IsContiguous(p) || !size)
        return true;
    uintptr_t start = uintptr_t(p) & ~uintptr_t(0xFFF);
    uintptr_t end = (uintptr_t(p) + size + 0xFFF) & ~uintptr_t(0xFFF);
    void* pages[1];
    ULONG_PTR count = 1;
    ULONG granularity;
    if (GetWriteWatch(0, reinterpret_cast<void*>(start), end - start, pages, &count, &granularity) != 0)
        return true; // write watch unavailable: assume dirty
    if (count == 0)
        return false;
    ResetWriteWatch(reinterpret_cast<void*>(start), end - start);
    return true;
}

void* XBAPI MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG_PTR LowestAcceptableAddress,
    ULONG_PTR HighestAcceptableAddress, ULONG Alignment, ULONG ProtectionType)
{
    uint32_t size = (NumberOfBytes + 0xFFF) & ~0xFFFu;
    uint32_t align = Alignment < 0x1000 ? 0x1000 : Alignment;
    if (!size)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_ContigLock);
    for (auto it = g_ContigFree.begin(); it != g_ContigFree.end(); ++it) {
        uint32_t start = it->first, len = it->second;
        uint32_t aligned = (start + align - 1) & ~(align - 1);
        if (aligned < LowestAcceptableAddress)
            aligned = (uint32_t(LowestAcceptableAddress) + align - 1) & ~(align - 1);
        if (aligned + size > start + len || aligned + size - 1 > HighestAcceptableAddress)
            continue;

        g_ContigFree.erase(it);
        if (aligned > start)
            g_ContigFree[start] = aligned - start;
        if (aligned + size < start + len)
            g_ContigFree[aligned + size] = start + len - (aligned + size);
        g_ContigUsed[aligned] = size;
        ++g_LayoutGeneration;

        void* p = reinterpret_cast<void*>(uintptr_t(g_ContigBase + aligned));
        if (!VirtualAlloc(p, size, MEM_COMMIT, HostProtect(ProtectionType)))
            Fatal("Failed to commit contiguous memory (%u bytes)", size);
        return p;
    }
    LOG_ERROR("MmAllocateContiguousMemoryEx(%lu): out of contiguous memory", NumberOfBytes);
    return nullptr;
}

void* XBAPI MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    return MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

void XBAPI MmFreeContiguousMemory(void* BaseAddress)
{
    std::lock_guard<std::mutex> lock(g_ContigLock);
    uint32_t off = uint32_t(uintptr_t(BaseAddress)) - g_ContigBase;
    auto it = g_ContigUsed.find(off);
    if (it == g_ContigUsed.end()) {
        LOG_WARN("MmFreeContiguousMemory(%p): not an allocation", BaseAddress);
        return;
    }
    uint32_t size = it->second;
    g_ContigUsed.erase(it);
    ++g_LayoutGeneration;
    VirtualFree(BaseAddress, size, MEM_DECOMMIT);

    // Insert and coalesce with neighbours.
    auto next = g_ContigFree.lower_bound(off);
    if (next != g_ContigFree.end() && off + size == next->first) {
        size += next->second;
        next = g_ContigFree.erase(next);
    }
    if (next != g_ContigFree.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            prev->second += size;
            return;
        }
    }
    g_ContigFree[off] = size;
}

void XBAPI MmPersistContiguousMemory(void* BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    (void)BaseAddress;
    (void)NumberOfBytes;
    (void)Persist;
}

ULONG_PTR XBAPI MmGetPhysicalAddress(void* BaseAddress)
{
    if (IsContiguous(BaseAddress))
        return ContiguousToPhysical(BaseAddress);
    return ULONG_PTR(BaseAddress) & 0x0FFFFFFF;
}

ULONG XBAPI MmQueryAllocationSize(void* BaseAddress)
{
    if (IsContiguous(BaseAddress)) {
        std::lock_guard<std::mutex> lock(g_ContigLock);
        auto it = g_ContigUsed.find(ContiguousToPhysical(BaseAddress));
        return it == g_ContigUsed.end() ? 0 : it->second;
    }
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) ? ULONG(mbi.RegionSize) : 0;
}

KERNEL_EXPORT(165, MmAllocateContiguousMemory);
KERNEL_EXPORT(166, MmAllocateContiguousMemoryEx);
KERNEL_EXPORT(171, MmFreeContiguousMemory);
KERNEL_EXPORT(178, MmPersistContiguousMemory);
KERNEL_EXPORT(173, MmGetPhysicalAddress);
KERNEL_EXPORT(180, MmQueryAllocationSize);

// ---------------------------------------------------------------------------
// The fixed range (netplay): while deterministic addresses are on (SetDeterministicAddresses: a netplay match, from
// the reboot into its level), the game's virtual memory is placed by the port, first-fit in one range reserved at a
// fixed address, instead of wherever Windows puts it, so that two PCs (or two runs) give the game the same addresses.
// The range starts with the netplay pool's arena (below). It stays reserved for the process; a reboot decommits it.
// ---------------------------------------------------------------------------
namespace {

constexpr uintptr_t kFixedBase = 0x20000000;
constexpr uint32_t kFixedSize = 0x10000000; // 256 MiB
constexpr uint32_t kArenaSize = 64u << 20;  // the pool's arena, at the start of the range
constexpr uint32_t kGranularity = 0x10000;  // Windows' allocation granularity, kept for the game's reservations
// The range's last part holds the game threads' own memory (stacks, thread objects, TLS): placed the same way, but
// not the game's state for netplay's snapshots (restoring another thread's stack while it is stopped would break it).
constexpr uint32_t kThreadZone = 32u << 20;

uint8_t* g_Fixed = nullptr;
std::atomic<bool> g_DetAddresses{ false };
std::mutex g_PlaceLock;
std::map<uintptr_t, uint32_t> g_Placed; // the game's reservations in the range: base -> size

bool EnsureFixed()
{
    if (g_Fixed)
        return true;
    void* base = VirtualAlloc(reinterpret_cast<void*>(kFixedBase), kFixedSize, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (base != reinterpret_cast<void*>(kFixedBase)) {
        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
        LOG_ERROR("Netplay: the fixed range %08X-%08X is taken on this PC; addresses will not be deterministic",
            uint32_t(kFixedBase), uint32_t(kFixedBase + kFixedSize));
        return false;
    }
    g_Fixed = static_cast<uint8_t*>(base);
    LOG_INFO("Netplay: fixed range reserved at %p (%u MiB)", base, kFixedSize >> 20);
    return true;
}

// The placed reservation containing `p`, or g_Placed.end().
std::map<uintptr_t, uint32_t>::iterator PlacedAt(const void* p)
{
    const uintptr_t a = uintptr_t(p);
    auto it = g_Placed.upper_bound(a);
    if (it == g_Placed.begin())
        return g_Placed.end();
    --it;
    return a < it->first + it->second ? it : g_Placed.end();
}

// First fit after the arena (or in the thread zone), at the allocation granularity; 0 when there is no room.
uintptr_t Place(uint32_t size, bool threadZone = false)
{
    size = (size + kGranularity - 1) & ~(kGranularity - 1);
    const uintptr_t zoneStart = kFixedBase + kFixedSize - kThreadZone;
    const uintptr_t low = threadZone ? zoneStart : kFixedBase + kArenaSize;
    const uintptr_t high = threadZone ? kFixedBase + kFixedSize : zoneStart;
    uintptr_t at = low;
    for (const auto& [base, length] : g_Placed) {
        if (base + length <= at)
            continue;
        if (at + size <= base)
            break;
        at = std::max(at, (base + length + kGranularity - 1) & ~uintptr_t(kGranularity - 1));
    }
    if (at + size > high)
        return 0;
    g_Placed[at] = size;
    return at;
}

bool InFixed(const void* p) { return g_Fixed && uintptr_t(p) >= kFixedBase && uintptr_t(p) < kFixedBase + kFixedSize; }

} // namespace

bool EnsureArena();

bool SetDeterministicAddresses(bool on)
{
    if (on && !EnsureFixed())
        return false;
    if (on) {
        std::lock_guard<std::mutex> lock(g_PoolLock); // pool blocks from the arena from now on
        EnsureArena();
    }
    if (on != g_DetAddresses)
        LOG_INFO("Netplay: deterministic addresses %s", on ? "on" : "off");
    g_DetAddresses = on;
    return true;
}

bool DeterministicAddresses() { return g_DetAddresses; }

std::string DescribeFixedAddress(uintptr_t address)
{
    if (!InFixed(reinterpret_cast<void*>(address)))
        return {};
    char text[160];
    std::lock_guard<std::mutex> lock(g_PlaceLock);
    auto it = PlacedAt(reinterpret_cast<void*>(address));
    if (it != g_Placed.end()) {
        std::snprintf(text, sizeof(text), "in the fixed range, placed %08X+%X", uint32_t(it->first), it->second);
    } else {
        auto next = g_Placed.upper_bound(address);
        uint32_t before = 0, beforeSize = 0;
        if (next != g_Placed.begin()) {
            auto prev = std::prev(next);
            before = uint32_t(prev->first);
            beforeSize = prev->second;
        }
        std::snprintf(text, sizeof(text), "in the fixed range, not placed (before it %08X+%X, after %08X)", before,
            beforeSize, next == g_Placed.end() ? 0u : uint32_t(next->first));
    }
    return text;
}

void* AllocFixedThreadMemory(uint32_t size)
{
    if (!g_DetAddresses || !g_Fixed)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_PlaceLock);
    const uintptr_t at = Place(size, true);
    if (!at)
        return nullptr;
    void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT, PAGE_READWRITE);
    if (!p)
        g_Placed.erase(at);
    return p;
}

// ---------------------------------------------------------------------------
// System memory and protection
// ---------------------------------------------------------------------------
void* XBAPI MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    if (g_DetAddresses) { // placed in the fixed range (netplay)
        std::lock_guard<std::mutex> lock(g_PlaceLock);
        if (const uintptr_t at = Place(NumberOfBytes)) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(at), NumberOfBytes, MEM_COMMIT, HostProtect(Protect));
            if (p) {
                ++g_LayoutGeneration;
                return p;
            }
            g_Placed.erase(at);
        }
    }
    // Write-watched, as the game's other memory, for netplay's snapshots (net/state.h).
    void* p = VirtualAlloc(nullptr, NumberOfBytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, HostProtect(Protect));
    if (!p)
        p = VirtualAlloc(nullptr, NumberOfBytes, MEM_RESERVE | MEM_COMMIT, HostProtect(Protect));
    if (p) {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_VirtualBases.insert(p);
        ++g_LayoutGeneration;
    }
    return p;
}

ULONG XBAPI MmFreeSystemMemory(void* BaseAddress, ULONG NumberOfBytes)
{
    if (InFixed(BaseAddress)) {
        std::lock_guard<std::mutex> lock(g_PlaceLock);
        auto it = PlacedAt(BaseAddress);
        if (it != g_Placed.end()) {
            VirtualFree(reinterpret_cast<void*>(it->first), it->second, MEM_DECOMMIT);
            g_Placed.erase(it);
            ++g_LayoutGeneration;
        }
        return (NumberOfBytes + 0xFFF) >> 12;
    }
    {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_VirtualBases.erase(BaseAddress);
        ++g_LayoutGeneration;
    }
    VirtualFree(BaseAddress, 0, MEM_RELEASE);
    return (NumberOfBytes + 0xFFF) >> 12;
}

ULONG XBAPI MmQueryAddressProtect(void* VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(VirtualAddress, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT ? mbi.Protect : 0;
}

void XBAPI MmSetAddressProtect(void* BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old;
    VirtualProtect(BaseAddress, NumberOfBytes, HostProtect(NewProtect), &old);
}

void XBAPI MmQueryStatistics(xbox::MM_STATISTICS* Statistics)
{
    // Report a retail 64 MiB console with a typical amount free, so the engine
    // sizes its pools exactly as it does on hardware.
    if (Statistics->Length != sizeof(xbox::MM_STATISTICS))
        return;
    Statistics->TotalPhysicalPages = 16384;
    Statistics->AvailablePages = 12800;
    Statistics->VirtualMemoryBytesCommitted = 0x00A00000;
    Statistics->VirtualMemoryBytesReserved = 0x01000000;
    Statistics->CachePagesCommitted = 256;
    Statistics->PoolPagesCommitted = 256;
    Statistics->StackPagesCommitted = 64;
    Statistics->ImagePagesCommitted = 2400;
}

KERNEL_EXPORT(167, MmAllocateSystemMemory);
KERNEL_EXPORT(172, MmFreeSystemMemory);
KERNEL_EXPORT(179, MmQueryAddressProtect);
KERNEL_EXPORT(182, MmSetAddressProtect);
KERNEL_EXPORT(181, MmQueryStatistics);

// ---------------------------------------------------------------------------
// Virtual memory
// ---------------------------------------------------------------------------
// Diagnostics: SWROTS_LOG_VM=1 logs the game's virtual memory calls (the first 3000).
static bool LogVm()
{
    static const int on = [] {
        char v[8] = "";
        return GetEnvironmentVariableA("SWROTS_LOG_VM", v, sizeof(v)) && v[0] == '1' ? 1 : 0;
    }();
    static std::atomic<int> count{ 0 };
    return on && count++ < 3000;
}

NTSTATUS XBAPI NtAllocateVirtualMemory(void** BaseAddress, ULONG_PTR ZeroBits, ULONG* AllocationSize,
    ULONG AllocationType, ULONG Protect)
{
    struct Logger {
        void** base;
        ULONG* size;
        void* askedBase;
        ULONG askedSize, type;
        ~Logger()
        {
            if (LogVm())
                LOG_INFO("VM: allocate %p+%lX type %lX -> %p+%lX", askedBase, askedSize, type, *base, *size);
        }
    } logger{ BaseAddress, AllocationSize, *BaseAddress, *AllocationSize, AllocationType };
    SIZE_T size = *AllocationSize;
    ULONG type = AllocationType & ~kMemNoZero;
    // A commit with no address reserves too (as on Windows): XAPI's heap makes its large blocks so (RtlAllocateHeap).
    if ((type & MEM_COMMIT) && !*BaseAddress)
        type |= MEM_RESERVE;
    if (g_Fixed && (g_DetAddresses || InFixed(*BaseAddress))) {
        // Netplay: new reservations placed in the fixed range; commits in a placed reservation.
        std::lock_guard<std::mutex> lock(g_PlaceLock);
        uintptr_t at = 0;
        if ((type & MEM_RESERVE) && !*BaseAddress && g_DetAddresses)
            at = Place(uint32_t(size));
        else if (InFixed(*BaseAddress) && PlacedAt(*BaseAddress) != g_Placed.end())
            at = uintptr_t(*BaseAddress);
        if (at) {
            const uintptr_t begin = at & ~uintptr_t(0xFFF);
            const uintptr_t end = (uintptr_t(at) + size + 0xFFF) & ~uintptr_t(0xFFF);
            if ((type & MEM_COMMIT) &&
                !VirtualAlloc(reinterpret_cast<void*>(begin), end - begin, MEM_COMMIT, HostProtect(Protect))) {
                LOG_WARN("NtAllocateVirtualMemory: commit %08X+%lu in the fixed range failed", uint32_t(begin), ULONG(size));
                if (type & MEM_RESERVE)
                    g_Placed.erase(at);
                return xbox::X_STATUS_NO_MEMORY;
            }
            *BaseAddress = reinterpret_cast<void*>(begin);
            *AllocationSize = ULONG((type & MEM_RESERVE) ? g_Placed[at] : uint32_t(end - begin));
            ++g_LayoutGeneration;
            return xbox::X_STATUS_SUCCESS;
        }
    }
    NTSTATUS status = -1;
    // New reservations are write-watched, as the game's other memory, for netplay's snapshots (net/state.h);
    // without it if Windows refuses (the snapshots then compare those pages instead).
    if (type & MEM_RESERVE) {
        void* base = *BaseAddress;
        status = ::NtAllocateVirtualMemory(GetCurrentProcess(), &base, ZeroBits, &size, type | MEM_WRITE_WATCH,
            HostProtect(Protect));
        if (status >= 0)
            *BaseAddress = base;
        else
            size = *AllocationSize;
    }
    if (status < 0)
        status = ::NtAllocateVirtualMemory(GetCurrentProcess(), BaseAddress, ZeroBits, &size, type, HostProtect(Protect));
    if (g_DetAddresses && status >= 0 && (type & MEM_RESERVE)) {
        static int logged = 0;
        if (logged++ < 20)
            LOG_WARN("Netplay: NtAllocateVirtualMemory(%08lX, %lu bytes) placed by Windows at %p (not deterministic)",
                AllocationType, ULONG(size), *BaseAddress);
    }
    *AllocationSize = ULONG(size);
    if (status < 0)
        LOG_WARN("NtAllocateVirtualMemory(%p, %lu, %08lX) -> %08lX", *BaseAddress, size, AllocationType, status);
    else if (type & MEM_RESERVE) {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_VirtualBases.insert(*BaseAddress);
    }
    if (status >= 0)
        ++g_LayoutGeneration; // a reservation, or pages committed in one
    return status;
}

NTSTATUS XBAPI NtFreeVirtualMemory(void** BaseAddress, ULONG* FreeSize, ULONG FreeType)
{
    if (LogVm())
        LOG_INFO("VM: free %p+%lX type %lX", *BaseAddress, *FreeSize, FreeType);
    if (InFixed(*BaseAddress)) {
        std::lock_guard<std::mutex> lock(g_PlaceLock);
        auto it = PlacedAt(*BaseAddress);
        if (it == g_Placed.end())
            return xbox::X_STATUS_INVALID_PARAMETER;
        if (FreeType & MEM_RELEASE) {
            VirtualFree(reinterpret_cast<void*>(it->first), it->second, MEM_DECOMMIT);
            *FreeSize = it->second;
            g_Placed.erase(it);
        } else {
            const uintptr_t begin = uintptr_t(*BaseAddress) & ~uintptr_t(0xFFF);
            const uint32_t length = *FreeSize ? *FreeSize : uint32_t(it->first + it->second - begin);
            VirtualFree(reinterpret_cast<void*>(begin), length, MEM_DECOMMIT);
        }
        ++g_LayoutGeneration;
        return xbox::X_STATUS_SUCCESS;
    }
    SIZE_T size = *FreeSize;
    if (FreeType & MEM_RELEASE) {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_VirtualBases.erase(*BaseAddress);
    }
    ++g_LayoutGeneration;
    NTSTATUS status = ::NtFreeVirtualMemory(GetCurrentProcess(), BaseAddress, &size, FreeType);
    *FreeSize = ULONG(size);
    return status;
}

NTSTATUS XBAPI NtQueryVirtualMemory(void* BaseAddress, MEMORY_BASIC_INFORMATION* MemoryInformation)
{
    const NTSTATUS status = ::NtQueryVirtualMemory(GetCurrentProcess(), BaseAddress, 0 /*MemoryBasicInformation*/,
        MemoryInformation, sizeof(MEMORY_BASIC_INFORMATION), nullptr);
    if (status >= 0 && InFixed(BaseAddress) && uintptr_t(BaseAddress) >= kFixedBase + kArenaSize) {
        // The fixed range is one Windows reservation: as Windows would show the game's own reservations, each placed
        // one is an allocation of its own (its regions end with it), and what is between them is free.
        std::lock_guard<std::mutex> lock(g_PlaceLock);
        MEMORY_BASIC_INFORMATION& m = *MemoryInformation;
        const uintptr_t at = uintptr_t(m.BaseAddress);
        auto it = PlacedAt(BaseAddress);
        if (it != g_Placed.end()) {
            m.AllocationBase = reinterpret_cast<void*>(it->first);
            const uintptr_t end = it->first + it->second;
            if (at + m.RegionSize > end)
                m.RegionSize = end - at;
        } else {
            auto next = g_Placed.upper_bound(uintptr_t(BaseAddress));
            const uintptr_t end = next == g_Placed.end() ? kFixedBase + kFixedSize : next->first;
            m.AllocationBase = nullptr;
            m.AllocationProtect = 0;
            m.State = MEM_FREE;
            m.Protect = PAGE_NOACCESS;
            m.Type = 0;
            m.RegionSize = end - at;
        }
    }
    return status;
}

KERNEL_EXPORT(184, NtAllocateVirtualMemory);
KERNEL_EXPORT(199, NtFreeVirtualMemory);
KERNEL_EXPORT(217, NtQueryVirtualMemory);

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------
// --- The deterministic pool (netplay) ---------------------------------------------------------
// While netplay's rollback runs, pool blocks come from an arena of the port's (in the game's virtual memory, so the
// snapshots cover it, write-watched), whose allocator keeps all its state inside the arena: a frame played again
// gets the same blocks at the same addresses, and a rollback undoes allocations with the rest of the memory. Blocks
// on the PC's heap from before the session that the game frees meanwhile are only freed when the session ends (a
// rollback could give the game one back): their list is in the arena too.

namespace {

constexpr uint32_t kArenaMagic = 0x4C4F4F50; // 'POOL'
constexpr int kClasses = 21;                 // block sizes 32 B .. 32 MiB, powers of two
constexpr uint32_t kDeferred = 8192;

struct ArenaHeader {
    uint32_t magic;
    uint32_t top;               // offset of the first byte never handed out
    uint32_t free[kClasses];    // each class's free list (offsets, 0 = empty)
    uint32_t deferredCount;
    uint32_t deferredLost;
    void* deferred[kDeferred];  // heap blocks the game freed during the session
};

struct BlockHeader { // 16 bytes before each block
    uint32_t magic;
    uint32_t sizeClass;
    uint32_t requested;
    uint32_t next; // in a free list
};

uint8_t* g_Arena = nullptr;
std::atomic<bool> g_ArenaOn{ false };

ArenaHeader& Arena() { return *reinterpret_cast<ArenaHeader*>(g_Arena); }

bool InArena(const void* p)
{
    return g_Arena && p >= g_Arena && p < g_Arena + kArenaSize;
}

int ClassFor(uint32_t bytes)
{
    const uint32_t need = bytes + sizeof(BlockHeader);
    for (int c = 0; c < kClasses; ++c)
        if ((32u << c) >= need)
            return c;
    return -1;
}

void* ArenaAlloc(uint32_t bytes)
{
    ArenaHeader& a = Arena();
    const int c = ClassFor(bytes);
    if (c < 0)
        return nullptr;
    const uint32_t size = 32u << c;
    uint32_t at = a.free[c];
    if (at) {
        a.free[c] = reinterpret_cast<BlockHeader*>(g_Arena + at)->next;
    } else {
        if (a.top + size > kArenaSize)
            return nullptr;
        at = a.top;
        a.top += size;
    }
    auto* h = reinterpret_cast<BlockHeader*>(g_Arena + at);
    h->magic = kArenaMagic;
    h->sizeClass = uint32_t(c);
    h->requested = bytes;
    h->next = 0;
    return h + 1;
}

void ArenaFree(void* p)
{
    auto* h = static_cast<BlockHeader*>(p) - 1;
    if (h->magic != kArenaMagic || h->sizeClass >= uint32_t(kClasses)) {
        LOG_WARN("ExFreePool(%p): not a block of the netplay pool", p);
        return;
    }
    ArenaHeader& a = Arena();
    h->magic = 0;
    h->next = a.free[h->sizeClass];
    a.free[h->sizeClass] = uint32_t(reinterpret_cast<uint8_t*>(h) - g_Arena);
}

void HeapFreeBlock(void* p)
{
    {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_PoolBlocks.erase(p);
        ++g_LayoutGeneration;
    }
    HeapFree(GetProcessHeap(), 0, p);
}

} // namespace

// The arena, made the first time: at the start of the fixed range (else anywhere, in the game's virtual memory).
bool EnsureArena()
{
    if (g_Arena)
        return true;
    void* base = nullptr;
    if (EnsureFixed())
        base = VirtualAlloc(g_Fixed, kArenaSize, MEM_COMMIT, PAGE_READWRITE);
    if (!base) {
        base = VirtualAlloc(nullptr, kArenaSize, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (!base) {
            LOG_ERROR("Netplay pool: no room for its %u MiB", kArenaSize >> 20);
            return false;
        }
        std::lock_guard<std::mutex> track(g_TrackLock);
        g_VirtualBases.insert(base); // released when the game reboots
    }
    ++g_LayoutGeneration;
    g_Arena = static_cast<uint8_t*>(base);
    ArenaHeader& a = Arena();
    a.magic = kArenaMagic;
    a.top = (sizeof(ArenaHeader) + 31) & ~31u;
    LOG_INFO("Netplay pool: %u MiB at %p", kArenaSize >> 20, base);
    return true;
}

bool UseArena() { return g_Arena && (g_ArenaOn || g_DetAddresses); }

bool SetDeterministicPool(bool on)
{
    std::lock_guard<std::mutex> lock(g_PoolLock);
    if (on && !EnsureArena())
        return false;
    if (!on && g_Arena && g_ArenaOn) {
        // No rollback can come back now: the heap blocks the game freed meanwhile are freed.
        ArenaHeader& a = Arena();
        for (uint32_t i = 0; i < a.deferredCount; ++i)
            HeapFreeBlock(a.deferred[i]);
        if (a.deferredCount || a.deferredLost)
            LOG_INFO("Netplay pool: %u freed heap block(s) released at the session's end%s", a.deferredCount,
                a.deferredLost ? " (some were released at once: the list was full)" : "");
        a.deferredCount = a.deferredLost = 0;
    }
    g_ArenaOn = on && g_Arena;
    return !on || g_Arena;
}

void* XBAPI ExAllocatePoolWithTag(ULONG NumberOfBytes, ULONG Tag)
{
    (void)Tag;
    if (UseArena()) {
        std::lock_guard<std::mutex> lock(g_PoolLock);
        if (UseArena()) {
            if (void* p = ArenaAlloc(NumberOfBytes))
                return p;
            static bool logged = false;
            if (!logged)
                LOG_WARN("Netplay pool: full (%lu bytes asked); blocks from the heap, not deterministic", NumberOfBytes);
            logged = true;
        }
    }
    void* p = HeapAlloc(GetProcessHeap(), 0, NumberOfBytes);
    if (p) {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_PoolBlocks.insert(p);
        ++g_LayoutGeneration;
    }
    return p;
}

void XBAPI ExFreePool(void* P)
{
    if (InArena(P)) {
        std::lock_guard<std::mutex> lock(g_PoolLock);
        ArenaFree(P);
        return;
    }
    if (g_ArenaOn) {
        std::lock_guard<std::mutex> lock(g_PoolLock);
        ArenaHeader& a = Arena();
        if (a.deferredCount < kDeferred) {
            a.deferred[a.deferredCount++] = P;
            return;
        }
        ++a.deferredLost;
    }
    HeapFreeBlock(P);
}

void* GameAlloc(uint32_t size)
{
    if (g_DetAddresses) {
        std::lock_guard<std::mutex> lock(g_PoolLock);
        if (UseArena())
            if (void* p = ArenaAlloc(size)) {
                std::memset(p, 0, size);
                return p;
            }
    }
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void GameFree(void* p)
{
    if (!p)
        return;
    if (InArena(p)) {
        std::lock_guard<std::mutex> lock(g_PoolLock);
        ArenaFree(p);
        return;
    }
    HeapFree(GetProcessHeap(), 0, p);
}

ULONG XBAPI ExQueryPoolBlockSize(void* PoolBlock)
{
    if (InArena(PoolBlock))
        return (static_cast<BlockHeader*>(PoolBlock) - 1)->requested;
    return ULONG(HeapSize(GetProcessHeap(), 0, PoolBlock));
}

KERNEL_EXPORT(15, ExAllocatePoolWithTag);
KERNEL_EXPORT(17, ExFreePool);
KERNEL_EXPORT(23, ExQueryPoolBlockSize);

// ---------------------------------------------------------------------------
// Reboot: everything the game allocated goes, contiguous memory starts empty.
// ---------------------------------------------------------------------------
std::vector<std::pair<uintptr_t, size_t>> GameMemoryRegions()
{
    std::vector<std::pair<uintptr_t, size_t>> regions;
    {
        std::lock_guard<std::mutex> lock(g_ContigLock);
        for (const auto& [off, size] : g_ContigUsed) {
            // Only its committed, readable pages.
            auto* p = reinterpret_cast<uint8_t*>(uintptr_t(g_ContigBase) + off);
            auto* end = p + size;
            MEMORY_BASIC_INFORMATION info;
            while (p < end && VirtualQuery(p, &info, sizeof(info))) {
                const size_t left = size_t(end - p);
                const size_t span = std::min<size_t>(info.RegionSize - size_t(p - static_cast<uint8_t*>(info.BaseAddress)), left);
                if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                    regions.push_back({ uintptr_t(p), span });
                p += span;
            }
        }
    }
    if (g_Fixed) { // the fixed range's committed parts (the pool's arena, the game's placed memory)
        auto* p = g_Fixed;
        MEMORY_BASIC_INFORMATION info;
        while (p < g_Fixed + kFixedSize - kThreadZone && VirtualQuery(p, &info, sizeof(info)) &&
               info.AllocationBase == g_Fixed) {
            if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                regions.push_back({ uintptr_t(info.BaseAddress),
                    std::min<size_t>(info.RegionSize, size_t(g_Fixed + kFixedSize - kThreadZone - p)) });
            p += info.RegionSize;
        }
    }
    std::lock_guard<std::mutex> lock(g_TrackLock);
    for (void* base : g_VirtualBases) {
        auto* p = static_cast<uint8_t*>(base);
        MEMORY_BASIC_INFORMATION info;
        while (VirtualQuery(p, &info, sizeof(info)) && info.AllocationBase == base) {
            if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                regions.push_back({ uintptr_t(info.BaseAddress), info.RegionSize });
            p += info.RegionSize;
        }
    }
    return regions;
}

std::vector<std::pair<uintptr_t, size_t>> GamePoolBlocks()
{
    std::vector<std::pair<uintptr_t, size_t>> blocks;
    std::lock_guard<std::mutex> lock(g_TrackLock);
    for (void* block : g_PoolBlocks)
        blocks.push_back({ uintptr_t(block), HeapSize(GetProcessHeap(), 0, block) });
    return blocks;
}

uint32_t MemoryLayoutGeneration() { return g_LayoutGeneration; }

MemoryUsage QueryMemoryUsage()
{
    MemoryUsage usage;
    {
        std::lock_guard<std::mutex> lock(g_ContigLock);
        usage.contiguousSize = g_ContigSize;
        for (const auto& [off, size] : g_ContigUsed)
            usage.contiguousUsed += size;
    }
    std::lock_guard<std::mutex> lock(g_TrackLock);
    usage.poolBlocks = g_PoolBlocks.size();
    for (void* base : g_VirtualBases) {
        // The committed parts of each allocation (its regions share its allocation base).
        auto* p = static_cast<uint8_t*>(base);
        MEMORY_BASIC_INFORMATION info;
        while (VirtualQuery(p, &info, sizeof(info)) && info.AllocationBase == base) {
            if (info.State == MEM_COMMIT)
                usage.virtualCommitted += info.RegionSize;
            p += info.RegionSize;
        }
    }
    return usage;
}

void ResetMemoryForReboot()
{
    size_t bases = 0, blocks = 0;
    {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        for (void* base : g_VirtualBases)
            bases += VirtualFree(base, 0, MEM_RELEASE) ? 1 : 0;
        for (void* block : g_PoolBlocks)
            blocks += HeapFree(GetProcessHeap(), 0, block) ? 1 : 0;
        g_VirtualBases.clear();
        g_PoolBlocks.clear();
        g_ArenaOn = false;
        if (g_Arena && g_Arena != g_Fixed)
            g_Arena = nullptr; // it was one of the virtual bases
    }
    if (g_Fixed) { // the fixed range stays reserved: the game's placed memory goes, the arena starts empty
        std::lock_guard<std::mutex> lock(g_PlaceLock);
        for (const auto& [base, size] : g_Placed)
            VirtualFree(reinterpret_cast<void*>(base), size, MEM_DECOMMIT);
        g_Placed.clear();
        if (g_Arena == g_Fixed) {
            VirtualFree(g_Arena, kArenaSize, MEM_DECOMMIT);
            VirtualAlloc(g_Arena, kArenaSize, MEM_COMMIT, PAGE_READWRITE);
            ArenaHeader& a = Arena();
            a.magic = kArenaMagic;
            a.top = (sizeof(ArenaHeader) + 31) & ~31u;
        }
    }
    std::lock_guard<std::mutex> lock(g_ContigLock);
    uint64_t used = 0;
    for (auto& [off, size] : g_ContigUsed) {
        VirtualFree(reinterpret_cast<void*>(uintptr_t(g_ContigBase + off)), size, MEM_DECOMMIT);
        used += size;
    }
    g_ContigUsed.clear();
    g_ContigFree.clear();
    if (g_ContigSize)
        g_ContigFree[0] = g_ContigSize;
    LOG_INFO("Reboot: freed %zu memory regions, %zu pool blocks, %llu MiB contiguous memory", bases, blocks, used >> 20);
}

} // namespace swrots::kernel
