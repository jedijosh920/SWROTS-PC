// The deterministic scheduler (see sched.h).

#include "kernel/sched.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "kernel/clock.h"
#include "kernel/kernel.h"
#include "kernel/reboot.h"

namespace swrots::kernel {

// ke.cpp: timers and DPCs, run by the scheduler while it runs.
uint64_t NextTimerDueMs();
bool RunDueTimersAndDpcs(uint64_t nowMs);
// reboot.cpp
HANDLE RebootEvent();

namespace {

enum class State { Ready, Waiting, Running, Suspended };

struct Pending {
    ApcFunction routine;
    void* context;
    void* ioStatus;
};

struct SThread {
    XboxThread* thread = nullptr;
    DWORD id = 0;
    HANDLE go = nullptr; // set when it is its turn
    State state = State::Ready;
    uint32_t suspends = 0; // NtSuspendThread's count, kept here (the thread takes no turn while it is not 0)
    // its wait
    DWORD count = 0;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
    BOOL waitAll = FALSE;
    BOOL alertable = FALSE;
    uint64_t deadline = ~0ull; // fixed clock, 100 ns
    DWORD result = 0;
    std::vector<Pending> apcs;
};

bool g_Requested = false;
std::atomic<bool> g_Active{ false };
std::mutex g_Lock;
std::vector<SThread*> g_Threads; // in the order they were made: the order of turns
SThread* g_Current = nullptr;
thread_local SThread* t_Self = nullptr;

struct Stats {
    uint64_t switches = 0, waits = 0, polls = 0, clockJumps = 0, external = 0, timers = 0;
    uint64_t trace = 0x84222325CBF29CE4ull; // a running hash of the turns (which thread, at what time)
} g_Stats;

// Each turn into the trace; every 2048 turns a log line, so that two runs' logs show where they part.
void Trace(SThread* next, uint64_t now)
{
    uint32_t index = 0;
    for (uint32_t i = 0; i < g_Threads.size(); ++i)
        if (g_Threads[i] == next)
            index = i;
    g_Stats.trace = (g_Stats.trace ^ (uint64_t(index) << 48) ^ now) * 0x100000001B3ull;
    if (g_Stats.switches % 2048 == 0)
        LOG_INFO("Scheduler trace: turn %llu, clock %llu, %zu threads, hash %016llX, %llu waits only the PC ended",
            g_Stats.switches, now, g_Threads.size(), g_Stats.trace, g_Stats.external);
}

constexpr uint64_t kUnitsPerMs = 10000;

SThread* FindByThread(XboxThread* t)
{
    for (SThread* s : g_Threads)
        if (s->thread == t)
            return s;
    return nullptr;
}

// Whether `s` may run now (its wait is over: then its result is set). Polling takes what it waited for (an
// auto-reset event, a semaphore's count), as the wait itself would.
bool Runnable(SThread* s, uint64_t now)
{
    if (s->suspends)
        return false;
    if (s->state == State::Ready)
        return true;
    if (s->state != State::Waiting)
        return false;
    if (s->alertable && !s->apcs.empty()) {
        s->result = WAIT_IO_COMPLETION;
        return true;
    }
    if (s->count) {
        ++g_Stats.polls;
        const DWORD r = WaitForMultipleObjectsEx(s->count, s->handles, s->waitAll, 0, FALSE);
        if (r != WAIT_TIMEOUT) {
            s->result = r;
            return true;
        }
    }
    if (s->deadline <= now) {
        s->result = WAIT_TIMEOUT;
        return true;
    }
    return false;
}

// The next thread to run after `from` (in turn order, `from` last), with g_Lock held. Runs timers and DPCs first;
// when no thread can run, moves the clock on to the earliest timeout or timer, or, when nothing would ever end the
// waits, waits for the PC (an event set by a host thread: not deterministic, counted).
SThread* PickNext(SThread* from)
{
    for (;;) {
        const uint64_t now = FixedClockPeek();
        if (RunDueTimersAndDpcs(now / kUnitsPerMs))
            ++g_Stats.timers;
        const size_t n = g_Threads.size();
        size_t start = 0;
        for (size_t i = 0; i < n; ++i)
            if (g_Threads[i] == from)
                start = i + 1;
        for (size_t k = 0; k < n; ++k) {
            SThread* s = g_Threads[(start + k) % n];
            if (Runnable(s, now))
                return s;
        }
        if (Rebooting())
            return nullptr;
        // Nobody can run: the clock moves on to the earliest wait's end or timer.
        uint64_t next = NextTimerDueMs();
        next = next == ~0ull ? ~0ull : next * kUnitsPerMs;
        for (SThread* s : g_Threads)
            if (s->state == State::Waiting)
                next = std::min(next, s->deadline);
        if (next != ~0ull) {
            FixedClockAdvanceTo(std::max(next, now + 1));
            ++g_Stats.clockJumps;
            continue;
        }
        // Waits only the PC can end (an event a host thread sets): wait for any of them, a little at a time.
        ++g_Stats.external;
        if (g_Stats.external <= 20)
            LOG_WARN("Scheduler: every game thread waits on something only the PC can end (not deterministic)");
        std::vector<HANDLE> all;
        for (SThread* s : g_Threads)
            if (s->state == State::Waiting)
                for (DWORD i = 0; i < s->count && all.size() < MAXIMUM_WAIT_OBJECTS - 1; ++i)
                    all.push_back(s->handles[i]);
        all.push_back(RebootEvent());
        g_Lock.unlock();
        WaitForMultipleObjects(DWORD(all.size()), all.data(), FALSE, 10);
        g_Lock.lock();
    }
}

// Hands the baton from `self` (state already set) to the next thread, and waits for self's turn. Returns with self
// running again, or exits the thread when the game reboots.
void SwitchAway(SThread* self, std::unique_lock<std::mutex>& lock)
{
    SThread* next = PickNext(self);
    if (!next) {
        lock.unlock();
        ExitGameThread();
    }
    next->state = State::Running;
    g_Current = next;
    if (next == self)
        return;
    ++g_Stats.switches;
    Trace(next, FixedClockPeek());
    SetEvent(next->go);
    lock.unlock();
    HANDLE wait[2] = { self->go, RebootEvent() };
    const DWORD r = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0)
        ExitGameThread();
    lock.lock();
}

// Zeroes up to `bytes` of the stack below the caller's (down to the TEB's limit): the game reads uninitialised locals,
// which would otherwise hold what the PC's wait functions (or the port's code) left there, which differs from run to
// run and from PC to PC.
__declspec(naked) void __stdcall ScrubBelow(uint32_t bytes)
{
    __asm {
        push edi
        mov ecx, [esp + 8]
        lea edi, [esp - 4]
        mov eax, edi
        sub eax, dword ptr fs:[8]
        cmp ecx, eax
        jbe size_ok
        mov ecx, eax
    size_ok:
        shr ecx, 2
        xor eax, eax
        std
        rep stosd
        cld
        pop edi
        ret 4
    }
}

constexpr uint32_t kScrubBytes = 0x10000;

void RunApcs(SThread* self)
{
    std::vector<Pending> apcs;
    apcs.swap(self->apcs);
    for (const Pending& p : apcs)
        p.routine(p.context, p.ioStatus, 0);
}

} // namespace

void SetDeterministicBoot(bool on)
{
    if (on != g_Requested)
        LOG_INFO("Scheduler: deterministic from the next start of the game: %s", on ? "on" : "off");
    g_Requested = on;
}

bool DeterministicBootRequested() { return g_Requested; }
bool SchedActive() { return g_Active; }
bool SchedHolding() { return g_Active && t_Self && g_Current == t_Self; }

static uint32_t ReadWordSafely(uint32_t address)
{
    __try {
        return *reinterpret_cast<const uint32_t*>(uintptr_t(address));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Diagnostics: a turn that lasts (the thread holding the baton never hands it over: it spins, or waits on something the
// scheduler does not see) is logged with where that thread is, every 5 s of it.
void StallWatch()
{
    uint64_t lastSwitches = ~0ull;
    int still = 0;
    for (;;) {
        Sleep(1000);
        if (!g_Active) {
            still = 0;
            continue;
        }
        uint64_t switches;
        DWORD holder = 0;
        uint32_t index = 0;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_Lock);
            switches = g_Stats.switches;
            holder = g_Current ? g_Current->id : 0;
            count = g_Threads.size();
            for (uint32_t i = 0; i < count; ++i)
                if (g_Threads[i] == g_Current)
                    index = i;
        }
        still = switches == lastSwitches ? still + 1 : 0;
        lastSwitches = switches;
        if (still == 0 || still % 5 != 0 || !holder)
            continue;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, holder);
        if (h && SuspendThread(h) != DWORD(-1)) {
            GetThreadContext(h, &c);
            uint32_t stack[12] = {};
            for (int i = 0; i < 12; ++i)
                stack[i] = ReadWordSafely(c.Esp + 4 * i);
            ResumeThread(h);
            LOG_WARN("Scheduler: thread %u (host %lu) has held its turn for %d s (turn %llu, %zu threads): eip %08lX esp "
                     "%08lX, stack %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                index, holder, still, switches, count, c.Eip, c.Esp, stack[0], stack[1], stack[2], stack[3], stack[4],
                stack[5], stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
        }
        if (h)
            CloseHandle(h);
    }
}

void SchedBoot()
{
    SchedReset();
    g_Active = g_Requested;
    static bool watching = false;
    if (g_Active && !watching) {
        watching = true;
        std::thread(StallWatch).detach();
    }
    if (g_Active) {
        g_Stats = Stats();
        SetFixedClock(false); // from its origin at every start: the same on every PC
        SetFixedClock(true, 0, true); // every scheduled thread's reads move it on
        LOG_INFO("Scheduler: on (the game's threads take turns; the fixed clock from the start)");
    }
}

void SchedReset()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    if (g_Active)
        SchedLogStats();
    for (SThread* s : g_Threads) {
        if (s->go)
            CloseHandle(s->go);
        delete s;
    }
    g_Threads.clear();
    g_Current = nullptr;
    if (g_Active && !g_Requested) {
        g_Active = false;
        SetFixedClock(false);
    }
}

void SchedAddThread(XboxThread* t, bool suspended)
{
    if (!g_Active)
        return;
    std::lock_guard<std::mutex> lock(g_Lock);
    auto* s = new SThread();
    s->thread = t;
    s->go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->state = suspended ? State::Suspended : State::Ready;
    g_Threads.push_back(s);
    LOG_INFO("Scheduler: thread %zu made%s at clock %llu, after %llu turns", g_Threads.size() - 1,
        suspended ? " suspended" : "", FixedClockPeek(), g_Stats.switches);
}

// A started thread's suspension, kept by the scheduler rather than Windows: a thread suspended by Windows while it
// holds the turn (a thread suspending itself, as the movie player's do) would keep it forever.
static SThread* StartedThread(HANDLE thread)
{
    const DWORD id = GetThreadId(thread);
    for (SThread* s : g_Threads)
        if (s->thread && s->thread->threadId == id && s->state != State::Suspended)
            return s;
    return nullptr;
}

bool SchedSuspend(HANDLE thread, ULONG* previous)
{
    if (!g_Active)
        return false;
    std::unique_lock<std::mutex> lock(g_Lock);
    SThread* s = StartedThread(thread);
    if (!s)
        return false;
    if (previous)
        *previous = s->suspends;
    ++s->suspends;
    SThread* self = t_Self;
    if (s == self && g_Current == self) { // itself: the turn goes on, back when resumed
        self->state = State::Ready;
        SwitchAway(self, lock);
    }
    return true;
}

bool SchedResume(HANDLE thread, ULONG* previous)
{
    if (!g_Active)
        return false;
    std::lock_guard<std::mutex> lock(g_Lock);
    SThread* s = StartedThread(thread);
    if (!s)
        return false;
    if (previous)
        *previous = s->suspends;
    if (s->suspends)
        --s->suspends;
    return true;
}

void SchedResumed(HANDLE thread)
{
    if (!g_Active)
        return;
    const DWORD id = GetThreadId(thread);
    std::lock_guard<std::mutex> lock(g_Lock);
    for (SThread* s : g_Threads)
        if (s->state == State::Suspended && s->thread && s->thread->threadId == id)
            s->state = State::Ready;
}

void SchedThreadStart(XboxThread* t)
{
    if (!g_Active)
        return;
    std::unique_lock<std::mutex> lock(g_Lock);
    SThread* s = FindByThread(t);
    if (!s)
        return;
    s->id = GetCurrentThreadId();
    t_Self = s;
    if (!g_Current) { // the first game thread: the baton is free
        s->state = State::Running;
        g_Current = s;
        return;
    }
    lock.unlock();
    HANDLE wait[2] = { s->go, RebootEvent() };
    if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0)
        ExitGameThread();
}

void SchedThreadEnd()
{
    SThread* self = t_Self;
    if (!g_Active || !self)
        return;
    std::unique_lock<std::mutex> lock(g_Lock);
    g_Threads.erase(std::remove(g_Threads.begin(), g_Threads.end(), self), g_Threads.end());
    t_Self = nullptr;
    if (g_Current == self) {
        SThread* next = g_Threads.empty() ? nullptr : PickNext(g_Threads.back());
        g_Current = next;
        if (next) {
            next->state = State::Running;
            SetEvent(next->go);
        }
    }
    if (self->go)
        CloseHandle(self->go);
    delete self;
}

static void (*g_TurnHook)() = nullptr;
void SetSchedTurnHook(void (*hook)()) { g_TurnHook = hook; }

DWORD SchedWait(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms, BOOL alertable)
{
    SThread* self = t_Self;
    if (!SchedHolding())
        return WAIT_FAILED;
    if (g_TurnHook)
        g_TurnHook();
    std::unique_lock<std::mutex> lock(g_Lock);
    ++g_Stats.waits;
    if (alertable && !self->apcs.empty()) {
        lock.unlock();
        RunApcs(self);
        return WAIT_IO_COMPLETION;
    }
    self->count = std::min<DWORD>(count, MAXIMUM_WAIT_OBJECTS);
    std::copy(handles, handles + self->count, self->handles);
    self->waitAll = waitAll;
    self->alertable = alertable;
    self->deadline = ms == INFINITE ? ~0ull : FixedClockPeek() + uint64_t(ms) * kUnitsPerMs;
    self->state = State::Waiting;
    // Its own wait may be over at once (it is first in line only after the others, as on a busy system).
    SwitchAway(self, lock);
    const DWORD result = self->result;
    lock.unlock();
    ScrubBelow(kScrubBytes);
    if (result == WAIT_IO_COMPLETION)
        RunApcs(self);
    return result;
}

void SchedYield()
{
    SThread* self = t_Self;
    if (!SchedHolding())
        return;
    if (g_TurnHook)
        g_TurnHook();
    std::unique_lock<std::mutex> lock(g_Lock);
    self->state = State::Ready;
    SwitchAway(self, lock);
    lock.unlock();
    ScrubBelow(kScrubBytes);
}

void SchedWaitUntil(uint64_t time)
{
    const uint64_t now = FixedClockPeek();
    if (time <= now) {
        SchedYield();
        return;
    }
    SThread* self = t_Self;
    if (!SchedHolding())
        return;
    std::unique_lock<std::mutex> lock(g_Lock);
    ++g_Stats.waits;
    self->count = 0;
    self->alertable = FALSE;
    self->deadline = time;
    self->state = State::Waiting;
    SwitchAway(self, lock);
    lock.unlock();
    ScrubBelow(kScrubBytes);
}

void SchedQueueApc(ApcFunction routine, void* context, void* ioStatus)
{
    if (SThread* self = t_Self)
        self->apcs.push_back({ routine, context, ioStatus });
}

uint64_t SchedTraceHash() { return g_Stats.trace * 0x100000001B3ull ^ g_Stats.switches; }

void SchedLogStats()
{
    LOG_INFO("Scheduler: %llu switches, %llu waits, %llu polls, %llu clock jumps, %llu timer rounds, %llu wait(s) only "
             "the PC could end",
        g_Stats.switches, g_Stats.waits, g_Stats.polls, g_Stats.clockJumps, g_Stats.timers, g_Stats.external);
}

} // namespace swrots::kernel
