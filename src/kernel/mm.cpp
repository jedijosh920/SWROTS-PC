// Memory: virtual memory, contiguous ("physical") memory and pool.

#include <windows.h>

#include <algorithm>
#include <atomic>
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
// System memory and protection
// ---------------------------------------------------------------------------
void* XBAPI MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
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
NTSTATUS XBAPI NtAllocateVirtualMemory(void** BaseAddress, ULONG_PTR ZeroBits, ULONG* AllocationSize,
    ULONG AllocationType, ULONG Protect)
{
    SIZE_T size = *AllocationSize;
    const ULONG type = AllocationType & ~kMemNoZero;
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
    *AllocationSize = ULONG(size);
    if (status < 0)
        LOG_WARN("NtAllocateVirtualMemory(%p, %lu, %08lX) -> %08lX", *BaseAddress, size, AllocationType, status);
    else if (AllocationType & MEM_RESERVE) {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_VirtualBases.insert(*BaseAddress);
    }
    if (status >= 0)
        ++g_LayoutGeneration; // a reservation, or pages committed in one
    return status;
}

NTSTATUS XBAPI NtFreeVirtualMemory(void** BaseAddress, ULONG* FreeSize, ULONG FreeType)
{
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
    return ::NtQueryVirtualMemory(GetCurrentProcess(), BaseAddress, 0 /*MemoryBasicInformation*/, MemoryInformation,
        sizeof(MEMORY_BASIC_INFORMATION), nullptr);
}

KERNEL_EXPORT(184, NtAllocateVirtualMemory);
KERNEL_EXPORT(199, NtFreeVirtualMemory);
KERNEL_EXPORT(217, NtQueryVirtualMemory);

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------
void* XBAPI ExAllocatePoolWithTag(ULONG NumberOfBytes, ULONG Tag)
{
    (void)Tag;
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
    {
        std::lock_guard<std::mutex> lock(g_TrackLock);
        g_PoolBlocks.erase(P);
        ++g_LayoutGeneration;
    }
    HeapFree(GetProcessHeap(), 0, P);
}
ULONG XBAPI ExQueryPoolBlockSize(void* PoolBlock) { return ULONG(HeapSize(GetProcessHeap(), 0, PoolBlock)); }

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
