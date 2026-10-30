#pragma once
// Native implementation of the Xbox kernel services the game imports.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kernel/xbox.h"

namespace swrots {
class XbeFile;
}

namespace swrots::kernel {

struct Paths {
    std::wstring gameData; // \Device\CdRom0 (D:), the game disc
    std::wstring saves;    // \Device\Harddisk0\Partition1 (E:), the Xbox save drive
    std::wstring cache;    // \Device\Harddisk0\Partition3 (Z:, the utility drive) and temporary files
    std::wstring mods;     // loose files that override the disc (D:)
    std::wstring logs;     // the game's own log (it writes D:\\Message.log), next to ours
};

// Order matters: Init once per process; BootInit each time the game image is (re)started,
// with the launch data page handed over by a reboot (or null).
void Init(const XbeFile& xbe, const Paths& paths);
void BootInit(const void* launchData);
// The game's reboots (HalReturnToFirmware into its own XBE): in this process by default
// (reboot.cpp); otherwise, or as the fallback, the game process exits for the frame to
// start it again with the launch data (core/window.h).
void SetInProcessReboot(bool enabled);
// Called just before the game process is started again (the reboot fallback), e.g. to hand the new
// process what the port remembers across reboots through its environment.
void SetBeforeRelaunch(void (*callback)());
// Starts the game again with its current launch data, as the game's own restarts do: the running
// mission from its start. Any thread; returns at once.
void RestartMission();
// Starts the game again with `arguments` as its launch data, a line of launch arguments as in Default_Xbox.cfg
// (e.g. "map:u112_mus_lava"), as the game's own level changes do. Any thread; returns at once.
void LaunchGame(const std::string& arguments);
// The launch arguments this start of the game was given (empty when none: a start from the dashboard or
// Default_Xbox.cfg).
const std::string& LaunchArguments();
// A disc file served from elsewhere for this start of the game: `rest` is its path on the disc (e.g.
// `\audio\xbox\mus_x.hwx`, any case), `host` the file's path on the PC. It comes before mods\ (cleared with an
// empty `host`; every start begins with none).
void SetDiscFileOverride(const std::string& rest, const std::wstring& host);
// A filter over the launch arguments the game writes for its own level changes (XLaunchNewImage), run just before
// it restarts: it may change them (e.g. the level). Several can be set (each once), run in order.
using LaunchFilter = void (*)(std::string& arguments);
void SetLaunchFilter(LaunchFilter filter);
// Development aid: restarts the game in this process every `seconds` (SWROTS_REBOOT_EVERY), as a level
// change does, to test what a restart leaves behind.
void StartRebootTest(unsigned seconds);
[[noreturn]] void RelaunchProcess(const void* launchData);
void InstallThunks(uint32_t thunkTableAddress);
void InstallFsPatches();

// --- Threads (thread.cpp) ------------------------------------------------

using StartRoutine = void(__stdcall*)(void* context);
using SystemRoutine = void(__stdcall*)(StartRoutine start, void* context);

struct XboxThread {
    // Must be first: the game receives pointers to this as KTHREAD*/ETHREAD*.
    alignas(16) uint8_t ethread[xbox::ETHREAD_Size];
    xbox::KPCR pcr;
    HANDLE hostHandle = nullptr;
    DWORD threadId = 0;
    LONG basePriority = 0;
    uint8_t* tlsData = nullptr;
    uint32_t tlsSize = 0;
    StartRoutine start = nullptr;
    void* context = nullptr;
    SystemRoutine system = nullptr;
    // Deterministic addresses (netplay): the thread object and TLS are in the fixed range (not freed with the
    // heap), and the thread runs on a stack there.
    bool fixedMemory = false;
    uint8_t* fixedStack = nullptr;
    uint32_t fixedStackSize = 0;
};

void ThreadingInit(const XbeFile& xbe);
XboxThread* CurrentThread();
// Gives a host-created thread the Xbox per-thread state so it may call game code.
XboxThread* AdoptCurrentThread();
XboxThread* ThreadFromKThread(void* kthread);
XboxThread* ThreadFromHandle(HANDLE handle);
// Calls `fn` for every thread that runs game code.
void ForEachThread(void (*fn)(XboxThread* t, void* ctx), void* ctx);
HANDLE CreateXboxThread(StartRoutine start, void* context, SystemRoutine system, uint32_t stackSize,
    uint32_t tlsSize, bool suspended, DWORD* threadId);
// Keeps a thread that runs game code on the one CPU core shared by all of them.
void PinToGameCpu(HANDLE thread);

// --- IRQL / DPC emulation (ke.cpp) ---------------------------------------
// On the single-core Xbox, raising IRQL to DISPATCH_LEVEL stops preemption, and
// XAPI relies on that as a lock. Here it takes a process-wide lock instead.
constexpr UCHAR kPassiveLevel = 0;
constexpr UCHAR kApcLevel = 1;
constexpr UCHAR kDispatchLevel = 2;
UCHAR RaiseIrql(UCHAR newIrql);
void LowerIrql(UCHAR newIrql);

// --- Timeouts -------------------------------------------------------------
// NT-style timeout (null = infinite, negative = relative 100ns, positive =
// absolute system time) converted to milliseconds.
DWORD TimeoutToMs(const LARGE_INTEGER* timeout);

// --- Reboot resets (see reboot.h) -----------------------------------------
void ResetThreadsForReboot();
void ResetFileSystemForReboot();
void ResetMemoryForReboot();

// What the game has allocated: the Xbox's contiguous memory in use (and its size), the virtual memory
// it committed outside it, and its pool blocks. Any thread.
struct MemoryUsage {
    uint64_t contiguousUsed = 0;
    uint64_t contiguousSize = 0;
    uint64_t virtualCommitted = 0;
    size_t poolBlocks = 0;
};
MemoryUsage QueryMemoryUsage();
// The game's committed memory regions (contiguous allocations and the committed parts of its virtual
// allocations), for research scans. Any thread.
std::vector<std::pair<uintptr_t, size_t>> GameMemoryRegions();
// The game's pool blocks (ExAllocatePoolWithTag: on the PC's heap) and their sizes. Any thread.
std::vector<std::pair<uintptr_t, size_t>> GamePoolBlocks();
// A number that moves on whenever the game allocates or frees memory (contiguous, virtual, system or pool), so
// netplay's snapshots (net/state.h) know when the memory they cover has changed shape. Any thread.
uint32_t MemoryLayoutGeneration();
// Netplay: while on, pool blocks (ExAllocatePoolWithTag) come from a deterministic allocator in an arena of the
// game's virtual memory, which the snapshots cover, and heap blocks the game frees are only freed when it is turned
// off (a rollback could give one back). The arena is made the first time (a change of the memory layout). False when
// it cannot be made. On the game's main thread at a frame boundary.
bool SetDeterministicPool(bool on);
// Netplay: while on (a match, from the reboot into its level), the game's virtual memory and pool blocks are placed by
// the port in a range reserved at a fixed address (0x20000000, 256 MiB), first-fit, so that every PC gives the game
// the same addresses. Stays on across reboots. False when the range is taken on this PC.
bool SetDeterministicAddresses(bool on);
bool DeterministicAddresses();
// While deterministic addresses are on: committed memory for a game thread's own use (its stack, its thread object
// and TLS), at an address the same on every PC, outside what netplay's snapshots cover. Null otherwise. Freed by
// the next reboot.
void* AllocFixedThreadMemory(uint32_t size);
// Memory the port hands to the game (a sound buffer's data, a surface header, an event object): zeroed, from the
// netplay pool's arena while deterministic addresses are on (the same address on every PC, saved with the game),
// else from the heap. GameFree takes either.
void* GameAlloc(uint32_t size);
void GameFree(void* p);

// --- File system (file.cpp) ------------------------------------------------
void FileSystemInit(const Paths& paths);
void CreateSymbolicLink(const std::string& link, const std::string& target);

} // namespace swrots::kernel
