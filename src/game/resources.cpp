// Loose-file resource overrides.
//
// The engine's resource reader (SceneResPacker::Read, game::kSceneResRead)
// reads each resource from the level PAK: it parses the resource's PAK header
// (which records where the resource ends), hands a PackFile reader to a
// per-type decode callback, then seeks the PAK to the resource's end. The
// engine also has a direct file I/O branch (development builds) that opens the
// resource's own path, e.g. d:\textures\levels\...\floor.stx, through StdFile.
//
// When mods\ has a loose copy of a resource, the hook keeps the PAK path (so
// the PAK stream stays in step) but wraps the decode callback: the wrapper
// builds the same StdFile + PackFile reader objects the direct I/O branch uses
// and decodes the loose file instead of the packed copy. The file system layer
// maps d:\ paths to mods\ when an override exists.
//
// A level PAK holds only what the level was built with, in the order it asks for
// it. A resource the level's PAK does not have further on (a character it never
// loads, e.g. Yoda in a versus arena) is taken from another PAK on the disc
// instead, without touching the level's stream: its data is decoded from an
// engine memory file (TFileMemory), or, for a memory-image resource (animations),
// copied into engine memory and handed to the type's in-place loader, as Read
// does with the level's own memory-image block.

#include "game/resources.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "debug/console.h"
#include "game/game.h"
#include "game/pak.h"

namespace swrots::game {

namespace {

// Engine path object (EFilePath): the full path is root(rootIndex) + dir + name.
struct EnginePath {
    uint8_t unknown0[4];
    uint8_t dirIsRoot;
    uint8_t pad;
    uint16_t rootIndex;
    char dir[64];
    char name[64];
};

// Engine objects with a vtable (StdFile, PackFile reader).
struct EngineObject {
    void** vtable;
};

// What SceneResPacker::Read hands its decode callback (a stack local in Read).
struct ReadSource {
    EngineObject* reader; // PackFile reader positioned at the resource data
    EnginePath* path;
    uint8_t fromPak;      // 1: PAK read, decoders may defer to the PAK (textures
                          //    become placeholders filled when precaching)
    uint8_t pad[3];
    void* record;         // resource record; +0x10 = raw data size
};

// Decode callback passed to SceneResPacker::Read: (scene, source, typeId, extra).
using DecodeFn = int(__cdecl*)(int scene, ReadSource* source, int typeId, int extra);

using ReadFn = int(__fastcall*)(void* packer, void* edx, DecodeFn decode, EnginePath* path, int typeId,
    void* record, int a5, int extra, int context);

// Engine helpers (see game.h).
using BuildPathFn = void(__fastcall*)(const EnginePath* path, void* edx, char* out, int dirOnly);
using PathStringFn = const char*(__fastcall*)(const EnginePath* path, void* edx, int dirOnly);
using StrCopyFn = char*(__cdecl*)(char* dst, const char* src);
using FileSizeFn = uint32_t(__cdecl*)(const EnginePath* path);
using StdFileCtorFn = EngineObject*(__fastcall*)(void* mem, void* edx, uint32_t arg);
using PackReaderCtorFn = EngineObject*(__fastcall*)(void* mem, void* edx, EngineObject* file, int arg);
using AllocFn = void*(__cdecl*)(uint32_t size);
using FileMemoryCtorFn = EngineObject*(__fastcall*)(void* mem, void* edx);
using FileMemoryOpenFn = bool(__fastcall*)(EngineObject* file, void* edx, const void* data, uint32_t size);
using ImageLoadFn = void(__fastcall*)(EngineObject* loader, void* edx, void* image, uint32_t size);

const auto BuildPath = reinterpret_cast<BuildPathFn>(uintptr_t(kEnginePathBuild));
const auto PathString = reinterpret_cast<PathStringFn>(uintptr_t(kEnginePathString));
const auto StrCopy = reinterpret_cast<StrCopyFn>(uintptr_t(kEngineStrCopy));
const auto FileSize = reinterpret_cast<FileSizeFn>(uintptr_t(kEngineFileSize));
const auto StdFileCtor = reinterpret_cast<StdFileCtorFn>(uintptr_t(kStdFileCtor));
const auto PackReaderCtor = reinterpret_cast<PackReaderCtorFn>(uintptr_t(kPackReaderCtor));
const auto FileMemoryCtor = reinterpret_cast<FileMemoryCtorFn>(uintptr_t(kFileMemoryCtor));

// Global engine services table (alloc at +0x50, StdFile gate at +0x38).
uint8_t* Services() { return *reinterpret_cast<uint8_t**>(uintptr_t(kEngineServices)); }

ReadFn g_OriginalRead = nullptr;
bool g_EngineMessageHookInstalled = false; // per boot: the services table is the game's
bool g_LogResources = false;
std::wstring g_Mods;
std::wstring g_GeneratedDir; // cache\disc: generated resources, served as loose files
std::wstring g_DumpDir; // empty: dumping disabled

// Reads in flight on this thread. Decoding a resource can load others (a level
// loads its contents), so Read re-enters; each wrapped decode takes the top
// entry on entry, before any nested read pushes its own.
struct PendingRead {
    DecodeFn decode;
    const char* path;
    uint8_t* packer;
    bool used;
    std::string* capture; // dumper: bytes read while decoding
    PendingRead* outer;
};
thread_local PendingRead* t_Top = nullptr;

template <typename Fn> Fn VirtualAt(EngineObject* object, uint32_t offset)
{
    return reinterpret_cast<Fn>(object->vtable[offset / 4]);
}

std::wstring LooseHostPath(const char* fullPath);

uint32_t LooseFileSize(const char* fullPath)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(LooseHostPath(fullPath).c_str(), GetFileExInfoStandard, &data))
        return 0;
    return data.nFileSizeLow;
}

AllocFn EngineAlloc() { return *reinterpret_cast<AllocFn*>(Services() + 0x50); }

// Mirrors the engine's direct I/O branch: wraps an opened engine file in a
// PackFile reader, decodes it as a non-PAK read of `size` bytes, then closes and
// deletes both.
int DecodeFromFile(EngineObject* file, uint32_t size, int scene, ReadSource* pakSource, int typeId, int extra)
{
    PendingRead* pending = t_Top;
    pending->used = true;
    EngineObject* reader = PackReaderCtor(EngineAlloc()(0x24), nullptr, file, 0);
    if (pakSource->record) // the decoder's size
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pakSource->record) + 0x10) = size;

    ReadSource source = *pakSource;
    source.reader = reader;
    source.fromPak = 0;
    int result = pending->decode(scene, &source, typeId, extra);

    VirtualAt<void(__fastcall*)(EngineObject*, void*)>(file, 0x10)(file, nullptr);
    VirtualAt<void(__fastcall*)(EngineObject*, void*, int)>(file, 0x00)(file, nullptr, 1);
    VirtualAt<void(__fastcall*)(EngineObject*, void*, int)>(reader, 0x00)(reader, nullptr, 1);
    return result;
}

// A loose file under mods\: opened with StdFile at the resource path.
int DecodeLooseFile(int scene, ReadSource* pakSource, int typeId, int extra)
{
    PendingRead* pending = t_Top;
    uint8_t* gate = *reinterpret_cast<uint8_t**>(Services() + 0x38);
    EnginePath* path = pakSource->path;

    EngineObject* file = StdFileCtor(EngineAlloc()(0x194), nullptr, gate ? *reinterpret_cast<uint32_t*>(gate + 0x24) : 0);
    StrCopy(reinterpret_cast<char*>(file) + 0x64, PathString(path, nullptr, 1));
    VirtualAt<void(__fastcall*)(EngineObject*, void*, EnginePath*, int)>(file, 0x0C)(file, nullptr, path, 1);
    // The loose file's own size, from the host.
    uint32_t engineSize = FileSize(path);
    uint32_t hostSize = LooseFileSize(pending->path);
    if (pakSource->record && hostSize != engineSize)
        LOG_WARN("Loose resource %s: engine size %u, file size %u", pending->path, engineSize, hostSize);
    return DecodeFromFile(file, hostSize, scene, pakSource, typeId, extra);
}

// Resource dumper: saves each resource's raw bytes to dump\<path> as the game
// reads them. While a resource decodes, the underlying file object's read
// method (vtable +0x2C, which every PackFile read goes through) is swapped for
// a recording shim, so nothing is read twice and the stream is never moved.
constexpr int kFileVtableSlots = 32;
constexpr int kFileReadSlot = 0x2C / 4;
constexpr uint32_t kProxyMarker = 0x5357524F; // stored after the copied slots

using FileReadFn = int(__fastcall*)(EngineObject* file, void* edx, void* buffer, uint32_t size);

int __fastcall RecordingRead(EngineObject* file, void* edx, void* buffer, uint32_t size)
{
    auto original = reinterpret_cast<FileReadFn>(file->vtable[kFileVtableSlots + 1]);
    int result = original(file, edx, buffer, size);
    if (PendingRead* top = t_Top; top && top->capture)
        top->capture->append(static_cast<const char*>(buffer), size);
    return result;
}

void WriteDump(const char* fullPath, const void* data, uint32_t size)
{
    std::wstring out = g_DumpDir;
    for (const char* p = fullPath + 2; *p; ++p) { // skip "d:"
        wchar_t c = wchar_t(static_cast<unsigned char>(*p));
        if (c == L'\\')
            CreateDirectoryW(out.c_str(), nullptr);
        out += c;
    }
    if (GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES)
        return; // first copy wins (resources repeat across levels)
    HANDLE f = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
}

int __cdecl DecodeDumped(int scene, ReadSource* source, int typeId, int extra)
{
    PendingRead* pending = t_Top;
    EngineObject* file = source->reader ? *reinterpret_cast<EngineObject**>(reinterpret_cast<uint8_t*>(source->reader) + 0x14) : nullptr;
    if (!file)
        return pending->decode(scene, source, typeId, extra);

    std::string capture;
    pending->capture = &capture;
    void** original = file->vtable;
    bool proxied = reinterpret_cast<uint32_t>(original[kFileVtableSlots]) == kProxyMarker;
    void* proxy[kFileVtableSlots + 2];
    if (!proxied) {
        std::memcpy(proxy, original, kFileVtableSlots * sizeof(void*));
        proxy[kFileVtableSlots] = reinterpret_cast<void*>(kProxyMarker);
        proxy[kFileVtableSlots + 1] = original[kFileReadSlot];
        proxy[kFileReadSlot] = reinterpret_cast<void*>(&RecordingRead);
        file->vtable = proxy;
    }
    int result = pending->decode(scene, source, typeId, extra);
    if (!proxied)
        file->vtable = original;
    pending->capture = nullptr;

    if (!capture.empty())
        WriteDump(pending->path, capture.data(), uint32_t(capture.size()));
    else if (g_LogResources)
        LOG_INFO("Not dumped (data not read at load): %s", pending->path);
    return result;
}

// Engine warnings (services table +0x68, printf-style) are silent in the retail
// "FINAL" build; forward them to our log. Installed on the first resource read,
// once the engine has set up its services table.
void __cdecl EngineWarn(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    LOG_WARN("[engine] %s", text);
    debug::AddConsoleLine(debug::LineKind::Engine, text);
}

void InstallEngineMessageHook()
{
    static bool& installed = g_EngineMessageHookInstalled;
    if (installed || !Services())
        return;
    installed = true;
    *reinterpret_cast<void**>(Services() + 0x68) = reinterpret_cast<void*>(&EngineWarn);
}

// Resource paths are d:\<dir>\<name>; overrides live at mods\<dir>\<name>.
bool IsFile(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// A loose file for a resource: mods\<path>, else a generated one, cache\disc\<path> (the file layer
// opens the same files for the resource's d:\ path).
std::wstring LooseHostPath(const char* fullPath)
{
    std::wstring rest;
    for (const char* p = fullPath + 2; *p; ++p)
        rest += wchar_t(static_cast<unsigned char>(*p));
    std::wstring mod = g_Mods + rest;
    if (g_GeneratedDir.empty() || IsFile(mod))
        return mod;
    return g_GeneratedDir + rest;
}

bool HasLooseCopy(const char* fullPath)
{
    if (g_Mods.empty() || _strnicmp(fullPath, "d:\\", 3) != 0)
        return false;
    return IsFile(LooseHostPath(fullPath));
}

// PAK indexes by host path, kept for the session (resource reads come from
// more than one thread).
std::mutex g_IndexLock;
std::unordered_map<std::wstring, PakIndex> g_PakIndexes;
std::vector<const PakIndex*> g_DiscPaks; // every level PAK, for resources a level lacks
const PakIndex* g_LevelIndex = nullptr;  // the PAK the current level streams from
uint8_t* g_LevelScene = nullptr;         // the scene levels load into (the scene load hook's)
bool g_DiscPaksIndexed = false;
std::wstring g_GameData;

const PakIndex* IndexForHostPath(const std::wstring& host)
{
    auto it = g_PakIndexes.find(host);
    if (it == g_PakIndexes.end()) {
        it = g_PakIndexes.emplace(host, PakIndex()).first;
        if (it->second.Load(host))
            LOG_INFO("PAK index %ls: %zu entries", host.c_str(), it->second.Count());
        else
            LOG_WARN("PAK index %ls: could not read", host.c_str());
    }
    return it->second.Count() ? &it->second : nullptr;
}

// `rest` is \pak\<name>; a copy under mods\ takes precedence.
std::wstring PakHostPath(const std::wstring& rest)
{
    std::wstring host = g_Mods + rest;
    return GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES ? g_GameData + rest : host;
}

const PakIndex* IndexForPak(const char* pakPath)
{
    // pakPath is "d:\pak\res_<level>.pak".
    if (_strnicmp(pakPath, "d:\\", 3) != 0)
        return nullptr;
    std::wstring rest;
    for (const char* c = pakPath + 2; *c; ++c)
        rest += wchar_t(static_cast<unsigned char>(*c));
    std::lock_guard<std::mutex> lock(g_IndexLock);
    return IndexForHostPath(PakHostPath(rest));
}

// Finds a resource's data outside the level's stream: earlier in the level's own
// PAK, else in any level PAK on the disc (segment files hold only textures of
// their own level). The disc's PAKs are indexed on first need.
template <typename Find>
const PakIndex::Entry* FindElsewhere(const PakIndex* level, Find find, const PakIndex*& owner)
{
    if (const PakIndex::Entry* e = level ? find(level) : nullptr) {
        owner = level;
        return e;
    }
    std::lock_guard<std::mutex> lock(g_IndexLock);
    if (!g_DiscPaksIndexed) {
        g_DiscPaksIndexed = true;
        WIN32_FIND_DATAW found;
        HANDLE find = FindFirstFileW((g_GameData + L"\\pak\\res_*.pak").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = found.cFileName;
                if (name.find(L"_seg") != std::wstring::npos)
                    continue;
                if (const PakIndex* index = IndexForHostPath(PakHostPath(L"\\pak\\" + name)))
                    g_DiscPaks.push_back(index);
            } while (FindNextFileW(find, &found));
            FindClose(find);
        }
    }
    for (const PakIndex* index : g_DiscPaks) {
        if (index == level)
            continue;
        if (const PakIndex::Entry* e = find(index)) {
            owner = index;
            return e;
        }
    }
    return nullptr;
}

// Stand-ins: a resource no PAK has, served as another (lower case, relative, with extension). An
// animation a character requires that was never made takes a close relative's place (see
// StandInAnimation), under its own name: the engine finds animations by name.
std::mutex g_AliasLock;
std::unordered_map<std::string, std::string> g_Aliases;

std::string AliasOf(const std::string& wanted)
{
    const std::string original = PrivateBodyOriginal(wanted);
    if (original != wanted)
        return original;
    std::lock_guard<std::mutex> lock(g_AliasLock);
    const auto it = g_Aliases.find(wanted);
    return it != g_Aliases.end() ? it->second : std::string();
}

const PakIndex::Entry* FindElsewhere(const PakIndex* level, const std::string& wanted, const PakIndex*& owner)
{
    if (const PakIndex::Entry* e = FindElsewhere(level, [&](const PakIndex* index) { return index->FindWithData(wanted); }, owner))
        return e;
    const std::string alias = AliasOf(wanted);
    return alias.empty() ? nullptr
                         : FindElsewhere(nullptr, [&](const PakIndex* index) { return index->FindWithData(alias); }, owner);
}

// Loads a resource the level's PAK stream does not have from another copy.
// Returns false (nothing done) when it cannot.
// The level's own memory-image resources are in memory already (the PAK's image block, packer
// +0x16C, loaded whole with the level); reading one only hands it to its type's loader. An image of
// the level's that is asked for after the stream passed it is taken from the block rather than
// copied. Registered images per level PAK, so none is handed to its loader twice.
std::mutex g_ImageLock;
std::unordered_map<const PakIndex*, std::unordered_set<uint32_t>> g_RegisteredImages;

void* RegisterLevelImage(uint8_t* packer, const PakIndex* level, const PakIndex::Entry& entry)
{
    auto* block = *reinterpret_cast<uint8_t**>(packer + kPackerImageBlock);
    auto* loader = *reinterpret_cast<EngineObject**>(packer + kPackerImageLoaders + entry.typeId * 4);
    if (!block || !loader || entry.imageOffset < 0 || entry.imageSize <= 0)
        return nullptr;
    uint8_t* image = block + entry.imageOffset;
    std::lock_guard<std::mutex> lock(g_ImageLock);
    if (g_RegisteredImages[level].insert(entry.offset).second)
        VirtualAt<ImageLoadFn>(loader, kImageLoaderLoadSlot)(loader, nullptr, image, uint32_t(entry.imageSize));
    packer[kPackerLastWasImage] = 1;
    return image;
}

// Resource patches (RegisterResourcePatch), by relative lower-case name.
std::mutex g_PatchLock;
std::unordered_map<std::string, ResourcePatch> g_Patches;

ResourcePatch PatchFor(const std::string& lowerName)
{
    std::lock_guard<std::mutex> lock(g_PatchLock);
    auto it = g_Patches.find(lowerName);
    return it == g_Patches.end() ? nullptr : it->second;
}

// Generated resources (RegisterResourceGenerator): made once per session and written to
// cache\disc\<name>, from where they are served as loose files (served from memory, a texture
// decodes but is not the one drawn); from memory only if the file cannot be written.
void WriteGeneratedFile(const std::string& lowerName, const std::vector<uint8_t>& data)
{
    if (g_GeneratedDir.empty())
        return;
    std::wstring path = g_GeneratedDir + L"\\";
    for (char c : lowerName)
        path += wchar_t(static_cast<unsigned char>(c));
    for (size_t i = g_GeneratedDir.size() + 1; (i = path.find(L'\\', i)) != std::wstring::npos; ++i)
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr) &&
        written == data.size();
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (!ok) {
        LOG_WARN("Generated: could not write %ls", path.c_str());
        DeleteFileW(path.c_str());
    }
}

// Kept for the session.
std::mutex g_GeneratorLock;
std::vector<ResourceGenerator> g_Generators;
std::unordered_map<std::string, std::vector<uint8_t>> g_Generated;

const std::vector<uint8_t>* Generated(const std::string& lowerName)
{
    std::lock_guard<std::mutex> lock(g_GeneratorLock);
    auto it = g_Generated.find(lowerName);
    if (it != g_Generated.end())
        return &it->second;
    for (ResourceGenerator generator : g_Generators) {
        std::vector<uint8_t> data;
        if (generator(lowerName, data) && !data.empty()) {
            LOG_INFO("Generated: %s (%zu bytes)", lowerName.c_str(), data.size());
            // Textures must be loose files (above); menus are taken from memory (served as a loose file, a
            // menu's screen was made empty).
            const bool menu = lowerName.size() > 4 && lowerName.compare(lowerName.size() - 4, 4, ".xml") == 0;
            if (!menu)
                WriteGeneratedFile(lowerName, data);
            return &(g_Generated[lowerName] = std::move(data));
        }
    }
    return nullptr;
}

// Decodes a generated resource as a non-PAK read (t_Top set by the caller).
int DecodeGenerated(uint8_t* packer, const std::vector<uint8_t>& data, EnginePath* path, int typeId, void* record,
    int extra, int context)
{
    EngineObject* file = FileMemoryCtor(EngineAlloc()(kFileMemorySize), nullptr);
    VirtualAt<FileMemoryOpenFn>(file, kFileMemoryOpenSlot)(file, nullptr, const_cast<uint8_t*>(data.data()), uint32_t(data.size()));
    packer[kPackerLastWasImage] = 0;
    ReadSource source = { nullptr, path, 0, {}, record };
    return DecodeFromFile(file, uint32_t(data.size()), context, &source, typeId, extra);
}

// `levelBlock`: the level's image block is in use (its stream is open), so the level's own images
// are taken from it.
bool ReadElsewhere(uint8_t* packer, const PakIndex* owner, const PakIndex::Entry& entry, EnginePath* path, int typeId,
    void* record, int extra, int context, int& result, bool levelBlock = true)
{
    if (levelBlock && entry.imageOffset >= 0 && entry.imageSize > 0 && owner == g_LevelIndex) {
        if (void* image = RegisterLevelImage(packer, owner, entry)) {
            result = int(reinterpret_cast<uintptr_t>(image));
            return true;
        }
    }
    std::vector<uint8_t> data;
    if (!owner->ReadData(entry, data) || data.empty()) {
        LOG_WARN("PAK: could not read %s from %ls", entry.name.c_str(), owner->Path().c_str());
        return false;
    }
    if (int(entry.typeId) != typeId)
        LOG_WARN("PAK: %s is type %u in %ls, requested as type %d", entry.name.c_str(), entry.typeId,
            owner->Path().c_str(), typeId);
    if (ResourcePatch patch = PatchFor(entry.name); patch && !(entry.imageOffset >= 0 && entry.imageSize > 0)) {
        size_t before = data.size();
        patch(data);
        LOG_INFO("PAK: %s patched (%zu -> %zu bytes)", entry.name.c_str(), before, data.size());
    }

    if (entry.imageOffset >= 0 && entry.imageSize > 0) {
        auto* loader = *reinterpret_cast<EngineObject**>(packer + kPackerImageLoaders + typeId * 4);
        if (!loader) {
            LOG_WARN("PAK: no memory-image loader for type %d (%s)", typeId, entry.name.c_str());
            return false;
        }
        // Kept for the rest of the level, as the level's own image block is.
        void* image = EngineAlloc()(uint32_t(data.size()));
        std::memcpy(image, data.data(), data.size());
        VirtualAt<ImageLoadFn>(loader, kImageLoaderLoadSlot)(loader, nullptr, image, uint32_t(data.size()));
        packer[kPackerLastWasImage] = 1;
        result = int(reinterpret_cast<uintptr_t>(image));
    } else {
        EngineObject* file = FileMemoryCtor(EngineAlloc()(kFileMemorySize), nullptr);
        VirtualAt<FileMemoryOpenFn>(file, kFileMemoryOpenSlot)(file, nullptr, data.data(), uint32_t(data.size()));
        packer[kPackerLastWasImage] = 0;
        ReadSource source = { nullptr, path, 0, {}, record };
        result = DecodeFromFile(file, uint32_t(data.size()), context, &source, typeId, extra);
    }
    LOG_INFO("PAK: %s loaded from %ls", entry.name.c_str(), owner->Path().c_str());
    return true;
}

// Moves a PAK stream forward by reading and discarding up to `target`. The
// engine's buffered file streams sequentially (it tracks its own disk read
// position), and its seek does not survive jumps of several buffers, so skip
// the way the engine itself advances: by reading.
void SeekForward(EngineObject* reader, uint32_t target)
{
    auto tell = VirtualAt<uint32_t(__fastcall*)(EngineObject*, void*)>(reader, 0x44);
    auto read = VirtualAt<void(__fastcall*)(EngineObject*, void*, void*, uint32_t)>(reader, 0x30);
    PendingRead* top = t_Top; // keep skipped bytes out of the dumper's capture
    t_Top = nullptr;
    static char scratch[0x10000];
    for (uint32_t pos = tell(reader, nullptr); pos < target; pos = tell(reader, nullptr))
        read(reader, nullptr, scratch, target - pos < sizeof(scratch) ? target - pos : uint32_t(sizeof(scratch)));
    t_Top = top;
}

// Loose resources are decoded straight from their files without the PAK read,
// so they never consume PAK entries. PAK reads that find the stream at an entry
// nobody asked for (the packed copy of a loose resource, or the textures of a
// replaced mesh) skip forward to the requested entry; resources that are not
// further on in the level's PAK are loaded from a copy elsewhere.
int __fastcall ReadHook(void* packer, void* edx, DecodeFn decode, EnginePath* path, int typeId, void* record, int a5,
    int extra, int context)
{
    if (!path)
        return g_OriginalRead(packer, edx, decode, path, typeId, record, a5, extra, context);

    InstallEngineMessageHook();
    char full[512] = {};
    BuildPath(path, nullptr, full, 0);
    if (g_LogResources)
        LOG_INFO("Resource type %d: %s", typeId, full);

    PendingRead pending = { decode, full, static_cast<uint8_t*>(packer), false, nullptr, t_Top };
    if (HasLooseCopy(full)) {
        t_Top = &pending;
        ReadSource source = { nullptr, path, 0, {}, record };
        int result = DecodeLooseFile(context, &source, typeId, extra);
        t_Top = pending.outer;
        LOG_INFO("Loose resource: %s", full);
        return result;
    }

    auto* p = static_cast<uint8_t*>(packer);
    auto* reader = *reinterpret_cast<EngineObject**>(p + 4);
    bool onDiscDrive = _strnicmp(full, "d:\\", 3) == 0;
    uint32_t mode = *reinterpret_cast<uint32_t*>(p + 0x1D0); // 0 files, 1 recording a PAK, 2 PAK stream
    bool packMode = mode == 2 && p[0x88] == 0 && reader && onDiscDrive;
    // The engine reads a resource as a file on the disc in mode 0 (e.g. once a level's stream is
    // done: an effect of a player class swapped out, whose copy the stream skipped, asked for later
    // by another character), and in mode 2 when the packer's "direct" flag (+8, set through the scene,
    // 0x4B1C0) is set. One that is not on the disc as a file would fail -- the "disc dirty or damaged"
    // screen, or garbage such as "OBSOLETE ANIMATION!" and a crash -- so it comes from a PAK's copy
    // instead, the level's own first.
    if (onDiscDrive && (mode == 0 || (mode == 2 && p[8] != 0))) {
        std::wstring onDisc = g_GameData;
        for (const char* c = full + 2; *c; ++c)
            onDisc += wchar_t(static_cast<unsigned char>(*c));
        if (GetFileAttributesW(onDisc.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const PakIndex* index = g_LevelIndex;
            if (mode == 2 && reader) {
                char pakPath[512] = {};
                BuildPath(reinterpret_cast<EnginePath*>(p + 0x174), nullptr, pakPath, 0);
                index = IndexForPak(pakPath);
            }
            std::string wanted = full + 3;
            for (char& c : wanted)
                c = char(tolower(static_cast<unsigned char>(c)));
            const PakIndex* owner = nullptr;
            if (const PakIndex::Entry* entry = FindElsewhere(index, wanted, owner)) {
                t_Top = &pending;
                int result = 0;
                bool done = ReadElsewhere(p, owner, *entry, path, typeId, record, extra, context, result, mode == 2);
                t_Top = pending.outer;
                if (done)
                    return result;
            } else if (const std::vector<uint8_t>* generated = Generated(wanted)) {
                t_Top = &pending;
                int result = DecodeGenerated(p, *generated, path, typeId, record, extra, context);
                t_Top = pending.outer;
                return result;
            }
        }
    }
    if (packMode && p[8] == 0) {
        char pakPath[512] = {};
        BuildPath(reinterpret_cast<EnginePath*>(p + 0x174), nullptr, pakPath, 0);
        if (const PakIndex* index = IndexForPak(pakPath)) {
            g_LevelIndex = index;
            std::string wanted = full + 3;
            for (char& c : wanted)
                c = char(tolower(static_cast<unsigned char>(c)));
            uint32_t pos = VirtualAt<uint32_t(__fastcall*)(EngineObject*, void*)>(reader, 0x44)(reader, nullptr);
            const PakIndex::Entry* next = index->AtOffset(pos);
            // A patched resource is decoded from an edited copy; its entry in the stream is passed.
            if (PatchFor(wanted)) {
                const PakIndex::Entry* entry = next && next->name == wanted ? next : index->FindAfter(wanted, pos);
                if (!entry)
                    entry = index->FindWithData(wanted);
                if (entry && entry->HasData()) {
                    if (entry->offset >= pos)
                        SeekForward(reader, entry->offset + entry->size);
                    t_Top = &pending;
                    int result = 0;
                    bool done = ReadElsewhere(p, index, *entry, path, typeId, record, extra, context, result);
                    t_Top = pending.outer;
                    if (done)
                        return result;
                }
            }
            if (!next || next->name != wanted) {
                if (const PakIndex::Entry* target = index->FindAfter(wanted, pos)) {
                    if (next) {
                        LOG_INFO("PAK: skipping from %s to %s", next->name.c_str(), wanted.c_str());
                        SeekForward(reader, target->offset);
                    }
                } else {
                    const PakIndex* owner = nullptr;
                    if (const PakIndex::Entry* entry = FindElsewhere(index, wanted, owner)) {
                        t_Top = &pending;
                        int result = 0;
                        bool done = ReadElsewhere(p, owner, *entry, path, typeId, record, extra, context, result);
                        t_Top = pending.outer;
                        if (done)
                            return result;
                    } else if (const std::vector<uint8_t>* generated = Generated(wanted)) {
                        t_Top = &pending;
                        int result = DecodeGenerated(p, *generated, path, typeId, record, extra, context);
                        t_Top = pending.outer;
                        return result;
                    } else {
                        LOG_WARN("PAK: %s is in no PAK", wanted.c_str());
                    }
                }
            }
        }
    }

    if (g_DumpDir.empty() || _strnicmp(full, "d:\\", 3) != 0)
        return g_OriginalRead(packer, edx, decode, path, typeId, record, a5, extra, context);
    t_Top = &pending;
    int result = g_OriginalRead(packer, edx, &DecodeDumped, path, typeId, record, a5, extra, context);
    t_Top = pending.outer;
    return result;
}

// --- Manual files ---------------------------------------------------------------------------------
// Some resources (a mesh's .ban animation binding) are "manual" PAK entries, read through their own
// call (game::kPackerOpenManual): it parses the entry header at the stream position and hands out the
// packer's reader at the data if the name matches. As with ordinary reads, an entry further on is
// skipped to, and one the level's PAK does not have further on is served through a memory file,
// leaving the level's stream where it is: from a loose copy (mods\, or a binding the engine rebuilt
// into cache\disc\ -- it reads the binding again right after writing it) or else another PAK's copy.

bool ReadHostFile(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 0x10000000;
    DWORD read = 0;
    if (ok) {
        data.resize(size_t(size.QuadPart));
        ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(file);
    return ok;
}

using OpenManualFn = EngineObject*(__fastcall*)(uint8_t* packer, void* edx, EnginePath* path);
OpenManualFn g_OriginalOpenManual = nullptr;

// A manual file from a loose copy (mods\, or a binding the engine rebuilt into cache\disc\), else
// another PAK's copy (not `index`'s), through a memory file; null when there is none.
EngineObject* OpenManualCopy(const char* full, const std::string& wanted, const PakIndex* index)
{
    std::vector<uint8_t> data;
    std::wstring from;
    if (HasLooseCopy(full) && ReadHostFile(LooseHostPath(full), data)) {
        from = LooseHostPath(full);
    } else {
        const PakIndex* owner = nullptr;
        const PakIndex::Entry* entry = FindElsewhere(index, wanted, owner);
        if (!entry || entry->rawSize == 0 || !owner->ReadData(*entry, data))
            return nullptr;
        from = owner->Path();
    }
    EngineObject* file = FileMemoryCtor(EngineAlloc()(kFileMemorySize), nullptr);
    VirtualAt<FileMemoryOpenFn>(file, kFileMemoryOpenSlot)(file, nullptr, data.data(), uint32_t(data.size()));
    LOG_INFO("PAK: %s (manual) loaded from %ls", wanted.c_str(), from.c_str());
    return PackReaderCtor(EngineAlloc()(0x24), nullptr, file, 0); // kept: the caller does not free it
}

// After a manual read, the scene (+0x8A0 == 2, 0x4B0D5) has the packer put its stream back where it
// was (0x51E40: thiscall, through the stream reader at +4). Once the level has loaded, the packer is
// no longer streaming (+0x1D0 is 0) and that reader is gone, but +4 still points at it: a character
// spawned then crashed there. Without a stream there is nothing to put back. Entry:
//   push esi / mov esi, ecx / mov eax, [esi + 4]   (6 bytes)
constexpr uint32_t kPackerEndManual = 0x00051E40;
constexpr uint32_t kPackerEndManualContinue = 0x00051E46;
constexpr uint8_t kPackerEndManualBytes[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04 };

__declspec(naked) void PackerEndManualGuard()
{
    __asm {
        cmp dword ptr [ecx + 0x1D0], 2
        jne done
        push esi
        mov esi, ecx
        mov eax, dword ptr [esi + 4]
        push kPackerEndManualContinue
        ret
    done:
        ret
    }
}

EngineObject* __fastcall OpenManualHook(uint8_t* packer, void* edx, EnginePath* path)
{
    auto* reader = *reinterpret_cast<EngineObject**>(packer + 4);
    char full[512] = {};
    if (path)
        BuildPath(path, nullptr, full, 0);
    if (!path || _strnicmp(full, "d:\\", 3) != 0)
        return g_OriginalOpenManual(packer, edx, path);
    std::string wanted = full + 3;
    for (char& c : wanted)
        c = char(tolower(static_cast<unsigned char>(c)));
    if (!reader || *reinterpret_cast<uint32_t*>(packer + 0x1D0) != 2) {
        // Not streaming (a character spawned once the level has loaded): the engine opens the disc file,
        // which for a PAK-only binding does not exist (its file reader then crashes, 0x232190). Those
        // are served from a copy like the ones the stream lacks.
        std::wstring onDisc = g_GameData;
        for (const char* c = full + 2; *c; ++c)
            onDisc += wchar_t(static_cast<unsigned char>(*c));
        if (GetFileAttributesW(onDisc.c_str()) != INVALID_FILE_ATTRIBUTES)
            return g_OriginalOpenManual(packer, edx, path);
        if (EngineObject* copy = OpenManualCopy(full, wanted, g_LevelIndex))
            return copy;
        return g_OriginalOpenManual(packer, edx, path);
    }
    char pakPath[512] = {};
    BuildPath(reinterpret_cast<EnginePath*>(packer + 0x174), nullptr, pakPath, 0);
    const PakIndex* index = IndexForPak(pakPath);
    if (!index)
        return g_OriginalOpenManual(packer, edx, path);
    uint32_t pos = VirtualAt<uint32_t(__fastcall*)(EngineObject*, void*)>(reader, 0x44)(reader, nullptr);
    const PakIndex::Entry* next = index->AtOffset(pos);
    if (next && next->name == wanted)
        return g_OriginalOpenManual(packer, edx, path);
    if (const PakIndex::Entry* target = index->FindAfter(wanted, pos)) {
        if (next) {
            LOG_INFO("PAK: skipping from %s to %s (manual)", next->name.c_str(), wanted.c_str());
            SeekForward(reader, target->offset);
        }
        return g_OriginalOpenManual(packer, edx, path);
    }
    if (EngineObject* copy = OpenManualCopy(full, wanted, index))
        return copy;
    return g_OriginalOpenManual(packer, edx, path);
}

// --- Declaring resources a level's PAK does not list ---------------------------------------------
// A scene loads only what its PAK header declared (see game::kSceneLoadResource); some loaders also
// ask first whether a resource is declared (game::kSceneIsDeclared) and skip it otherwise -- that
// answer must stay (declaring there makes a level take optional content it left out, e.g. grapple
// animations, as preloaded). When a load finds a resource undeclared that exists in another PAK
// on the disc, it is declared the way
// the engine's own folder scan (0x4F230) does -- a node named after the path, in a group under the
// path's key in the type's map -- and the load is retried; the read then finds it through
// ReadElsewhere. Only character content is declared (models, textures, animations, effects): a
// level's menus, text and level files stay its own. Nodes and
// groups are allocated here (the scene's pools are sized exactly from the PAK header), and the
// folder array is grown before the engine may append to it.

using SceneLoadFn = void*(__fastcall*)(uint8_t* scene, void* edx, const EnginePath* path, int typeId, int flags);
using AddStringFn = uint16_t(__fastcall*)(uint8_t* scene, void* edx, const char* text);
using NodeSetPathFn = void(__fastcall*)(uint8_t* node, void* edx, const EnginePath* path, uint8_t* scene);
using MapFindFn = int(__fastcall*)(uint8_t* map, void* edx, const char* key);
using MapInsertFn = void(__fastcall*)(uint8_t* map, void* edx, uint32_t key, uint8_t* group);
using PathKeyFn = const char*(__fastcall*)(const EnginePath* path, void* edx, int withoutExtension);
using IsDeclaredFn = bool(__fastcall*)(uint8_t* scene, void* edx, const EnginePath* path, int typeId);
using PathFromTextFn = void*(__fastcall*)(void* path, void* edx, const char* text, int kind);

SceneLoadFn g_OriginalSceneLoad = nullptr;
const auto SceneIsDeclared = reinterpret_cast<IsDeclaredFn>(uintptr_t(kSceneIsDeclared));
const auto SceneAddString = reinterpret_cast<AddStringFn>(uintptr_t(kSceneAddString));
const auto NodeSetPath = reinterpret_cast<NodeSetPathFn>(uintptr_t(kNodeSetPath));
const auto ResourceMapFind = reinterpret_cast<MapFindFn>(uintptr_t(kResourceMapFind));
const auto ResourceMapInsert = reinterpret_cast<MapInsertFn>(uintptr_t(kResourceMapInsert));
const auto PathKey = reinterpret_cast<PathKeyFn>(uintptr_t(kEnginePathKey));
const auto PathFromText = reinterpret_cast<PathFromTextFn>(uintptr_t(kEnginePathFromText));

constexpr int kFolderHeadroom = 256;
std::mutex g_DeclareLock;
std::unordered_map<uint8_t*, int> g_FolderCapacity; // per scene, once grown here

template <typename T> T& At(uint8_t* base, uint32_t offset) { return *reinterpret_cast<T*>(base + offset); }

// Room for `extra` more folders: the engine appends without checking.
void ReserveFolders(uint8_t* scene, int extra)
{
    int count = At<int>(scene, kSceneFolderCount);
    auto it = g_FolderCapacity.find(scene);
    int capacity = it == g_FolderCapacity.end() ? count : it->second;
    if (count + extra <= capacity)
        return;
    int grown = count + extra + kFolderHeadroom;
    auto* folders = static_cast<uint8_t*>(EngineAlloc()(grown * kSceneFolderSize));
    std::memcpy(folders, At<uint8_t*>(scene, kSceneFolders), count * kSceneFolderSize);
    for (int i = count; i < grown; ++i) { // as the array constructor (0x4A7D0) leaves them
        uint8_t* folder = folders + i * kSceneFolderSize;
        std::memset(folder, 0, kSceneFolderSize);
        At<uint16_t>(folder, 0x22) = 0xFFFF;
        At<uint16_t>(folder, 0x24) = 0xFFFF;
    }
    At<uint8_t*>(scene, kSceneFolders) = folders; // the old array stays allocated
    g_FolderCapacity[scene] = grown;
}

// The engine's animation index (SceneBoneAnimAPI, 0x65C50) files every declared animation by folder
// and name, but builds it only once, at the first lookup (a character's setup), and marks it built by
// clearing +0x50 of the lookup object ([kGameContext] + 0x148). Declaring an animation after that
// sets the flag again, so the next lookup adds it (existing names are kept, not duplicated).
constexpr int kAnimationType = 10;
constexpr int kSoundType = 41; // .hwx
constexpr int kMeshType = 4;

void MarkAnimationIndexStale()
{
    auto* context = *reinterpret_cast<uint8_t**>(uintptr_t(kGameContext));
    auto* index = context ? *reinterpret_cast<uint8_t**>(context + kGameContextAnimationIndex) : nullptr;
    if (index)
        index[kAnimationIndexStale] = 1;
}

bool Declare(uint8_t* scene, const EnginePath* path, int typeId, uint32_t size)
{
    uint8_t* table = At<uint8_t*>(scene, kSceneTypeTables + typeId * 4);
    if (!table)
        return false;
    uint8_t* map = table + kTypeTableMap;
    std::lock_guard<std::mutex> lock(g_DeclareLock);
    ReserveFolders(scene, 16);

    auto* node = static_cast<uint8_t*>(EngineAlloc()(20));
    std::memset(node, 0, 20);
    NodeSetPath(node, nullptr, path, scene);
    At<uint32_t>(node, 0x10) = size;
    if (typeId == kAnimationType)
        MarkAnimationIndexStale();

    char key[256];
    strncpy_s(key, PathKey(path, nullptr, 1), _TRUNCATE);
    int index = ResourceMapFind(map, nullptr, key);
    if (index >= 0) {
        uint8_t* group = At<uint8_t*>(At<uint8_t*>(map, 8), index * 8 + 4);
        At<uint8_t*>(node, 0) = group;
        At<uint8_t*>(node, 4) = At<uint8_t*>(group, 4);
        At<uint8_t*>(group, 4) = node;
        return true;
    }
    auto* group = static_cast<uint8_t*>(EngineAlloc()(8));
    uint8_t* items = At<uint8_t*>(map, 8);
    // Group +0: as the map's other groups have it (the folder scan passes it through).
    At<uint16_t>(group, 0) = At<int>(map, 4) > 0 ? At<uint16_t>(At<uint8_t*>(items, 4), 0) : 0;
    At<uint16_t>(group, 2) = SceneAddString(scene, nullptr, key);
    At<uint8_t*>(group, 4) = node;
    At<uint8_t*>(node, 0) = group;
    ResourceMapInsert(map, nullptr, At<uint16_t>(group, 2), group);
    return true;
}

bool IsCharacterContent(const std::string& path)
{
    // Plus the characters' HUD portraits and select-screen heads (see characters.cpp).
    static const char* const kFolders[] = { "meshes\\", "animation\\", "effects\\", "textures\\",
        "interfc\\hud_b\\hud_face_", "interfc\\front_end\\s_selduel_" };
    for (const char* folder : kFolders)
        if (path.rfind(folder, 0) == 0)
            return true;
    return false;
}

// Declares an undeclared resource that another PAK has. True when it did.
bool DeclareFromDisc(uint8_t* scene, const EnginePath* path, int typeId, const PakIndex** declaredFrom = nullptr,
    const PakIndex::Entry** declared = nullptr)
{
    if (!path || typeId < 0 || typeId >= 0x31 || SceneIsDeclared(scene, nullptr, path, typeId))
        return false;
    char full[512] = {};
    BuildPath(path, nullptr, full, 0);
    if (_strnicmp(full, "d:\\", 3) != 0)
        return false;
    std::string stem = full + 3;
    for (char& c : stem)
        c = char(tolower(static_cast<unsigned char>(c)));
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos && stem.find('\\', dot) == std::string::npos)
        stem.erase(dot);
    // A sound effect is asked for by its bare name (d:\wep_ls_swing_quick_d_01); the PAKs keep it as
    // audio\xbox\<name>.hwx. Levels hold only their own characters' (a Sith's saber swings, Vader's
    // breathing, Force lightning), so a character a level does not have played silence.
    const bool sound = typeId == kSoundType && stem.find('\\') == std::string::npos;
    if (!sound && !IsCharacterContent(stem))
        return false;

    const PakIndex* owner = nullptr;
    const std::string stored = sound ? "audio\\xbox\\" + stem : stem;
    const PakIndex::Entry* entry = FindElsewhere(nullptr,
        [&](const PakIndex* index) { return index->FindStemWithData(stored, uint32_t(typeId)); }, owner);
    if (entry && sound) {
        // Read under its bare name: served as the stored one.
        std::lock_guard<std::mutex> lock(g_AliasLock);
        g_Aliases[stem] = entry->name;
        g_Aliases[stem + ".hwx"] = entry->name;
    }
    if (!entry && PrivateBodyOriginal(stem) != stem) {
        const std::string original = PrivateBodyOriginal(stem);
        entry = FindElsewhere(nullptr,
            [&](const PakIndex* index) { return index->FindStemWithData(original, uint32_t(typeId)); }, owner);
    }
    if (!entry && typeId == kAnimationType) {
        const std::string alias = AliasOf(stem + ".bnm");
        if (!alias.empty())
            entry = FindElsewhere(nullptr, [&](const PakIndex* index) { return index->FindWithData(alias); }, owner);
    }
    if (!entry) {
        std::string name = full + 3;
        for (char& c : name)
            c = char(tolower(static_cast<unsigned char>(c)));
        // A mod's own resource, under a name no PAK has (a new character mesh): read as a loose file.
        WIN32_FILE_ATTRIBUTE_DATA loose{};
        if (HasLooseCopy(full) && GetFileAttributesExW(LooseHostPath(full).c_str(), GetFileExInfoStandard, &loose) &&
            Declare(scene, path, typeId, loose.nFileSizeLow)) {
            LOG_INFO("Declare: %s (type %d), loose", full, typeId);
            return true;
        }
        const std::vector<uint8_t>* generated = Generated(name);
        if (!generated || !Declare(scene, path, typeId, uint32_t(generated->size())))
            return false;
        LOG_INFO("Declare: %s (type %d), generated", full, typeId);
        return true;
    }
    uint32_t size = entry->imageOffset >= 0 && entry->imageSize > 0 ? uint32_t(entry->imageSize) : entry->rawSize;
    if (!Declare(scene, path, typeId, size)) {
        LOG_WARN("Declare: no table for type %d (%s)", typeId, full);
        return false;
    }
    LOG_INFO("Declare: %s (type %d) from %ls", full, typeId, owner->Path().c_str());
    if (declaredFrom)
        *declaredFrom = owner;
    if (declared)
        *declared = entry;
    return true;
}

// A character definition (type 38, meshes\chars\common\<name>.xml) names the character's main
// definition (s_<name>.xml) and its collision shape. A level loads those for its own characters
// from its PAK stream at load time -- which registers the character's class info -- so for a
// character declared here they are declared and loaded here too.
constexpr int kCharacterDefinitionType = 38;
constexpr int kAnimEventTreeType = 31;

// Declares and loads a resource named `text` (relative, lower case) that this level lacks. Text
// resources (tables) are only declared: their loaders parse them with options of their own.
bool LoadUndeclared(uint8_t* scene, const std::string& text, const PakIndex::Entry& entry, bool load = true)
{
    alignas(4) uint8_t path[0x100] = {};
    PathFromText(path, nullptr, text.c_str(), 1); // relative, as the engine names resources
    auto* enginePath = reinterpret_cast<const EnginePath*>(path);
    if (SceneIsDeclared(scene, nullptr, enginePath, int(entry.typeId)))
        return false;
    if (!DeclareFromDisc(scene, enginePath, int(entry.typeId)))
        return false;
    if (!load)
        return true;
    void* loaded;
    if (entry.typeId == kAnimEventTreeType) {
        // The main definition is an AnimEventTree, which only its manager can load (it supplies
        // the type's loader for the call).
        loaded = reinterpret_cast<void*(__cdecl*)(const EnginePath*)>(uintptr_t(kLoadAnimEventTree))(enginePath);
    } else {
        loaded = g_OriginalSceneLoad(scene, nullptr, enginePath, int(entry.typeId), 0);
    }
    if (!loaded)
        LOG_WARN("Declare: %s did not load", text.c_str());
    return loaded != nullptr;
}

void LoadReferences(uint8_t* scene, const std::vector<uint8_t>& data)
{
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        uint32_t length;
        std::memcpy(&length, &data[i], 4);
        if (length < 8 || length > 200 || i + 4 + length > data.size())
            continue;
        std::string text(reinterpret_cast<const char*>(&data[i + 4]), length);
        bool printable = true;
        for (char c : text)
            printable &= c >= 0x20 && c < 0x7F;
        if (!printable || text.find('.') == std::string::npos)
            continue;
        for (char& c : text)
            c = char(tolower(static_cast<unsigned char>(c)));
        if (!IsCharacterContent(text))
            continue;
        i += 3 + length;

        const PakIndex* owner = nullptr;
        if (const PakIndex::Entry* entry = FindElsewhere(nullptr, text, owner))
            LoadUndeclared(scene, text, *entry);
    }
}

// A character's own tables (meshes\chars\common\r_<name>.csv reactions, a_<name>.csv attacks), from
// its definition's PAK, declared up front: the table hooks below catch the tables opened by name,
// but at least one loader (the attack table's) checks declarations on its own and skips an
// undeclared table silently -- the character then fights without its attacks' data.
void DeclareCharacterTables(uint8_t* scene, const PakIndex* owner, const std::string& character)
{
    const std::string suffix = "_" + character + ".csv";
    for (const PakIndex::Entry* entry : owner->WithPrefix("meshes\\chars\\common\\")) {
        const std::string& name = entry->name;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            name.find('\\', name.size() - suffix.size() - 1) == std::string::npos &&
            LoadUndeclared(scene, name, *entry, false))
            LOG_INFO("Declare: table %s (declared)", name.c_str());
    }
}

// The engine keeps pointers into a scene's string table (e.g. the animation index's names), and
// adding a string can reallocate its buffer. Room for the names a declaration may add is made once,
// at the scene's first load, before anything is built on it.
constexpr uint32_t kStringReserve = 0x10000;
std::unordered_set<uint8_t*> g_ReservedScenes;

void ReserveSceneStrings(uint8_t* scene)
{
    std::lock_guard<std::mutex> lock(g_DeclareLock);
    if (!g_ReservedScenes.insert(scene).second)
        return;
    uint8_t* table = scene + kSceneStrings;
    auto grow = reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kStringTableGrow));
    for (int i = 0; i < 8 && At<uint32_t>(table, kStringTableFree) < kStringReserve; ++i)
        grow(table, nullptr);
}

// Character tables (meshes\chars\common\<name>.csv: a character's reactions and behaviour,
// including its parent classes', e.g. a padawan's b_JediKnight.csv) are opened by name
// (game::kOpenCharacterTable), which only opens tables the level declares. A table the level lacks
// is declared from the disc first; the engine then loads and parses it as usual.
using OpenCharacterTableFn = void*(__cdecl*)(const char* name);
OpenCharacterTableFn g_OriginalOpenCharacterTable = nullptr;

void DeclareCharacterTable(const char* name)
{
    if (!name || !g_LevelScene)
        return;
    std::string file = std::string("meshes\\chars\\common\\") + name + ".csv";
    for (char& c : file)
        c = char(tolower(static_cast<unsigned char>(c)));
    const PakIndex* owner = nullptr;
    if (const PakIndex::Entry* entry = FindElsewhere(g_LevelIndex, file, owner))
        LoadUndeclared(g_LevelScene, file, *entry, false);
}

void* __cdecl OpenCharacterTableHook(const char* name)
{
    DeclareCharacterTable(name);
    return g_OriginalOpenCharacterTable(name);
}

// Event tables (a character's attacks, a_<name>.csv) are loaded by a second path
// (game::kLoadEventTable), with the name taken from the character: [[object + 0x434] + 4] + 0x2C.
using LoadEventTableFn = void*(__cdecl*)(uint8_t* object);
LoadEventTableFn g_OriginalLoadEventTable = nullptr;

void* __cdecl LoadEventTableHook(uint8_t* object)
{
    auto* info = object ? *reinterpret_cast<uint8_t**>(object + 0x434) : nullptr;
    auto* names = info ? *reinterpret_cast<uint8_t**>(info + 4) : nullptr;
    DeclareCharacterTable(names ? *reinterpret_cast<const char**>(names + 0x2C) : nullptr);
    return g_OriginalLoadEventTable(object);
}

// Missing animations. Before a character's script is built, the engine looks up every animation it
// requires by name (game::kAnimationLookup) and fails the character if any is missing -- a character a
// level never had needs its own animations, animations it shares with other characters (a Jedi
// brute's movement is the Neimoidian brute's), and the grapple animations of the characters it can
// fight. The engine's own list of missing names is resolved here: each name is looked for as a file
// (<name>.bnm) in the disc's PAKs, declared and loaded into the level, and the lookup runs again.
struct EngineArray {
    int32_t capacity;
    int32_t count;
    const char** data;
};
using AnimationLookupFn = bool(__fastcall*)(void* self, void* edx, void* script, void* classInfo, void* arg3,
    EngineArray* found, EngineArray* missing, int flag);
AnimationLookupFn g_OriginalAnimationLookup = nullptr;

// Animations found nowhere (a borrowed mesh's binding can ask for Anakin_Frc_Jump_C1/C2, which no PAK
// has) are left out of the found list, but the binding built from it (0x6890D, kept by fixes.cpp)
// reads one entry per name asked for. The list is laid out again in the names' order, each missing
// name taking its nearest neighbour's animation (the names are sorted, so a close relative). An entry
// carries its animation's name at +4.
constexpr uint32_t kEngineArrayResize = 0x00021B70; // thiscall TArray (int count, const T* fill)
constexpr uint32_t kBindingLookupReturn = 0x0006890D;  // the binding rebuild's lookup call returns here

void AlignFoundAnimations(const EngineArray* names, EngineArray* found, const EngineArray* missing, int from)
{
    // Only for a list of names, all accounted for as found or missing.
    if (!names || !found || !missing || from != 0 || !names->data ||
        names->count != found->count + missing->count || found->count >= names->count)
        return;
    std::vector<const char*> compact;
    for (int i = 0; i < found->count; ++i)
        if (found->data[i])
            compact.push_back(found->data[i]);
    const char* none = nullptr;
    reinterpret_cast<void(__fastcall*)(EngineArray*, void*, int, const char* const*)>(uintptr_t(kEngineArrayResize))(
        found, nullptr, names->count, &none);
    if (found->count != names->count)
        return;
    size_t next = 0;
    int filled = 0;
    for (int i = 0; i < names->count; ++i) {
        const char* name = names->data[i];
        if (next < compact.size() && name && _stricmp(compact[next] + 4, name) == 0) {
            found->data[i] = compact[next++];
            continue;
        }
        found->data[i] = !compact.empty() ? compact[next < compact.size() ? next : compact.size() - 1] : nullptr;
        ++filled;
    }
    LOG_INFO("Declare: %d animation(s) found nowhere; a neighbour stands in for each", filled);
}

bool AnimationLookupResolved(void* self, void* edx, void* script, void* classInfo, void* arg3, EngineArray* found,
    EngineArray* missing, int flag);

bool __fastcall AnimationLookupHook(void* self, void* edx, void* script, void* classInfo, void* arg3,
    EngineArray* found, EngineArray* missing, int flag)
{
    const int foundBefore = found ? found->count : 0;
    const bool result = AnimationLookupResolved(self, edx, script, classInfo, arg3, found, missing, flag);
    if (missing && missing->count > 0 && uintptr_t(_ReturnAddress()) == kBindingLookupReturn)
        AlignFoundAnimations(static_cast<const EngineArray*>(classInfo), found, missing, foundBefore);
    return result;
}

// An animation file name's stand-in, for one never made: a grapple move against one opponent,
// <attacker>_<move>_<opponent>gr|gb_partN (anakin_atk_sse5_jdbrutegr_part2: Anakin's fifth combo's
// finish on a Jedi brute, which the story never pairs but an unlocked profile does), takes the same
// move's version against a Jedi (anakin_atk_sse5_jedigr_part2). A few optional animations the
// classes name were never made either, and take a close relative's place (needed when a level loads
// its characters' optional moves, see EnableOptionalMoves). Empty when there is no rule.
std::string StandInAnimation(const std::string& file)
{
    static const struct { const char* missing; const char* standIn; } kNeverMade[] = {
        { "anakin_frc_jump_c1.bnm", "anakin_frc_jump_v1.bnm" },  // a force jump's versions
        { "anakin_frc_jump_c2.bnm", "anakin_frc_jump_v2.bnm" },
        { "gpdroid_nav_mount_v3.bnm", "gpdroid_nav_mount_v2.bnm" }, // the grapple droid climbing on
        { "gpdroid_nav_holeout.bnm", "gpdroid_nav_mount_v1.bnm" },  // and out of a hole
        { "ctroop_nav_coverout_l.bnm", "ctroop_nav_coverout_r.bnm" }, // a clone leaving cover
        { "ctroop_nav_slide_in.bnm", "ctroop_nav_fallloop.bnm" },     // and sliding down a slope
        { "ctroop_nav_slide_loop.bnm", "ctroop_nav_fallloop.bnm" },
        { "ctroop_nav_slide_out.bnm", "ctroop_nav_fallsoftland.bnm" },
    };
    for (const auto& n : kNeverMade)
        if (file == n.missing)
            return n.standIn;
    for (const char* side : { "gr_part", "gb_part" }) {
        const size_t at = file.find(side);
        if (at == std::string::npos)
            continue;
        const size_t start = file.rfind('_', at);
        if (start == std::string::npos || file.compare(start + 1, at - start - 1, "jedi") == 0)
            return std::string();
        return file.substr(0, start + 1) + "jedi" + file.substr(at);
    }
    return std::string();
}

bool AnimationLookupResolved(void* self, void* edx, void* script, void* classInfo, void* arg3, EngineArray* found,
    EngineArray* missing, int flag)
{
    int foundBefore = found ? found->count : 0;
    bool result = g_OriginalAnimationLookup(self, edx, script, classInfo, arg3, found, missing, flag);
    if (!missing || missing->count <= 0 || !g_LevelScene)
        return result;
    int resolved = 0;
    for (int i = 0; i < missing->count; ++i) {
        const char* name = missing->data[i];
        if (!name)
            continue;
        std::string file = name;
        for (char& c : file)
            c = char(tolower(static_cast<unsigned char>(c)));
        file += ".bnm";
        const PakIndex* owner = nullptr;
        const PakIndex::Entry* entry = FindElsewhere(g_LevelIndex,
            [&](const PakIndex* index) { return index->FindFileWithData(file, kAnimationType); }, owner);
        if (entry && LoadUndeclared(g_LevelScene, entry->name, *entry)) {
            ++resolved;
            continue;
        }
        if (entry)
            continue;
        // Found nowhere: a stand-in, declared under the wanted name in the stand-in's folder.
        const std::string standIn = StandInAnimation(file);
        const PakIndex::Entry* standInEntry = standIn.empty() ? nullptr : FindElsewhere(g_LevelIndex,
            [&](const PakIndex* index) { return index->FindFileWithData(standIn, kAnimationType); }, owner);
        if (!standInEntry)
            continue;
        const std::string folder = standInEntry->name.substr(0, standInEntry->name.rfind('\\') + 1);
        {
            std::lock_guard<std::mutex> lock(g_AliasLock);
            g_Aliases[folder + file] = standInEntry->name;
        }
        if (LoadUndeclared(g_LevelScene, folder + file, *standInEntry)) {
            LOG_INFO("Declare: %s, never made, stands in as %s", (folder + file).c_str(), standInEntry->name.c_str());
            ++resolved;
        }
    }
    if (resolved == 0)
        return result;
    LOG_INFO("Declare: %d of %d missing animation(s) found on the disc", resolved, missing->count);
    // Declaring marked the index for a rebuild; look again with fresh results.
    missing->count = 0;
    if (found)
        found->count = foundBefore;
    return g_OriginalAnimationLookup(self, edx, script, classInfo, arg3, found, missing, flag);
}

// Character meshes the port holds a reference to for the level (one each). A level's own meshes are
// referenced by the characters using them only: replacing the player live destroys the level's player,
// and its mesh would be freed with it, then reloaded for the next character of that body under a model
// still bound to the old data (0x6754A).
std::mutex g_PinLock;
const uint8_t* g_PinScene = nullptr;
std::unordered_set<const void*> g_PinnedMeshes;

bool IsCharacterMesh(const EnginePath* path, int typeId)
{
    if (typeId != kMeshType)
        return false;
    char dir[MAX_PATH] = {};
    strncpy_s(dir, PathString(path, nullptr, 1), _TRUNCATE);
    _strlwr_s(dir);
    return std::strncmp(dir, "meshes\\chars\\", 13) == 0;
}

void* __fastcall SceneLoadHook(uint8_t* scene, void* edx, const EnginePath* path, int typeId, int flags)
{
    ReserveSceneStrings(scene);
    g_LevelScene = scene;
    void* result = g_OriginalSceneLoad(scene, edx, path, typeId, flags);
    if (result && IsCharacterMesh(path, typeId)) {
        bool pin = false;
        {
            std::lock_guard<std::mutex> lock(g_PinLock);
            if (g_PinScene != scene) {
                g_PinScene = scene;
                g_PinnedMeshes.clear();
            }
            pin = g_PinnedMeshes.insert(result).second;
        }
        if (pin)
            g_OriginalSceneLoad(scene, edx, path, typeId, flags);
    }
    // A null result is also the answer to "is it loaded yet?" for declared resources.
    const PakIndex* owner = nullptr;
    const PakIndex::Entry* entry = nullptr;
    if (!result && DeclareFromDisc(scene, path, typeId, &owner, &entry)) {
        result = g_OriginalSceneLoad(scene, edx, path, typeId, flags);
        // A second load: a reference of the port's own, kept for the level, as the level keeps its own
        // resources. Without it, the resource is freed when the last character using it goes (a spawned
        // character dying), and the next one reloads the mesh under a model still bound to the old data
        // (0x6754A: the animation reads empty slots).
        if (result)
            g_OriginalSceneLoad(scene, edx, path, typeId, flags);
        std::vector<uint8_t> data;
        if (result && typeId == kCharacterDefinitionType && owner->ReadData(*entry, data)) {
            LoadReferences(scene, data);
            std::string character = entry->name.substr(entry->name.rfind('\\') + 1);
            DeclareCharacterTables(scene, owner, character.substr(0, character.find('.')));
        }
    }
    return result;
}

} // namespace

std::string PrivateBodyName(const std::string& mesh, const std::string& className)
{
    uint32_t hash = 0x811C9DC5;
    for (char c : className) {
        hash ^= uint8_t(tolower(static_cast<unsigned char>(c)));
        hash *= 0x01000193;
    }
    char tag[16];
    sprintf_s(tag, "%s%08x", kPrivateBodyTag, hash);
    return mesh + tag;
}

std::string PrivateBodyOriginal(const std::string& name)
{
    const size_t at = name.find(kPrivateBodyTag);
    const size_t tagLength = std::strlen(kPrivateBodyTag) + 8;
    if (at == std::string::npos || name.size() < at + tagLength)
        return name;
    return name.substr(0, at) + name.substr(at + tagLength);
}

bool DiscHasResource(const std::string& lowerName)
{
    const PakIndex* owner = nullptr;
    return FindElsewhere(nullptr, lowerName, owner) != nullptr;
}

bool HasLooseResource(const std::string& lowerName)
{
    return HasLooseCopy(("d:\\" + lowerName).c_str());
}

void RegisterResourcePatch(const std::string& lowerName, ResourcePatch patch)
{
    std::lock_guard<std::mutex> lock(g_PatchLock);
    g_Patches[lowerName] = patch;
}

void RegisterResourceGenerator(ResourceGenerator generator)
{
    std::lock_guard<std::mutex> lock(g_GeneratorLock);
    if (std::find(g_Generators.begin(), g_Generators.end(), generator) == g_Generators.end())
        g_Generators.push_back(generator);
}

bool ReadDiscResource(const std::string& lowerName, std::vector<uint8_t>& data)
{
    const PakIndex* owner = nullptr;
    const PakIndex::Entry* entry = FindElsewhere(nullptr, lowerName, owner);
    return entry && !(entry->imageOffset >= 0 && entry->imageSize > 0) && owner->ReadData(*entry, data) && !data.empty();
}

std::vector<std::string> DiscResourceNames(const std::string& lowerPrefix)
{
    const PakIndex* owner = nullptr;
    FindElsewhere(nullptr, std::string(), owner); // indexes the disc's PAKs
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(g_IndexLock);
    for (const PakIndex* index : g_DiscPaks) {
        for (const PakIndex::Entry* entry : index->WithPrefix(lowerPrefix)) {
            if (std::find(names.begin(), names.end(), entry->name) == names.end())
                names.push_back(entry->name);
        }
    }
    return names;
}

namespace {

void ListLoose(const std::wstring& folder, const std::string& relative, std::vector<std::string>& names)
{
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (found.cFileName[0] == L'.')
            continue;
        std::string name;
        for (const wchar_t* c = found.cFileName; *c; ++c)
            name += char(*c < 128 ? towlower(*c) : '_');
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ListLoose(folder + L"\\" + found.cFileName, relative + name + "\\", names);
        else
            names.push_back(relative + name);
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

} // namespace

std::vector<std::string> LooseResourceNames(const std::string& lowerPrefix)
{
    std::vector<std::string> names;
    if (g_Mods.empty())
        return names;
    std::wstring folder = g_Mods + L"\\"; // g_Mods has no trailing backslash (see LooseHostPath)
    for (char c : lowerPrefix)
        folder += wchar_t(static_cast<unsigned char>(c));
    while (folder.back() == L'\\')
        folder.pop_back();
    ListLoose(folder, lowerPrefix, names);
    return names;
}

bool DeclareDiscResource(const std::string& lowerName)
{
    if (!g_LevelScene)
        return false;
    const PakIndex* owner = nullptr;
    const PakIndex::Entry* entry = FindElsewhere(g_LevelIndex, lowerName, owner);
    return entry && LoadUndeclared(g_LevelScene, lowerName, *entry, false);
}

bool DeclareGeneratedResource(const std::string& lowerName, int typeId)
{
    uint8_t* scene = g_LevelScene;
    const std::vector<uint8_t>* generated = scene ? Generated(lowerName) : nullptr;
    if (!generated)
        return false;
    alignas(4) uint8_t path[0x100] = {};
    PathFromText(path, nullptr, lowerName.c_str(), 1); // relative, as the engine names resources
    auto* enginePath = reinterpret_cast<const EnginePath*>(path);
    if (SceneIsDeclared(scene, nullptr, enginePath, typeId) ||
        !Declare(scene, enginePath, typeId, uint32_t(generated->size())))
        return false;
    LOG_INFO("Declare: %s (type %d), generated", lowerName.c_str(), typeId);
    return true;
}

bool LoadGeneratedResource(const std::string& lowerName, int typeId)
{
    uint8_t* scene = g_LevelScene;
    if (!scene || !Generated(lowerName))
        return false;
    DeclareGeneratedResource(lowerName, typeId);
    alignas(4) uint8_t path[0x100] = {};
    PathFromText(path, nullptr, lowerName.c_str(), 1);
    void* loaded = g_OriginalSceneLoad(scene, nullptr, reinterpret_cast<const EnginePath*>(path), typeId, 0);
    if (!loaded)
        LOG_WARN("Generated: %s did not load", lowerName.c_str());
    return loaded != nullptr;
}

void InstallResourceHooks(const std::wstring& gameData, const std::wstring& modsDir, const std::wstring& cacheDir,
    const std::wstring& dumpDir, bool logResources)
{
    g_EngineMessageHookInstalled = false;
    g_GameData = gameData;
    g_Mods = modsDir;
    g_GeneratedDir = cacheDir.empty() ? std::wstring() : cacheDir + L"\\disc";
    g_DumpDir = dumpDir;
    if (!g_DumpDir.empty()) {
        CreateDirectoryW(g_DumpDir.c_str(), nullptr);
        LOG_INFO("Dumping resources to %ls", g_DumpDir.c_str());
    }
    g_LogResources = logResources;
    // Trampoline: the displaced "sub esp, 0x90" (6 bytes), then jump back.
    uint8_t* stub = AllocStub(16);
    static const uint8_t kPrologue[6] = { 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00 };
    std::memcpy(stub, kPrologue, sizeof(kPrologue));
    stub[6] = 0xE9;
    int32_t rel = int32_t(kSceneResRead + 6) - int32_t(uintptr_t(stub) + 11);
    std::memcpy(stub + 7, &rel, 4);
    g_OriginalRead = reinterpret_cast<ReadFn>(stub);
    PatchJump(kSceneResRead, reinterpret_cast<const void*>(&ReadHook));

    // Trampoline: the displaced "sub esp, 0x10C" (6 bytes), then jump back.
    g_FolderCapacity.clear(); // a rebooted game's scenes and image blocks are new
    g_RegisteredImages.clear();
    g_ReservedScenes.clear();
    g_LevelScene = nullptr;
    g_LevelIndex = nullptr;
    uint8_t* loadStub = AllocStub(16);
    static const uint8_t kLoadPrologue[6] = { 0x81, 0xEC, 0x0C, 0x01, 0x00, 0x00 };
    std::memcpy(loadStub, kLoadPrologue, sizeof(kLoadPrologue));
    loadStub[6] = 0xE9;
    rel = int32_t(kSceneLoadResource + 6) - int32_t(uintptr_t(loadStub) + 11);
    std::memcpy(loadStub + 7, &rel, 4);
    g_OriginalSceneLoad = reinterpret_cast<SceneLoadFn>(loadStub);
    PatchJump(kSceneLoadResource, reinterpret_cast<const void*>(&SceneLoadHook));

    // Trampoline: the displaced "sub esp, 0x74 / push ebx / xor ebx, ebx" (6 bytes), then jump back.
    uint8_t* manualStub = AllocStub(16);
    static const uint8_t kManualPrologue[6] = { 0x83, 0xEC, 0x74, 0x53, 0x33, 0xDB };
    std::memcpy(manualStub, kManualPrologue, sizeof(kManualPrologue));
    manualStub[6] = 0xE9;
    rel = int32_t(kPackerOpenManual + 6) - int32_t(uintptr_t(manualStub) + 11);
    std::memcpy(manualStub + 7, &rel, 4);
    g_OriginalOpenManual = reinterpret_cast<OpenManualFn>(manualStub);

    // Trampoline: the displaced "sub esp, 0x2C / push ebx / push ebp" (5 bytes), then jump back.
    uint8_t* lookupStub = AllocStub(16);
    static const uint8_t kLookupPrologue[5] = { 0x83, 0xEC, 0x2C, 0x53, 0x55 };
    std::memcpy(lookupStub, kLookupPrologue, sizeof(kLookupPrologue));
    lookupStub[5] = 0xE9;
    rel = int32_t(kAnimationLookup + 5) - int32_t(uintptr_t(lookupStub) + 10);
    std::memcpy(lookupStub + 6, &rel, 4);
    g_OriginalAnimationLookup = reinterpret_cast<AnimationLookupFn>(lookupStub);
    PatchJump(kAnimationLookup, reinterpret_cast<const void*>(&AnimationLookupHook));

    // Trampoline: the displaced "sub esp, 0x158" (6 bytes), then jump back.
    uint8_t* tableStub = AllocStub(16);
    static const uint8_t kTablePrologue[6] = { 0x81, 0xEC, 0x58, 0x01, 0x00, 0x00 };
    std::memcpy(tableStub, kTablePrologue, sizeof(kTablePrologue));
    tableStub[6] = 0xE9;
    rel = int32_t(kOpenCharacterTable + 6) - int32_t(uintptr_t(tableStub) + 11);
    std::memcpy(tableStub + 7, &rel, 4);
    g_OriginalOpenCharacterTable = reinterpret_cast<OpenCharacterTableFn>(tableStub);
    PatchJump(kOpenCharacterTable, reinterpret_cast<const void*>(&OpenCharacterTableHook));

    // Trampoline: the displaced "push ecx / mov eax, [esp + 8]" (5 bytes), then jump back.
    uint8_t* eventStub = AllocStub(16);
    static const uint8_t kEventPrologue[5] = { 0x51, 0x8B, 0x44, 0x24, 0x08 };
    std::memcpy(eventStub, kEventPrologue, sizeof(kEventPrologue));
    eventStub[5] = 0xE9;
    rel = int32_t(kLoadEventTable + 5) - int32_t(uintptr_t(eventStub) + 10);
    std::memcpy(eventStub + 6, &rel, 4);
    g_OriginalLoadEventTable = reinterpret_cast<LoadEventTableFn>(eventStub);
    PatchJump(kLoadEventTable, reinterpret_cast<const void*>(&LoadEventTableHook));
    PatchJump(kPackerOpenManual, reinterpret_cast<const void*>(&OpenManualHook));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kPackerEndManual)), kPackerEndManualBytes,
            sizeof(kPackerEndManualBytes)) == 0) {
        PatchJump(kPackerEndManual, reinterpret_cast<const void*>(&PackerEndManualGuard));
        PatchNop(kPackerEndManual + 5, sizeof(kPackerEndManualBytes) - 5);
    } else {
        LOG_WARN("Resources: unexpected code at the manual file end (0x51E40); not guarded");
    }
}

} // namespace swrots::game
