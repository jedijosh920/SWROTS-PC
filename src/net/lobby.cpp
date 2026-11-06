// Online play's lobby: announcing a public room, listing the public rooms (see lobby.h).

#include "net/lobby.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "net/session.h"

namespace swrots::net {

namespace {

// The project's lobby (lobby/: deployed once on Cloudflare's free Workers plan); [Online] Lobby overrides it, and
// an empty one turns Public Rooms off.
constexpr const wchar_t* kDefaultLobby = L"";
constexpr auto kAnnounceEvery = std::chrono::seconds(30);
constexpr auto kListEvery = std::chrono::seconds(5);
constexpr auto kListWanted = std::chrono::seconds(2);
using Clock = std::chrono::steady_clock;

std::mutex g_Lock;
std::vector<PublicRoom> g_Rooms;
std::string g_Status = "SEARCHING...";
Clock::time_point g_ListWantedAt = {};
std::atomic<bool> g_Started{ false };

std::wstring LobbyAddress()
{
    wchar_t test[512] = L"";
    if (GetEnvironmentVariableW(L"SWROTS_ONLINE_LOBBY", test, 512)) // tests: a lobby on this PC
        return test;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\settings.ini";
    wchar_t url[512] = L"";
    GetPrivateProfileStringW(L"Online", L"Lobby", kDefaultLobby, url, 512, ini.c_str());
    std::wstring u = url;
    while (!u.empty() && (u.back() == L'/' || u.back() == L' '))
        u.pop_back();
    return u;
}

// One HTTPS (or HTTP) request; the answer's status (0: no answer) and body.
int Request(const std::wstring& base, const wchar_t* method, const std::wstring& path, const std::string& body,
    std::string& answer)
{
    answer.clear();
    const std::wstring url = base + path;
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = L"", rest[1024] = L"";
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = rest;
    parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return 0;
    HINTERNET session = WinHttpOpen(L"SWROTS-PC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"SWROTS-PC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return 0;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);
    int status = 0;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, method, rest, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (request) {
        const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(request, headers, body.empty() ? 0 : DWORD(-1), body.empty() ? nullptr : (void*)body.data(),
                DWORD(body.size()), DWORD(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size,
                nullptr);
            status = int(code);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || !available || answer.size() > (1 << 20))
                    break;
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || !read)
                    break;
                answer.append(chunk.data(), read);
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status;
}

std::string Json(const std::string& text)
{
    std::string out = "\"";
    for (char c : text)
        if (c == '"' || c == '\\')
            out += std::string("\\") + c;
        else if (uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F)
            out += c;
    return out + "\"";
}

std::vector<std::string> Split(const std::string& line, char by)
{
    std::vector<std::string> parts(1);
    for (char c : line)
        if (c == by)
            parts.emplace_back();
        else if (c != '\r')
            parts.back() += c;
    return parts;
}

// What the host's room shows in the list (empty: nothing to announce).
std::string Announcement(std::string& code)
{
    const OnlineStatusInfo s = OnlineStatus();
    const bool open = s.host && s.room.open && !s.roomCode.empty() && s.phase != OnlinePhase::Off &&
        s.phase != OnlinePhase::Ended;
    code = open ? s.roomCode : std::string();
    if (!open)
        return {};
    static const char* const kArenas[] = { "HOST PICKS", "RANDOM", "LOSER PICKS", "WINNER PICKS" };
    const std::string arena = !s.arenaPlayed.empty() ? s.arenaPlayed : kArenas[s.room.arena < 4 ? s.room.arena : 0];
    return "{\"code\":" + Json(s.roomCode) + ",\"name\":" + Json(s.localName) + ",\"mode\":\"VERSUS\",\"arena\":" +
        Json(arena) + ",\"players\":" + std::to_string(s.guestHere ? 2 : 1) + ",\"version\":" +
        std::to_string(OnlineProtocol()) + "}";
}

void Run()
{
    std::string announced, announcedCode;
    Clock::time_point announcedAt = {}, listedAt = {};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const std::wstring base = LobbyAddress();
        const auto now = Clock::now();
        std::string code;
        const std::string body = Announcement(code);
        if (!base.empty()) {
            // the host's public room: announced again every 30 s, at once when it changes; taken away once closed
            if (!announcedCode.empty() && announcedCode != code) {
                std::string answer;
                const int status = Request(base, L"DELETE", L"/rooms/" + std::wstring(announcedCode.begin(),
                    announcedCode.end()), "", answer);
                LOG_INFO("Lobby: room %s taken away (%d)", announcedCode.c_str(), status);
                announcedCode.clear();
                announced.clear();
            }
            if (!body.empty() && (body != announced || now - announcedAt > kAnnounceEvery)) {
                std::string answer;
                const int status = Request(base, L"POST", L"/rooms", body, answer);
                if (body != announced)
                    LOG_INFO("Lobby: room %s announced (%d %s)", code.c_str(), status, answer.substr(0, 40).c_str());
                announcedAt = now;
                if (status == 200) {
                    announced = body;
                    announcedCode = code;
                }
            }
        }
        // the list, while Public Rooms is up
        bool wanted;
        {
            std::lock_guard<std::mutex> lock(g_Lock);
            wanted = now - g_ListWantedAt < kListWanted;
        }
        if (!wanted || now - listedAt < kListEvery)
            continue;
        listedAt = now;
        std::vector<PublicRoom> rooms;
        std::string status;
        if (base.empty()) {
            status = "NO LOBBY SERVER";
        } else {
            std::string answer;
            const int code200 = Request(base, L"GET", L"/rooms.txt?version=" + std::to_wstring(OnlineProtocol()), "",
                answer);
            if (code200 != 200) {
                status = "LOBBY UNREACHABLE";
            } else {
                for (const std::string& line : Split(answer, '\n')) {
                    const std::vector<std::string> f = Split(line, '\t');
                    if (f.size() < 6 || f[0].size() != 15)
                        continue;
                    PublicRoom r;
                    r.code = f[0];
                    r.name = f[1];
                    r.mode = f[2];
                    r.arena = f[3];
                    r.players = atoi(f[4].c_str());
                    r.slots = atoi(f[5].c_str());
                    if (r.code != code) // not this PC's own room
                        rooms.push_back(r);
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_Lock);
        g_Rooms = rooms;
        g_Status = status;
    }
}

} // namespace

void StartLobby()
{
    bool expected = false;
    if (!g_Started.compare_exchange_strong(expected, true))
        return;
    std::thread(&Run).detach();
}

void LobbyWantList()
{
    StartLobby();
    std::lock_guard<std::mutex> lock(g_Lock);
    if (Clock::now() - g_ListWantedAt > std::chrono::seconds(10)) // a new visit: SEARCHING... until the first answer
        g_Status = "SEARCHING...";
    g_ListWantedAt = Clock::now();
}

void LobbyRooms(std::vector<PublicRoom>& rooms, std::string& status)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    rooms = g_Rooms;
    status = g_Status;
}

} // namespace swrots::net
