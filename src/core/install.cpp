#include "core/install.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/icon.h"
#include "core/log.h"
#include "core/xbe.h"
#include "game/game.h"

namespace swrots {

namespace {

const wchar_t* kTitle = L"Star Wars: Episode III - Revenge of the Sith";

// --- Progress window ------------------------------------------------------------------------

struct Progress {
    std::mutex lock;
    std::wstring status;
    std::atomic<uint64_t> done = 0, total = 0;
    std::atomic<bool> cancel = false, finished = false;
    bool ok = false;
    std::wstring error;

    void SetStatus(const std::wstring& s)
    {
        std::lock_guard<std::mutex> g(lock);
        status = s;
    }
};

Progress* g_Progress = nullptr;

LRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        if (MessageBoxW(hwnd, L"Stop setting up the game?", kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES)
            g_Progress->cancel = true;
        return 0;
    case WM_PAINT: {
        const UINT dpi = GetDpiForWindow(hwnd);
        auto S = [dpi](int v) { return MulDiv(v, int(dpi), 96); }; // layout in 96-DPI units
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        FillRect(mem, &client, GetSysColorBrush(COLOR_WINDOW));
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        std::wstring status;
        {
            std::lock_guard<std::mutex> g(g_Progress->lock);
            status = g_Progress->status;
        }
        RECT text = { S(16), S(14), client.right - S(16), S(60) };
        DrawTextW(mem, status.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        RECT bar = { S(16), S(64), client.right - S(16), S(84) };
        FrameRect(mem, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
        const uint64_t total = g_Progress->total, done = g_Progress->done;
        if (total) {
            RECT fill = bar;
            InflateRect(&fill, -S(2), -S(2));
            fill.right = fill.left + LONG((fill.right - fill.left) * std::min(1.0, double(done) / double(total)));
            FillRect(mem, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
            wchar_t pct[64];
            swprintf_s(pct, L"%.0f%%  (%.1f of %.1f GB)", 100.0 * done / total, done / 1e9, total / 1e9);
            RECT pr = { S(16), S(90), client.right - S(16), S(112) };
            DrawTextW(mem, pct, -1, &pr, DT_LEFT);
        }
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        DeleteObject(font);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Runs `work` on a worker thread while showing its progress.
void RunWithProgress(Progress& p, const std::function<void()>& work)
{
    g_Progress = &p;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ProgressProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SWROTSSetup";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(520, int(dpi), 96), MulDiv(124, int(dpi), 96) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Setting up Star Wars: Episode III", style,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr,
        wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, 1, 100, nullptr);

    std::thread worker([&] {
        work();
        p.finished = true;
        PostMessageW(hwnd, WM_NULL, 0, 0);
    });
    MSG msg;
    while (!p.finished && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    DestroyWindow(hwnd);
    g_Progress = nullptr;
}

// --- Helpers ------------------------------------------------------------------------------------

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = wchar_t(towlower(c));
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == suffix;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void DeleteTree(const std::wstring& path)
{
    if (!Exists(path))
        return;
    std::wstring from = path;
    from.push_back(L'\0'); // double-terminated list
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

uint64_t FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// Runs Windows' tar.exe (libarchive: reads 7z, zip, rar, ...). `output`
// receives stdout if given. While it runs, `poll` is called every 200 ms.
bool RunTar(const std::wstring& args, std::string* output, Progress& p, const std::function<void()>& poll)
{
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\tar.exe\" " + args;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (output) {
        CreatePipe(&readPipe, &writePipe, &sa, 0);
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = nullptr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        return false;
    }
    if (writePipe)
        CloseHandle(writePipe);
    std::thread reader;
    if (output)
        reader = std::thread([&] {
            char buf[4096];
            DWORD n;
            while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n)
                output->append(buf, n);
        });
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT) {
        if (p.cancel) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (poll)
            poll();
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (reader.joinable())
        reader.join();
    if (readPipe)
        CloseHandle(readPipe);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0 && !p.cancel;
}

// Where the Xbox game partition's header ("MICROSOFT*XBOX*MEDIA", sector 32) can be: an XISO
// starts with the game partition, full disc images have the video partition first.
constexpr uint64_t kXboxPartitionBases[] = { 0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
constexpr uint64_t kXboxHeaderOffset = 32 * 2048;
constexpr char kXboxMagic[] = "MICROSOFT*XBOX*MEDIA";

// Streams the start of `member` out of the archive (tar -xO) and looks for the Xbox game
// partition header, stopping as soon as it is found or cannot be there any more: a few
// seconds, instead of unpacking gigabytes to find out.
bool ArchiveImageIsXbox(const std::wstring& archive, const std::wstring& member, Progress& p)
{
    p.SetStatus(L"Checking the disc image...");
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\tar.exe\" -xOf \"" + archive + L"\" \"" + member + L"\"";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 1 << 20))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    CloseHandle(writePipe);

    uint64_t last = 0;
    for (uint64_t base : kXboxPartitionBases)
        last = std::max(last, base + kXboxHeaderOffset + sizeof(kXboxMagic) - 1);
    char seen[std::size(kXboxPartitionBases)][sizeof(kXboxMagic)] = {};
    std::vector<char> buffer(1 << 20);
    uint64_t position = 0;
    bool found = false;
    DWORD n = 0;
    while (!found && !p.cancel && position < last && ReadFile(readPipe, buffer.data(), DWORD(buffer.size()), &n, nullptr) && n) {
        for (size_t i = 0; i < std::size(kXboxPartitionBases); ++i) {
            const uint64_t at = kXboxPartitionBases[i] + kXboxHeaderOffset;
            for (uint64_t b = std::max(at, position); b < std::min(at + sizeof(kXboxMagic) - 1, position + n); ++b)
                seen[i][b - at] = buffer[size_t(b - position)];
            if (position + n >= at + sizeof(kXboxMagic) - 1 && memcmp(seen[i], kXboxMagic, sizeof(kXboxMagic) - 1) == 0)
                found = true;
        }
        position += n;
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    LOG_INFO("Setup: %ls %s an Xbox disc image (%llu bytes read)", member.c_str(), found ? "is" : "is not", position);
    return found;
}

// Finds the disc image in an archive and unpacks it to `dir`. Returns its path.
std::wstring UnpackArchive(const std::wstring& archive, const std::wstring& dir, Progress& p)
{
    p.SetStatus(L"Reading " + archive.substr(archive.find_last_of(L"\\/") + 1) + L"...");
    std::string listing;
    if (!RunTar(L"-tvf \"" + archive + L"\"", &listing, p, nullptr)) {
        if (!p.cancel)
            p.error = L"Could not read the archive. Unpack it yourself and choose the .iso inside.";
        return {};
    }
    // bsdtar -tv: "mode links uid gid size month day year|time name"
    std::string name;
    uint64_t size = 0;
    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        std::string line = listing.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? listing.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t pos = 0;
        std::string fields[8];
        for (std::string& field : fields) {
            while (pos < line.size() && line[pos] == ' ')
                ++pos;
            size_t e = line.find(' ', pos);
            field = line.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? line.size() : e;
        }
        while (pos < line.size() && line[pos] == ' ')
            ++pos;
        std::string member = line.substr(pos);
        if ((member.size() > 4 && _stricmp(member.c_str() + member.size() - 4, ".iso") == 0)
            || (member.size() > 5 && _stricmp(member.c_str() + member.size() - 5, ".xiso") == 0)) {
            name = member;
            size = _strtoui64(fields[4].c_str(), nullptr, 10);
            break;
        }
    }
    if (name.empty()) {
        p.error = L"The archive does not contain an .iso disc image.";
        return {};
    }
    std::wstring wname(name.begin(), name.end());
    if (!ArchiveImageIsXbox(archive, wname, p)) {
        if (!p.cancel)
            p.error = L"The disc image in this archive is not an Xbox disc.";
        return {};
    }
    std::wstring out = dir + L"\\" + wname;
    p.SetStatus(L"Unpacking " + wname + L"...");
    p.done = 0;
    p.total = size;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!RunTar(L"-xf \"" + archive + L"\" -C \"" + dir + L"\" \"" + wname + L"\"", nullptr, p,
            [&] { p.done = FileSize(out); })) {
        if (!p.cancel)
            p.error = L"Could not unpack the disc image from the archive.";
        return {};
    }
    return out;
}

// --- XISO ---------------------------------------------------------------------------------------

constexpr uint32_t kSector = 2048;

struct IsoFile {
    std::wstring path; // relative
    uint32_t sector, size;
    bool directory;
};

class Iso {
public:
    ~Iso()
    {
        if (m_file != INVALID_HANDLE_VALUE)
            CloseHandle(m_file);
    }

    bool Open(const std::wstring& path)
    {
        m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_file == INVALID_HANDLE_VALUE)
            return false;
        // The game partition starts at 0 (XISO) or after the video partition (full disc images).
        for (uint64_t base : { 0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull }) {
            char magic[20];
            if (Read(base + 32 * kSector, magic, 20) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
                m_base = base;
                uint32_t root[2];
                Read(base + 32 * kSector + 20, root, 8);
                m_rootSector = root[0];
                m_rootSize = root[1];
                return true;
            }
        }
        return false;
    }

    bool Read(uint64_t offset, void* out, uint32_t bytes)
    {
        OVERLAPPED ov = {};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD n = 0;
        return ReadFile(m_file, out, bytes, &n, &ov) && n == bytes;
    }

    bool List(std::vector<IsoFile>& files) { return ListDirectory(m_rootSector, m_rootSize, L"", files); }

    bool ReadFileData(const IsoFile& f, std::vector<uint8_t>& out)
    {
        out.resize(f.size);
        return f.size == 0 || Read(Offset(f.sector), out.data(), f.size);
    }

    uint64_t Offset(uint32_t sector) const { return m_base + uint64_t(sector) * kSector; }

private:
    // A directory is a binary tree of entries: left/right subtree offsets (in
    // dwords, 0xFFFF = none), start sector, size, attributes, name.
    bool ListDirectory(uint32_t sector, uint32_t size, const std::wstring& prefix, std::vector<IsoFile>& files)
    {
        if (size == 0)
            return true;
        std::vector<uint8_t> table(size);
        if (!Read(Offset(sector), table.data(), size))
            return false;
        std::vector<uint32_t> pending = { 0 };
        while (!pending.empty()) {
            const uint32_t off = pending.back() * 4;
            pending.pop_back();
            if (off + 14 > size)
                continue;
            const uint8_t* e = table.data() + off;
            const uint16_t left = uint16_t(e[0] | e[1] << 8), right = uint16_t(e[2] | e[3] << 8);
            if (left == 0xFFFF && right == 0xFFFF && e[13] == 0xFF)
                continue; // padding
            uint32_t start, bytes;
            memcpy(&start, e + 4, 4);
            memcpy(&bytes, e + 8, 4);
            const uint8_t attributes = e[12], nameLength = e[13];
            if (off + 14 + nameLength > size)
                return false;
            std::wstring name;
            for (int i = 0; i < nameLength; ++i)
                name += wchar_t(e[14 + i]);
            if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos)
                return false;
            const std::wstring path = prefix + name;
            const bool directory = (attributes & 0x10) != 0;
            files.push_back({ path, start, bytes, directory });
            if (directory && !ListDirectory(start, bytes, path + L"\\", files))
                return false;
            if (left && left != 0xFFFF)
                pending.push_back(left);
            if (right && right != 0xFFFF)
                pending.push_back(right);
        }
        return true;
    }

    HANDLE m_file = INVALID_HANDLE_VALUE;
    uint64_t m_base = 0;
    uint32_t m_rootSector = 0, m_rootSize = 0;
};

bool ExtractIso(const std::wstring& image, const std::wstring& target, Progress& p)
{
    p.SetStatus(L"Reading the disc image...");
    Iso iso;
    std::vector<IsoFile> files;
    if (!iso.Open(image) || !iso.List(files)) {
        p.error = L"This file is not an Xbox disc image.";
        return false;
    }
    // The game itself, checked before any file is copied.
    p.SetStatus(L"Checking the game...");
    auto xbe = std::find_if(files.begin(), files.end(),
        [](const IsoFile& f) { return !f.directory && _wcsicmp(f.path.c_str(), L"default.xbe") == 0; });
    std::vector<uint8_t> xbeBytes;
    if (xbe == files.end() || !iso.ReadFileData(*xbe, xbeBytes)) {
        p.error = L"The disc image does not contain the game (no default.xbe).";
        return false;
    }
    game::Release release;
    if (!game::FindRelease(Md5Hex(xbeBytes.data(), xbeBytes.size()), &release)) {
        p.error = std::wstring(L"This disc image is a different version of the game. Supported: ") +
            game::kSupportedReleases + L".";
        return false;
    }
    LOG_INFO("Setup: %s release", game::ReleaseName(release));
    uint64_t total = 0;
    for (const IsoFile& f : files)
        if (!f.directory)
            total += f.size;
    p.done = 0;
    p.total = total;
    CreateDirectoryW(target.c_str(), nullptr);
    std::vector<uint8_t> buffer(4 << 20);
    for (const IsoFile& f : files) {
        if (p.cancel)
            return false;
        const std::wstring out = target + L"\\" + f.path;
        if (f.directory) {
            CreateDirectoryW(out.c_str(), nullptr);
            continue;
        }
        p.SetStatus(L"Copying game files...\n" + f.path);
        HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            p.error = L"Could not write " + out;
            return false;
        }
        uint64_t offset = iso.Offset(f.sector);
        uint32_t left = f.size;
        bool ok = true;
        while (left && ok && !p.cancel) {
            const uint32_t n = std::min<uint32_t>(left, uint32_t(buffer.size()));
            DWORD written = 0;
            ok = iso.Read(offset, buffer.data(), n) && WriteFile(h, buffer.data(), n, &written, nullptr) && written == n;
            offset += n;
            left -= n;
            p.done += n;
        }
        CloseHandle(h);
        if (!ok) {
            p.error = L"Could not copy " + f.path + L" (the disc image may be damaged, or the drive is full).";
            return false;
        }
    }
    return !p.cancel;
}

// --- Shortcuts ----------------------------------------------------------------------------------

void CreateShortcut(REFKNOWNFOLDERID folder, const std::wstring& exe, const std::wstring& dir, const std::wstring& icon)
{
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &base)))
        return;
    const std::wstring link = std::wstring(base) + L"\\Star Wars Episode III - Revenge of the Sith.lnk";
    CoTaskMemFree(base);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))))
        return;
    sl->SetPath(exe.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetDescription(kTitle);
    if (Exists(icon))
        sl->SetIconLocation(icon.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(link.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

std::wstring ChooseImage()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrTitle = L"Choose your Star Wars: Episode III disc image";
    ofn.lpstrFilter = L"Disc images and archives (*.iso, *.xiso, *.7z, *.zip, *.rar)\0*.iso;*.xiso;*.7z;*.zip;*.rar\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

} // namespace

bool EnsureGameData(const std::wstring& exeDir, const std::wstring& gameData, const std::wstring& imagePath)
{
    if (Exists(gameData + L"\\default.xbe"))
        return true;

    std::wstring image = imagePath;
    if (image.empty()) {
        const int answer = MessageBoxW(nullptr,
            L"The game files are not installed yet.\n\n"
            L"Choose your Star Wars: Episode III - Revenge of the Sith (Xbox) disc image: an .iso / .xiso file, "
            L"or a .7z / .zip / .rar archive containing one. The game files are copied into the GameData folder "
            L"next to swrots.exe (about 2.5 GB).",
            kTitle, MB_OKCANCEL | MB_ICONINFORMATION);
        if (answer != IDOK)
            return false;
        image = ChooseImage();
        if (image.empty())
            return false;
    }
    if (Exists(gameData)) {
        // An empty or unusable folder from an earlier attempt.
        if (!RemoveDirectoryW(gameData.c_str())) {
            MessageBoxW(nullptr, (L"The folder " + gameData + L" exists but does not contain the game. Move or delete "
                                  L"it, then start the game again.").c_str(), kTitle, MB_OK | MB_ICONERROR);
            return false;
        }
    }
    LOG_INFO("Setup: installing game files from %ls", image.c_str());

    const std::wstring partial = gameData + L".partial", unpackDir = exeDir + L"\\cache\\setup";
    Progress p;
    RunWithProgress(p, [&] {
        DeleteTree(partial);
        std::wstring iso = image;
        if (!EndsWith(image, L".iso") && !EndsWith(image, L".xiso")) {
            CreateDirectoryW((exeDir + L"\\cache").c_str(), nullptr);
            iso = UnpackArchive(image, unpackDir, p);
            if (iso.empty())
                return;
        }
        if (!ExtractIso(iso, partial, p))
            return;
        p.SetStatus(L"Checking the game files...");
        XbeFile xbe;
        std::string error;
        if (!xbe.Load(partial + L"\\default.xbe", error)) {
            p.error = L"The disc image does not contain the game (no default.xbe).";
            return;
        }
        game::Release release;
        if (!game::FindRelease(xbe.Md5Hex(), &release)) {
            p.error = std::wstring(L"This disc image is a different version of the game. Supported: ") +
                game::kSupportedReleases + L".";
            return;
        }
        p.ok = MoveFileExW(partial.c_str(), gameData.c_str(), 0) != 0;
        if (!p.ok)
            p.error = L"Could not create " + gameData;
    });
    DeleteTree(unpackDir);
    if (!p.ok) {
        DeleteTree(partial);
        LOG_WARN("Setup did not finish: %ls", p.cancel ? L"cancelled" : p.error.c_str());
        if (!p.cancel)
            MessageBoxW(nullptr, (L"Setup could not finish.\n\n" + p.error).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    LOG_INFO("Setup: game files installed");

    // The game's own icon for shortcuts, and the shortcuts themselves.
    XbeFile xbe;
    std::string error;
    const std::wstring icon = exeDir + L"\\swrots.ico";
    if (xbe.Load(gameData + L"\\default.xbe", error))
        WriteIconFile(xbe, icon);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CreateShortcut(FOLDERID_Programs, exe, exeDir, icon);
    if (MessageBoxW(nullptr, L"The game is installed and has been added to the Start menu.\n\nAlso add a shortcut "
                             L"to the desktop?", kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        CreateShortcut(FOLDERID_Desktop, exe, exeDir, icon);
    CoUninitialize();
    return true;
}

} // namespace swrots
