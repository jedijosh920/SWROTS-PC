// swrots.exe -- process bootstrap.
//
// The Xbox executable is linked to run at fixed addresses starting at 0x10000.
// Windows fills low memory with its own allocations as soon as a process
// starts, so the only reliable way to own that range is to be the image that
// occupies it: this executable is linked at 0x10000 with a large zero-filled
// section (g_GameImage) spanning the whole game image.
//
// The loader then hands control to core.dll, which copies the game into
// this range -- overwriting the loader's own code -- so SwrotsMain never
// returns here. No CRT is linked; keep this file free of library calls.

#include <windows.h>

// Must cover the game image (0x10000 .. 0x96DBD8) plus the loader's own
// sections that precede this array.
static unsigned char g_GameImage[0x01000000];

// Xbox "contiguous" (physical) memory is addressed at 0x80000000+. Reserve it
// before any DLL can land there. Write-watching lets the renderer re-upload a
// texture or buffer only when the game has actually written to it.
static const ULONG_PTR kContiguousBase = 0x80000000;
static const SIZE_T kContiguousSize = 0x08000000; // 128 MiB

typedef void(__cdecl* SwrotsMainFn)(void* reserveBase, unsigned reserveSize, void* contiguousBase, unsigned contiguousSize);

static void Fail(const wchar_t* message)
{
    MessageBoxW(nullptr, message, L"Star Wars: Episode III - Revenge of the Sith", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

// core.dll's fixed base (CMakeLists.txt): kept free until it loads, so that nothing else (a DLL injected by an
// overlay, a heap) takes it first; netplay needs the port's code and state at the same addresses on every PC.
static const ULONG_PTR kCoreBase = 0x60000000;
static const SIZE_T kCoreRoom = 0x01000000;

extern "C" DWORD CALLBACK LoaderEntry()
{
    void* coreRoom = VirtualAlloc((void*)kCoreBase, kCoreRoom, MEM_RESERVE, PAGE_NOACCESS);
    void* contiguous = VirtualAlloc((void*)kContiguousBase, kContiguousSize, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);

    if (coreRoom)
        VirtualFree(coreRoom, 0, MEM_RELEASE);
    HMODULE core = LoadLibraryW(L"core.dll");
    if (!core)
        Fail(L"core.dll could not be loaded. Reinstall the game.");

    SwrotsMainFn main = (SwrotsMainFn)GetProcAddress(core, "SwrotsMain");
    if (!main)
        Fail(L"core.dll is damaged (missing entry point).");

    main(g_GameImage, sizeof(g_GameImage), contiguous, contiguous ? (unsigned)kContiguousSize : 0);

    // Unreachable: SwrotsMain exits the process.
    ExitProcess(1);
}
