#pragma once
// Facts about the XBE this port runs: the retail NTSC release of
// Star Wars: Episode III - Revenge of the Sith (LA-023, v5.0, XDK 5849).
// Every address below is that exact binary's, which is why the runtime refuses
// to start on anything with a different MD5. The PAL release (France, Europe)
// is the same code linked a little differently: its addresses are found from
// these ones (game/release.h).

#include <cstdint>

#include "game/release.h"

namespace swrots::game {

inline constexpr uint32_t kTitleId = 0x4C410017;

inline constexpr uint32_t kImageBase = 0x00010000;
inline constexpr uint32_t kImageEnd = 0x0096E000; // end of the last section, page aligned
// Where the loader's PE headers are preserved (unused tail of swrots.exe's
// reserved range; must stay within 256 MiB of the image base for e_lfanew).
inline constexpr uint32_t kPeHeaderCopy = 0x00FF0000;

// XAPI / CRT entry points (statically linked into .text, run natively).
inline GAME_VA(kMainCrtStartup, 0x004A363C);
inline GAME_VA(kQueryPerformanceCounter, 0x004A341F);
inline GAME_VA(kQueryPerformanceFrequency, 0x004A3430); // hardcodes 733,333,333 Hz
// Engine timer "now": rdtsc >> 4, read as 733 MHz / 16 ticks (the timer divides
// by a hardcoded 45833.3 ticks per millisecond). Drives the frame limiter and delta time.
inline GAME_VA(kEngineTimerNow, 0x00227170);
// SceneResPacker::Read (thiscall, 7 args): reads one resource from the level PAK,
// or through StdFile when the packer's force-direct flag (+8) is set.
inline GAME_VA(kSceneResRead, 0x000527F0);
// EFilePath::Build (thiscall: char* out, bool dirOnly) -> root + dir [+ name].
inline GAME_VA(kEnginePathBuild, 0x00233420);
// EFilePath::String (thiscall: bool dirOnly) -> static buffer with root + dir [+ name].
inline GAME_VA(kEnginePathString, 0x00233520);
inline GAME_VA(kEngineStrCopy, 0x00223300);   // strcpy (cdecl)
inline GAME_VA(kEngineFileSize, 0x00230C70);  // cdecl (const EFilePath*)
inline GAME_VA(kStdFileCtor, 0x00232A10);     // thiscall (uint32_t), object 0x194 bytes
inline GAME_VA(kPackReaderCtor, 0x00232D20);  // thiscall (StdFile*, int), object 0x24 bytes
// TFileMemory (StdFile.cpp): a file object over a memory buffer. Open (vtable +0x40, thiscall
// (const void* data, uint32_t size)) copies the data into its own engine allocation; slot 0 is the
// deleting destructor.
inline GAME_VA(kFileMemoryCtor, 0x002326D0);   // thiscall (), object 0x6C bytes
inline constexpr uint32_t kFileMemorySize = 0x6C;
inline constexpr uint32_t kFileMemoryOpenSlot = 0x40;
// SceneResPacker fields. Memory-image resources (animations) are not decoded from the stream:
// Read hands a pointer into the PAK's memory-image block to the type's loader
// (StdSimpleMemoryImageManager, loaders[typeId] at +0xA8, vtable +0x10 thiscall (void* image,
// uint32_t size), which relocates the image in place), sets +0x89 and returns the pointer.
inline constexpr uint32_t kPackerImageLoaders = 0xA8;
inline constexpr uint32_t kPackerLastWasImage = 0x89;
inline constexpr uint32_t kPackerImageBlock = 0x16C; // -> the level PAK's memory-image block, in memory
inline constexpr uint32_t kImageLoaderLoadSlot = 0x10;
// Manual PAK entries (e.g. .ban): thiscall packer (const EFilePath*) -> the packer's reader at the
// entry's data, or null when the entry at the stream position is not it (ret 4).
inline GAME_VA(kPackerOpenManual, 0x00051F80);
// Declared resources (SceneRes / SceneResTypes.cpp). A scene loads only resources its PAK header
// declared: the load method (thiscall (const EFilePath*, int typeId, int flags), ret 0xC) finds the
// path in the type's table and returns null when it is not there, without reading anything.
//   scene +0x14F8 + typeId*4: type table {+0 extension, +0x10 map {capacity, count, items, strings}}
//     map items {u16 key (path without extension, string index), u16, group*}, sorted by key;
//     group {u16, u16 key, node* first}; node (20 bytes) {group*, next, loaded, u16 folder,
//     u16 name, u32 size}
//   scene +0x15C0 folders (0x26 bytes {name, u16 parent, u16 name length}), +0x15C4 count: sized
//     exactly from the PAK header, so it must grow before a folder is added
//   scene +0x17DC string table (grows as needed)
inline GAME_VA(kSceneLoadResource, 0x00050930);
inline GAME_VA(kSceneIsDeclared, 0x0004BDA0);   // thiscall scene (const EFilePath*, int typeId) -> bool
inline constexpr uint32_t kSceneTypeTables = 0x14F8;
inline constexpr uint32_t kSceneFolders = 0x15C0;
inline constexpr uint32_t kSceneFolderCount = 0x15C4;
inline constexpr uint32_t kSceneFolderSize = 0x26;
inline constexpr uint32_t kTypeTableMap = 0x10;
inline GAME_VA(kSceneAddString, 0x0004F160);    // thiscall scene (const char*) -> u16 index
// The scene's string table (StdStringTable): {+0 capacity, +4 free bytes, +8 used, +0xC offsets,
// +0x18 buffer}; growing (thiscall, 0x2B600) doubles the buffer and moves it.
inline constexpr uint32_t kSceneStrings = 0x17DC;
inline constexpr uint32_t kStringTableFree = 0x04;
inline GAME_VA(kStringTableGrow, 0x0002B600);
inline GAME_VA(kNodeSetPath, 0x00055250);       // thiscall node (const EFilePath*, scene); adds folders
inline GAME_VA(kResourceMapFind, 0x00055CC0);   // thiscall map (const char* key) -> index or -1
inline GAME_VA(kResourceMapInsert, 0x00056610); // thiscall map (u16 key, group*)
inline GAME_VA(kEnginePathKey, 0x00233200);     // thiscall EFilePath (1) -> path without extension
inline GAME_VA(kEnginePathFromText, 0x00233B70); // thiscall EFilePath (const char*, int kind)
// The game layer's context (set by 0x1FA880). +0x148: the animation lookup object (SceneBoneAnimAPI,
// vtable 0x564CA8), whose +0x50 means "index not built yet" (0x65C50 builds it and clears the flag).
inline GAME_VA(kGameContext, 0x006966C0);
inline constexpr uint32_t kGameContextAnimationIndex = 0x148;
inline constexpr uint32_t kAnimationIndexStale = 0x50;
// The animation lookup (the lookup object's vtable slot 0): thiscall (script, classInfo, arg,
// TArray* found, TArray* missing, int), ret 0x18; `missing` ({capacity, count, char** names}) gets
// the required animations it could not find, which fails the character (0x1F8AB0).
inline GAME_VA(kAnimationLookup, 0x00065C50);
// Opens a character table (cdecl (const char* name) -> table) at Meshes\Chars\Common\<name>.csv,
// only if the level declares it ("Couldn't open character table file" otherwise).
inline GAME_VA(kOpenCharacterTable, 0x001B2900);
// Loads a character's event table (ScriptEvent.cpp; cdecl (character) -> table) at
// Meshes\Chars\Common\<name>.csv, the name at [[character + 0x434] + 4] + 0x2C (e.g. a_Yoda).
inline GAME_VA(kLoadEventTable, 0x001B2AC0);
// CharEventManager.cpp: gets or loads an AnimEventTree (s_<character>.xml), cdecl (const EFilePath*).
inline GAME_VA(kLoadAnimEventTree, 0x0014B5B0);
inline GAME_VA(kEngineServices, 0x00645F7C);  // -> services table (+0x50 alloc, +0x38 engine core)
// Renderer object = [[kEngineServices] + 0x38] + 8; its int fpsLimit (vars_xbox.cfg) drives the frame limiter.
inline constexpr uint32_t kRendererFpsLimit = 0xB8;
// Options object byte fields ([[kEngineServices] + 0x38] + 8), registered console variables unless noted.
inline constexpr uint32_t kOptionsFps = 0x38;        // fps: draw the fps counter
inline constexpr uint32_t kOptionsAiDisabled = 0xC4; // aiDisabled
inline constexpr uint32_t kOptionsGod = 0xC9;        // god
inline constexpr uint32_t kOptionsHideDebugDisplays = 0xD7; // not a variable: 1 hides the debug displays
// The engine console (console.cpp), vtable kConsoleVtable, created by the engine at startup and
// kept at [[kLaunchSettingsHolder] + 0x48]. Its commands (help, listvars, set...) reach it
// through the global kConsole, which the Xbox build never sets (the variable setters check it
// for null, the commands do not). Its text output is not in the Xbox build: the print slots are
// empty stubs.
inline GAME_VA(kLaunchSettingsHolder, 0x0066F7A4); // [+4] launch settings, [+0x48] console
inline constexpr uint32_t kConsoleInHolder = 0x48;
inline GAME_VA(kConsole, 0x0065E360);
inline GAME_VA(kConsoleVtable, 0x0055FF58);
inline constexpr uint32_t kConsoleExecuteSlot = 0x04; // thiscall (const char* line, bool echo)
inline constexpr uint32_t kConsolePrintSlot = 0x50;   // thiscall (const TString* text); "> line", results
inline constexpr uint32_t kConsolePrintTextSlot = 0x54; // thiscall (const char* text); command output
// The console's variable registry (console +0x1C, vtable 0x55FF0C) and command table (+0x2B4). Both
// keep a hash table of 37 buckets at +4: {vtable, bucket count, buckets[37]}, nodes {value, name, next}.
// In the Xbox build the built-in commands that enumerate them (help, listvars) and `set` (registry
// slot +0x08) are empty stubs; the registry's find (+0x04) and set (+0x1C, as vars_xbox.cfg uses) work.
inline constexpr uint32_t kConsoleRegistry = 0x1C;
inline constexpr uint32_t kConsoleCommands = 0x2B4;
inline constexpr uint32_t kRegistryFindSlot = 0x04;   // thiscall (const char* name) -> variable or null
inline constexpr uint32_t kRegistrySetSlot = 0x1C;    // thiscall (const char* name, const char* value, int)
inline constexpr uint32_t kHashBuckets = 0x0C;        // from the owner: +4 table, +8 count, +0xC buckets
// Variable objects: vtable {Get(TString* out), Set(const TString* in), TypeName()}, +0x10 -> the value.
inline GAME_VA(kTStringCtor, 0x00225100);  // thiscall (): empty string
inline GAME_VA(kTStringDtor, 0x00225230);  // thiscall ()
inline GAME_VA(kTStringAssign, 0x00225A10); // thiscall (const char*)
// Creates the level's player (Manager_Base.cpp; cdecl (TString* class, int variant), -1 = the level's
// default) through the character factory ([0x692DF4] vtable +0x74, by class name), named Player1.
inline GAME_VA(kSpawnPlayer, 0x000B1F90);
// Unlocks everything in the signed-in profile (TVaderGameOptions' developer command "unlockprofile",
// not registered in the retail build; cdecl ()): story progress, the fighters' unlock bytes
// (+0x2A8..), arenas, bonus missions, concept art. The profile is saved with it by the game.
inline GAME_VA(kUnlockProfile, 0x002E59F0);
// Versus character select (ShellSelectJediHandlers.cpp): class names of its 9 duelists (IAnakin,
// IObiwan, IDooku, IGrievous, IMace, ISerra, ICinDrallig, IVader, IOldObiwan). Confirming copies the
// chosen entries to the player setting (options +0xAC) and the duel's player 1/2 classes.
inline GAME_VA(kDuelistClasses, 0x00650C08);
inline constexpr int kDuelistCount = 9;
// Character variant per duelist slot and player (int [slot * 2 + player]): Anakin and Obi-Wan use their
// duel variants (Anakin_Duel, ...), the others 0. A slot given another class needs its default (0).
inline GAME_VA(kDuelistVariants, 0x005CC1C0);
// Class registry (InstanceManager.cpp), created on first use: [kClassRegistry] -> {.., +8 buckets[237]},
// nodes {classInfo, const char* name, next}. Lookups compare names as text (with +0x3BE set they
// would compare addresses; the retail game leaves it clear).
inline GAME_VA(kClassRegistry, 0x0068EBE8);
inline constexpr uint32_t kClassRegistryBuckets = 0x08;
inline constexpr int kClassRegistryBucketCount = 237;
// Character variants (costumes): each class's list is static data in .data -- a 0x2C-byte header whose
// first field names the class (e.g. "Anakin" for IAnakin), then records of 20 bytes {const char* name,
// const char* mesh (under meshes\chars\, e.g. "AnakinDuel\AnakinDuel"), ?, code, ?} until a null
// name. A character's constructor points its +0x1E0 at its class's list.
inline constexpr uint32_t kCharacterVariants = 0x1E0;
inline constexpr uint32_t kVariantRecords = 0x2C;
inline constexpr uint32_t kVariantRecordSize = 20;
inline GAME_VA(kRDataStart, 0x0055F660);
inline GAME_VA_END(kRDataEnd, 0x00612BC8);
inline GAME_VA(kDataStart, 0x00612BE0);
inline GAME_VA_END(kDataEnd, 0x00962BF8);
inline GAME_VA(kUnhandledExceptionFilter, 0x004A319F);
inline GAME_VA(kXMountUtilityDrive, 0x004A0A7B);
inline GAME_VA(kXFormatUtilityDrive, 0x004A0B80);
// XAPI startup routine that locates the Xbox kernel's INIT section and edits a
// GDT descriptor (privileged); meaningless on Windows, so it becomes a RET.
inline GAME_VA(kXapiReclaimKernelInit, 0x004A4E16);
// XAPI CancelIo: walks the kernel's per-thread IRP list; replaced by the host's.
inline GAME_VA(kXapiCancelIo, 0x004EE8F9);

// Instructions that address the Xbox kernel's per-processor/per-thread data
// through the FS segment. On Windows FS points at the TEB instead, so each one
// is rewritten into a call to a stub that reads our emulated KPCR.
enum class FsKind : uint8_t {
    Fs20Eax, // mov eax, fs:[0x20]         KPCR.Prcb
    Fs24Eax, // movzx eax, byte fs:[0x24]  KPCR.Irql
    Fs28Eax, // mov eax, fs:[0x28]         KPCR.PrcbData.CurrentThread
    Fs04Ecx, // mov ecx, fs:[4]            KPCR.NtTib.StackBase (TLS array)
    Fs04Edi, // mov edi, fs:[4]
};

struct FsPatchSite {
    uint32_t address;
    uint8_t length;
    FsKind kind;
};

// fs:[0] (the SEH chain) is layout-compatible with Windows and left alone. North American addresses
// (game::Va gives the running release's).
inline constexpr FsPatchSite kFsPatchSites[] = {
    { 0x004A054D, 8, FsKind::Fs24Eax },
    { 0x004A0559, 6, FsKind::Fs28Eax },
    { 0x004A0564, 7, FsKind::Fs04Ecx },
    { 0x004A0575, 8, FsKind::Fs24Eax },
    { 0x004A0581, 6, FsKind::Fs28Eax },
    { 0x004A0587, 7, FsKind::Fs04Ecx },
    { 0x004A18EB, 6, FsKind::Fs20Eax },
    { 0x004A319F, 6, FsKind::Fs20Eax },
    { 0x004A31EB, 6, FsKind::Fs28Eax },
    { 0x004A32B8, 8, FsKind::Fs24Eax },
    { 0x004A32C4, 6, FsKind::Fs28Eax },
    { 0x004A32E1, 6, FsKind::Fs28Eax },
    { 0x004A35D2, 6, FsKind::Fs20Eax },
    { 0x004A35EE, 6, FsKind::Fs28Eax },
    { 0x004A35F4, 7, FsKind::Fs04Edi },
    { 0x004A4B69, 6, FsKind::Fs20Eax },
    { 0x004A4F29, 6, FsKind::Fs20Eax },
    { 0x004A6BFB, 6, FsKind::Fs20Eax },
    { 0x004A6C6F, 6, FsKind::Fs20Eax },
    { 0x004A74EE, 6, FsKind::Fs20Eax },
    { 0x004A751D, 6, FsKind::Fs20Eax },
    { 0x004E8610, 8, FsKind::Fs24Eax },
    { 0x004E8626, 6, FsKind::Fs28Eax },
    { 0x004E8637, 7, FsKind::Fs04Ecx },
    { 0x004E875B, 8, FsKind::Fs24Eax },
    { 0x004E8770, 6, FsKind::Fs28Eax },
    { 0x004E8781, 7, FsKind::Fs04Ecx },
    { 0x004E87D5, 7, FsKind::Fs04Ecx },
};

// Statically linked XDK symbols (see tools/gen_symbols.py).
enum class SymbolKind : uint8_t { Sdk, Xapi, Data };

struct SdkSymbol {
    uint32_t address;
    const char* name;
    SymbolKind kind;
};

const SdkSymbol* SdkSymbols(uint32_t* count);
const SdkSymbol* FindSdkSymbol(const char* name);
// Nearest symbol at or below `address` (for crash reports); may be null.
const SdkSymbol* NearestSdkSymbol(uint32_t address);

// Reconstructed name of the game function containing `address`, from symbols\swrots.map
// next to the exe (written by tools/symbols; absent in normal installs). Fills `out` with
// "Name+0xOFF" and returns true if found.
bool DescribeGameAddress(uint32_t address, char* out, size_t size);

// Debugging: logs up to `max` return addresses into game code found on the stack from `stackTop`
// (values right after a CALL), named where the symbol map knows them. Old frames can show up too.
void LogGameCallers(const char* tag, const void* stackTop, int max = 16);

} // namespace swrots::game
