// core.dll entry: maps the game into the address range reserved by
// swrots.exe, wires it to the native runtime, and starts it.

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>

#include "audio/audio.h"
#include "core/crash.h"
#include "core/icon.h"
#include "core/install.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/window.h"
#include "core/xbe.h"
#include "d3d/recorder.h"
#include "debug/console.h"
#include "debug/menu.h"
#include "game/aliases.h"
#include "game/characters.h"
#include "game/coop.h"
#include "game/devoptions.h"
#include "game/fixes.h"
#include "game/freecam.h"
#include "game/menus.h"
#include "game/game.h"
#include "game/resources.h"
#include "game/roster.h"
#include "game/versus.h"
#include "d3d/d3d.h"
#include "input/controls.h"
#include "kernel/kernel.h"
#include "kernel/mm.h"
#include "kernel/reboot.h"
#include "xapi/xapi.h"
#include "version.h"

namespace swrots {

static XbeFile g_Xbe;

// The system the game runs on, for bug reports: Windows' version, or Wine's (Proton, Linux, macOS), and
// the CPU.
static void LogSystem()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using WineVersion = const char*(__cdecl*)();
    using WineHost = void(__cdecl*)(const char** sysname, const char** release);
    auto wineVersion = ntdll ? reinterpret_cast<WineVersion>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
    auto wineBuild = ntdll ? reinterpret_cast<WineVersion>(GetProcAddress(ntdll, "wine_get_build_id")) : nullptr;
    auto wineHost = ntdll ? reinterpret_cast<WineHost>(GetProcAddress(ntdll, "wine_get_host_version")) : nullptr;
    if (wineVersion) {
        const char* sysname = "?";
        const char* release = "?";
        if (wineHost)
            wineHost(&sysname, &release);
        LOG_INFO("System: Wine %s (%s) on %s %s", wineVersion(), wineBuild ? wineBuild() : "?", sysname, release);
    } else {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        OSVERSIONINFOW v = { sizeof(v) };
        if (rtlGetVersion && rtlGetVersion(&v) == 0)
            LOG_INFO("System: Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
    wchar_t cpu[128] = {};
    DWORD size = sizeof(cpu);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
        RRF_RT_REG_SZ, nullptr, cpu, &size);
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    LOG_INFO("System: %ls, %lu logical cores", cpu[0] ? cpu : L"unknown CPU", si.dwNumberOfProcessors);
}

static std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/"));
}

// e_lfanew pointing at the saved PE headers (see PrepareImageRange).
static LONG g_PeLfanew = 0;

// Once per process: makes the game's address range writable and moves this process's PE
// headers out of the way of the XBE header.
static void PrepareImageRange(void* reserveBase, unsigned reserveSize)
{
    uint32_t reserveEnd = uint32_t(uintptr_t(reserveBase)) + reserveSize;
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(reinterpret_cast<void*>(game::kImageBase), &mbi, sizeof(mbi));
    if (uintptr_t(mbi.AllocationBase) != game::kImageBase || reserveEnd < game::kImageEnd)
        Fatal("The game's address range is not reserved (swrots.exe is damaged or was relocated).");

    uint8_t* base = reinterpret_cast<uint8_t*>(game::kImageBase);
    SIZE_T size = game::kImageEnd - game::kImageBase;
    DWORD old;
    if (!VirtualProtect(base, reserveEnd - game::kImageBase, PAGE_EXECUTE_READWRITE, &old))
        Fatal("Could not make the game image writable (%lu)", GetLastError());

    // Windows keeps reading this process's PE headers (e.g. the default stack
    // size for new threads), but the game needs its XBE header at the same
    // address. Save the PE headers in the unused tail of the reserved range and
    // leave just enough at 0x10000 -- 'MZ' and e_lfanew -- to reach them. Both
    // fields overlay parts of the XBE header the game never reads.
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    uint32_t peHeaderSize = nt->OptionalHeader.SizeOfHeaders;
    uint8_t* peCopy = reinterpret_cast<uint8_t*>(game::kPeHeaderCopy);
    if (game::kPeHeaderCopy + peHeaderSize > reserveEnd)
        Fatal("PE header copy does not fit in the reserved range");
    std::memcpy(peCopy, base, peHeaderSize);
    g_PeLfanew = LONG(game::kPeHeaderCopy - game::kImageBase) + dos->e_lfanew;
    (void)size;
}

// Each boot: copies the XBE into its link-time addresses, inside this process's image, as
// the Xbox loads it fresh when the game reboots.
static void LoadGameImage()
{
    uint8_t* base = reinterpret_cast<uint8_t*>(game::kImageBase);
    std::memset(base, 0, game::kImageEnd - game::kImageBase);

    const XbeHeader& h = g_Xbe.Header();
    std::memcpy(base, g_Xbe.Bytes().data(), h.SizeOfHeaders);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = g_PeLfanew;

    auto* sections = reinterpret_cast<XbeSectionHeader*>(uintptr_t(h.SectionHeadersAddress));
    for (uint32_t i = 0; i < h.NumberOfSections; ++i) {
        XbeSectionHeader& s = sections[i];
        std::memcpy(reinterpret_cast<void*>(uintptr_t(s.VirtualAddress)), g_Xbe.RawSectionData(s), s.RawSize);
        s.SectionReferenceCount = (s.Flags & kXbeSectionPreload) ? 1 : 0;
        LOG_DEBUG("Section %-10s %08X-%08X", g_Xbe.SectionName(s).c_str(), s.VirtualAddress, s.VirtualAddress + s.VirtualSize);
    }
}

// The XBE entry point (mainCRTStartup) takes no arguments and does a plain RET;
// it starts the real main thread and returns.
static void __stdcall StartXbeEntry(void* entry)
{
    reinterpret_cast<void(__cdecl*)()>(entry)();
}

// What each boot of the game needs from settings.ini and the folders.
struct BootConfig {
    kernel::Paths paths;
    std::wstring dumpDir;
    bool debugDisplays = false;
    bool logResources = false;
};
static BootConfig g_Boot;

// Starts the game image (already loaded): wires it to the runtime and runs its entry point.
// On the first boot and after each in-process reboot (kernel/reboot.cpp).
static void StartGame(const void* launchData)
{
    kernel::BootInit(launchData);
    kernel::InstallThunks(g_Xbe.KernelThunkAddress());
    kernel::InstallFsPatches();
    xapi::InstallHooks();
    debug::InstallConsoleHooks();
    game::InstallDevOptions(g_Boot.debugDisplays);
    game::InstallGameFixes();
    game::InstallVersus();
    game::InstallRoster();
    game::InstallAnimationAliases();
    game::InstallCharacters();
    game::InstallMenus();
    game::InstallCoop();
    game::InstallResourceHooks(g_Boot.paths.gameData, g_Boot.paths.mods, g_Boot.paths.cache, g_Boot.dumpDir,
        g_Boot.logResources);
    game::InstallFreeCamera();
    audio::InitXboxGlobals();

    const XbeHeader& h = g_Xbe.Header();
    uint32_t tlsSize = 0;
    if (h.TlsAddress) {
        auto* tls = reinterpret_cast<const XbeTls*>(uintptr_t(h.TlsAddress));
        tlsSize = 4 + (tls->DataEndAddress - tls->DataStartAddress) + tls->SizeOfZeroFill;
    }
    LOG_INFO("Starting game at %08X", g_Xbe.EntryPoint());
    HANDLE main = kernel::CreateXboxThread(&StartXbeEntry, reinterpret_cast<void*>(uintptr_t(g_Xbe.EntryPoint())),
        nullptr, h.PeStackCommit, tlsSize, false, nullptr);
    if (!main)
        Fatal("Could not start the game thread");
    CloseHandle(main);
}

// In-process reboot, once the old instance's threads are gone.
static void ResetRuntime()
{
    audio::ResetForReboot();
    d3d::ResetForReboot();
    input::ResetPortsForReboot();
}

static void RebootGame(const void* launchData)
{
    LoadGameImage();
    StartGame(launchData);
}

static void Run(void* reserveBase, unsigned reserveSize, void* contiguousBase, unsigned contiguousSize)
{
    // Real pixels: without this, Windows treats the game as a 96-DPI program on a scaled
    // display and stretches its window (a blurry picture, a smaller fullscreen resolution).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring exeDir = ExeDirectory();
    // Everything lives next to the executable: settings.ini (player settings,
    // plus optional [Mods] and [Debug] sections), the game data extracted from
    // the disc, saves, cache and mods.
    std::wstring ini = exeDir + L"\\settings.ini";
    bool console = GetPrivateProfileIntW(L"Debug", L"Console", 0, ini.c_str()) != 0;
    // One log per session: a new session keeps the last one as swrots.previous.log (so a
    // crash report survives starting the game again); a fallback restart (--relaunch)
    // continues it.
    const std::wstring logDir = exeDir + L"\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);
    const std::wstring logPath = logDir + L"\\swrots.log";
    const bool relaunch = wcsstr(GetCommandLineW(), L"--relaunch") != nullptr;
    if (!relaunch)
        MoveFileExW(logPath.c_str(), (logDir + L"\\swrots.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    LogInit(logPath.c_str(), console, relaunch);
    if (relaunch)
        LOG_INFO("---- The game was restarted to reboot (fallback) ----");
    LOG_INFO("Star Wars: Episode III - Revenge of the Sith (PC) starting, SWROTS-PC %s", SWROTS_VERSION);
    LogSystem();
    LoadSettings(ini);

    // First run: install the game files from the player's disc image
    // (`swrots.exe --install <image>` preselects it).
    std::wstring installImage;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--install") == 0)
            installImage = argv[i + 1];
    LocalFree(argv);
    if (!EnsureGameData(exeDir, exeDir + L"\\GameData", installImage))
        ExitProcess(0);

    kernel::Paths paths;
    paths.gameData = exeDir + L"\\GameData";
    paths.saves = exeDir + L"\\saves";
    paths.cache = exeDir + L"\\cache";
    paths.mods = exeDir + L"\\mods";
    paths.logs = logDir;
    LOG_INFO("Game data: %ls", paths.gameData.c_str());
    LOG_INFO("Saves: %ls", paths.saves.c_str());
    LOG_INFO("Cache: %ls", paths.cache.c_str());
    LOG_INFO("Mods: %ls", paths.mods.c_str());
    LoadSettings(exeDir + L"\\settings.ini");
    input::LoadControls(exeDir + L"\\controls.ini");

    std::string error;
    if (!g_Xbe.Load(paths.gameData + L"\\default.xbe", error))
        Fatal("%s\n\nCopy the contents of your Star Wars: Episode III disc into:\n%ls", error.c_str(), paths.gameData.c_str());
    std::string md5 = g_Xbe.Md5Hex();
    if (md5 != game::kRetailMd5)
        Fatal("default.xbe is not the supported retail release (MD5 %s).", md5.c_str());

    PrepareImageRange(reserveBase, reserveSize);
    LoadGameImage();
    kernel::ContiguousInit(contiguousBase, contiguousSize);
    kernel::Init(g_Xbe, paths);
    // The game reboots itself in this process (kernel/reboot.cpp); [Debug] ProcessReboot=1
    // restarts the game process instead, as earlier versions did.
    kernel::SetInProcessReboot(GetPrivateProfileIntW(L"Debug", L"ProcessReboot", 0, ini.c_str()) == 0);
    kernel::SetRebootHandlers({ &ResetRuntime, &RebootGame, &kernel::RelaunchProcess });
    wchar_t rebootEvery[16] = {};
    if (GetEnvironmentVariableW(L"SWROTS_REBOOT_EVERY", rebootEvery, 16)) {
        // Not passed on: a restart that falls back to a new process must not start the test again.
        SetEnvironmentVariableW(L"SWROTS_REBOOT_EVERY", nullptr);
        kernel::StartRebootTest(unsigned(_wtoi(rebootEvery)));
    }
    char commands[8192] = {};
    if (GetEnvironmentVariableA("SWROTS_COMMANDS", commands, sizeof(commands))) {
        // Development aid: "<seconds>:<console command>;..." run that long after the start, for unattended
        // tests of live actions (spawn, saber). Not passed on to a relaunched process.
        SetEnvironmentVariableW(L"SWROTS_COMMANDS", nullptr);
        std::thread([script = std::string(commands)] {
            const auto start = GetTickCount64();
            size_t at = 0;
            while (at < script.size()) {
                size_t end = script.find(';', at);
                if (end == std::string::npos)
                    end = script.size();
                const std::string item = script.substr(at, end - at);
                at = end + 1;
                const size_t colon = item.find(':');
                if (colon == std::string::npos)
                    continue;
                const ULONGLONG due = start + ULONGLONG(atof(item.substr(0, colon).c_str()) * 1000);
                while (GetTickCount64() < due)
                    Sleep(50);
                debug::QueueConsoleCommand(item.substr(colon + 1));
            }
        }).detach();
    }
    xapi::EnableSdkTrace(GetPrivateProfileIntW(L"Debug", L"TraceSdk", 0, ini.c_str()) != 0);
    g_Boot.paths = paths;
    if (GetPrivateProfileIntW(L"Mods", L"DumpResources", 0, ini.c_str()))
        g_Boot.dumpDir = exeDir + L"\\dump";
    g_Boot.debugDisplays = GetPrivateProfileIntW(L"Debug", L"DebugDisplays", 0, ini.c_str()) != 0;
    g_Boot.logResources = GetPrivateProfileIntW(L"Debug", L"LogResources", 0, ini.c_str()) != 0;
    wchar_t traceOpen[128] = L"";
    GetPrivateProfileStringW(L"Debug", L"TraceOpen", L"", traceOpen, 128, ini.c_str());
    if (traceOpen[0])
        SetEnvironmentVariableW(L"SWROTS_TRACE_OPEN", traceOpen);
    wchar_t menuKey[32] = L"";
    GetPrivateProfileStringW(L"Debug", L"MenuKey", L"~", menuKey, 32, ini.c_str());
    const bool debugMenu = GetPrivateProfileIntW(L"Debug", L"DebugMenu", 0, ini.c_str()) != 0;
    debug::ConfigureMenu(debugMenu, menuKey);
    // The character tools (play as, spawn) need every level's optional moves; normal play keeps the
    // levels as they were.
    game::EnableOptionalMoves(GetPrivateProfileIntW(L"Debug", L"OptionalMoves", debugMenu ? 1 : 0, ini.c_str()) != 0);
    game::SetRestartOnChange(GetPrivateProfileIntW(L"Debug", L"AutoRestart", 1, ini.c_str()) != 0);
    d3d::ConfigureFlightRecorder(GetPrivateProfileIntW(L"Debug", L"FlightRecorder", 0, ini.c_str()) != 0,
        exeDir + L"\\flight");
    d3d::SetScreenshotDirectory(exeDir + L"\\screenshots");
    InstallCrashHandler();

    const Settings& settings = GetSettings();
    CreateMainWindow(g_Xbe);
    CreateGameWindow(settings.fullscreen);
    StartGame(nullptr);

    // Development aid: SWROTS_WATCHDOG=<seconds> dumps every game thread after
    // that long, to see where a stalled game is waiting.
    char watchdog[16];
    if (GetEnvironmentVariableA("SWROTS_WATCHDOG", watchdog, sizeof(watchdog))) {
        DWORD seconds = DWORD(atoi(watchdog));
        CreateThread(nullptr, 0x10000, [](void* p) -> DWORD {
            Sleep(DWORD(uintptr_t(p)) * 1000);
            DumpAllThreads("watchdog");
            return 0;
        }, reinterpret_cast<void*>(uintptr_t(seconds)), 0, nullptr);
    }

    RunMessageLoop();
}

} // namespace swrots

extern "C" __declspec(dllexport) void __cdecl SwrotsMain(void* reserveBase, unsigned reserveSize, void* contiguousBase,
    unsigned contiguousSize)
{
    try {
        swrots::Run(reserveBase, reserveSize, contiguousBase, contiguousSize);
    } catch (const std::exception& e) {
        swrots::Fatal("Startup failed: %s", e.what());
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
