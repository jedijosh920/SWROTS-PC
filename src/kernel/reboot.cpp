// In-process reboot.
//
// The game reboots itself to change levels or restart a mission (HalReturnToFirmware
// with the game's own XBE as the new image, as XLaunchNewImage does). On the Xbox that
// reloads the executable from scratch. Here the same process does it: every thread that
// runs game code is stopped, the kernel and runtime state the game created is released,
// and the boot handler (core/main.cpp) reloads the image and starts it again, handing
// over the launch data page as the Xbox kernel does. The window stays as it is.
//
// Stopping threads safely: a thread must not be stopped while it holds a host lock (the
// C runtime heap, D3D's, our own). Threads that wait go through GameWait, which also
// waits for the reboot event and ends the thread there, holding nothing. A thread that
// is running is suspended and inspected: if it is executing game code (or was already
// suspended in a system call) its context is redirected to exit the thread; otherwise it
// is resumed and looked at again a moment later. If that does not finish within a few
// seconds, the reboot falls back to restarting the process.

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "game/game.h"
#include "kernel/kernel.h"
#include "kernel/reboot.h"
#include "kernel/sched.h"

namespace swrots::kernel {

namespace {

HANDLE g_RebootEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual reset
std::atomic<bool> g_Rebooting = false;
RebootHandlers g_Handlers;

std::mutex g_HandleLock;
std::unordered_set<HANDLE> g_GameHandles;

bool InGameImage(DWORD eip) { return eip >= game::kImageBase && eip < game::kImageEnd; }

bool InModule(DWORD eip, HMODULE module)
{
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(reinterpret_cast<void*>(uintptr_t(eip)), &mbi, sizeof(mbi))
        && mbi.AllocationBase == reinterpret_cast<void*>(module);
}

HMODULE CoreModule()
{
    static HMODULE module = [] {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CoreModule), &m);
        return m;
    }();
    return module;
}

void __stdcall ExitRedirected() { ExitThread(0); }

// Where a stopped thread's context is redirected: a fresh aligned frame, then exit.
__declspec(naked) void ExitStub()
{
    __asm {
        and esp, 0xFFFFFFF0
        call ExitRedirected
    }
}

// Stops one thread; false if it did not stop by `deadline`.
bool StopThread(XboxThread* t, ULONGLONG deadline)
{
    HANDLE h = t->hostHandle;
    while (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
        if (GetTickCount64() > deadline)
            return false;
        const DWORD previous = SuspendThread(h);
        if (previous == DWORD(-1)) {
            Sleep(1);
            continue;
        }
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        bool redirect = false;
        if (GetThreadContext(h, &c)) {
            // Game code holds no host locks. A thread that was already suspended (by
            // itself in a system call, or created suspended) is not in our code either.
            redirect = InGameImage(c.Eip) || (previous > 0 && !InModule(c.Eip, CoreModule()));
        }
        if (redirect) {
            c.Eip = DWORD(uintptr_t(&ExitStub));
            SetThreadContext(h, &c);
            while (ResumeThread(h) > 1) {
            }
            WaitForSingleObject(h, 200);
        } else {
            ResumeThread(h);
            Sleep(1);
        }
    }
    return true;
}

bool StopGameThreads()
{
    const ULONGLONG deadline = GetTickCount64() + 4000;
    size_t count = 0;
    // A second pass catches threads started while the first was stopping others.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<XboxThread*> threads;
        ForEachThread([](XboxThread* t, void* ctx) { static_cast<std::vector<XboxThread*>*>(ctx)->push_back(t); }, &threads);
        for (XboxThread* t : threads) {
            if (!StopThread(t, deadline)) {
                LOG_ERROR("Reboot: thread %lu did not stop", t->threadId);
                return false;
            }
        }
        count = threads.size();
    }
    LOG_INFO("Reboot: %zu game threads stopped", count);
    return true;
}

void CloseGameHandles()
{
    std::unordered_set<HANDLE> handles;
    {
        std::lock_guard<std::mutex> lock(g_HandleLock);
        handles.swap(g_GameHandles);
    }
    for (HANDLE h : handles) {
        CancelIoEx(h, nullptr); // no late read into memory that is about to be reloaded
        CloseHandle(h);
    }
    LOG_INFO("Reboot: %zu game handles closed", handles.size());
}

struct RebootRequest {
    std::vector<uint8_t> launchData;
};

DWORD WINAPI RebootWorker(void* param)
{
    auto* request = static_cast<RebootRequest*>(param);
    LOG_INFO("---- Reboot: restarting the game in-process ----");
    g_Rebooting = true;
    SetEvent(g_RebootEvent);
    WakeKernelThreads();

    if (!StopGameThreads()) {
        LOG_ERROR("Reboot: falling back to restarting the process");
        LogFlush();
        if (g_Handlers.fallback)
            g_Handlers.fallback(request->launchData.empty() ? nullptr : request->launchData.data());
        ExitProcess(1);
    }

    // Release everything the old instance created, then start the new one. Timers and DPCs
    // first: they can point at the stopped threads' stacks.
    ResetDispatcherForReboot();
    if (g_Handlers.resetRuntime)
        g_Handlers.resetRuntime();
    CloseGameHandles();
    ResetKernelForReboot();
    ResetEvent(g_RebootEvent);
    g_Rebooting = false;
    LOG_INFO("Reboot: starting the game");
    if (g_Handlers.boot)
        g_Handlers.boot(request->launchData.empty() ? nullptr : request->launchData.data());
    delete request;
    return 0;
}

} // namespace

bool Rebooting() { return g_Rebooting; }

void SetRebootHandlers(const RebootHandlers& handlers) { g_Handlers = handlers; }

void TrackGameHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE)
        return;
    std::lock_guard<std::mutex> lock(g_HandleLock);
    g_GameHandles.insert(h);
}

bool UntrackGameHandle(HANDLE h)
{
    std::lock_guard<std::mutex> lock(g_HandleLock);
    return g_GameHandles.erase(h) != 0;
}

void ExitGameThread()
{
    SchedThreadEnd(); // the baton goes on (deterministic scheduler)
    ExitThread(0);
}

HANDLE RebootEvent() { return g_RebootEvent; }

DWORD GameWait(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms, BOOL alertable)
{
    if (SchedHolding()) { // the deterministic scheduler: the wait is a turn handed over
        if (g_Rebooting)
            ExitGameThread();
        return SchedWait(count, handles, waitAll, ms, alertable);
    }
    if (!CurrentThread())
        return count ? WaitForMultipleObjectsEx(count, handles, waitAll, ms, alertable) : SleepEx(ms, alertable);
    if (count == 0) {
        DWORD r = WaitForSingleObjectEx(g_RebootEvent, ms, alertable);
        if (r == WAIT_OBJECT_0)
            ExitGameThread();
        return r;
    }
    if (!waitAll && count < MAXIMUM_WAIT_OBJECTS) {
        HANDLE all[MAXIMUM_WAIT_OBJECTS];
        std::memcpy(all, handles, count * sizeof(HANDLE));
        all[count] = g_RebootEvent;
        DWORD r = WaitForMultipleObjectsEx(count + 1, all, FALSE, ms, alertable);
        if (r == WAIT_OBJECT_0 + count)
            ExitGameThread();
        return r;
    }
    // Wait-all (or a full handle array): wait in slices and look at the reboot flag.
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD slice = 50;
        if (ms != INFINITE) {
            ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= ms)
                slice = 0;
            else if (ms - elapsed < slice)
                slice = DWORD(ms - elapsed);
        }
        DWORD r = WaitForMultipleObjectsEx(count, handles, waitAll, slice, alertable);
        if (r != WAIT_TIMEOUT)
            return r;
        if (g_Rebooting)
            ExitGameThread();
        if (ms != INFINITE && GetTickCount64() - start >= ms)
            return WAIT_TIMEOUT;
    }
}

void RebootInProcess(const void* launchData)
{
    auto* request = new RebootRequest();
    if (launchData) {
        auto* p = static_cast<const uint8_t*>(launchData);
        request->launchData.assign(p, p + 4096);
    }
    HANDLE worker = CreateThread(nullptr, 0x100000, RebootWorker, request, 0, nullptr);
    if (!worker)
        Fatal("Could not start the reboot");
    CloseHandle(worker);
    ExitGameThread(); // this thread is a game thread too
}

} // namespace swrots::kernel
