// Remaining kernel services: Rtl helpers, time, console settings, crypto,
// XBE sections, firmware/HAL, debug output and exported variables.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <timeapi.h>

#include "core/crash.h"
#include "core/log.h"
#include "core/xbe.h"
#include "kernel/clock.h"
#include "kernel/exports.h"
#include "kernel/host_nt.h"
#include "kernel/kernel.h"
#include "audio/audio.h"
#include "core/settings.h"
#include "core/window.h"
#include "kernel/mm.h"
#include "kernel/reboot.h"
#include "kernel/sched.h"
#include "kernel/sha1.h"

namespace swrots::kernel {

// ---------------------------------------------------------------------------
// Exported variables
// ---------------------------------------------------------------------------
volatile ULONG KeTickCount = 0;
void* LaunchDataPage = nullptr;
static char g_ImageFileName[] = "\\Device\\CdRom0\\default.xbe";
xbox::ANSI_STRING XeImageFileName = { sizeof(g_ImageFileName) - 1, sizeof(g_ImageFileName), g_ImageFileName };
xbox::XBOX_HARDWARE_INFO XboxHardwareInfo = { 0, 0xD3, 0xD4, 0, 0 };
// Final retail kernel; also keeps XAPI's hot-patches for older kernels inert.
xbox::XBOX_KRNL_VERSION XboxKrnlVersion = { 1, 0, 5838, 1 };
UCHAR XboxHDKey[16] = {};
UCHAR XboxSignatureKey[16] = {};
UCHAR XboxAlternateSignatureKeys[16][16] = {};
ULONG HalDiskCachePartitionCount = 3;
ULONG HalBootSMCVideoMode = 1; // component (HDTV) AV pack
UCHAR IdexChannelObject[0x100] = {};

KERNEL_EXPORT(156, KeTickCount);
KERNEL_EXPORT(164, LaunchDataPage);
KERNEL_EXPORT(326, XeImageFileName);
KERNEL_EXPORT(322, XboxHardwareInfo);
KERNEL_EXPORT(324, XboxKrnlVersion);
KERNEL_EXPORT(323, XboxHDKey);
KERNEL_EXPORT(325, XboxSignatureKey);
KERNEL_EXPORT(354, XboxAlternateSignatureKeys);
KERNEL_EXPORT(40, HalDiskCachePartitionCount);
KERNEL_EXPORT(356, HalBootSMCVideoMode);
KERNEL_EXPORT(357, IdexChannelObject);

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
static LARGE_INTEGER g_BootQpc, g_QpcFreq;

static void TickThreadMain()
{
    timeBeginPeriod(1);
    ULONGLONG start = GetTickCount64();
    for (;;) {
        Sleep(1);
        // Under the fixed clock (netplay) the clock keeps it in step itself (kernel/clock.cpp).
        if (!FixedClockOn())
            KeTickCount = ULONG(GetTickCount64() - start);
    }
}

void XBAPI KeQuerySystemTime(LARGE_INTEGER* CurrentTime)
{
    if (FixedClockOn()) {
        CurrentTime->QuadPart = LONGLONG(FixedClockSystemTime());
        return;
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    CurrentTime->LowPart = ft.dwLowDateTime;
    CurrentTime->HighPart = LONG(ft.dwHighDateTime);
}

ULONGLONG XBAPI KeQueryInterruptTime()
{
    if (FixedClockOn())
        return FixedClockNow();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return ULONGLONG((now.QuadPart - g_BootQpc.QuadPart) * 10000000.0 / double(g_QpcFreq.QuadPart));
}

void XBAPI KeStallExecutionProcessor(ULONG MicroSeconds)
{
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    LONGLONG ticks = LONGLONG(MicroSeconds) * g_QpcFreq.QuadPart / 1000000;
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
}

KERNEL_EXPORT(128, KeQuerySystemTime);
KERNEL_EXPORT(125, KeQueryInterruptTime);
KERNEL_EXPORT(151, KeStallExecutionProcessor);

// ---------------------------------------------------------------------------
// Rtl
// ---------------------------------------------------------------------------
void XBAPI RtlInitAnsiString(xbox::ANSI_STRING* DestinationString, const char* SourceString)
{
    DestinationString->Buffer = const_cast<char*>(SourceString);
    if (SourceString) {
        size_t len = strlen(SourceString);
        DestinationString->Length = USHORT(len);
        DestinationString->MaximumLength = USHORT(len + 1);
    } else {
        DestinationString->Length = DestinationString->MaximumLength = 0;
    }
}

NTSTATUS XBAPI RtlAnsiStringToUnicodeString(xbox::UNICODE_STRING* DestinationString, xbox::ANSI_STRING* SourceString,
    BOOLEAN AllocateDestinationString)
{
    USHORT bytes = USHORT(SourceString->Length * sizeof(wchar_t));
    if (AllocateDestinationString) {
        DestinationString->MaximumLength = USHORT(bytes + sizeof(wchar_t));
        DestinationString->Buffer = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, DestinationString->MaximumLength));
        if (!DestinationString->Buffer)
            return xbox::X_STATUS_NO_MEMORY;
    } else if (DestinationString->MaximumLength < bytes) {
        return xbox::X_STATUS_BUFFER_TOO_SMALL;
    }
    for (USHORT i = 0; i < SourceString->Length; ++i)
        DestinationString->Buffer[i] = wchar_t(static_cast<unsigned char>(SourceString->Buffer[i]));
    DestinationString->Length = bytes;
    if (DestinationString->MaximumLength > bytes)
        DestinationString->Buffer[SourceString->Length] = 0;
    return xbox::X_STATUS_SUCCESS;
}

BOOLEAN XBAPI RtlEqualString(xbox::ANSI_STRING* String1, xbox::ANSI_STRING* String2, BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;
    return (CaseInSensitive ? _strnicmp(String1->Buffer, String2->Buffer, String1->Length)
                            : strncmp(String1->Buffer, String2->Buffer, String1->Length)) == 0;
}

SIZE_T XBAPI RtlCompareMemoryUlong(void* Source, SIZE_T Length, ULONG Pattern)
{
    auto* p = static_cast<ULONG*>(Source);
    SIZE_T n = Length / sizeof(ULONG), i = 0;
    while (i < n && p[i] == Pattern)
        ++i;
    return i * sizeof(ULONG);
}

ULONG XBAPI RtlNtStatusToDosError(NTSTATUS Status) { return ::RtlNtStatusToDosError(Status); }
void XBAPI RtlRaiseException(EXCEPTION_RECORD* ExceptionRecord) { ::RtlRaiseException(ExceptionRecord); }

void XBAPI RtlUnwind(void* TargetFrame, void* TargetIp, EXCEPTION_RECORD* ExceptionRecord, void* ReturnValue)
{
    ::RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue);
}

void XBAPI RtlTimeToTimeFields(LARGE_INTEGER* Time, xbox::TIME_FIELDS* TimeFields) { ::RtlTimeToTimeFields(Time, TimeFields); }
BOOLEAN XBAPI RtlTimeFieldsToTime(xbox::TIME_FIELDS* TimeFields, LARGE_INTEGER* Time) { return ::RtlTimeFieldsToTime(TimeFields, Time); }

KERNEL_EXPORT(289, RtlInitAnsiString);
KERNEL_EXPORT(260, RtlAnsiStringToUnicodeString);
KERNEL_EXPORT(279, RtlEqualString);
KERNEL_EXPORT(269, RtlCompareMemoryUlong);
KERNEL_EXPORT(301, RtlNtStatusToDosError);
KERNEL_EXPORT(302, RtlRaiseException);
KERNEL_EXPORT(312, RtlUnwind);
KERNEL_EXPORT(305, RtlTimeToTimeFields);
KERNEL_EXPORT(304, RtlTimeFieldsToTime);

// ---------------------------------------------------------------------------
// Console settings (EEPROM)
// ---------------------------------------------------------------------------
enum : ULONG { REG_DWORD_X = 4, REG_BINARY_X = 3 };

// Dashboard video settings: widescreen (0x00010000, from the player's settings),
// 480p (0x00080000) and 720p (0x00020000).
static ULONG VideoFlags() { return (GetSettings().widescreen ? 0x00010000 : 0) | 0x00080000 | 0x00020000; }

NTSTATUS XBAPI ExQueryNonVolatileSetting(ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength, ULONG* ResultLength)
{
    ULONG dword;
    switch (ValueIndex) {
    case 0x7: dword = 1; break;          // XC_LANGUAGE: English
    case 0x8: dword = VideoFlags(); break; // XC_VIDEO
    case 0x9: dword = 0; break;          // XC_AUDIO: stereo
    case 0xA: case 0xB: case 0xC: dword = 0; break; // parental controls off
    case 0x11: dword = 0; break;         // XC_MISC
    case 0x12: dword = 1; break;         // XC_DVD_REGION
    case 0x0: case 0x3: case 0x6: dword = 0; break; // time zone biases: UTC
    case 0x2: case 0x5: dword = 0; break;           // time zone dates
    case 0x103: dword = 0x00400100; break;          // XC_FACTORY_AV_REGION: NTSC-M
    case 0x104: dword = 1; break;                   // XC_FACTORY_GAME_REGION: North America
    case 0x1: case 0x4: {                           // time zone names (4 chars)
        if (Type) *Type = REG_BINARY_X;
        if (ResultLength) *ResultLength = 4;
        if (ValueLength < 4) return xbox::X_STATUS_BUFFER_TOO_SMALL;
        std::memcpy(Value, ValueIndex == 1 ? "UTC\0" : "UTC\0", 4);
        return xbox::X_STATUS_SUCCESS;
    }
    case 0xFF: { // XC_MAX_OS: whole user-settings block
        const ULONG size = 0x60;
        if (Type) *Type = REG_BINARY_X;
        if (ResultLength) *ResultLength = size;
        if (ValueLength < size) return xbox::X_STATUS_BUFFER_TOO_SMALL;
        std::memset(Value, 0, size);
        auto* u = static_cast<ULONG*>(Value);
        u[0x2C / 4] = 1;          // Language
        u[0x30 / 4] = VideoFlags(); // VideoFlags
        return xbox::X_STATUS_SUCCESS;
    }
    default:
        LOG_WARN("ExQueryNonVolatileSetting(0x%lX): unknown setting", ValueIndex);
        return xbox::X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (Type) *Type = REG_DWORD_X;
    if (ResultLength) *ResultLength = sizeof(ULONG);
    if (ValueLength < sizeof(ULONG)) return xbox::X_STATUS_BUFFER_TOO_SMALL;
    *static_cast<ULONG*>(Value) = dword;
    return xbox::X_STATUS_SUCCESS;
}
KERNEL_EXPORT(24, ExQueryNonVolatileSetting);

// ---------------------------------------------------------------------------
// Crypto (save-game signatures)
// ---------------------------------------------------------------------------
void XBAPI XcSHAInit(UCHAR* pbSHAContext) { Sha1Init(reinterpret_cast<Sha1Context*>(pbSHAContext)); }
void XBAPI XcSHAUpdate(UCHAR* pbSHAContext, UCHAR* pbInput, ULONG dwInputLength)
{
    Sha1Update(reinterpret_cast<Sha1Context*>(pbSHAContext), pbInput, dwInputLength);
}
void XBAPI XcSHAFinal(UCHAR* pbSHAContext, UCHAR* pbDigest) { Sha1Final(reinterpret_cast<Sha1Context*>(pbSHAContext), pbDigest); }

void XBAPI XcHMAC(UCHAR* pbKeyMaterial, ULONG cbKeyMaterial, UCHAR* pbData, ULONG cbData, UCHAR* pbData2, ULONG cbData2,
    UCHAR* HmacData)
{
    UCHAR key[64] = {};
    if (cbKeyMaterial > 64) {
        Sha1Context c;
        Sha1Init(&c);
        Sha1Update(&c, pbKeyMaterial, cbKeyMaterial);
        Sha1Final(&c, key);
    } else {
        std::memcpy(key, pbKeyMaterial, cbKeyMaterial);
    }
    UCHAR pad[64], inner[20];
    Sha1Context c;
    for (int i = 0; i < 64; ++i) pad[i] = key[i] ^ 0x36;
    Sha1Init(&c);
    Sha1Update(&c, pad, 64);
    if (pbData) Sha1Update(&c, pbData, cbData);
    if (pbData2) Sha1Update(&c, pbData2, cbData2);
    Sha1Final(&c, inner);
    for (int i = 0; i < 64; ++i) pad[i] = key[i] ^ 0x5C;
    Sha1Init(&c);
    Sha1Update(&c, pad, 64);
    Sha1Update(&c, inner, 20);
    Sha1Final(&c, HmacData);
}

KERNEL_EXPORT(335, XcSHAInit);
KERNEL_EXPORT(336, XcSHAUpdate);
KERNEL_EXPORT(337, XcSHAFinal);
KERNEL_EXPORT(340, XcHMAC);

// ---------------------------------------------------------------------------
// XBE sections. Everything is mapped at startup; loading a section that was
// unloaded restores its original contents, as reading it from disc would.
// ---------------------------------------------------------------------------
static const XbeFile* g_Xbe = nullptr;

NTSTATUS XBAPI XeLoadSection(XbeSectionHeader* Section)
{
    if (Section->SectionReferenceCount++ == 0 && g_Xbe) {
        auto* dst = reinterpret_cast<uint8_t*>(uintptr_t(Section->VirtualAddress));
        std::memcpy(dst, g_Xbe->RawSectionData(*Section), Section->RawSize);
        if (Section->VirtualSize > Section->RawSize)
            std::memset(dst + Section->RawSize, 0, Section->VirtualSize - Section->RawSize);
    }
    return xbox::X_STATUS_SUCCESS;
}

NTSTATUS XBAPI XeUnloadSection(XbeSectionHeader* Section)
{
    if (Section->SectionReferenceCount)
        --Section->SectionReferenceCount;
    return xbox::X_STATUS_SUCCESS;
}

KERNEL_EXPORT(327, XeLoadSection);
KERNEL_EXPORT(328, XeUnloadSection);

// ---------------------------------------------------------------------------
// HAL / firmware / debug
// ---------------------------------------------------------------------------
// Quick reboot into a new XBE (XLaunchNewImage). When the target is the game
// itself, start it again in this process (reboot.cpp) and hand the launch data
// page to the new instance, as the Xbox kernel does across a quick reboot.
static std::wstring g_LaunchDataFile;
static std::vector<uint8_t> g_PendingLaunchData; // from a process restart (--relaunch)
static bool g_InProcessReboot = true;
// The launch data this game image started with (a reboot's or a process relaunch's): what started the
// running mission. The page itself does not keep it: the game rewrites it while it runs.
static std::vector<uint8_t> g_MissionLaunchData;

void SetInProcessReboot(bool enabled) { g_InProcessReboot = enabled; }

static void (*g_BeforeRelaunch)() = nullptr;
void SetBeforeRelaunch(void (*callback)()) { g_BeforeRelaunch = callback; }

// The fallback: restart the whole game process instead.
void RelaunchProcess(const void* launchData)
{
    HANDLE f = CreateFileW(g_LaunchDataFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        if (launchData)
            WriteFile(f, launchData, 4096, &written, nullptr);
        CloseHandle(f);
    }
    // A new process starts the game with the launch data (its own window replaces this one).
    if (g_BeforeRelaunch)
        g_BeforeRelaunch();
    audio::Silence();
    LogFlush();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --relaunch";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOG_ERROR("Could not restart the game (%lu)", GetLastError());
    }
    LogClose();
    ExitProcess(0);
}

// Reboots with `launchData` (null: the current page) from a thread of its own: the reboot stops every game
// thread, the caller's too.
static const uint8_t kNoLaunchData = 0; // RebootWith: a start as from the dashboard (no launch data at all)

static void RebootWith(const void* launchData)
{
    LogFlush();
    HANDLE worker = CreateThread(nullptr, 0, [](void* data) -> DWORD {
        const void* launchData = data == &kNoLaunchData ? nullptr : data ? data : LaunchDataPage;
        if (g_InProcessReboot)
            RebootInProcess(launchData); // ends this thread; the reboot worker takes over
        RelaunchProcess(launchData);
    }, const_cast<void*>(launchData), 0, nullptr);
    if (worker)
        CloseHandle(worker);
}

void RestartMission()
{
    // Without a reboot so far (a level booted from Default_Xbox.cfg), the current page starts the same level again.
    RebootWith(g_MissionLaunchData.empty() ? nullptr : g_MissionLaunchData.data());
}

// The launch data page (XLaunchNewImage, 0x4A1789): its type (0, a title's own), the title's id (checked by
// XGetLaunchInfo, 0x4A186A), the image's path at +8 and the title's data at +0x400 (3 KB). The game's data is
// a line of launch arguments in Default_Xbox.cfg's form (0x6FA31 splits it at spaces), as its level changes
// write it (0x136080: map:, nextmap:, ...).
static std::vector<uint8_t> g_LaunchRequest;
static std::string g_BootArguments;

static LaunchFilter g_LaunchObserverEarly = nullptr; // set with the observer below (it is declared after)
void LaunchGame(const std::string& launchArguments)
{
    std::string arguments = launchArguments;
    if (g_LaunchObserverEarly) // online play: the host's own starts go to the guest too
        g_LaunchObserverEarly(arguments);
    g_LaunchRequest.assign(4096, 0);
    const uint32_t certificate = *reinterpret_cast<const uint32_t*>(uintptr_t(0x10118));
    const uint32_t title = *reinterpret_cast<const uint32_t*>(uintptr_t(certificate + 8));
    std::memcpy(g_LaunchRequest.data() + 4, &title, 4);
    const char path[] = "d:\\default.xbe";
    std::memcpy(g_LaunchRequest.data() + 8, path, sizeof path);
    std::memcpy(g_LaunchRequest.data() + 0x400, arguments.data(), std::min<size_t>(arguments.size(), 3071));
    LOG_INFO("Starting the game again with '%s'", arguments.c_str());
    RebootWith(g_LaunchRequest.data());
}

const std::string& LaunchArguments() { return g_BootArguments; }

void RestartFromDashboard()
{
    g_MissionLaunchData.clear();
    LOG_INFO("Starting the game again as from the dashboard");
    RebootWith(&kNoLaunchData);
}

void StartRebootTest(unsigned seconds)
{
    if (!seconds)
        return;
    LOG_INFO("Reboot test: restarting every %u seconds", seconds);
    HANDLE thread = CreateThread(nullptr, 0, [](void* param) -> DWORD {
        const DWORD ms = DWORD(uintptr_t(param)) * 1000;
        for (;;) {
            Sleep(ms);
            LOG_INFO("Reboot test: restarting");
            RestartMission();
        }
    }, reinterpret_cast<void*>(uintptr_t(seconds)), 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

static std::vector<LaunchFilter> g_LaunchFilters;
void SetLaunchFilter(LaunchFilter filter)
{
    if (filter && std::find(g_LaunchFilters.begin(), g_LaunchFilters.end(), filter) == g_LaunchFilters.end())
        g_LaunchFilters.push_back(filter);
}
static LaunchFilter g_LaunchObserver = nullptr;
void SetLaunchObserver(LaunchFilter observer)
{
    g_LaunchObserver = observer;
    g_LaunchObserverEarly = observer;
}

void XBAPI HalReturnToFirmware(ULONG Routine)
{
    if (Routine == 2 && LaunchDataPage && (!g_LaunchFilters.empty() || g_LaunchObserver)) {
        char* text = static_cast<char*>(LaunchDataPage) + 0x400;
        std::string arguments(text, strnlen(text, 3072));
        const std::string before = arguments;
        for (LaunchFilter filter : g_LaunchFilters)
            filter(arguments);
        if (g_LaunchObserver)
            g_LaunchObserver(arguments);
        if (arguments != before) {
            std::memset(text, 0, 3072);
            std::memcpy(text, arguments.data(), std::min<size_t>(arguments.size(), 3071));
        }
    }
    const char* path = LaunchDataPage ? static_cast<const char*>(LaunchDataPage) + 8 : "";
    LOG_INFO("HalReturnToFirmware(%lu), launch path '%s'", Routine, path);
    LogFlush();
    if (Routine == 2 && LaunchDataPage) {
        std::string p = path;
        for (char& c : p) c = char(tolower(static_cast<unsigned char>(c)));
        if (p.find("default.xbe") != std::string::npos) {
            LogFlush();
            if (g_InProcessReboot)
                RebootInProcess(LaunchDataPage);
            LOG_INFO("Relaunching the game process with launch data");
            RelaunchProcess(LaunchDataPage);
        }
    }
    ExitProcess(0);
}

void XBAPI HalInitiateShutdown()
{
    HalReturnToFirmware(0);
}

BOOLEAN XBAPI HalIsResetOrShutdownPending() { return FALSE; }

void XBAPI HalRegisterShutdownNotification(void* ShutdownRegistration, BOOLEAN Register)
{
    (void)ShutdownRegistration;
    (void)Register;
}

void XBAPI KeBugCheck(ULONG BugCheckCode)
{
    Fatal("The game stopped with Xbox bug check 0x%08lX", BugCheckCode);
}

ULONG __cdecl DbgPrint(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), Format, args);
    va_end(args);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    LOG_INFO("[game] %s", buf);
    return 0;
}

// Video encoder queries. XDK display-mode enumeration asks for the AV
// capabilities: report a component (HDTV) cable, NTSC-M 60 Hz, plus the
// dashboard video options from XC_VIDEO.
void XBAPI AvSendTVEncoderOption(void* RegisterBase, ULONG Option, ULONG Param, ULONG* Result)
{
    (void)RegisterBase;
    (void)Param;
    constexpr ULONG AV_QUERY_AV_CAPABILITIES = 6;
    constexpr ULONG AV_PACK_HDTV = 0x00000004;
    constexpr ULONG AV_STANDARD_NTSC_M = 0x00000100;
    constexpr ULONG AV_FLAGS_60Hz = 0x00400000;
    if (!Result)
        return;
    if (Option == AV_QUERY_AV_CAPABILITIES)
        *Result = AV_PACK_HDTV | AV_STANDARD_NTSC_M | AV_FLAGS_60Hz | VideoFlags();
    else
        *Result = 0;
}

KERNEL_EXPORT(2, AvSendTVEncoderOption);
KERNEL_EXPORT(49, HalReturnToFirmware);
KERNEL_EXPORT(360, HalInitiateShutdown);
KERNEL_EXPORT(358, HalIsResetOrShutdownPending);
KERNEL_EXPORT(47, HalRegisterShutdownNotification);
KERNEL_EXPORT(95, KeBugCheck);
KERNEL_EXPORT(8, DbgPrint);

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
void Init(const XbeFile& xbe, const Paths& paths)
{
    g_Xbe = &xbe;
    // Launch data from a previous game process that rebooted into the game (the fallback).
    g_LaunchDataFile = paths.cache + L"\\launchdata.bin";
    if (wcsstr(GetCommandLineW(), L"--relaunch")) {
        HANDLE f = CreateFileW(g_LaunchDataFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<uint8_t> data(4096);
            DWORD read = 0;
            ReadFile(f, data.data(), 4096, &read, nullptr);
            CloseHandle(f);
            if (read == 4096)
                g_PendingLaunchData = std::move(data);
        }
    }
    DeleteFileW(g_LaunchDataFile.c_str()); // a normal start must not see it
    QueryPerformanceFrequency(&g_QpcFreq);
    QueryPerformanceCounter(&g_BootQpc);
    std::thread(TickThreadMain).detach();
    FileSystemInit(paths);
    ThreadingInit(xbe);
}

// Each start of the game image: the launch data page the Xbox kernel keeps across a
// reboot, in contiguous memory as there.
void BootInit(const void* launchData)
{
    SchedBoot(); // the deterministic scheduler, when a netplay match asked for it
    if (!launchData && !g_PendingLaunchData.empty())
        launchData = g_PendingLaunchData.data();
    LaunchDataPage = nullptr;
    g_BootArguments.clear();
    if (launchData) {
        if (launchData != g_MissionLaunchData.data()) {
            const auto* data = static_cast<const uint8_t*>(launchData);
            g_MissionLaunchData.assign(data, data + 4096);
        }
        void* page = MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
        std::memcpy(page, launchData, 4096);
        LaunchDataPage = page;
        char* text = static_cast<char*>(page) + 0x400;
        g_BootArguments.assign(text, strnlen(text, 3072));
        // The port's own arguments for the dev map and Heroes vs Villains (game/devmap.h, game/hvv.h) belong to its
        // level alone: the game's level changes keep every argument but the map (a Quit from a match to the
        // menus would carry them into every later level), so they go unless the map is the dev map's level.
        static const char* const kLevelOnly[] = { "devmap:", "custommap:", "hvv", "versus:" };
        std::string lower = " " + g_BootArguments + " ";
        for (char& c : lower)
            c = char(tolower(static_cast<unsigned char>(c)));
        const bool devMapLevel = lower.find(" map:u107_uta_grievoussurvival ") != std::string::npos || // (not previousmap:)
            (lower.find(" map:u107_uta_grievous_01 ") != std::string::npos && lower.find(" versus:1 ") != std::string::npos);
        if (!devMapLevel) {
            std::string kept, word;
            for (size_t i = 0; i <= g_BootArguments.size(); ++i) {
                if (i < g_BootArguments.size() && g_BootArguments[i] != ' ') {
                    word += g_BootArguments[i];
                    continue;
                }
                bool levelOnly = false;
                for (const char* prefix : kLevelOnly)
                    levelOnly |= _strnicmp(word.c_str(), prefix, strlen(prefix)) == 0;
                if (!word.empty() && !levelOnly)
                    kept += (kept.empty() ? "" : " ") + word;
                word.clear();
            }
            if (kept.size() != g_BootArguments.size()) {
                g_BootArguments = kept;
                std::memset(text, 0, 3072);
                std::memcpy(text, kept.data(), std::min<size_t>(kept.size(), 3071));
            }
        }
        // only the level from the arguments: they can hold the player's profile name, and logs get shared
        std::string map;
        const size_t at = g_BootArguments.find("map:");
        if (at != std::string::npos && (at == 0 || g_BootArguments[at - 1] == ' '))
            map = g_BootArguments.substr(at, g_BootArguments.find(' ', at) - at);
        LOG_INFO("Launch data handed over (type %lu)%s%s", *static_cast<DWORD*>(page), map.empty() ? "" : ": ",
            map.c_str());
    }
    g_PendingLaunchData.clear();
}

} // namespace swrots::kernel
