#include "core/crash.h"

#include <cstdio>
#include <dbghelp.h>
#include <intrin.h>

#include "core/log.h"
#include "game/game.h"
#include "kernel/kernel.h"

namespace swrots {

static void DescribeAddress(uint32_t address, char* out, size_t size)
{
    if (address >= game::kImageBase && address < game::kImageEnd) {
        char name[256];
        if (game::DescribeGameAddress(address, name, sizeof(name))) {
            snprintf(out, size, "%08X (%s)", address, name);
            return;
        }
        const game::SdkSymbol* s = game::NearestSdkSymbol(address);
        if (s && address - s->address < 0x2000)
            snprintf(out, size, "%08X (%s+0x%X)", address, s->name, address - s->address);
        else
            snprintf(out, size, "%08X (game)", address);
        return;
    }
    // Our own code: resolve function + line from the PDB.
    static bool symInit = false;
    if (!symInit) {
        symInit = true;
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &disp, sym)) {
        IMAGEHLP_LINE64 line = { sizeof(line) };
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisp, &line)) {
            const char* file = strrchr(line.FileName, '\\');
            snprintf(out, size, "%08X (%s+0x%llX, %s:%lu)", address, sym->Name, disp, file ? file + 1 : line.FileName,
                line.LineNumber);
        } else {
            snprintf(out, size, "%08X (%s+0x%llX)", address, sym->Name, disp);
        }
        return;
    }

    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(uintptr_t(address)), &module)) {
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        snprintf(out, size, "%08X (%ls+0x%X)", address, name ? name + 1 : path, address - uint32_t(uintptr_t(module)));
    } else {
        snprintf(out, size, "%08X", address);
    }
}

static void Report(EXCEPTION_POINTERS* info, const char* source)
{
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    const CONTEXT* c = info->ContextRecord;
    char where[160];
    DescribeAddress(c->Eip, where, sizeof(where));

    LOG_ERROR("==== Unhandled exception (%s) ====", source);
    LOG_ERROR("Code %08lX at %s", er->ExceptionCode, where);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        LOG_ERROR("  %s address %08lX", er->ExceptionInformation[0] ? "writing" : "reading", er->ExceptionInformation[1]);
    LOG_ERROR("EAX %08lX EBX %08lX ECX %08lX EDX %08lX", c->Eax, c->Ebx, c->Ecx, c->Edx);
    LOG_ERROR("ESI %08lX EDI %08lX EBP %08lX ESP %08lX", c->Esi, c->Edi, c->Ebp, c->Esp);

    // Probable return addresses on the stack.
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(c->Esp));
    int shown = 0;
    for (int i = 0; i < 256 && shown < 16; ++i) {
        uint32_t v;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v >= game::kImageBase + 0x1000 && v < game::kImageEnd) {
            char d[160];
            DescribeAddress(v, d, sizeof(d));
            LOG_ERROR("  [esp+%03X] %s", i * 4, d);
            ++shown;
        }
    }
    LogFlush();

    char msg[512];
    snprintf(msg, sizeof(msg), "The game crashed (exception %08lX at %s).\n\nDetails were written to swrots.log.",
        er->ExceptionCode, where);
    if (!GetEnvironmentVariableA("SWROTS_NO_DIALOGS", nullptr, 0))
        MessageBoxA(nullptr, msg, "Star Wars: Episode III - Revenge of the Sith", MB_OK | MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(), 1);
}

static void DumpThread(kernel::XboxThread* t, void*)
{
    if (t->threadId == GetCurrentThreadId())
        return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t->threadId);
    if (!h)
        return;
    if (SuspendThread(h) != DWORD(-1)) {
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &c)) {
            char where[160];
            DescribeAddress(c.Eip, where, sizeof(where));
            LOG_INFO("Thread %lu at %s  eax=%08lX ecx=%08lX edx=%08lX esp=%08lX", t->threadId, where, c.Eax, c.Ecx,
                c.Edx, c.Esp);
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(c.Esp));
            int shown = 0;
            for (int i = 0; i < 2048 && shown < 10; ++i) {
                uint32_t v;
                __try {
                    v = sp[i];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (v >= game::kImageBase + 0x1000 && v < game::kImageEnd) {
                    char d[160];
                    DescribeAddress(v, d, sizeof(d));
                    LOG_INFO("    [esp+%04X] %s", i * 4, d);
                    ++shown;
                }
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

void DumpAllThreads(const char* reason)
{
    LOG_INFO("==== Thread dump (%s) ====", reason);
    kernel::ForEachThread(DumpThread, nullptr);
    LogFlush();
}

void LogGameStack(const char* reason)
{
    LOG_INFO("Call stack (%s):", reason);
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
    int shown = 0;
    for (int i = 0; i < 1024 && shown < 12; ++i) {
        uint32_t v;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v >= game::kImageBase + 0x1000 && v < game::kImageEnd) {
            char d[160];
            DescribeAddress(v, d, sizeof(d));
            LOG_INFO("  %s", d);
            ++shown;
        }
    }
}

LONG __stdcall GameUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    Report(info, "game thread");
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG __stdcall TopLevelFilter(EXCEPTION_POINTERS* info)
{
    Report(info, "host");
    return EXCEPTION_EXECUTE_HANDLER;
}

// First-chance logging: the game may handle some exceptions itself, so these
// are only recorded, not treated as fatal.
static LONG __stdcall FirstChance(EXCEPTION_POINTERS* info)
{
    static volatile LONG s_logged = 0;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 /*C++*/ || code == 0x406D1388 /*thread name*/ || code == DBG_PRINTEXCEPTION_C ||
        code == 0x4001000A /*DBG_PRINTEXCEPTION_WIDE_C*/ || code == 0x000006BA /*RPC*/)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&s_logged) <= 64) {
        char where[160];
        DescribeAddress(info->ContextRecord->Eip, where, sizeof(where));
        LOG_WARN("First-chance exception %08lX at %s", code, where);
        // For the first few, list likely return addresses into game code.
        if (s_logged <= 2) {
            const CONTEXT* c = info->ContextRecord;
            LOG_WARN("    eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax,
                c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(info->ContextRecord->Esp));
            int shown = 0;
            for (int i = 0; i < 1024 && shown < 32; ++i) {
                uint32_t v;
                __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (v >= 0x11000 && v < 0x4EF000) {
                    DescribeAddress(v, where, sizeof(where));
                    LOG_WARN("    [esp+%04X] %s", i * 4, where);
                    ++shown;
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static volatile LONG g_Frames = 0;
constexpr DWORD kStallMs = 20000;    // no frame for this long: the game stopped (a level loads in a few seconds)
constexpr DWORD kWatchEveryMs = 2000;

void NoteFrame()
{
    InterlockedIncrement(&g_Frames);
}

static DWORD __stdcall Watchdog(void*)
{
    LONG seen = g_Frames;
    ULONGLONG lastChange = GetTickCount64();
    bool reported = false;
    for (;;) {
        Sleep(kWatchEveryMs);
        const LONG now = g_Frames;
        const ULONGLONG tick = GetTickCount64();
        if (now != seen) {
            if (reported)
                LOG_WARN("Watchdog: frames again after %.0f s", double(tick - lastChange) / 1000.0);
            seen = now;
            lastChange = tick;
            reported = false;
        } else if (now > 0 && !reported && tick - lastChange >= kStallMs) {
            reported = true;
            LOG_WARN("Watchdog: no frame for %.0f s; the game seems to have stopped", double(tick - lastChange) / 1000.0);
            DumpAllThreads("watchdog");
        }
    }
}

void InstallCrashHandler()
{
    AddVectoredExceptionHandler(1, FirstChance);
    SetUnhandledExceptionFilter(TopLevelFilter);
    if (HANDLE thread = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr))
        CloseHandle(thread);
}

} // namespace swrots
