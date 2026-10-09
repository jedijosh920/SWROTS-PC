#pragma once
#include <windows.h>

namespace swrots {

void InstallCrashHandler();

// Suspends every game thread and logs where it is (for diagnosing hangs).
void DumpAllThreads(const char* reason);

// The hang watchdog: NoteFrame() once a frame; when no frame came for a long time (a game that stopped, not a
// level loading), the game threads are dumped to the log once, and a line follows when frames come again.
void NoteFrame();

// Logs probable game return addresses found on the current stack.
void LogGameStack(const char* reason);

// Replacement for XAPI's UnhandledExceptionFilter (the last-chance handler
// wrapped around every Xbox thread).
LONG __stdcall GameUnhandledExceptionFilter(EXCEPTION_POINTERS* info);

} // namespace swrots
