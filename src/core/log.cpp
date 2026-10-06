#include "core/log.h"

#include <windows.h>
#include <clocale>
#include <cstdio>

namespace swrots {

static CRITICAL_SECTION g_LogLock;
static HANDLE g_LogFile = INVALID_HANDLE_VALUE;
static bool g_Console = false;
static LARGE_INTEGER g_Start, g_Freq;

void LogInit(const wchar_t* path, bool console, bool append)
{
    // Text is written as UTF-8: with the C runtime's default locale, a path with other than English letters
    // (a user name in Cyrillic, say) could not be converted, and the whole line came out empty.
    setlocale(LC_CTYPE, ".UTF8");
    InitializeCriticalSection(&g_LogLock);
    QueryPerformanceFrequency(&g_Freq);
    QueryPerformanceCounter(&g_Start);
    // After a quick reboot the previous instance may still be closing the log.
    for (int attempt = 0; path && attempt < 50; ++attempt) {
        g_LogFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_LogFile != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION)
            break;
        Sleep(100);
    }
    if (append && g_LogFile != INVALID_HANDLE_VALUE)
        SetFilePointer(g_LogFile, 0, nullptr, FILE_END);
    if (console) {
        AllocConsole();
        g_Console = true;
    }
}

void LogWriteV(LogLevel level, const char* fmt, va_list args)
{
    static const char* kLevel[] = { "DBG", "INF", "WRN", "ERR" };
    char line[2048];

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_Start.QuadPart) / double(g_Freq.QuadPart);

    int n = snprintf(line, sizeof(line), "[%9.4f][%05lu][%s] ", t, GetCurrentThreadId(), kLevel[int(level)]);
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    if (m < 0) // still unconvertible: keep the message's format at least
        m = snprintf(line + n, sizeof(line) - n - 2, "(unprintable text) %s", fmt);
    if (m < 0) m = 0;
    n += (m < int(sizeof(line)) - n - 2) ? m : int(sizeof(line)) - n - 3;
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_LogLock);
    DWORD written;
    if (g_LogFile != INVALID_HANDLE_VALUE)
        WriteFile(g_LogFile, line, n, &written, nullptr);
    if (g_Console)
        WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), line, n, &written, nullptr);
    LeaveCriticalSection(&g_LogLock);

    line[n] = '\0';
    OutputDebugStringA(line);
}

void LogWrite(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWriteV(level, fmt, args);
    va_end(args);
}

void LogClose()
{
    EnterCriticalSection(&g_LogLock);
    if (g_LogFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_LogFile);
        CloseHandle(g_LogFile);
        g_LogFile = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_LogLock);
}

void LogFlush()
{
    if (g_LogFile != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_LogFile);
}

void Fatal(const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    LogWrite(LogLevel::Error, "FATAL: %s", msg);
    LogFlush();
    if (!GetEnvironmentVariableA("SWROTS_NO_DIALOGS", nullptr, 0)) {
        wchar_t text[1024]; // the message is UTF-8 (see LogInit)
        if (!MultiByteToWideChar(CP_UTF8, 0, msg, -1, text, 1024))
            text[0] = 0;
        MessageBoxW(nullptr, text, L"Star Wars: Episode III - Revenge of the Sith", MB_OK | MB_ICONERROR);
    }
    TerminateProcess(GetCurrentProcess(), 1);
    __assume(0);
}

} // namespace swrots
