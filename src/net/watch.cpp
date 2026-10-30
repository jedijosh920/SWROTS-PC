// Netplay diagnostics: who writes a word of the game's memory (see watch.h).

#include "net/watch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"

namespace swrots::net {

namespace {

// A write seen: noted by the exception handler, on any thread, without a lock (a slot taken with an atomic index).
struct Hit {
    uint32_t slot;
    uint32_t thread;
    uint32_t eip;
    uint32_t value;
    uint32_t frame;
    uint32_t resim;
};
constexpr uint32_t kMaxHits = 8192;
Hit g_Hits[kMaxHits];
std::atomic<uint32_t> g_HitCount{ 0 };
std::atomic<uint32_t> g_HitsLost{ 0 };

uint32_t g_Addresses[kWatchSlots] = {};
int g_Count = 0;
std::atomic<bool> g_Watching{ false };
volatile uint32_t g_Frame = 0;
volatile uint32_t g_Resim = 0;
volatile uint32_t g_Suppress = 0;
void* g_Handler = nullptr;

LONG __stdcall OnSingleStep(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_Watching)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    const DWORD hits = c->Dr6 & 0xF;
    if (!hits)
        return EXCEPTION_CONTINUE_SEARCH; // a real single step (a debugger's)
    for (int i = 0; i < g_Count && !g_Suppress; ++i) {
        if (!(hits & (1u << i)))
            continue;
        const uint32_t n = g_HitCount.fetch_add(1);
        if (n >= kMaxHits) {
            ++g_HitsLost;
            continue;
        }
        Hit& h = g_Hits[n];
        h.slot = uint32_t(i);
        h.thread = GetCurrentThreadId();
        h.eip = c->Eip; // the instruction after the write (a data breakpoint traps after it)
        h.value = *reinterpret_cast<const volatile uint32_t*>(uintptr_t(g_Addresses[i]));
        h.frame = g_Frame;
        h.resim = g_Resim;
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Dr7: each slot enabled locally, on writes (RW 01), 4 bytes (LEN 11).
DWORD Dr7For(int count)
{
    DWORD dr7 = 0;
    for (int i = 0; i < count; ++i)
        dr7 |= (1u << (i * 2)) | (0x1u << (16 + i * 4)) | (0x3u << (18 + i * 4));
    return dr7;
}

// Sets the debug registers of every thread that runs game code (the caller's own excepted: a thread cannot set
// its own), on a helper thread so that the caller's are set too.
void ApplyToThreads(int count)
{
    const DWORD dr7 = Dr7For(count);
    struct Job {
        DWORD dr7;
        std::vector<DWORD> threads;
    } job{ dr7, {} };
    kernel::ForEachThread([](kernel::XboxThread* t, void* ctx) { static_cast<Job*>(ctx)->threads.push_back(t->threadId); },
        &job);
    std::thread helper([&job] {
        uint32_t set = 0;
        for (DWORD id : job.threads) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
            if (!h)
                continue;
            if (SuspendThread(h) != DWORD(-1)) {
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c)) {
                    c.Dr0 = g_Addresses[0];
                    c.Dr1 = g_Addresses[1];
                    c.Dr2 = g_Addresses[2];
                    c.Dr3 = g_Addresses[3];
                    c.Dr6 = 0;
                    c.Dr7 = job.dr7;
                    if (SetThreadContext(h, &c))
                        ++set;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        LOG_INFO("Netplay watch: debug registers set on %u of %zu game threads", set, job.threads.size());
    });
    helper.join();
}

} // namespace

bool WatchWords(const uint32_t* addresses, int count)
{
    if (g_Watching)
        WatchStopAndLog("replaced");
    count = std::clamp(count, 0, kWatchSlots);
    if (!count)
        return false;
    if (!g_Handler)
        g_Handler = AddVectoredExceptionHandler(1, OnSingleStep);
    if (!g_Handler)
        return false;
    for (int i = 0; i < kWatchSlots; ++i)
        g_Addresses[i] = i < count ? (addresses[i] & ~3u) : 0;
    g_Count = count;
    g_HitCount = 0;
    g_HitsLost = 0;
    g_Watching = true;
    ApplyToThreads(count);
    std::string list;
    for (int i = 0; i < count; ++i) {
        char word[16];
        std::snprintf(word, sizeof(word), " %08X", g_Addresses[i]);
        list += word;
    }
    LOG_INFO("Netplay watch: watching writes to%s", list.c_str());
    return true;
}

void WatchStopAndLog(const char* title)
{
    if (!g_Watching)
        return;
    g_Count = 0;
    ApplyToThreads(0);
    g_Watching = false;
    const uint32_t total = std::min(g_HitCount.load(), kMaxHits);
    LOG_INFO("Netplay watch (%s): %u write(s) seen%s", title, g_HitCount.load(),
        g_HitsLost ? " (the first ones kept)" : "");
    // Per word: each (thread, instruction) that wrote it, how often in frames shown and in frames played again,
    // and the frames and values of its first writes.
    for (int slot = 0; slot < kWatchSlots && g_Addresses[slot]; ++slot) {
        struct Writer {
            uint32_t thread, eip, shown, again, firstFrame, firstValue;
        };
        std::vector<Writer> writers;
        for (uint32_t i = 0; i < total; ++i) {
            const Hit& h = g_Hits[i];
            if (h.slot != uint32_t(slot))
                continue;
            auto it = std::find_if(writers.begin(), writers.end(),
                [&](const Writer& w) { return w.thread == h.thread && w.eip == h.eip; });
            if (it == writers.end()) {
                writers.push_back({ h.thread, h.eip, 0, 0, h.frame, h.value });
                it = writers.end() - 1;
            }
            (h.resim ? it->again : it->shown)++;
        }
        LOG_INFO("Netplay watch:   %08X: %zu writer(s)", g_Addresses[slot], writers.size());
        for (const Writer& w : writers)
            LOG_INFO("Netplay watch:     thread %5u, after %08X: %u in frames shown, %u in frames played again "
                     "(first at frame %u: %08X)",
                w.thread, w.eip, w.shown, w.again, w.firstFrame, w.firstValue);
    }
    for (int i = 0; i < kWatchSlots; ++i)
        g_Addresses[i] = 0;
}

bool Watching() { return g_Watching; }

void WatchSuppress(bool on) { g_Suppress = on ? 1 : 0; }

void WatchSetFrame(uint32_t frame, bool resimulated)
{
    g_Frame = frame;
    g_Resim = resimulated ? 1 : 0;
}

} // namespace swrots::net
