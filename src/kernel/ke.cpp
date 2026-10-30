// Scheduling and synchronization: IRQL, DPCs, timers, dispatcher objects,
// waits, threads and critical sections.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/crash.h"
#include "core/log.h"
#include "kernel/exports.h"
#include "kernel/host_nt.h"
#include "kernel/kernel.h"
#include "kernel/reboot.h"
#include "kernel/sched.h"
#include "kernel/clock.h"

namespace swrots::kernel {

using namespace xbox;

// ---------------------------------------------------------------------------
// Timeouts
// ---------------------------------------------------------------------------
DWORD TimeoutToMs(const LARGE_INTEGER* timeout)
{
    if (!timeout)
        return INFINITE;
    LONGLONG t = timeout->QuadPart;
    if (t < 0) {
        LONGLONG ms = (-t + 9999) / 10000;
        return ms >= LONGLONG(INFINITE) ? INFINITE - 1 : DWORD(ms);
    }
    if (t == 0)
        return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    LONGLONG delta = t - ((LONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime);
    return delta <= 0 ? 0 : DWORD((delta + 9999) / 10000);
}

// ---------------------------------------------------------------------------
// IRQL. kDispatchLevel and above holds the global dispatcher lock, which is how
// the uniprocessor Xbox guarantees exclusion for code that raises IRQL.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_DispatcherLock;
static bool g_DispatcherLockInit = [] { InitializeCriticalSection(&g_DispatcherLock); return true; }();

// NTSTATUS for a host wait result.
static NTSTATUS StatusOfWait(DWORD r)
{
    if (r == WAIT_TIMEOUT)
        return X_STATUS_TIMEOUT;
    if (r == WAIT_IO_COMPLETION)
        return X_STATUS_USER_APC;
    if (r == WAIT_FAILED)
        return NTSTATUS(0xC0000008); // STATUS_INVALID_HANDLE
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + MAXIMUM_WAIT_OBJECTS)
        return NTSTATUS(0x00000080 + (r - WAIT_ABANDONED_0)); // STATUS_ABANDONED_WAIT_0 + n
    return NTSTATUS(r - WAIT_OBJECT_0);
}

UCHAR RaiseIrql(UCHAR newIrql)
{
    XboxThread* t = CurrentThread();
    UCHAR old = t ? t->pcr.Irql : kPassiveLevel;
    if (old < kDispatchLevel && newIrql >= kDispatchLevel)
        EnterCriticalSection(&g_DispatcherLock);
    if (t)
        t->pcr.Irql = newIrql;
    return old;
}

void LowerIrql(UCHAR newIrql)
{
    XboxThread* t = CurrentThread();
    UCHAR old = t ? t->pcr.Irql : kPassiveLevel;
    if (t)
        t->pcr.Irql = newIrql;
    if (old >= kDispatchLevel && newIrql < kDispatchLevel)
        LeaveCriticalSection(&g_DispatcherLock);
}

UCHAR XBFASTCALL KfRaiseIrql(UCHAR NewIrql) { return RaiseIrql(NewIrql); }
void XBFASTCALL KfLowerIrql(UCHAR NewIrql) { LowerIrql(NewIrql); }
UCHAR XBAPI KeRaiseIrqlToDpcLevel() { return RaiseIrql(kDispatchLevel); }
KERNEL_EXPORT(160, KfRaiseIrql);
KERNEL_EXPORT(161, KfLowerIrql);
KERNEL_EXPORT(129, KeRaiseIrqlToDpcLevel);

// ---------------------------------------------------------------------------
// DPCs: run on a dedicated thread at kDispatchLevel.
// ---------------------------------------------------------------------------
static std::mutex g_DpcLock;
static std::condition_variable g_DpcCv;
static std::deque<KDPC*> g_DpcQueue;
static bool g_DpcThreadStarted = false;

static void DpcThreadMain()
{
    AdoptCurrentThread();
    PinToGameCpu(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        KDPC* dpc;
        {
            std::unique_lock<std::mutex> lock(g_DpcLock);
            g_DpcCv.wait(lock, [] { return !g_DpcQueue.empty() || Rebooting(); });
            if (Rebooting()) {
                lock.unlock();
                ExitGameThread();
            }
            dpc = g_DpcQueue.front();
            g_DpcQueue.pop_front();
            dpc->Inserted = FALSE;
        }
        UCHAR old = RaiseIrql(kDispatchLevel);
        dpc->DeferredRoutine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
        LowerIrql(old);
    }
}

static void QueueDpc(KDPC* dpc, void* arg1, void* arg2)
{
    std::lock_guard<std::mutex> lock(g_DpcLock);
    if (!g_DpcThreadStarted && !SchedActive()) { // the scheduler runs DPCs itself (RunDueTimersAndDpcs)
        std::thread(DpcThreadMain).detach();
        g_DpcThreadStarted = true;
    }
    if (dpc->Inserted)
        return;
    dpc->Inserted = TRUE;
    dpc->SystemArgument1 = arg1;
    dpc->SystemArgument2 = arg2;
    g_DpcQueue.push_back(dpc);
    g_DpcCv.notify_one();
}

void XBAPI KeInitializeDpc(KDPC* Dpc, PKDEFERRED_ROUTINE DeferredRoutine, void* DeferredContext)
{
    Dpc->Type = 19; // DpcObject
    Dpc->Inserted = FALSE;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

BOOLEAN XBAPI KeInsertQueueDpc(KDPC* Dpc, void* SystemArgument1, void* SystemArgument2)
{
    BOOLEAN was = Dpc->Inserted;
    QueueDpc(Dpc, SystemArgument1, SystemArgument2);
    return !was;
}

BOOLEAN XBAPI KeRemoveQueueDpc(KDPC* Dpc)
{
    std::lock_guard<std::mutex> lock(g_DpcLock);
    auto it = std::find(g_DpcQueue.begin(), g_DpcQueue.end(), Dpc);
    if (it == g_DpcQueue.end())
        return FALSE;
    g_DpcQueue.erase(it);
    Dpc->Inserted = FALSE;
    return TRUE;
}
KERNEL_EXPORT(107, KeInitializeDpc);
KERNEL_EXPORT(119, KeInsertQueueDpc);
KERNEL_EXPORT(137, KeRemoveQueueDpc);

// ---------------------------------------------------------------------------
// Dispatcher objects that live in game memory (KEVENT, KTIMER, KTHREAD) are
// shadowed by host events, created on first use.
// ---------------------------------------------------------------------------
static std::mutex g_ObjectLock;
static std::unordered_map<void*, HANDLE> g_ObjectEvents;

static HANDLE HostEventFor(DISPATCHER_HEADER* header)
{
    if (header->Type == ThreadObject)
        return ThreadFromKThread(header)->hostHandle;

    std::lock_guard<std::mutex> lock(g_ObjectLock);
    auto it = g_ObjectEvents.find(header);
    if (it != g_ObjectEvents.end())
        return it->second;
    bool manual = header->Type == EventNotificationObject || header->Type == TimerNotificationObject;
    HANDLE h = CreateEventW(nullptr, manual, header->SignalState != 0, nullptr);
    g_ObjectEvents[header] = h;
    return h;
}

static void SignalObject(DISPATCHER_HEADER* header)
{
    header->SignalState = 1;
    SetEvent(HostEventFor(header));
}

// Game code may clear SignalState inline (KeClearEvent is a macro); resync the
// host event before waiting on it.
// Marks proxy objects (see ObReferenceObjectByHandle), whose state lives in
// the host event rather than in SignalState.
constexpr UCHAR kProxyMarker = 0xEE;

static HANDLE PrepareWait(DISPATCHER_HEADER* header)
{
    HANDLE h = HostEventFor(header);
    if (header->Type != ThreadObject && header->Inserted != kProxyMarker && header->SignalState == 0)
        ResetEvent(h);
    return h;
}

static NTSTATUS WaitResult(DWORD r, DISPATCHER_HEADER** objects, ULONG count)
{
    if (r == WAIT_TIMEOUT)
        return X_STATUS_TIMEOUT;
    if (r == WAIT_IO_COMPLETION)
        return X_STATUS_USER_APC;
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) {
        DISPATCHER_HEADER* h = objects[r - WAIT_OBJECT_0];
        if (h->Type == EventSynchronizationObject || h->Type == TimerSynchronizationObject)
            h->SignalState = 0;
        return NTSTATUS(r - WAIT_OBJECT_0);
    }
    return X_STATUS_UNSUCCESSFUL;
}

LONG XBAPI KeSetEvent(KEVENT* Event, LONG Increment, BOOLEAN Wait)
{
    (void)Increment;
    (void)Wait;
    LONG old = Event->Header.SignalState;
    SignalObject(&Event->Header);
    return old;
}
KERNEL_EXPORT(145, KeSetEvent);

NTSTATUS XBAPI KeWaitForSingleObject(void* Object, ULONG WaitReason, CHAR WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
{
    (void)WaitReason;
    (void)WaitMode;
    auto* header = static_cast<DISPATCHER_HEADER*>(Object);
    HANDLE h = PrepareWait(header);
    DWORD r = GameWait(1, &h, FALSE, TimeoutToMs(Timeout), Alertable);
    return WaitResult(r, &header, 1);
}

NTSTATUS XBAPI KeWaitForMultipleObjects(ULONG Count, void* Object[], ULONG WaitType, ULONG WaitReason, CHAR WaitMode,
    BOOLEAN Alertable, LARGE_INTEGER* Timeout, void* WaitBlockArray)
{
    (void)WaitReason;
    (void)WaitMode;
    (void)WaitBlockArray;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DISPATCHER_HEADER* headers[MAXIMUM_WAIT_OBJECTS];
    if (Count > MAXIMUM_WAIT_OBJECTS)
        return X_STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < Count; ++i) {
        headers[i] = static_cast<DISPATCHER_HEADER*>(Object[i]);
        handles[i] = PrepareWait(headers[i]);
    }
    DWORD r = GameWait(Count, handles, WaitType == 0 /*WaitAll*/, TimeoutToMs(Timeout), Alertable);
    return WaitType == 0 && r < WAIT_OBJECT_0 + Count ? X_STATUS_SUCCESS : WaitResult(r, headers, Count);
}
KERNEL_EXPORT(159, KeWaitForSingleObject);
KERNEL_EXPORT(158, KeWaitForMultipleObjects);

// ---------------------------------------------------------------------------
// Timers: one scheduler thread services every KTIMER.
// ---------------------------------------------------------------------------
struct TimerEntry {
    ULONGLONG due;   // host QPC-based milliseconds
    LONG period;
};
static std::mutex g_TimerLock;
static std::condition_variable g_TimerCv;
static std::unordered_map<KTIMER*, TimerEntry> g_Timers;
static bool g_TimerThreadStarted = false;

// The fixed clock's milliseconds while the deterministic scheduler runs (it fires the timers), else the PC's.
static ULONGLONG NowMs() { return SchedActive() ? FixedClockPeek() / 10000 : GetTickCount64(); }

static void TimerThreadMain()
{
    std::unique_lock<std::mutex> lock(g_TimerLock);
    for (;;) {
        // During a reboot, timers may belong to threads that are gone (a KTIMER on a stack).
        if (Rebooting()) {
            g_TimerCv.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        ULONGLONG now = NowMs();
        ULONGLONG next = ~0ull;
        for (auto it = g_Timers.begin(); it != g_Timers.end();) {
            KTIMER* timer = it->first;
            if (it->second.due <= now) {
                KDPC* dpc = timer->Dpc;
                SignalObject(&timer->Header);
                if (it->second.period > 0) {
                    it->second.due = now + it->second.period;
                    ++it;
                } else {
                    timer->Header.Inserted = FALSE;
                    it = g_Timers.erase(it);
                }
                if (dpc)
                    QueueDpc(dpc, nullptr, nullptr);
                continue;
            }
            next = std::min(next, it->second.due);
            ++it;
        }
        if (next == ~0ull)
            g_TimerCv.wait(lock);
        else
            g_TimerCv.wait_for(lock, std::chrono::milliseconds(next - now));
    }
}

void XBAPI KeInitializeTimerEx(KTIMER* Timer, ULONG Type)
{
    Timer->Header.Type = UCHAR(TimerNotificationObject + Type);
    Timer->Header.Inserted = FALSE;
    Timer->Header.Size = sizeof(KTIMER) / sizeof(LONG);
    Timer->Header.SignalState = 0;
    Timer->DueTime.QuadPart = 0;
    Timer->Period = 0;
    Timer->Dpc = nullptr;
}

BOOLEAN XBAPI KeSetTimerEx(KTIMER* Timer, LARGE_INTEGER DueTime, LONG Period, KDPC* Dpc)
{
    std::lock_guard<std::mutex> lock(g_TimerLock);
    if (!g_TimerThreadStarted && !SchedActive()) { // the scheduler fires them itself (RunDueTimersAndDpcs)
        std::thread(TimerThreadMain).detach();
        g_TimerThreadStarted = true;
    }
    BOOLEAN wasInserted = g_Timers.erase(Timer) != 0;
    Timer->Header.SignalState = 0;
    Timer->Header.Inserted = TRUE;
    Timer->Dpc = Dpc;
    Timer->Period = Period;
    ResetEvent(HostEventFor(&Timer->Header));
    g_Timers[Timer] = { NowMs() + TimeoutToMs(&DueTime), Period };
    g_TimerCv.notify_one();
    return wasInserted;
}

BOOLEAN XBAPI KeSetTimer(KTIMER* Timer, LARGE_INTEGER DueTime, KDPC* Dpc)
{
    return KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN XBAPI KeCancelTimer(KTIMER* Timer)
{
    std::lock_guard<std::mutex> lock(g_TimerLock);
    Timer->Header.Inserted = FALSE;
    return g_Timers.erase(Timer) != 0;
}
// While the deterministic scheduler runs (kernel/sched.h), it fires timers and runs DPCs itself, on the game thread
// that holds the baton, when the fixed clock reaches them: the earliest timer's due time, or ~0.
uint64_t NextTimerDueMs()
{
    std::lock_guard<std::mutex> lock(g_TimerLock);
    uint64_t next = ~0ull;
    for (const auto& [timer, entry] : g_Timers)
        next = std::min<uint64_t>(next, entry.due);
    return next;
}

bool RunDueTimersAndDpcs(uint64_t nowMs)
{
    bool any = false;
    // Due timers, in due order (then by address: the same on every PC with deterministic addresses).
    for (;;) {
        KTIMER* due = nullptr;
        KDPC* dpc = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_TimerLock);
            ULONGLONG best = ~0ull;
            for (const auto& [timer, entry] : g_Timers) {
                if (entry.due <= nowMs && (entry.due < best || (entry.due == best && timer < due))) {
                    best = entry.due;
                    due = timer;
                }
            }
            if (!due)
                break;
            TimerEntry& entry = g_Timers[due];
            dpc = due->Dpc;
            SignalObject(&due->Header);
            if (entry.period > 0) {
                entry.due += ULONGLONG(entry.period);
            } else {
                due->Header.Inserted = FALSE;
                g_Timers.erase(due);
            }
        }
        if (dpc)
            QueueDpc(dpc, nullptr, nullptr);
        any = true;
    }
    // Queued DPCs, in order, at dispatch level.
    for (;;) {
        KDPC* dpc = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_DpcLock);
            if (g_DpcQueue.empty())
                break;
            dpc = g_DpcQueue.front();
            g_DpcQueue.pop_front();
            dpc->Inserted = FALSE;
        }
        UCHAR old = RaiseIrql(kDispatchLevel);
        dpc->DeferredRoutine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
        LowerIrql(old);
        any = true;
    }
    return any;
}

KERNEL_EXPORT(113, KeInitializeTimerEx);
KERNEL_EXPORT(150, KeSetTimerEx);
KERNEL_EXPORT(149, KeSetTimer);
KERNEL_EXPORT(97, KeCancelTimer);

// ---------------------------------------------------------------------------
// Handle-based objects map directly onto host kernel objects.
// ---------------------------------------------------------------------------
static void WarnNamed(const xbox::OBJECT_ATTRIBUTES* attrs, const char* what)
{
    if (attrs && attrs->ObjectName && attrs->ObjectName->Length)
        LOG_WARN("%s: named object '%.*s' created unnamed", what, attrs->ObjectName->Length, attrs->ObjectName->Buffer);
}

NTSTATUS XBAPI NtCreateEvent(HANDLE* EventHandle, xbox::OBJECT_ATTRIBUTES* ObjectAttributes, ULONG EventType, BOOLEAN InitialState)
{
    WarnNamed(ObjectAttributes, "NtCreateEvent");
    NTSTATUS status = ::NtCreateEvent(EventHandle, EVENT_ALL_ACCESS, nullptr, EventType, InitialState);
    if (status >= 0) {
        TrackGameHandle(*EventHandle);
        *EventHandle = ToGameHandle(*EventHandle);
    }
    return status;
}

NTSTATUS XBAPI NtCreateSemaphore(HANDLE* SemaphoreHandle, xbox::OBJECT_ATTRIBUTES* ObjectAttributes, LONG InitialCount, LONG MaximumCount)
{
    WarnNamed(ObjectAttributes, "NtCreateSemaphore");
    NTSTATUS status = ::NtCreateSemaphore(SemaphoreHandle, SEMAPHORE_ALL_ACCESS, nullptr, InitialCount, MaximumCount);
    if (status >= 0) {
        TrackGameHandle(*SemaphoreHandle);
        *SemaphoreHandle = ToGameHandle(*SemaphoreHandle);
    }
    return status;
}

NTSTATUS XBAPI NtSetEvent(HANDLE EventHandle, LONG* PreviousState)
{
    return ::NtSetEvent(ToHostHandle(EventHandle), PreviousState);
}
NTSTATUS XBAPI NtPulseEvent(HANDLE EventHandle, LONG* PreviousState)
{
    return ::NtPulseEvent(ToHostHandle(EventHandle), PreviousState);
}
NTSTATUS XBAPI NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, LONG* PreviousCount)
{
    return ::NtReleaseSemaphore(ToHostHandle(SemaphoreHandle), ReleaseCount, PreviousCount);
}

NTSTATUS XBAPI NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
{
    return StatusOfWait(GameWait(1, &Handle, FALSE, TimeoutToMs(Timeout), Alertable));
}

NTSTATUS XBAPI NtWaitForSingleObjectEx(HANDLE Handle, CHAR WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
{
    (void)WaitMode;
    return StatusOfWait(GameWait(1, &Handle, FALSE, TimeoutToMs(Timeout), Alertable));
}

NTSTATUS XBAPI NtYieldExecution()
{
    if (SchedHolding()) {
        SchedYield();
        return X_STATUS_SUCCESS;
    }
    return ::NtYieldExecution();
}

KERNEL_EXPORT(189, NtCreateEvent);
KERNEL_EXPORT(193, NtCreateSemaphore);
KERNEL_EXPORT(225, NtSetEvent);
KERNEL_EXPORT(205, NtPulseEvent);
KERNEL_EXPORT(222, NtReleaseSemaphore);
KERNEL_EXPORT(233, NtWaitForSingleObject);
KERNEL_EXPORT(234, NtWaitForSingleObjectEx);
KERNEL_EXPORT(238, NtYieldExecution);

NTSTATUS XBAPI KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Interval)
{
    (void)WaitMode;
    const DWORD ms = TimeoutToMs(Interval);
    if (ms == 0) { // a yield
        if (Rebooting())
            ExitGameThread();
        if (SchedHolding()) {
            SchedYield();
            return X_STATUS_SUCCESS;
        }
        return ::NtDelayExecution(Alertable, Interval);
    }
    return GameWait(0, nullptr, FALSE, ms, Alertable) == WAIT_IO_COMPLETION ? X_STATUS_USER_APC : X_STATUS_SUCCESS;
}
KERNEL_EXPORT(99, KeDelayExecutionThread);

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
NTSTATUS XBAPI PsCreateSystemThreadEx(HANDLE* ThreadHandle, ULONG ThreadExtensionSize, ULONG KernelStackSize,
    ULONG TlsDataSize, HANDLE* ThreadId, StartRoutine StartRoutine, void* StartContext, BOOLEAN CreateSuspended,
    BOOLEAN DebuggerThread, SystemRoutine SystemRoutine)
{
    (void)DebuggerThread;
    if (ThreadExtensionSize)
        LOG_WARN("PsCreateSystemThreadEx: ignoring thread extension of %lu bytes", ThreadExtensionSize);
    DWORD tid = 0;
    HANDLE h = CreateXboxThread(StartRoutine, StartContext, SystemRoutine, KernelStackSize, TlsDataSize, CreateSuspended != 0, &tid);
    if (!h)
        return X_STATUS_INSUFFICIENT_RESOURCES;
    LOG_DEBUG("Thread %lu created: start %p ctx %p stack %lu tls %lu", tid, StartRoutine, StartContext, KernelStackSize, TlsDataSize);
    TrackGameHandle(h);
    *ThreadHandle = ToGameHandle(h);
    if (ThreadId) // netplay: not the host's id, which differs from PC to PC (the game's handle's number)
        *ThreadId = SchedActive() ? *ThreadHandle : reinterpret_cast<HANDLE>(uintptr_t(tid));
    return X_STATUS_SUCCESS;
}

void XBAPI PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    SchedThreadEnd();
    ExitThread(DWORD(ExitStatus));
}

NTSTATUS XBAPI NtSuspendThread(HANDLE ThreadHandle, ULONG* PreviousSuspendCount)
{
    ThreadHandle = ToHostHandle(ThreadHandle);
    if (SchedSuspend(ThreadHandle, PreviousSuspendCount)) // the deterministic scheduler keeps it
        return X_STATUS_SUCCESS;
    return ::NtSuspendThread(ThreadHandle, PreviousSuspendCount);
}

NTSTATUS XBAPI NtResumeThread(HANDLE ThreadHandle, ULONG* PreviousSuspendCount)
{
    ThreadHandle = ToHostHandle(ThreadHandle);
    NTSTATUS status = X_STATUS_SUCCESS;
    if (!SchedResume(ThreadHandle, PreviousSuspendCount)) { // a started thread's: the scheduler's count
        SchedResumed(ThreadHandle); // one made suspended: Windows resumes it, it takes turns from now
        status = ::NtResumeThread(ThreadHandle, PreviousSuspendCount);
    }
    // The deterministic scheduler: the threads ready to run take their turn first, as a preemptive kernel would run
    // the resumed one soon (the movie player resumes its threads in a loop until one of them answers).
    if (SchedHolding())
        SchedYield();
    return status;
}

LONG XBAPI KeQueryBasePriorityThread(void* Thread)
{
    return ThreadFromKThread(Thread)->basePriority;
}

LONG XBAPI KeSetBasePriorityThread(void* Thread, LONG Priority)
{
    XboxThread* t = ThreadFromKThread(Thread);
    LONG old = t->basePriority;
    t->basePriority = Priority;
    SetThreadPriority(t->hostHandle, Priority);
    return old;
}

BOOLEAN XBAPI KeSetDisableBoostThread(void* Thread, ULONG Disable)
{
    XboxThread* t = ThreadFromKThread(Thread);
    BOOL old = FALSE;
    GetThreadPriorityBoost(t->hostHandle, &old);
    SetThreadPriorityBoost(t->hostHandle, Disable != 0);
    return BOOLEAN(old);
}

KERNEL_EXPORT(255, PsCreateSystemThreadEx);
KERNEL_EXPORT(258, PsTerminateSystemThread);
KERNEL_EXPORT(231, NtSuspendThread);
KERNEL_EXPORT(224, NtResumeThread);
KERNEL_EXPORT(124, KeQueryBasePriorityThread);
KERNEL_EXPORT(143, KeSetBasePriorityThread);
KERNEL_EXPORT(144, KeSetDisableBoostThread);

// ---------------------------------------------------------------------------
// Object manager
// ---------------------------------------------------------------------------
OBJECT_TYPE ExEventObjectType = {};
OBJECT_TYPE IoFileObjectType = {};
OBJECT_TYPE PsThreadObjectType = {};
KERNEL_EXPORT(16, ExEventObjectType);
KERNEL_EXPORT(71, IoFileObjectType);
KERNEL_EXPORT(259, PsThreadObjectType);

// Event handles referenced as kernel objects get a proxy KEVENT whose host
// event is (a duplicate of) the handle's own event, so KeSetEvent /
// KeWaitForSingleObject on the object act on the real event.
static std::unordered_map<HANDLE, KEVENT*> g_EventProxies;

static KEVENT* EventProxyFor(HANDLE handle)
{
    std::lock_guard<std::mutex> lock(g_ObjectLock);
    auto it = g_EventProxies.find(handle);
    if (it != g_EventProxies.end())
        return it->second;
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    auto* ev = static_cast<KEVENT*>(GameAlloc(sizeof(KEVENT))); // the game holds it (netplay: a fixed address)
    ev->Header.Type = EventNotificationObject;
    ev->Header.Size = sizeof(KEVENT) / sizeof(LONG);
    ev->Header.Inserted = kProxyMarker;
    g_ObjectEvents[&ev->Header] = dup;
    g_EventProxies[handle] = ev;
    return ev;
}

NTSTATUS XBAPI ObReferenceObjectByHandle(HANDLE Handle, OBJECT_TYPE* ObjectType, void** ReturnedObject)
{
    Handle = ToHostHandle(Handle);
    if (ObjectType == &PsThreadObjectType || ObjectType == nullptr) {
        if (XboxThread* t = ThreadFromHandle(Handle)) {
            *ReturnedObject = t;
            return X_STATUS_SUCCESS;
        }
    }
    if (ObjectType == &ExEventObjectType || ObjectType == nullptr) {
        if (KEVENT* ev = EventProxyFor(Handle)) {
            *ReturnedObject = ev;
            return X_STATUS_SUCCESS;
        }
    }
    const char* typeName = ObjectType == &IoFileObjectType ? "file" : ObjectType == &ExEventObjectType ? "event"
                         : ObjectType == &PsThreadObjectType ? "thread" : "?";
    LOG_WARN("ObReferenceObjectByHandle(%p, %s): unsupported object", Handle, typeName);
    static bool once = false;
    if (!once) {
        once = true;
        LogGameStack("ObReferenceObjectByHandle");
    }
    *ReturnedObject = nullptr;
    return X_STATUS_INVALID_HANDLE;
}

void XBFASTCALL ObfDereferenceObject(void* Object) { (void)Object; }
KERNEL_EXPORT(246, ObReferenceObjectByHandle);
KERNEL_EXPORT(250, ObfDereferenceObject);

// ---------------------------------------------------------------------------
// Critical sections (Xbox layout: event header + counts, 28 bytes). The event
// header's first word holds a lazily created host auto-reset event.
// ---------------------------------------------------------------------------
static HANDLE CsEvent(xbox::RTL_CRITICAL_SECTION* cs)
{
    auto* slot = reinterpret_cast<HANDLE volatile*>(&cs->Event.WaitListHead);
    HANDLE h = *slot;
    if (h)
        return ToHostHandle(h);
    HANDLE created = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE numbered = ToGameHandle(created); // lives in game memory: the game's numbering (netplay)
    HANDLE prev = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), numbered, nullptr);
    if (prev) {
        ForgetGameHandle(numbered);
        CloseHandle(created);
        return ToHostHandle(prev);
    }
    TrackGameHandle(created);
    return created;
}

void XBAPI RtlInitializeCriticalSection(xbox::RTL_CRITICAL_SECTION* cs)
{
    std::memset(cs, 0, sizeof(*cs));
    cs->Event.Type = EventSynchronizationObject;
    cs->Event.Size = sizeof(KEVENT) / sizeof(LONG);
    cs->LockCount = -1;
}

void XBAPI RtlEnterCriticalSection(xbox::RTL_CRITICAL_SECTION* cs)
{
    HANDLE self = reinterpret_cast<HANDLE>(uintptr_t(GetCurrentThreadId()));
    if (InterlockedIncrement(&cs->LockCount) == 0) {
        cs->OwningThread = self;
        cs->RecursionCount = 1;
        return;
    }
    if (cs->OwningThread == self) {
        ++cs->RecursionCount;
        return;
    }
    HANDLE event = CsEvent(cs);
    GameWait(1, &event, FALSE, INFINITE, FALSE);
    cs->OwningThread = self;
    cs->RecursionCount = 1;
}

void XBAPI RtlLeaveCriticalSection(xbox::RTL_CRITICAL_SECTION* cs)
{
    if (--cs->RecursionCount != 0) {
        InterlockedDecrement(&cs->LockCount);
        return;
    }
    cs->OwningThread = nullptr;
    if (InterlockedDecrement(&cs->LockCount) >= 0)
        SetEvent(CsEvent(cs));
}
KERNEL_EXPORT(291, RtlInitializeCriticalSection);
KERNEL_EXPORT(277, RtlEnterCriticalSection);
KERNEL_EXPORT(294, RtlLeaveCriticalSection);

// ---------------------------------------------------------------------------
// Reboot (reboot.cpp): the game's threads are gone; drop what they left behind.
// ---------------------------------------------------------------------------
void WakeKernelThreads()
{
    g_DpcCv.notify_all();
}

void ResetDispatcherForReboot()
{
    {
        std::lock_guard<std::mutex> lock(g_DpcLock);
        for (KDPC* dpc : g_DpcQueue)
            (void)dpc;
        g_DpcQueue.clear();
        g_DpcThreadStarted = false; // the DPC thread exited; the next DPC starts a new one
    }
    {
        std::lock_guard<std::mutex> lock(g_TimerLock);
        g_Timers.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_ObjectLock);
        for (auto& [object, event] : g_ObjectEvents)
            CloseHandle(event);
        g_ObjectEvents.clear();
        for (auto& [handle, proxy] : g_EventProxies)
            GameFree(proxy);
        g_EventProxies.clear();
    }
    // A stopped thread may have held it (game code at raised IRQL).
    DeleteCriticalSection(&g_DispatcherLock);
    InitializeCriticalSection(&g_DispatcherLock);
}

void ResetKernelForReboot()
{
    ResetThreadsForReboot();
    ResetFileSystemForReboot();
    ResetMemoryForReboot();
}

} // namespace swrots::kernel
