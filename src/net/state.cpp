// Netplay's snapshots of the game (see state.h).

#include "net/state.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/xbe.h"
#include "game/game.h"
#include "kernel/kernel.h"

namespace swrots::net {

namespace {

constexpr uint32_t kPage = 0x1000;

// A stretch of memory the snapshots cover, cut in pages (chunks of kPage from its start; the last may be shorter).
struct Region {
    uint8_t* base;
    uint32_t size;
    uint32_t firstPage; // its first page's index among all regions' pages
    bool watched;       // Windows' write watch tells which pages were written; else they are compared
    char name[40];
};

// A page's slot in the journal: which page, its hash and content before the frame changed it.
struct JournalSlot {
    uint32_t page;
    uint64_t oldHash;
};

// A kept frame: its journal (the pages the frame's start differs in from the previous frame's, as they were
// before), its checksum and the main thread's stack at its boundary.
struct FrameRecord {
    uint32_t frame;
    uint32_t firstSlot; // in the journal ring
    uint32_t slots;
    uint64_t checksum;
    uint32_t stackBytes;
    uint8_t* stack; // a buffer of config.stackKiB
};

struct HostState {
    const char* name;
    void* data;
    uint32_t size;
};

struct Exclusion {
    uintptr_t begin, end;
    const char* why;
};

struct Session {
    bool armed = false;
    StateConfig config;
    uint32_t generation = 0;
    const uint8_t* frameStack = nullptr;
    const uint8_t* stackBase = nullptr;

    std::vector<Region> regions;
    uint32_t pages = 0;
    uint8_t* shadow = nullptr;        // pages x kPage
    std::vector<uint64_t> hashes;     // per page
    std::vector<uint8_t> excluded;    // per page: left out of the checksum
    uint64_t checksum = 0;            // the sum of the pages' mixed hashes

    // the journal: a ring of page slots, their contents alongside
    std::vector<JournalSlot> slots;
    uint8_t* slotData = nullptr;      // slots x kPage
    uint32_t slotHead = 0;            // where the next slot goes
    uint32_t slotsUsed = 0;

    // kept frames: a ring of window + 1 records, oldest .. latest
    std::vector<FrameRecord> records;
    uint32_t firstRecord = 0, recordCount = 0;

    // scratch for GetWriteWatch: the most pages a watched region has
    std::vector<void*> written;
};

Session g_S;
std::vector<HostState> g_HostStates;
std::vector<Exclusion> g_Exclusions;
StateCosts g_Costs;
const char* g_LastError = "";

// --- hashing ---------------------------------------------------------------------------------------------------

inline uint64_t Rotl(uint64_t v, int s) { return (v << s) | (v >> (64 - s)); }

uint64_t HashBytes(const uint8_t* p, uint32_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = Rotl(h ^ (w * 0xC2B2AE3D27D4EB4Full), 31) * 0x9E3779B97F4A7C15ull;
    }
    for (; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 29);
}

// A page's share of the checksum: its hash mixed with its index, so equal pages at two places do not cancel.
inline uint64_t Mix(uint64_t hash, uint32_t page)
{
    uint64_t v = hash ^ (uint64_t(page) * 0xD6E8FEB86659FD93ull);
    v = (v ^ (v >> 32)) * 0xD6E8FEB86659FD93ull;
    return v ^ (v >> 32);
}

// --- pages -----------------------------------------------------------------------------------------------------

const Region& RegionOfPage(uint32_t page)
{
    auto it = std::upper_bound(g_S.regions.begin(), g_S.regions.end(), page,
        [](uint32_t p, const Region& r) { return p < r.firstPage; });
    return *(it - 1);
}

inline uint8_t* PageAddress(const Region& r, uint32_t page) { return r.base + (page - r.firstPage) * kPage; }

inline uint32_t PageBytes(const Region& r, uint32_t page)
{
    const uint32_t offset = (page - r.firstPage) * kPage;
    return std::min(kPage, r.size - offset);
}

inline uint8_t* ShadowOf(uint32_t page) { return g_S.shadow + size_t(page) * kPage; }

// The page's hash becomes `hash`, the checksum follows.
inline void SetHash(uint32_t page, uint64_t hash)
{
    if (!g_S.excluded[page]) {
        g_S.checksum -= Mix(g_S.hashes[page], page);
        g_S.checksum += Mix(hash, page);
    }
    g_S.hashes[page] = hash;
}

// Calls `fn(page)` for every page of every region that may differ from its shadow: the written ones of watched
// regions (their write watch reset), all of the others.
template <typename Fn> void ForEachCandidatePage(Fn&& fn)
{
    for (const Region& r : g_S.regions) {
        const uint32_t count = (r.size + kPage - 1) / kPage;
        if (!r.watched) {
            for (uint32_t i = 0; i < count; ++i)
                fn(r, r.firstPage + i);
            continue;
        }
        ULONG_PTR found = g_S.written.size();
        ULONG granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, r.base, r.size, g_S.written.data(), &found, &granularity) != 0) {
            for (uint32_t i = 0; i < count; ++i) // the watch failed: look at every page
                fn(r, r.firstPage + i);
            continue;
        }
        for (ULONG_PTR i = 0; i < found; ++i)
            fn(r, r.firstPage + uint32_t((static_cast<uint8_t*>(g_S.written[i]) - r.base) / kPage));
    }
}

void ResetWatches()
{
    for (const Region& r : g_S.regions)
        if (r.watched)
            ResetWriteWatch(r.base, r.size);
}

// --- the journal and the frame records -------------------------------------------------------------------------

uint32_t SlotCapacity() { return uint32_t(g_S.slots.size()); }

FrameRecord& RecordAt(uint32_t i) { return g_S.records[(g_S.firstRecord + i) % g_S.records.size()]; }
FrameRecord& LatestRecord() { return RecordAt(g_S.recordCount - 1); }

// Forgets the oldest kept frame and its journal.
void DropOldestRecord()
{
    FrameRecord& oldest = RecordAt(0);
    g_S.slotsUsed -= oldest.slots;
    g_S.firstRecord = (g_S.firstRecord + 1) % g_S.records.size();
    --g_S.recordCount;
}

// A new kept frame at the end (the oldest forgotten when the ring is full).
FrameRecord& PushRecord(uint32_t frame)
{
    if (g_S.recordCount == g_S.records.size())
        DropOldestRecord();
    ++g_S.recordCount;
    FrameRecord& r = LatestRecord();
    r.frame = frame;
    r.firstSlot = g_S.slotHead;
    r.slots = 0;
    r.checksum = 0;
    r.stackBytes = 0;
    return r;
}

// A slot for the latest record's journal, the oldest frames forgotten to make room; null when even that does not
// (the frame alone changed more than the journal holds).
JournalSlot* TakeSlot(uint8_t*& data)
{
    while (g_S.slotsUsed == SlotCapacity() && g_S.recordCount > 1)
        DropOldestRecord();
    if (g_S.slotsUsed == SlotCapacity())
        return nullptr;
    const uint32_t index = g_S.slotHead;
    g_S.slotHead = (g_S.slotHead + 1) % SlotCapacity();
    ++g_S.slotsUsed;
    ++LatestRecord().slots;
    data = g_S.slotData + size_t(index) * kPage;
    return &g_S.slots[index];
}

// --- what to cover ---------------------------------------------------------------------------------------------

void AddRegion(uint8_t* base, uint32_t size, bool watched, const char* name)
{
    if (!size)
        return;
    Region r = {};
    r.base = base;
    r.size = size;
    r.watched = watched;
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    g_S.regions.push_back(r);
}

// Whether Windows watches writes to the region (it was allocated with MEM_WRITE_WATCH).
bool CanWatch(void* base, uint32_t size)
{
    void* page = nullptr;
    ULONG_PTR count = 1;
    ULONG granularity = 0;
    return GetWriteWatch(0, base, size, &page, &count, &granularity) == 0;
}

void FindRegions()
{
    // The game's allocations: contiguous memory, virtual and system memory.
    for (const auto& [base, size] : kernel::GameMemoryRegions()) {
        auto* p = reinterpret_cast<uint8_t*>(base);
        const bool contiguous = base >= 0x80000000u;
        AddRegion(p, uint32_t(size), CanWatch(p, uint32_t(size)), contiguous ? "contiguous" : "virtual");
    }
    // The image's writable data (its .data and .bss); its code is the port's to patch, and is not state.
    const auto* header = reinterpret_cast<const XbeHeader*>(uintptr_t(game::kImageBase));
    const auto* sections = reinterpret_cast<const XbeSectionHeader*>(uintptr_t(header->SectionHeadersAddress));
    for (uint32_t i = 0; i < header->NumberOfSections; ++i) {
        const XbeSectionHeader& s = sections[i];
        if (!(s.Flags & kXbeSectionWritable) || (s.Flags & kXbeSectionExecutable))
            continue;
        char name[40];
        std::snprintf(name, sizeof(name), "image %s",
            s.SectionNameAddress ? reinterpret_cast<const char*>(uintptr_t(s.SectionNameAddress)) : "?");
        const uint32_t begin = s.VirtualAddress & ~(kPage - 1);
        const uint32_t end = (s.VirtualAddress + s.VirtualSize + kPage - 1) & ~(kPage - 1);
        AddRegion(reinterpret_cast<uint8_t*>(uintptr_t(begin)), end - begin, false, name);
    }
    // Pool blocks (on the PC's heap).
    for (const auto& [base, size] : kernel::GamePoolBlocks())
        AddRegion(reinterpret_cast<uint8_t*>(base), uint32_t(size), false, "pool");
    // The main thread's Xbox state: its thread object and processor region, and its thread-local storage.
    if (kernel::XboxThread* t = kernel::CurrentThread()) {
        AddRegion(reinterpret_cast<uint8_t*>(t), uint32_t(offsetof(kernel::XboxThread, hostHandle)), false, "main thread");
        AddRegion(t->tlsData, t->tlsSize, false, "main thread TLS");
    }
    for (const HostState& h : g_HostStates)
        AddRegion(static_cast<uint8_t*>(h.data), h.size, false, h.name);

    std::sort(g_S.regions.begin(), g_S.regions.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
    // Overlaps (a host state inside another region) would be kept twice and restored in an unknown order.
    for (size_t i = 1; i < g_S.regions.size(); ++i) {
        Region& prev = g_S.regions[i - 1];
        Region& r = g_S.regions[i];
        if (prev.base + prev.size > r.base) {
            LOG_WARN("Netplay: %s (%p) overlaps %s (%p); kept once", r.name, r.base, prev.name, prev.base);
            const uint8_t* end = std::max(prev.base + prev.size, r.base + r.size);
            prev.size = uint32_t(end - prev.base);
            prev.watched = prev.watched && r.watched;
            g_S.regions.erase(g_S.regions.begin() + i--);
        }
    }
    uint32_t page = 0;
    size_t mostWatched = 0;
    for (Region& r : g_S.regions) {
        r.firstPage = page;
        const uint32_t count = (r.size + kPage - 1) / kPage;
        page += count;
        if (r.watched)
            mostWatched = std::max<size_t>(mostWatched, count);
    }
    g_S.pages = page;
    g_S.written.assign(std::max<size_t>(mostWatched, 1), nullptr);
}

void ApplyExclusions()
{
    g_S.excluded.assign(g_S.pages, 0);
    for (const Exclusion& e : g_Exclusions) {
        for (const Region& r : g_S.regions) {
            const uintptr_t begin = std::max(e.begin, uintptr_t(r.base));
            const uintptr_t end = std::min(e.end, uintptr_t(r.base) + r.size);
            if (begin >= end)
                continue;
            const uint32_t first = r.firstPage + uint32_t((begin - uintptr_t(r.base)) / kPage);
            const uint32_t last = r.firstPage + uint32_t((end - 1 - uintptr_t(r.base)) / kPage);
            for (uint32_t p = first; p <= last; ++p)
                g_S.excluded[p] = 1;
        }
    }
}

double MsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void FreeSession()
{
    if (g_S.shadow)
        VirtualFree(g_S.shadow, 0, MEM_RELEASE);
    if (g_S.slotData)
        VirtualFree(g_S.slotData, 0, MEM_RELEASE);
    for (FrameRecord& r : g_S.records)
        delete[] r.stack;
    g_S = Session();
}

bool SaveStack(FrameRecord& record, const void* frameStack)
{
    if (frameStack != g_S.frameStack) {
        g_LastError = "the frame boundary is not where it was (the game's stack differs)";
        return false;
    }
    record.stackBytes = uint32_t(g_S.stackBase - g_S.frameStack);
    std::memcpy(record.stack, g_S.frameStack, record.stackBytes);
    return true;
}

} // namespace

// --- the session -----------------------------------------------------------------------------------------------

bool StateArm(const StateConfig& config, const void* frameStack, uint32_t frame, uint64_t& checksum)
{
    StateDisarm();
    const auto start = std::chrono::steady_clock::now();
    g_S.config = config;
    g_S.generation = kernel::MemoryLayoutGeneration();
    g_S.frameStack = static_cast<const uint8_t*>(frameStack);
    g_S.stackBase = static_cast<const uint8_t*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    if (g_S.frameStack >= g_S.stackBase || uint32_t(g_S.stackBase - g_S.frameStack) > config.stackKiB * 1024u) {
        LOG_ERROR("Netplay: the game's stack at the frame boundary (%u bytes) does not fit the %u KiB kept",
            uint32_t(g_S.stackBase - g_S.frameStack), config.stackKiB);
        FreeSession();
        return false;
    }
    FindRegions();
    ApplyExclusions();

    // The shadow copy: every page as it is now.
    g_S.shadow = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(g_S.pages) * kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const uint32_t slots = config.journalMiB * (0x100000 / kPage);
    g_S.slotData = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(slots) * kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_S.shadow || !g_S.slotData) {
        LOG_ERROR("Netplay: no room for the snapshots (%u MiB of shadow, %u MiB of journal)", g_S.pages / 256,
            config.journalMiB);
        FreeSession();
        return false;
    }
    g_S.slots.assign(slots, JournalSlot{});
    g_S.records.resize(config.window + 1);
    for (FrameRecord& r : g_S.records)
        r.stack = new uint8_t[config.stackKiB * 1024u];

    g_S.hashes.assign(g_S.pages, 0);
    g_S.checksum = 0;
    uint64_t watched = 0, tracked = 0;
    for (const Region& r : g_S.regions) {
        tracked += r.size;
        if (r.watched)
            watched += r.size;
        const uint32_t count = (r.size + kPage - 1) / kPage;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t page = r.firstPage + i;
            const uint32_t bytes = PageBytes(r, page);
            std::memcpy(ShadowOf(page), PageAddress(r, page), bytes);
            SetHash(page, HashBytes(PageAddress(r, page), bytes));
        }
    }
    ResetWatches();

    g_S.armed = true;
    FrameRecord& record = PushRecord(frame);
    if (!SaveStack(record, frameStack)) {
        FreeSession();
        return false;
    }
    record.checksum = checksum = g_S.checksum;

    g_Costs = StateCosts();
    g_Costs.regions = uint32_t(g_S.regions.size());
    g_Costs.trackedBytes = tracked;
    g_Costs.watchedBytes = watched;
    LOG_INFO("Netplay: snapshots of %u regions, %.1f MiB (%.1f MiB write-watched, %.1f MiB compared), %u frames "
             "kept, %u MiB of journal, %u bytes of stack; armed in %.1f ms",
        g_Costs.regions, tracked / 1048576.0, watched / 1048576.0, (tracked - watched) / 1048576.0,
        config.window, config.journalMiB, record.stackBytes, MsSince(start));
    for (const Region& r : g_S.regions)
        if (r.size >= 0x100000)
            LOG_DEBUG("Netplay:   %-24s %p %8u KiB%s", r.name, r.base, r.size / 1024, r.watched ? " (watched)" : "");
    return true;
}

void StateDisarm()
{
    if (g_S.armed || g_S.shadow || g_S.slotData)
        FreeSession();
}

bool StateArmed() { return g_S.armed; }
uint32_t StateLayoutGeneration() { return g_S.generation; }

bool StateSave(uint32_t frame, const void* frameStack, uint64_t& checksum)
{
    if (!g_S.armed) {
        g_LastError = "no session";
        return false;
    }
    if (frame != LatestRecord().frame + 1) {
        g_LastError = "frames must be saved in order";
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    FrameRecord& record = PushRecord(frame);
    bool full = false;
    ForEachCandidatePage([&](const Region& r, uint32_t page) {
        if (full)
            return;
        const uint8_t* now = PageAddress(r, page);
        uint8_t* shadow = ShadowOf(page);
        const uint32_t bytes = PageBytes(r, page);
        if (std::memcmp(now, shadow, bytes) == 0)
            return; // written with what it held
        uint8_t* data = nullptr;
        JournalSlot* slot = TakeSlot(data);
        if (!slot) {
            full = true;
            return;
        }
        slot->page = page;
        slot->oldHash = g_S.hashes[page];
        std::memcpy(data, shadow, bytes);
        std::memcpy(shadow, now, bytes);
        SetHash(page, HashBytes(now, bytes));
    });
    if (full) {
        // The frame cannot be kept: the pages taken so far are in the shadow already, so the session is no longer
        // consistent. The caller arms a new one.
        g_LastError = "the journal is full (one frame changed more memory than it holds)";
        g_S.armed = false;
        return false;
    }
    if (!SaveStack(record, frameStack)) {
        g_S.armed = false;
        return false;
    }
    record.checksum = checksum = g_S.checksum;
    g_Costs.saveMs = MsSince(start);
    g_Costs.savedPages = record.slots;
    g_Costs.journalPages = g_S.slotsUsed;
    return true;
}

bool StateLoad(uint32_t frame, const void* frameStack)
{
    if (!g_S.armed) {
        g_LastError = "no session";
        return false;
    }
    if (g_S.recordCount == 0 || frame < RecordAt(0).frame || frame > LatestRecord().frame) {
        g_LastError = "the frame is not kept";
        return false;
    }
    if (frameStack != g_S.frameStack) {
        g_LastError = "the frame boundary is not where it was (the game's stack differs)";
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    uint32_t loaded = 0;
    // What changed since the last save goes back to the shadow (the last saved frame's start).
    ForEachCandidatePage([&](const Region& r, uint32_t page) {
        uint8_t* now = PageAddress(r, page);
        const uint8_t* shadow = ShadowOf(page);
        const uint32_t bytes = PageBytes(r, page);
        if (std::memcmp(now, shadow, bytes) != 0) {
            std::memcpy(now, shadow, bytes);
            ++loaded;
        }
    });
    // Then the journal, newest first, down to the frame asked for.
    while (LatestRecord().frame > frame) {
        FrameRecord& record = LatestRecord();
        for (uint32_t i = record.slots; i-- > 0;) {
            const uint32_t index = (record.firstSlot + i) % SlotCapacity();
            const JournalSlot& slot = g_S.slots[index];
            const uint8_t* data = g_S.slotData + size_t(index) * kPage;
            const Region& r = RegionOfPage(slot.page);
            const uint32_t bytes = PageBytes(r, slot.page);
            std::memcpy(PageAddress(r, slot.page), data, bytes);
            std::memcpy(ShadowOf(slot.page), data, bytes);
            SetHash(slot.page, slot.oldHash);
            ++loaded;
        }
        g_S.slotHead = record.firstSlot;
        g_S.slotsUsed -= record.slots;
        --g_S.recordCount;
    }
    ResetWatches();
    FrameRecord& record = LatestRecord();
    std::memcpy(const_cast<uint8_t*>(g_S.frameStack), record.stack, record.stackBytes);
    g_Costs.loadMs = MsSince(start);
    g_Costs.loadedPages = loaded;
    g_Costs.journalPages = g_S.slotsUsed;
    if (g_S.checksum != record.checksum) {
        g_LastError = "the loaded frame's checksum is not the one saved";
        return false;
    }
    return true;
}

uint32_t StateOldestFrame() { return g_S.recordCount ? RecordAt(0).frame : 0; }
uint32_t StateLatestFrame() { return g_S.recordCount ? LatestRecord().frame : 0; }

uint64_t StateChecksum(uint32_t frame)
{
    for (uint32_t i = 0; i < g_S.recordCount; ++i)
        if (RecordAt(i).frame == frame)
            return RecordAt(i).checksum;
    return 0;
}

const char* StateLastError() { return g_LastError; }

void StateCopyPageHashes(std::vector<uint64_t>& out) { out = g_S.hashes; }

std::string StateDescribeDifferences(const std::vector<uint64_t>& before, size_t max)
{
    std::string text;
    size_t found = 0;
    for (uint32_t page = 0; page < g_S.pages && page < before.size(); ++page) {
        if (g_S.excluded[page] || before[page] == g_S.hashes[page])
            continue;
        if (found++ < max) {
            const Region& r = RegionOfPage(page);
            char line[128];
            std::snprintf(line, sizeof(line), "\n  %-24s %p (+0x%X)", r.name, PageAddress(r, page),
                uint32_t(PageAddress(r, page) - r.base));
            text += line;
        }
    }
    if (found > max) {
        char line[64];
        std::snprintf(line, sizeof(line), "\n  ... %zu pages in all", found);
        text += line;
    }
    return text;
}

void RegisterHostState(const char* name, void* data, uint32_t size) { g_HostStates.push_back({ name, data, size }); }

void ExcludeFromChecksum(const void* data, uint32_t size, const char* why)
{
    g_Exclusions.push_back({ uintptr_t(data), uintptr_t(data) + size, why });
    if (g_S.armed)
        LOG_WARN("Netplay: %s left out of the checksums from the next session on", why);
}

const StateCosts& StateCurrentCosts() { return g_Costs; }

} // namespace swrots::net
