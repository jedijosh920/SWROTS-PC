// File system: Xbox object names -> host paths, and NT file services.
//
//   \??\D:                       -> symbolic link -> \Device\CdRom0
//   \Device\CdRom0\...           -> <GameData>\...
//   \Device\Harddisk0\Partition1 -> <saves>\...  (E:, save games)
//   \Device\Harddisk0\Partition3 -> <cache>\...  (Z:, utility drive)
//   other partitions            -> <cache>\partitionN\...
//
// Everything else is forwarded to the host's NT file API, which the Xbox
// kernel's file API was derived from.

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <intrin.h>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/game.h"
#include "kernel/exports.h"
#include "kernel/host_nt.h"
#include "kernel/kernel.h"
#include "kernel/reboot.h"

namespace swrots::kernel {

// Host FILE_DIRECTORY_INFORMATION (not declared by winternl.h).
struct HostDirectoryInformation {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
};

static std::wstring g_GameData;
static std::wstring g_Saves;
static std::wstring g_Cache;
static std::wstring g_Mods;
static std::wstring g_Logs;

static std::mutex g_LinkLock;
static std::map<std::string, std::string> g_Links; // lower-case link name -> target

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

static std::string FromObjectString(const xbox::OBJECT_STRING* s)
{
    return s && s->Buffer ? std::string(s->Buffer, s->Length) : std::string();
}

static NTSTATUS Check(const char* what, HANDLE handle, NTSTATUS status)
{
    if (status < 0)
        LOG_WARN("%s(%p) failed: %08lX", what, handle, status);
    return status;
}

static std::wstring Widen(const std::string& s)
{
    std::wstring w(s.size(), L'\0');
    for (size_t i = 0; i < s.size(); ++i)
        w[i] = wchar_t(static_cast<unsigned char>(s[i]));
    return w;
}

void CreateSymbolicLink(const std::string& link, const std::string& target)
{
    std::lock_guard<std::mutex> lock(g_LinkLock);
    g_Links[Lower(link)] = target;
    LOG_INFO("Symbolic link %s -> %s", link.c_str(), target.c_str());
}

void ResetLinkHandlesForReboot();

// Reboot: back to the links the kernel creates before starting a title.
// The game's error loop (QuietFileLog, below), per boot.
static bool g_GameErrorLoop = false;
static int g_MessageLogOpens = 0; // this boot's (a few at boot: it starts the file)

void ResetFileSystemForReboot()
{
    g_GameErrorLoop = false;
    g_MessageLogOpens = 0;
    {
        std::lock_guard<std::mutex> lock(g_LinkLock);
        g_Links.clear();
    }
    CreateSymbolicLink("\\??\\D:", "\\Device\\CdRom0");
    ResetLinkHandlesForReboot();
}

void FileSystemInit(const Paths& paths)
{
    g_GameData = paths.gameData;
    g_Saves = paths.saves;
    g_Cache = paths.cache;
    g_Mods = paths.mods;
    g_Logs = paths.logs;
    CreateDirectoryW(g_Mods.c_str(), nullptr);
    CreateDirectoryW(g_Saves.c_str(), nullptr);
    CreateDirectoryW(g_Cache.c_str(), nullptr);
    // The Xbox dashboard creates these on the save drive.
    CreateDirectoryW((g_Saves + L"\\TDATA").c_str(), nullptr);
    CreateDirectoryW((g_Saves + L"\\UDATA").c_str(), nullptr);

    // Links the Xbox kernel creates before starting a title.
    CreateSymbolicLink("\\??\\D:", "\\Device\\CdRom0");
}

// Host folder for an Xbox hard disk partition: 1 is the save drive (E:),
// 3 the utility drive (Z:); the others (the system partition and the other
// cache partitions) are created under the cache folder if a title uses them.
static std::wstring PartitionRoot(const std::string& number)
{
    if (number == "1")
        return g_Saves;
    if (number == "3")
        return g_Cache;
    std::wstring root = g_Cache + L"\\partition" + Widen(number);
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

// Files generated for the disc drive, kept under cache\disc\ and read from there (a file there
// takes the disc's place; a mods\ copy takes precedence). The engine's own: a development-minded
// engine rebuilds a mesh's animation binding (.ban) when the one it loaded lacks animations the
// character needs (a character used in a level it was not built for) and writes it next to the
// mesh; the disc is not writable, so such writes go to cache\disc\. Only .ban files are written
// there: the engine's development modes can also write whole PAKs, which must never shadow the
// disc's. The port's: resources it makes (game::RegisterResourceGenerator).
static bool IsGeneratedFile(const std::string& rest)
{
    std::string lower = Lower(rest);
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".ban") == 0;
}

static std::wstring GeneratedPath(const std::string& rest)
{
    return g_Cache + L"\\disc" + Widen(rest);
}

// Resolves an Xbox object name to a host NT path ("\??\C:\...").
static bool ResolveDevicePath(const std::string& path, std::wstring& host)
{
    std::string lower = Lower(path);
    auto starts = [&](const char* prefix) { return lower.rfind(prefix, 0) == 0; };

    if (starts("\\??\\")) {
        // \??\X:\rest -> link target + rest
        size_t colon = lower.find(':');
        if (colon == std::string::npos)
            return false;
        std::string link = lower.substr(0, colon + 1);
        std::string target;
        {
            std::lock_guard<std::mutex> lock(g_LinkLock);
            auto it = g_Links.find(link);
            if (it == g_Links.end())
                return false;
            target = it->second;
        }
        return ResolveDevicePath(target + path.substr(colon + 1), host);
    }

    std::string rest;
    std::wstring root;
    if (starts("\\device\\cdrom0")) {
        root = g_GameData;
        rest = path.substr(strlen("\\device\\cdrom0"));
        // The engine writes its message log onto the disc drive; keep the game files untouched.
        if (!g_Logs.empty() && Lower(rest) == "\\message.log") {
            host = L"\\??\\" + g_Logs + L"\\Message.log";
            return true;
        }
        // Mod overrides: a file at the same relative path under mods\ replaces
        // the disc's copy. Directories resolve to mods\ only when the disc has
        // none (resource folders that otherwise exist only inside PAKs).
        if (!g_Mods.empty() && rest.size() > 1) {
            std::wstring candidate = g_Mods + Widen(rest);
            DWORD attrs = GetFileAttributesW(candidate.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    root = g_Mods;
                    LOG_INFO("Mod override: %s", rest.c_str());
                } else if (GetFileAttributesW((g_GameData + Widen(rest)).c_str()) == INVALID_FILE_ATTRIBUTES) {
                    root = g_Mods;
                }
            }
        }
        // Generated files likewise, after mods\; their folders when neither the disc nor mods\ has
        // them (a listing of the folder sizes the files' reads).
        if (root == g_GameData && rest.size() > 1) {
            DWORD generated = GetFileAttributesW(GeneratedPath(rest).c_str());
            if (generated != INVALID_FILE_ATTRIBUTES && (!(generated & FILE_ATTRIBUTE_DIRECTORY) ||
                    GetFileAttributesW((g_GameData + Widen(rest)).c_str()) == INVALID_FILE_ATTRIBUTES))
                root = g_Cache + L"\\disc";
        }
    } else if (starts("\\device\\harddisk0\\partition")) {
        size_t n = strlen("\\device\\harddisk0\\partition");
        size_t end = lower.find('\\', n);
        std::string num = lower.substr(n, end == std::string::npos ? std::string::npos : end - n);
        if (num.empty() || num == "0")
            return false; // raw disk access is not supported
        root = PartitionRoot(num);
        rest = end == std::string::npos ? std::string() : path.substr(end);
    } else {
        return false;
    }

    host = L"\\??\\" + root + Widen(rest);
    // Drop a trailing separator except for a bare drive root.
    while (host.size() > 7 && host.back() == L'\\')
        host.pop_back();
    return true;
}

// Builds host OBJECT_ATTRIBUTES for an Xbox one. `storage` keeps the name alive.
static NTSTATUS TranslateAttributes(const xbox::OBJECT_ATTRIBUTES* xoa, ::OBJECT_ATTRIBUTES& oa, UNICODE_STRING& name,
    std::wstring& storage, std::string* xboxPathOut = nullptr)
{
    std::string xpath = FromObjectString(xoa->ObjectName);
    HANDLE root = xoa->RootDirectory;

    if (root == reinterpret_cast<HANDLE>(-3)) { // ObDosDevicesDirectory()
        xpath = "\\??\\" + xpath;
        root = nullptr;
    }

    if (root) {
        // Relative to a directory handle we opened earlier.
        storage = Widen(xpath);
    } else if (!ResolveDevicePath(xpath, storage)) {
        LOG_WARN("Unresolvable Xbox path '%s'", xpath.c_str());
        return xbox::X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (xboxPathOut)
        *xboxPathOut = xpath;

    name.Buffer = storage.data();
    name.Length = USHORT(storage.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, root, nullptr);
    return xbox::X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Symbolic links
// ---------------------------------------------------------------------------
static std::mutex g_PseudoLock;
static std::map<uintptr_t, std::string> g_LinkHandles;
static uintptr_t g_NextPseudo = 0x7F000000;

void ResetLinkHandlesForReboot()
{
    std::lock_guard<std::mutex> lock(g_PseudoLock);
    g_LinkHandles.clear();
}

static bool IsPseudoHandle(HANDLE h)
{
    std::lock_guard<std::mutex> lock(g_PseudoLock);
    return g_LinkHandles.count(uintptr_t(h)) != 0;
}

NTSTATUS XBAPI IoCreateSymbolicLink(xbox::OBJECT_STRING* SymbolicLinkName, xbox::OBJECT_STRING* DeviceName)
{
    CreateSymbolicLink(FromObjectString(SymbolicLinkName), FromObjectString(DeviceName));
    return xbox::X_STATUS_SUCCESS;
}

NTSTATUS XBAPI IoDeleteSymbolicLink(xbox::OBJECT_STRING* SymbolicLinkName)
{
    std::lock_guard<std::mutex> lock(g_LinkLock);
    return g_Links.erase(Lower(FromObjectString(SymbolicLinkName))) ? xbox::X_STATUS_SUCCESS : xbox::X_STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS XBAPI NtOpenSymbolicLinkObject(HANDLE* LinkHandle, xbox::OBJECT_ATTRIBUTES* ObjectAttributes)
{
    std::string name = Lower(FromObjectString(ObjectAttributes->ObjectName));
    std::string target;
    {
        std::lock_guard<std::mutex> lock(g_LinkLock);
        auto it = g_Links.find(name);
        if (it == g_Links.end())
            return xbox::X_STATUS_OBJECT_NAME_NOT_FOUND;
        target = it->second;
    }
    std::lock_guard<std::mutex> lock(g_PseudoLock);
    uintptr_t h = (g_NextPseudo += 4);
    g_LinkHandles[h] = target;
    *LinkHandle = reinterpret_cast<HANDLE>(h);
    return xbox::X_STATUS_SUCCESS;
}

NTSTATUS XBAPI NtQuerySymbolicLinkObject(HANDLE LinkHandle, xbox::OBJECT_STRING* LinkTarget, ULONG* ReturnedLength)
{
    std::string target;
    {
        std::lock_guard<std::mutex> lock(g_PseudoLock);
        auto it = g_LinkHandles.find(uintptr_t(LinkHandle));
        if (it == g_LinkHandles.end())
            return xbox::X_STATUS_INVALID_HANDLE;
        target = it->second;
    }
    if (ReturnedLength)
        *ReturnedLength = ULONG(target.size());
    if (LinkTarget->MaximumLength < target.size())
        return xbox::X_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(LinkTarget->Buffer, target.data(), target.size());
    LinkTarget->Length = USHORT(target.size());
    return xbox::X_STATUS_SUCCESS;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink);
KERNEL_EXPORT(69, IoDeleteSymbolicLink);
KERNEL_EXPORT(203, NtOpenSymbolicLinkObject);
KERNEL_EXPORT(215, NtQuerySymbolicLinkObject);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
// Debugging: env SWROTS_TRACE_OPEN=<text> logs the game functions on the stack when a path
// containing <text> is opened (return addresses into game code, named from symbols\swrots.map).
static void TraceOpenCallers(const std::string& xpath, void* stackTop)
{
    static char filter[128] = {};
    static bool loaded = false;
    if (!loaded) {
        GetEnvironmentVariableA("SWROTS_TRACE_OPEN", filter, sizeof(filter));
        loaded = true;
    }
    if (!*filter || Lower(xpath).find(Lower(filter)) == std::string::npos)
        return;
    LOG_INFO("Callers of open '%s':", xpath.c_str());
    game::LogGameCallers("open", stackTop);
}


// The game writes D:\Message.log (logs\Message.log) at boot and when something went wrong. Its exception
// handler can fail and start over without end (a fault it cannot handle),
// rewriting the file thousands of times a second: that is said once, and the file layer's debug lines
// stop (they filled a log with 70 MB).

static bool QuietFileLog(const std::string& xpath)
{
    if (g_GameErrorLoop)
        return true;
    if (xpath.size() < 11 || _stricmp(xpath.c_str() + xpath.size() - 11, "Message.log") != 0)
        return false;
    if (++g_MessageLogOpens == 200) {
        g_GameErrorLoop = true;
        LOG_ERROR("The game is stuck in its own error handler (it keeps rewriting logs\\Message.log); the game has "
                  "stopped. The lines above say what went wrong.");
    }
    return false;
}

NTSTATUS XBAPI NtCreateFile(HANDLE* FileHandle, ACCESS_MASK DesiredAccess, xbox::OBJECT_ATTRIBUTES* ObjectAttributes,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, LARGE_INTEGER* AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    ::OBJECT_ATTRIBUTES oa;
    UNICODE_STRING name;
    std::wstring storage;
    std::string xpath;
    NTSTATUS status = TranslateAttributes(ObjectAttributes, oa, name, storage, &xpath);
    if (status < 0) {
        IoStatusBlock->Status = status;
        return status;
    }

    // A generated file created on the disc drive goes to cache\disc (see IsGeneratedFile).
    std::wstring discRoot = L"\\??\\" + g_GameData;
    if (CreateDisposition != FILE_OPEN && !oa.RootDirectory && storage.size() > discRoot.size() &&
        _wcsnicmp(storage.c_str(), discRoot.c_str(), discRoot.size()) == 0) {
        std::wstring rest = storage.substr(discRoot.size());
        std::string narrow;
        for (wchar_t c : rest)
            narrow += char(c < 0x80 ? c : '_'); // game paths are ASCII
        if (IsGeneratedFile(narrow)) {
            std::wstring target = GeneratedPath(narrow);
            for (size_t i = g_Cache.size() + 1; (i = target.find(L'\\', i + 1)) != std::wstring::npos;)
                CreateDirectoryW(target.substr(0, i).c_str(), nullptr);
            storage = L"\\??\\" + target;
            name.Buffer = storage.data();
            name.Length = USHORT(storage.size() * sizeof(wchar_t));
            name.MaximumLength = name.Length;
            LOG_INFO("Generated file %s goes to cache\\disc", narrow.c_str());
        }
    }

    // The Xbox I/O manager lets any handle query/set basic file information;
    // Windows requires the attribute access rights for that.
    DesiredAccess |= FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (DesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL))
        DesiredAccess |= FILE_WRITE_ATTRIBUTES;
    // Xbox titles rarely share; the host must allow the game's overlapping opens.
    ShareAccess |= FILE_SHARE_READ | FILE_SHARE_WRITE;
    // Unbuffered I/O would impose host sector alignment on the game's buffers.
    CreateOptions &= ~FILE_NO_INTERMEDIATE_BUFFERING;

    status = ::NtCreateFile(FileHandle, DesiredAccess, &oa, reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock),
        AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, nullptr, 0);
    if (!QuietFileLog(xpath))
        LOG_DEBUG("NtCreateFile '%s' disp %lu opts %08lX -> %08lX handle %p", xpath.c_str(), CreateDisposition, CreateOptions,
            status, status >= 0 ? *FileHandle : nullptr);
    if (status >= 0)
        TrackGameHandle(*FileHandle);
    TraceOpenCallers(xpath, _AddressOfReturnAddress());
    return status;
}

NTSTATUS XBAPI NtOpenFile(HANDLE* FileHandle, ACCESS_MASK DesiredAccess, xbox::OBJECT_ATTRIBUTES* ObjectAttributes,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions)
{
    return NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, nullptr, 0, ShareAccess, FILE_OPEN, OpenOptions);
}

NTSTATUS XBAPI NtReadFile(HANDLE FileHandle, HANDLE Event, xbox::PIO_APC_ROUTINE ApcRoutine, void* ApcContext,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, void* Buffer, ULONG Length, LARGE_INTEGER* ByteOffset)
{
    return Check("NtReadFile", FileHandle, ::NtReadFile(FileHandle, Event, reinterpret_cast<PIO_APC_ROUTINE>(ApcRoutine),
        ApcContext, reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock), Buffer, Length, ByteOffset, nullptr));
}

NTSTATUS XBAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, xbox::PIO_APC_ROUTINE ApcRoutine, void* ApcContext,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, void* Buffer, ULONG Length, LARGE_INTEGER* ByteOffset)
{
    return Check("NtWriteFile", FileHandle, ::NtWriteFile(FileHandle, Event, reinterpret_cast<PIO_APC_ROUTINE>(ApcRoutine),
        ApcContext, reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock), Buffer, Length, ByteOffset, nullptr));
}

NTSTATUS XBAPI NtQueryInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock, void* FileInformation,
    ULONG Length, ULONG FileInformationClass)
{
    switch (FileInformationClass) {
    case xbox::XFileBasicInformation:
    case xbox::XFileStandardInformation:
    case xbox::XFileInternalInformation:
    case xbox::XFilePositionInformation:
    case xbox::XFileModeInformation:
    case xbox::XFileAlignmentInformation:
    case xbox::XFileNetworkOpenInformation:
        return Check("NtQueryInformationFile", FileHandle, ::NtQueryInformationFile(FileHandle,
            reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock), FileInformation, Length, FileInformationClass));
    default:
        LOG_WARN("NtQueryInformationFile: unsupported class %lu", FileInformationClass);
        IoStatusBlock->Status = xbox::X_STATUS_INVALID_PARAMETER;
        return xbox::X_STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS XBAPI NtSetInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock, void* FileInformation,
    ULONG Length, ULONG FileInformationClass)
{
    if (FileInformationClass == xbox::XFileRenameInformation) {
        // Xbox: { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING FileName; }
        struct XRename {
            BOOLEAN ReplaceIfExists;
            HANDLE RootDirectory;
            xbox::OBJECT_STRING FileName;
        };
        auto* xr = static_cast<XRename*>(FileInformation);
        xbox::OBJECT_ATTRIBUTES xoa = { xr->RootDirectory, &xr->FileName, 0 };
        ::OBJECT_ATTRIBUTES oa;
        UNICODE_STRING name;
        std::wstring storage;
        NTSTATUS status = TranslateAttributes(&xoa, oa, name, storage);
        if (status < 0)
            return status;
        std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + storage.size() * sizeof(wchar_t));
        auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
        ri->ReplaceIfExists = xr->ReplaceIfExists;
        ri->RootDirectory = oa.RootDirectory;
        ri->FileNameLength = DWORD(storage.size() * sizeof(wchar_t));
        std::memcpy(ri->FileName, storage.data(), ri->FileNameLength);
        return ::NtSetInformationFile(FileHandle, reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock), ri, ULONG(buf.size()),
            FileInformationClass);
    }
    if (!g_GameErrorLoop)
        LOG_DEBUG("NtSetInformationFile(%p, class %lu)", FileHandle, FileInformationClass);
    return Check("NtSetInformationFile", FileHandle, ::NtSetInformationFile(FileHandle,
        reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock), FileInformation, Length, FileInformationClass));
}

NTSTATUS XBAPI NtQueryVolumeInformationFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock, void* FsInformation,
    ULONG Length, ULONG FsInformationClass)
{
    NTSTATUS status = ::NtQueryVolumeInformationFile(FileHandle, reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock),
        FsInformation, Length, FsInformationClass);
    if (status >= 0 && FsInformationClass == 3 /*FileFsSizeInformation*/) {
        // Present an Xbox-sized volume so 32-bit free-space math cannot overflow.
        struct FsSize {
            LARGE_INTEGER TotalAllocationUnits;
            LARGE_INTEGER AvailableAllocationUnits;
            ULONG SectorsPerAllocationUnit;
            ULONG BytesPerSector;
        };
        auto* fs = static_cast<FsSize*>(FsInformation);
        fs->SectorsPerAllocationUnit = 32;
        fs->BytesPerSector = 512;
        fs->TotalAllocationUnits.QuadPart = 0x40000;     // 4 GiB
        fs->AvailableAllocationUnits.QuadPart = 0x20000; // 2 GiB free
    }
    Check("NtQueryVolumeInformationFile", FileHandle, status);
    if (FsInformationClass != 3)
        LOG_DEBUG("NtQueryVolumeInformationFile class %lu -> %08lX", FsInformationClass, status);
    return status;
}

NTSTATUS XBAPI NtQueryFullAttributesFile(xbox::OBJECT_ATTRIBUTES* ObjectAttributes, void* FileInformation)
{
    ::OBJECT_ATTRIBUTES oa;
    UNICODE_STRING name;
    std::wstring storage;
    NTSTATUS status = TranslateAttributes(ObjectAttributes, oa, name, storage);
    if (status < 0)
        return status;
    return ::NtQueryFullAttributesFile(&oa, FileInformation);
}

// The path of an open directory relative to the disc ("" for its root, else "\folder\..."), or
// false when it is not a disc directory.
static bool DiscRelative(HANDLE dir, std::wstring& relative)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFinalPathNameByHandleW(dir, buf, DWORD(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= std::size(buf))
        return false;
    std::wstring path(buf, n);
    if (path.rfind(L"\\\\?\\", 0) == 0)
        path.erase(0, 4);
    if (path.size() < g_GameData.size() || _wcsnicmp(path.c_str(), g_GameData.c_str(), g_GameData.size()) != 0
        || (path.size() > g_GameData.size() && path[g_GameData.size()] != L'\\'))
        return false;
    relative = path.substr(g_GameData.size());
    return true;
}

// A disc directory listing must describe mod overrides, not the disc's copies: the game
// sizes some reads from the listing (vars_xbox.cfg is read with the size FindFirstFile
// reports). Returns the overriding mods\ file for `name` in the directory `dir`, if any, else a
// generated one (cache\disc\).
static bool FindModOverride(HANDLE dir, const wchar_t* name, size_t nameChars, WIN32_FILE_ATTRIBUTE_DATA& data)
{
    std::wstring folder;
    if (g_Mods.empty() || !DiscRelative(dir, folder))
        return false;
    const std::wstring relative = folder + L"\\" + std::wstring(name, nameChars);
    for (const std::wstring& candidate : { g_Mods + relative, g_Cache + L"\\disc" + relative }) {
        if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &data)
            && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return true;
    }
    return false;
}

// A disc directory listing must also list the files only mods\ or cache\disc\ has for the
// directory (a loose file in a folder the disc has too, e.g. mods\gameinfo\missionlist.txt, or a
// generated interfc\front_end\ texture): the game finds files and their sizes by listing. They come
// after the disc's own entries; the names still to report, per open listing, start at a restart.
static std::mutex g_ListingLock;
static std::map<HANDLE, std::vector<std::wstring>> g_ListingExtras;

static bool MatchesMask(const wchar_t* name, const wchar_t* mask)
{
    if (!*mask)
        return !*name;
    if (*mask == L'*')
        return MatchesMask(name, mask + 1) || (*name && MatchesMask(name + 1, mask));
    return *name && (*mask == L'?' || towlower(*name) == towlower(*mask)) && MatchesMask(name + 1, mask + 1);
}

static std::vector<std::wstring> ExtraListing(HANDLE dir, const std::wstring& mask)
{
    std::vector<std::wstring> names;
    std::wstring folder;
    if (g_Mods.empty() || !DiscRelative(dir, folder))
        return names;
    for (const std::wstring& root : { g_Mods, g_Cache + L"\\disc" }) {
        WIN32_FIND_DATAW found;
        HANDLE find = FindFirstFileW((root + folder + L"\\*").c_str(), &found);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do {
            std::wstring name = found.cFileName;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !MatchesMask(name.c_str(), mask.empty() ? L"*" : mask.c_str()) ||
                GetFileAttributesW((g_GameData + folder + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES)
                continue;
            if (std::none_of(names.begin(), names.end(), [&](const std::wstring& n) { return _wcsicmp(n.c_str(), name.c_str()) == 0; }))
                names.push_back(name);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return names;
}

NTSTATUS XBAPI NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, xbox::PIO_APC_ROUTINE ApcRoutine, void* ApcContext,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, void* FileInformation, ULONG Length, ULONG FileInformationClass,
    xbox::OBJECT_STRING* FileMask, BOOLEAN RestartScan)
{
    if (ApcRoutine)
        LOG_WARN("NtQueryDirectoryFile: APC completion not supported");
    if (FileInformationClass != xbox::XFileDirectoryInformation) {
        LOG_WARN("NtQueryDirectoryFile: unsupported class %lu", FileInformationClass);
        return xbox::X_STATUS_INVALID_PARAMETER;
    }

    std::wstring mask = Widen(FromObjectString(FileMask));
    UNICODE_STRING umask = { USHORT(mask.size() * 2), USHORT(mask.size() * 2), mask.data() };

    alignas(8) uint8_t hostBuf[sizeof(HostDirectoryInformation) + MAX_PATH * sizeof(wchar_t)];
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status = ::NtQueryDirectoryFile(FileHandle, nullptr, nullptr, nullptr, &iosb, hostBuf, sizeof(hostBuf),
        1 /*FileDirectoryInformation*/, TRUE, mask.empty() ? nullptr : &umask, RestartScan);
    // A file only in mods\ or cache\disc\ (not on the disc): a lookup by exact name finds it there,
    // and a listing reports it after the disc's entries.
    auto addEntry = [&](const std::wstring& name) {
        WIN32_FILE_ATTRIBUTE_DATA added;
        if (name.size() >= MAX_PATH || !FindModOverride(FileHandle, name.c_str(), name.size(), added))
            return false;
        auto* h = reinterpret_cast<HostDirectoryInformation*>(hostBuf);
        memset(h, 0, sizeof(HostDirectoryInformation));
        auto time = [](const FILETIME& t) { LARGE_INTEGER v; v.QuadPart = LONGLONG(t.dwHighDateTime) << 32 | t.dwLowDateTime; return v; };
        h->CreationTime = time(added.ftCreationTime);
        h->LastAccessTime = time(added.ftLastAccessTime);
        h->LastWriteTime = h->ChangeTime = time(added.ftLastWriteTime);
        h->FileAttributes = added.dwFileAttributes;
        h->FileNameLength = ULONG(name.size() * sizeof(wchar_t));
        memcpy(h->FileName, name.c_str(), h->FileNameLength);
        return true; // the size is filled in below, as for an override
    };
    const bool wildcard = mask.empty() || mask.find_first_of(L"*?<>\"") != std::wstring::npos;
    if (!wildcard) {
        if (status < 0 && RestartScan && addEntry(mask))
            status = 0;
    } else {
        std::lock_guard<std::mutex> lock(g_ListingLock);
        if (RestartScan)
            g_ListingExtras[FileHandle] = ExtraListing(FileHandle, mask == L"*.*" ? std::wstring(L"*") : mask);
        auto it = g_ListingExtras.find(FileHandle);
        if (status < 0 && (status == NTSTATUS(0x80000006L) || status == NTSTATUS(0xC000000FL)) && it != g_ListingExtras.end()) {
            while (!it->second.empty() && status < 0) { // STATUS_NO_MORE_FILES / STATUS_NO_SUCH_FILE
                std::wstring name = it->second.front();
                it->second.erase(it->second.begin());
                if (addEntry(name))
                    status = 0;
            }
            if (it->second.empty())
                g_ListingExtras.erase(it);
        }
    }
    if (status >= 0) {
        auto* h = reinterpret_cast<HostDirectoryInformation*>(hostBuf);
        auto* x = static_cast<xbox::FILE_DIRECTORY_INFORMATION*>(FileInformation);
        ULONG nameChars = h->FileNameLength / sizeof(wchar_t);
        ULONG need = ULONG(offsetof(xbox::FILE_DIRECTORY_INFORMATION, FileName)) + nameChars;
        if (need > Length) {
            status = xbox::X_STATUS_BUFFER_TOO_SMALL;
        } else {
            x->NextEntryOffset = 0;
            x->FileIndex = h->FileIndex;
            x->CreationTime = h->CreationTime;
            x->LastAccessTime = h->LastAccessTime;
            x->LastWriteTime = h->LastWriteTime;
            x->ChangeTime = h->ChangeTime;
            x->EndOfFile = h->EndOfFile;
            x->AllocationSize = h->AllocationSize;
            x->FileAttributes = h->FileAttributes;
            WIN32_FILE_ATTRIBUTE_DATA mod;
            if (!(h->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FindModOverride(FileHandle, h->FileName, nameChars, mod)) {
                x->EndOfFile.QuadPart = LONGLONG(mod.nFileSizeHigh) << 32 | mod.nFileSizeLow;
                x->AllocationSize.QuadPart = (x->EndOfFile.QuadPart + 4095) & ~LONGLONG(4095);
                x->LastWriteTime.QuadPart = LONGLONG(mod.ftLastWriteTime.dwHighDateTime) << 32 | mod.ftLastWriteTime.dwLowDateTime;
            }
            x->FileNameLength = nameChars;
            for (ULONG i = 0; i < nameChars; ++i)
                x->FileName[i] = char(h->FileName[i] < 0x100 ? h->FileName[i] : '_');
            IoStatusBlock->Information = need;
        }
    }
    IoStatusBlock->Status = status;
    if (Event)
        SetEvent(Event);
    return status;
}

NTSTATUS XBAPI NtFlushBuffersFile(HANDLE FileHandle, xbox::IO_STATUS_BLOCK* IoStatusBlock)
{
    return Check("NtFlushBuffersFile", FileHandle, ::NtFlushBuffersFile(FileHandle, reinterpret_cast<PIO_STATUS_BLOCK>(IoStatusBlock)));
}

NTSTATUS XBAPI NtFsControlFile(HANDLE FileHandle, HANDLE Event, xbox::PIO_APC_ROUTINE ApcRoutine, void* ApcContext,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, ULONG FsControlCode, void* InputBuffer, ULONG InputBufferLength,
    void* OutputBuffer, ULONG OutputBufferLength)
{
    LOG_WARN("NtFsControlFile(%p, code %08lX): not supported", FileHandle, FsControlCode);
    IoStatusBlock->Status = xbox::X_STATUS_INVALID_DEVICE_REQUEST;
    return xbox::X_STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS XBAPI NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, xbox::PIO_APC_ROUTINE ApcRoutine, void* ApcContext,
    xbox::IO_STATUS_BLOCK* IoStatusBlock, ULONG IoControlCode, void* InputBuffer, ULONG InputBufferLength,
    void* OutputBuffer, ULONG OutputBufferLength)
{
    LOG_WARN("NtDeviceIoControlFile(%p, code %08lX): not supported", FileHandle, IoControlCode);
    IoStatusBlock->Status = xbox::X_STATUS_INVALID_DEVICE_REQUEST;
    return xbox::X_STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS XBAPI NtClose(HANDLE Handle)
{
    {
        std::lock_guard<std::mutex> lock(g_PseudoLock);
        if (g_LinkHandles.erase(uintptr_t(Handle)))
            return xbox::X_STATUS_SUCCESS;
    }
    UntrackGameHandle(Handle);
    return Check("NtClose", Handle, ::NtClose(Handle));
}

// ApcRoutine XAPI passes for ReadFileEx/WriteFileEx: the context is the Win32
// completion routine, and the IO_STATUS_BLOCK is the head of the OVERLAPPED.
void XBAPI NtUserIoApcDispatcher(void* ApcContext, xbox::IO_STATUS_BLOCK* IoStatusBlock, ULONG Reserved)
{
    (void)Reserved;
    using Completion = void(__stdcall*)(DWORD errorCode, DWORD bytes, void* overlapped);
    DWORD error = IoStatusBlock->Status < 0 ? RtlNtStatusToDosError(IoStatusBlock->Status) : 0;
    reinterpret_cast<Completion>(ApcContext)(error, DWORD(IoStatusBlock->Information), IoStatusBlock);
}

NTSTATUS XBAPI FscSetCacheSize(ULONG NumberOfCachePages)
{
    (void)NumberOfCachePages;
    return xbox::X_STATUS_SUCCESS;
}

KERNEL_EXPORT(190, NtCreateFile);
KERNEL_EXPORT(202, NtOpenFile);
KERNEL_EXPORT(219, NtReadFile);
KERNEL_EXPORT(236, NtWriteFile);
KERNEL_EXPORT(211, NtQueryInformationFile);
KERNEL_EXPORT(226, NtSetInformationFile);
KERNEL_EXPORT(218, NtQueryVolumeInformationFile);
KERNEL_EXPORT(210, NtQueryFullAttributesFile);
KERNEL_EXPORT(207, NtQueryDirectoryFile);
KERNEL_EXPORT(198, NtFlushBuffersFile);
KERNEL_EXPORT(200, NtFsControlFile);
KERNEL_EXPORT(196, NtDeviceIoControlFile);
KERNEL_EXPORT(187, NtClose);
KERNEL_EXPORT(232, NtUserIoApcDispatcher);
KERNEL_EXPORT(37, FscSetCacheSize);

} // namespace swrots::kernel
