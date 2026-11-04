// Online play in the game's main menu (see onlinemenu.h).
//
// The main menu (main_menu.xbl_xml) has four top-level rows, MainMenuButton items 24 apart from y 179: Single Player,
// Multi Player, Settings, Features (each a text id, its down and up neighbours, and for Settings the screen it opens),
// with a veil behind each row and a fifth veil (debugveil, y 277) spare. A row "online" is put between Multi Player and
// Settings: a copy of Settings' (its highlight bar too), opening the port's screen "online"; Settings and Features
// move one row down, and the neighbours are linked through it.
//
// The port's screens are the Settings screen's (BuildSettingsScreen):
// - "online": Host Game, Join Game, Online Settings, and a status line. Join Game opens "online_code", the game's
//   code screen (codes.xbl_xml: the Settings > Codes keyboard, CodesKeyboard: A-Z, 0-9, Back, Space, Done) with a
//   keyboard of the port's own (made by 0x2D1740, vtable 0x5D61E0) whose Done (slot 29) joins with the code typed (15
//   letters: its init, slot 0, sets the length, +0x24) and which takes Ctrl+V or Y (the clipboard's code). Its text is
//   wide characters at +0x28 (16), +0x48 the end, +0x50/+0x54/+0x58 its Back, Space and Done keys' handlers (their
//   +0x20 set while the text is empty). The same keyboard types the player's name for Online Settings.
// - "online_settings": the player's name ([Online] Name; by default the profile's).
// - "online_room": the room (both players, from Host Game or a join on): the host's settings (game mode, rounds to
//   win, arena, auto rematch), the two players and their connection, the room code (A copies it), Start (the host:
//   both games to the Versus select, the guest's following) and Close Room / Leave Room.

#include "game/onlinemenu.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "core/log.h"
#include "core/patch.h"
#include "core/window.h"
#include "game/menus.h"
#include "game/resources.h"
#include "net/session.h"
#include "net/transport.h"

namespace swrots::game {

namespace {

constexpr const char* kRowName = "online";
constexpr const char* kRowText = "IDS_PORT_ONLINE";
constexpr const char* kScreen = "online";
constexpr const char* kScreenFile = "interfc\\front_end\\xml\\online.xml";
constexpr const char* kCodeScreen = "online_code";
constexpr const char* kCodeFile = "interfc\\front_end\\xml\\online_code.xml";
constexpr const char* kCodesFile = "interfc\\front_end\\xml\\codes.xml";
constexpr float kRowStep = 24.0f;

// --- The main menu's row ---------------------------------------------------------------------------------------------
// An item's y: after "screenItem", its name, a u32 and its x.
size_t ItemY(const std::vector<uint8_t>& data, size_t item)
{
    uint32_t nameLength;
    std::memcpy(&nameLength, data.data() + item + 14, 4);
    return item + 14 + 4 + nameLength + 4 + 4;
}

void MoveItem(std::vector<uint8_t>& data, size_t item, float by)
{
    float y;
    const size_t at = ItemY(data, item);
    std::memcpy(&y, data.data() + at, 4);
    y += by;
    std::memcpy(data.data() + at, &y, 4);
}

bool PatchRows(std::vector<uint8_t>& data)
{
    size_t settings, settingsEnd, multi, multiEnd, features, featuresEnd, existing, existingEnd;
    if (!MenuRowBlock(data, "settings", settings, settingsEnd) || !MenuRowBlock(data, "multiplayer", multi, multiEnd) ||
        !MenuRowBlock(data, "features", features, featuresEnd) || MenuRowBlock(data, kRowName, existing, existingEnd) ||
        !(multi < settings && settings < features)) {
        LOG_WARN("Online menu: the main menu is not as expected; no Online entry");
        return false;
    }
    const size_t nameAt = MenuString("screenItem").size();
    // the new row: Settings' (name, text, down features, up multiplayer, screen settings), its screen ours, down Settings
    std::vector<uint8_t> row(data.begin() + settings, data.begin() + settingsEnd);
    size_t rowEnd = row.size();
    bool ok = MenuReplaceString(row, 0, rowEnd, "settings", kRowName) &&
              MenuReplaceString(row, nameAt + 4, rowEnd, "IDS_SETTINGS", kRowText);
    const std::vector<uint8_t> text = MenuString(kRowText);
    const size_t textAt = ok ? MenuFind(row, text, nameAt, rowEnd) : std::string::npos;
    ok = textAt != std::string::npos &&
         MenuReplaceString(row, textAt + text.size(), rowEnd, "settings", kScreen) && // the screen (after the links)
         MenuReplaceString(row, textAt + text.size(), rowEnd, "features", "settings");  // down
    if (!ok) {
        LOG_WARN("Online menu: the Settings row is not as expected; no Online entry");
        return false;
    }
    // the edits from the last back (each moves only what follows it): Features one row lower; Settings one row lower,
    // up the new row; the new row before Settings; Multi Player's down the new row
    MoveItem(data, features, kRowStep);
    MoveItem(data, settings, kRowStep);
    size_t settingsLinks = settingsEnd;
    if (!MenuReplaceString(data, settings + nameAt + 4, settingsLinks, "multiplayer", kRowName)) {
        LOG_WARN("Online menu: the Settings row's links are not as expected; no Online entry");
        return false;
    }
    data.insert(data.begin() + settings, row.begin(), row.end());
    size_t multiLinks = settings;
    if (!MenuReplaceString(data, multi + nameAt + 4, multiLinks, "settings", kRowName)) {
        LOG_WARN("Online menu: the Multi Player row's links are not as expected; no Online entry");
        return false;
    }
    // the screen's item count, after its name and its first item's
    uint32_t nameLength, firstLength;
    std::memcpy(&nameLength, data.data() + 4, 4);
    if (8 + nameLength >= data.size())
        return false;
    std::memcpy(&firstLength, data.data() + 4 + 4 + nameLength, 4);
    const size_t countAt = 4 + 4 + nameLength + 4 + firstLength;
    uint32_t items;
    std::memcpy(&items, data.data() + countAt, 4);
    ++items;
    std::memcpy(data.data() + countAt, &items, 4);
    LOG_INFO("Online menu: the main menu has an Online entry");
    return true;
}

// --- The main menu's intro ------------------------------------------------------------------------------------------
// The main menu's intro (MainMenuIntro, vtable 0x5D55F0) animates its rows: its init (slot 0, 0x2CCBB0) lists their
// handlers (each row item's +0x80) in a vector at +0x3C ({capacity, count, data}; push 0x13130), found by name
// (0xD40C0, thiscall on the screen [[+8]+0x24], (const char*) -> item): Single Player, Multi Player, Settings,
// Features. The Online row is put in that list after Multi Player, so it comes in with the others.
constexpr uint32_t kIntroVtable = 0x005D55F0;
constexpr uint32_t kIntroInit = 0x002CCBB0;
constexpr uint32_t kFindItem = 0x000D40C0;
constexpr uint32_t kVectorPush = 0x00013130;

void __fastcall IntroInit(uint8_t* intro, void*)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kIntroInit))(intro, nullptr);
    __try {
        uint8_t* owner = *reinterpret_cast<uint8_t**>(intro + 8);
        uint8_t* screen = owner ? *reinterpret_cast<uint8_t**>(owner + 0x24) : nullptr;
        uint8_t* item = screen ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(
                                     uintptr_t(kFindItem))(screen, nullptr, kRowName)
                               : nullptr;
        if (!item)
            return;
        uint32_t handler = *reinterpret_cast<uint32_t*>(item + 0x80);
        uint8_t* list = intro + 0x3C;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const uint32_t*)>(uintptr_t(kVectorPush))(list, nullptr,
            &handler);
        const uint32_t count = *reinterpret_cast<uint32_t*>(list + 4);
        uint32_t* data = *reinterpret_cast<uint32_t**>(list + 8);
        if (count >= 3 && data) { // after Multi Player: [sp, mp, online, settings, features]
            const uint32_t last = data[count - 1];
            for (uint32_t i = count - 1; i > 2; --i)
                data[i] = data[i - 1];
            data[2] = last;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Online menu: the main menu's intro is not as expected");
    }
}

// --- The screens' texts ----------------------------------------------------------------------------------------------
std::string g_Typed;      // the guest's code, as typed
std::string g_Copied;     // the room code last put on the clipboard
std::string g_Note;       // a note of the menu's own (a code refused)
ULONGLONG g_CopiedAt = 0; // when the host last copied its code with A (the row says so for 2 s)

std::wstring Wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

bool CopyToClipboard(const std::string& text)
{
    if (!OpenClipboard(GameWindow()))
        return false;
    EmptyClipboard();
    const std::wstring wide = Wide(text);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (memory) {
        std::memcpy(GlobalLock(memory), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
        ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        if (!ok)
            GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

std::string ClipboardText()
{
    std::string text;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(GameWindow()))
        return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(data))) {
            for (size_t i = 0; w[i] && i < 256; ++i)
                text += w[i] < 128 ? char(w[i]) : '?';
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

std::string Shorten(std::string text, size_t most)
{
    if (text.size() > most)
        text = text.substr(0, most - 3) + "...";
    for (char& c : text)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return text;
}

// --- The rows of the three screens -------------------------------------------------------------------------------------
// "online": Host Game, Join Game, Online Settings, and a line of status. "online_settings": the player's name.
// "online_room" (both players, from Host Game or a join on): the host's settings, the two players, the room code,
// Start (the host) and Leave.
enum Screen { kOnline, kSettingsScreen, kRoom, kScreenCount };
constexpr const char* kScreenNames[kScreenCount] = { "online", "online_settings", "online_room" };
constexpr const char* kSettingsFileName = "interfc\\front_end\\xml\\online_settings.xml";
constexpr const char* kRoomFileName = "interfc\\front_end\\xml\\online_room.xml";

enum Row {
    kHost, kJoin, kSettings, kStatus,
    kName,
    kMode, kRounds, kArena, kRematch, kPlayer1, kPlayer2, kCode, kStart, kLeave,
    kRowCount
};
struct RowDef {
    const char* name;
    const char* text;
    bool button;
    Screen screen;
};
constexpr RowDef kRowDefs[kRowCount] = {
    { "onlinehost", "IDS_PORT_ONLINE_HOST", true, kOnline },
    { "onlinejoin", "IDS_PORT_ONLINE_JOIN", true, kOnline },
    { "onlinesettings", "IDS_PORT_ONLINE_SETTINGS", true, kOnline },
    { "onlinestatus", "IDS_PORT_ONLINE_STATUS", false, kOnline },
    { "onlinename", "IDS_PORT_ONLINE_NAME", false, kSettingsScreen },
    { "roommode", "IDS_PORT_ROOM_MODE", false, kRoom },
    { "roomrounds", "IDS_PORT_ROOM_ROUNDS", false, kRoom },
    { "roomarena", "IDS_PORT_ROOM_ARENA", false, kRoom },
    { "roomrematch", "IDS_PORT_ROOM_REMATCH", false, kRoom },
    { "roomp1", "IDS_PORT_ROOM_P1", false, kRoom },
    { "roomp2", "IDS_PORT_ROOM_P2", false, kRoom },
    { "roomcode", "IDS_PORT_ROOM_CODE", false, kRoom },
    { "roomstart", "IDS_PORT_ROOM_START", true, kRoom },
    { "roomleave", "IDS_PORT_ROOM_LEAVE", true, kRoom },
};

std::vector<ShellScreenRow> Rows(Screen screen)
{
    std::vector<ShellScreenRow> rows;
    for (const RowDef& r : kRowDefs)
        if (r.screen == screen)
            rows.push_back({ r.name, r.text, r.button });
    return rows;
}

// A value row's text id (its value, as the control's On text).
std::string ValueId(int row)
{
    std::string id = std::string("IDS_PORT_V_") + kRowDefs[row].name;
    for (char& c : id)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return id;
}

constexpr uint8_t kRoundChoices[] = { 1, 2, 3, 5 };

bool Idle(const net::OnlineStatusInfo& s)
{
    return s.phase == net::OnlinePhase::Off || s.phase == net::OnlinePhase::Ended;
}

// The quality of the connection in words and bars (the room's player rows).
std::string Quality(int pingMs)
{
    if (pingMs < 0)
        return "";
    const int bars = pingMs < 40 ? 4 : pingMs < 80 ? 3 : pingMs < 150 ? 2 : 1;
    return "  " + std::string(size_t(bars), '|') + std::string(size_t(4 - bars), '.') + " " + std::to_string(pingMs) + " MS";
}

void UpdateTexts()
{
    const net::OnlineStatusInfo s = net::OnlineStatus();
    // online: the status line
    std::string status;
    switch (s.phase) {
    case net::OnlinePhase::Off: status = g_Note.empty() ? "NOT CONNECTED" : g_Note; break;
    case net::OnlinePhase::Opening: status = "OPENING A ROOM..."; break;
    case net::OnlinePhase::Waiting: status = "IN YOUR ROOM"; break;
    case net::OnlinePhase::Connecting: status = "CALLING THE HOST..."; break;
    case net::OnlinePhase::Preparing: case net::OnlinePhase::Starting: status = "JOINING..."; break;
    case net::OnlinePhase::Playing: status = "IN A ROOM"; break;
    case net::OnlinePhase::Ended: status = g_Note.empty() ? (s.lost ? Shorten(s.message, 40) : "NOT CONNECTED") : g_Note; break;
    }
    SetMenuText(ValueId(kStatus).c_str(), Wide(Shorten(status, 40)));
    SetMenuText(ValueId(kName).c_str(), Wide(net::OnlinePlayerName()));
    // the room
    SetMenuText(ValueId(kMode).c_str(), L"VERSUS");
    SetMenuText(ValueId(kRounds).c_str(), Wide(std::to_string(s.room.rounds)));
    static const wchar_t* const kArenaTexts[] = { L"HOST PICKS", L"RANDOM", L"LOSER PICKS", L"WINNER PICKS" };
    SetMenuText(ValueId(kArena).c_str(), kArenaTexts[s.room.arena < std::size(kArenaTexts) ? s.room.arena : 0]);
    SetMenuText(ValueId(kRematch).c_str(), s.room.rematch ? L"ON" : L"OFF");
    const std::string me = Shorten(s.localName.empty() ? net::OnlinePlayerName() : s.localName, 15);
    const std::string other = Shorten(s.remoteName, 15);
    std::string p1, p2;
    if (s.host) {
        p1 = me;
        p2 = s.guestHere ? other + Quality(s.pingMs) : "WAITING...";
    } else {
        p1 = other.empty() ? "..." : other + Quality(s.pingMs);
        p2 = me;
    }
    SetMenuText(ValueId(kPlayer1).c_str(), Wide(p1));
    SetMenuText(ValueId(kPlayer2).c_str(), Wide(p2));
    std::string code = "-";
    if (s.host && !s.roomCode.empty() && !Idle(s))
        code = s.roomCode;
    else if (!s.host && !g_Typed.empty())
        code = g_Typed;
    if (s.host && s.phase == net::OnlinePhase::Waiting && !s.roomCode.empty() && g_Copied != s.roomCode &&
        CopyToClipboard(s.roomCode)) { // the host's code goes to the clipboard as soon as there is one
        g_Copied = s.roomCode;
        LOG_INFO("Online menu: room code %s copied to the clipboard", s.roomCode.c_str());
    }
    if (g_CopiedAt && GetTickCount64() - g_CopiedAt < 2000)
        code = "CODE COPIED";
    SetMenuText(ValueId(kCode).c_str(), Wide(code));
}

// Rows that cannot be chosen now: skipped by the cursor and dimmed (rows that only tell are never chosen either).
bool RowDisabled(int row)
{
    const net::OnlineStatusInfo s = net::OnlineStatus();
    const bool idle = Idle(s);
    switch (row) {
    case kHost: case kJoin: return !idle;
    case kSettings: return false;
    case kStatus: return true;
    case kName: return !idle;
    case kMode: return true; // Versus only for now
    case kRounds: case kArena: case kRematch: return !s.host || idle;
    case kPlayer1: case kPlayer2: return true;
    case kCode: return !(s.host && !idle && !s.roomCode.empty());
    case kStart: return !(s.host && s.matchOn && s.phase == net::OnlinePhase::Playing);
    case kLeave: return false;
    default: return false;
    }
}

// Rows shown dimmed: the disabled ones, but not the rows that only tell.
bool RowDimmed(int row)
{
    if (row == kRounds || row == kArena || row == kRematch) { // the guest sees the host's settings, not dimmed
        const net::OnlineStatusInfo s = net::OnlineStatus();
        if (!s.host && !Idle(s))
            return false;
    }
    return RowDisabled(row) && row != kStatus && row != kPlayer1 && row != kPlayer2 && row != kMode;
}

constexpr uint32_t kMakeOnOff = 0x002D9690;   // SettingsScreenOnOffControl
constexpr uint32_t kOnOffVtable = 0x005D7000;
constexpr uint32_t kMakeButton = 0x002D93F0;  // SettingsScreenButton
constexpr uint32_t kButtonVtable = 0x005D6E10;
constexpr uint32_t kItemEvent = 0x002C62D0;    // an item's events (B goes back)
constexpr uint32_t kProfiles = 0x007E8F24;
constexpr uint32_t kShellObject = 0x007F637C;
constexpr uint32_t kCurrentProfile = 0x0024CC50;
constexpr int kVtableSlots = 32;
constexpr int kChosen = 0x23, kLeft = 0x1F, kRight = 0x20, kB = 0x26;
using DrawFn = char(__fastcall*)(uint8_t*, void*, uint32_t);
using ItemEventFn = char(__fastcall*)(uint8_t*, void*, int, int);
uint32_t g_ValueVtable[kVtableSlots], g_ButtonVtable[kVtableSlots];
DrawFn g_OnOffDraw = nullptr;
ItemEventFn g_OnOffEvent = nullptr, g_ButtonEvent = nullptr;

int RowOf(uint8_t* handler)
{
    const uint8_t* item = *reinterpret_cast<uint8_t**>(handler + 8);
    if (!item)
        return -1;
    const uint32_t key = *reinterpret_cast<const uint32_t*>(item + 4);
    for (int i = 0; i < kRowCount; ++i)
        if (key == MenuKey(kRowDefs[i].name))
            return i;
    return -1;
}

// The cursor's way through the rows that can be chosen: each row's up and down neighbours (the item's +0x18 and
// +0x1C, the keys of their names) set as it draws, past the disabled ones (on its own screen).
void LinkRow(uint8_t* handler, int row)
{
    uint32_t* item = *reinterpret_cast<uint32_t**>(handler + 8);
    if (!item)
        return;
    std::vector<int> mine;
    for (int i = 0; i < kRowCount; ++i)
        if (kRowDefs[i].screen == kRowDefs[row].screen)
            mine.push_back(i);
    const int n = int(mine.size());
    const int at = int(std::find(mine.begin(), mine.end(), row) - mine.begin());
    const auto next = [&](int step) {
        for (int i = 1; i <= n; ++i) {
            const int r = mine[size_t(((at + step * i) % n + n) % n)];
            if (!RowDisabled(r))
                return r;
        }
        return row;
    };
    item[0x18 / 4] = MenuKey(kRowDefs[next(-1)].name);
    item[0x1C / 4] = MenuKey(kRowDefs[next(1)].name);
}

// A draw with the item dimmed (its colour, item +0x94 RGBA, as the main menu dims its own: alpha 0x69).
template <typename Draw>
char DrawDimmed(uint8_t* handler, bool dim, Draw draw)
{
    uint8_t* item = *reinterpret_cast<uint8_t**>(handler + 8);
    if (!dim || !item)
        return draw();
    uint8_t saved[4];
    std::memcpy(saved, item + 0x94, 4);
    for (int i = 0; i < 3; ++i)
        item[0x94 + i] = uint8_t(saved[i] * 45 / 100);
    item[0x97] = uint8_t(std::min<int>(saved[3], 0x69));
    const char result = draw();
    std::memcpy(item + 0x94, saved, 4);
    return result;
}

uint8_t* CurrentProfile()
{
    uint8_t* profiles = *reinterpret_cast<uint8_t**>(uintptr_t(kProfiles));
    return profiles ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(uintptr_t(kCurrentProfile))(profiles, nullptr)
                    : nullptr;
}

// While a screen of ours is up: the session's state takes this PC to the right one (a room once in one, the Online
// screen once it is over).
ULONGLONG g_LastRedirect = 0;
void KeepOnRightScreen(int row)
{
    const net::OnlineStatusInfo s = net::OnlineStatus();
    const Screen screen = kRowDefs[row].screen;
    const bool inRoom = !Idle(s);
    if (GetTickCount64() - g_LastRedirect < 1000)
        return;
    if (screen == kOnline && inRoom) {
        g_LastRedirect = GetTickCount64();
        GoToShellScreen(kScreenNames[kRoom]);
    } else if (screen == kRoom && !inRoom) {
        g_LastRedirect = GetTickCount64();
        GoToShellScreen(kScreenNames[kOnline]);
    }
}

// --- The game's own popup ----------------------------------------------------------------------------------------------
// The shell's popups are items of a screen (the Settings screen's, which the port's screens copy, has one named
// DefaultPopup: a title, a message, A to close). As the Codes keyboard shows its own (0x2D0930): the popup found by
// name on the screen up (0x2F5A00, thiscall on the shell [0x7F637C] (name, screen 0: the current, 0)), its title
// (0x2E8B80) and message (0x2E99E0) as the string table gives them ([[0x645F7C]+0x38]+0x94, vtable slot 1: (out, id,
// 0)), then shown (its slot 22, +0x58, with the handler that owns it).
constexpr uint32_t kFindPopup = 0x002F5A00, kPopupTitle = 0x002E8B80, kPopupMessage = 0x002E99E0;
constexpr uint32_t kEngine = 0x00645F7C, kTextToStringFn = 0x00222FA0;
bool g_LostPopup = false; // the session ended without this player leaving: the popup at the Online screen

const void* GameText(const char* id)
{
    uint32_t text = 0;
    reinterpret_cast<void*(__cdecl*)(uint32_t*, const char*)>(uintptr_t(kTextToStringFn))(&text, id);
    uint8_t* engine = *reinterpret_cast<uint8_t**>(uintptr_t(kEngine));
    uint8_t* gui = *reinterpret_cast<uint8_t**>(engine + 0x38);
    uint8_t* table = *reinterpret_cast<uint8_t**>(gui + 0x94);
    void* out = nullptr;
    using LookupFn = void**(__fastcall*)(uint8_t*, void*, void**, uint32_t*, int);
    void** got = reinterpret_cast<LookupFn>((*reinterpret_cast<void* const* const*>(table))[1])(table, nullptr, &out,
        &text, 0);
    return got ? *got : nullptr;
}

bool ShowPopup(uint8_t* owner, const char* titleId, const char* messageId)
{
    __try {
        uint8_t* shell = *reinterpret_cast<uint8_t**>(uintptr_t(kShellObject));
        if (!shell)
            return false;
        uint8_t* popup = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*, void*, int)>(
            uintptr_t(kFindPopup))(shell, nullptr, "DefaultPopup", nullptr, 0);
        if (!popup)
            return false;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kPopupTitle))(popup, nullptr,
            GameText(titleId));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kPopupMessage))(popup, nullptr,
            GameText(messageId));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(
            (*reinterpret_cast<void* const* const*>(popup))[0x58 / 4])(popup, nullptr, owner);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Online menu: the popup could not be shown");
        return false;
    }
}

// A value row shows its text as the control's On text (the control shows On for the profile's subtitles setting,
// +0x2A, which it is given for the draw).
char __fastcall ValueDraw(uint8_t* handler, void*, uint32_t value)
{
    const int row = RowOf(handler);
    if (row < 0 || kRowDefs[row].button)
        return g_OnOffDraw(handler, nullptr, value);
    if (row == kStatus || row == kName || row == kMode) // once a frame on each screen
        UpdateTexts();
    if (row == kStatus || row == kMode)
        KeepOnRightScreen(row);
    if (row == kStatus && g_LostPopup) {
        g_LostPopup = false;
        ShowPopup(handler, "IDS_PORT_LOST_TITLE", "IDS_PORT_LOST_TEXT");
    }
    LinkRow(handler, row);
    handler[0x23C] = 1;
    uint8_t* profile = CurrentProfile();
    const uint8_t saved = profile ? profile[0x2A] : 0;
    if (profile)
        profile[0x2A] = 1;
    const std::string text = ValueId(row);
    SetOnOffTexts(text.c_str(), text.c_str());
    const char result = DrawDimmed(handler, RowDimmed(row), [&] { return g_OnOffDraw(handler, nullptr, value); });
    SetOnOffTexts(nullptr, nullptr);
    if (profile)
        profile[0x2A] = saved;
    return result;
}

void OpenKeyboard(bool forName);

// B: back as the game's screens go, except in a room once the match is on (Leave is the way out).
char Back(uint8_t* handler, int event, int value)
{
    const int row = RowOf(handler);
    if (row >= 0 && kRowDefs[row].screen == kRoom) {
        const net::OnlineStatusInfo s = net::OnlineStatus();
        if (s.matchOn)
            return 1;
        if (!Idle(s)) { // the host's room before a guest, or a guest still calling: leave it
            net::OnlineLeave();
            g_Note.clear();
        }
        GoToShellScreen(kScreenNames[kOnline]);
        return 1;
    }
    if (row >= 0 && kRowDefs[row].screen == kSettingsScreen) {
        GoToShellScreen(kScreenNames[kOnline]);
        return 1;
    }
    return reinterpret_cast<ItemEventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
}

char __fastcall ValueEvent(uint8_t* handler, void*, int event, int value)
{
    if (event == kB)
        return Back(handler, event, value);
    if (event != kLeft && event != kRight && event != kChosen)
        return g_OnOffEvent(handler, nullptr, event, value);
    const int row = RowOf(handler);
    if (row < 0 || RowDisabled(row))
        return 1;
    const net::OnlineStatusInfo s = net::OnlineStatus();
    if (row == kCode && event == kChosen) { // Copy Code (the row says CODE COPIED for 2 s)
        if (CopyToClipboard(s.roomCode)) {
            g_Copied = s.roomCode;
            g_CopiedAt = GetTickCount64();
            g_OnOffEvent(handler, nullptr, event, value); // its sound
        }
        return 1;
    }
    if (row == kName && event == kChosen) {
        OpenKeyboard(true);
        return 1;
    }
    net::OnlineRoomSettings room = s.room;
    const int step = event == kLeft ? -1 : 1;
    if (row == kRounds) {
        int at = 0;
        for (int i = 0; i < int(std::size(kRoundChoices)); ++i)
            if (kRoundChoices[i] == room.rounds)
                at = i;
        at = (at + step + int(std::size(kRoundChoices))) % int(std::size(kRoundChoices));
        room.rounds = kRoundChoices[at];
    } else if (row == kArena) {
        room.arena = uint8_t((room.arena + step + 4) % 4); // the host, random, the loser, the winner
    } else if (row == kRematch) {
        room.rematch = room.rematch ? 0 : 1;
    } else {
        return 1;
    }
    net::SetOnlineRoom(room);
    UpdateTexts();
    g_OnOffEvent(handler, nullptr, event, value); // its sound
    return 1;
}

// Start: both games to the Versus fighters' select (the Versus row's own way: the profile's mode bytes +0x32 and +0x33
// cleared, 0x2CBEC0); the guest's follows the host's screen.
bool GoToVersusSelect()
{
    __try {
        if (uint8_t* profile = CurrentProfile()) {
            profile[0x32] = 0;
            profile[0x33] = 0;
        }
        // the shell gives its screens the events of two pads only, player 1's ([[0x7F33F0]+0x414]) and player 2's
        // (+0x418; 0x2A82D0): the host's pad and the guest's, on both games (the select screen then gives player 1's
        // to player 1, the other's to player 2: 0x2D54A0)
        if (uint8_t* shellState = *reinterpret_cast<uint8_t**>(uintptr_t(0x007F33F0))) {
            *reinterpret_cast<int*>(shellState + 0x414) = 0;
            *reinterpret_cast<int*>(shellState + 0x418) = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return GoToShellScreen("select_jedi");
}

char __fastcall ButtonEvent(uint8_t* handler, void*, int event, int value)
{
    const int row = RowOf(handler);
    if (event == kB)
        return Back(handler, event, value);
    if (row < 0 || event != kChosen)
        return g_ButtonEvent(handler, nullptr, event, value);
    if (RowDisabled(row))
        return 1;
    const net::OnlineStatusInfo s = net::OnlineStatus();
    switch (row) {
    case kHost:
        g_Note.clear();
        g_Copied.clear();
        g_Typed.clear();
        LOG_INFO("Online menu: Host Game");
        if (!net::OnlineHost())
            g_Note = Shorten(net::OnlineStatus().message, 40);
        else
            GoToShellScreen(kScreenNames[kRoom]);
        UpdateTexts();
        return 1;
    case kJoin:
        LOG_INFO("Online menu: Join Game");
        OpenKeyboard(false);
        return 1;
    case kSettings:
        GoToShellScreen(kScreenNames[kSettingsScreen]);
        return 1;
    case kStart:
        LOG_INFO("Online menu: Start (Versus)");
        GoToVersusSelect();
        return 1;
    case kLeave:
        g_Note.clear();
        g_CopiedAt = 0;
        LOG_INFO("Online menu: Leave Room");
        net::OnlineLeave(); // in a match the game goes back to its main menu
        if (!s.matchOn)
            GoToShellScreen(kScreenNames[kOnline]);
        return 1;
    default:
        return 1;
    }
}

DrawFn g_ButtonDraw = nullptr;

char __fastcall ButtonDraw(uint8_t* handler, void*, uint32_t value)
{
    const int row = RowOf(handler);
    if (row < 0)
        return g_ButtonDraw(handler, nullptr, value);
    if (row == kHost)
        UpdateTexts();
    LinkRow(handler, row);
    return DrawDimmed(handler, RowDimmed(row), [&] { return g_ButtonDraw(handler, nullptr, value); });
}

uint8_t* MakeValue()
{
    uint8_t* handler = reinterpret_cast<uint8_t*(__cdecl*)()>(uintptr_t(kMakeOnOff))();
    if (handler)
        *reinterpret_cast<uint32_t**>(handler) = g_ValueVtable;
    return handler;
}
uint8_t* __fastcall MakeValueSlot(uint8_t*, void*) { return MakeValue(); }
uint8_t* MakeButton()
{
    uint8_t* handler = reinterpret_cast<uint8_t*(__cdecl*)()>(uintptr_t(kMakeButton))();
    if (handler)
        *reinterpret_cast<uint32_t**>(handler) = g_ButtonVtable;
    return handler;
}
uint8_t* __fastcall MakeButtonSlot(uint8_t*, void*) { return MakeButton(); }
void __fastcall Nothing(uint8_t*, void*) {}

// --- The code keyboard -----------------------------------------------------------------------------------------------
constexpr uint32_t kMakeCodesKeyboard = 0x002D1740;
constexpr uint32_t kCodesKeyboardVtable = 0x005D61E0;
constexpr uint32_t kCodesKeyboardInit = 0x002D0420;
constexpr uint32_t kKeyboardDraw = 0x002D0820;
constexpr uint32_t kCodesKeyboardDone = 0x002D0930;
constexpr uint32_t kKeyboardClear = 0x002D0460;
constexpr int kInitSlot = 0, kDrawSlot = 3, kMakeSlot = 10, kSoundSlot = 11, kDoneSlot = 29, kClearSlot = 30;
constexpr int kErrorSound = 0x10;
constexpr int kTextAt = 0x28, kTextEnd = 0x48, kMaxLength = 0x24, kBackKey = 0x50, kSpaceKey = 0x54, kDoneKey = 0x58;
constexpr int kCodeLength = 15; // the text holds 16 characters, its end mark one
uint32_t g_KeyboardVtable[kVtableSlots];
bool g_PasteDown = false;
bool g_KeyboardForName = false; // the keyboard types the player's name (Online Settings), not a room code

void Sound(uint8_t* keyboard, int id)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(
        (*reinterpret_cast<void* const* const*>(keyboard))[kSoundSlot])(keyboard, nullptr, id);
}

std::string KeyboardText(uint8_t* keyboard)
{
    const wchar_t* text = reinterpret_cast<const wchar_t*>(keyboard + kTextAt);
    const wchar_t* end = *reinterpret_cast<wchar_t* const*>(keyboard + kTextEnd);
    std::string out;
    for (const wchar_t* c = text; c < end && c < text + kCodeLength + 1 && *c; ++c)
        out += char(*c);
    return out;
}

void SetKeyboardText(uint8_t* keyboard, const std::string& text)
{
    wchar_t* buffer = reinterpret_cast<wchar_t*>(keyboard + kTextAt);
    const int most = std::min(*reinterpret_cast<int*>(keyboard + kMaxLength), kCodeLength);
    int n = 0;
    for (char c : text) {
        if (n >= most)
            break;
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == ' ' && g_KeyboardForName))
            buffer[n++] = wchar_t(c);
    }
    buffer[n] = 0;
    *reinterpret_cast<wchar_t**>(keyboard + kTextEnd) = buffer + n;
    for (int key : { kBackKey, kSpaceKey, kDoneKey })
        if (uint8_t* handler = *reinterpret_cast<uint8_t**>(keyboard + key))
            handler[0x20] = n == 0 ? 1 : 0;
}

bool Paste(uint8_t* keyboard);

void __fastcall KeyboardInit(uint8_t* keyboard, void*)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kCodesKeyboardInit))(keyboard, nullptr);
    *reinterpret_cast<int*>(keyboard + kMaxLength) = kCodeLength;
    g_PasteDown = true; // a Ctrl+V held from before the screen does not count
    if (g_KeyboardForName) {
        const std::string name = net::OnlinePlayerName();
        SetKeyboardText(keyboard, name == "NONAME" ? std::string() : name);
    }
}

// Ctrl+V: the clipboard's code (its letters and digits) as the text.
void __fastcall KeyboardDraw(uint8_t* keyboard, void*, uint32_t value)
{
    const bool down = !g_KeyboardForName && GameWindowActive() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState('V') & 0x8000);
    if (down && !g_PasteDown)
        Paste(keyboard);
    g_PasteDown = down;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint32_t)>(uintptr_t(kKeyboardDraw))(keyboard, nullptr, value);
}

// Y on any key (the keys' events, vtables 0x5D5FA0 Key, 0x5D60F0 Space, 0x5D6010 Back, 0x5D6080 Done: their
// keyboard is the parent item's handler, item +0x28, its +0x80) pastes too, for a controller.
constexpr int kY = 0x2C;
constexpr uint32_t kKeyVtables[] = { 0x005D5FA0, 0x005D60F0, 0x005D6010, 0x005D6080 };
constexpr uint32_t kKeyEvents[] = { 0x002D05D0, 0x002D0680, 0x002D0700, 0x002D0760 };
constexpr int kEventSlot = 4;

bool Paste(uint8_t* keyboard)
{
    if (g_KeyboardForName || !Idle(net::OnlineStatus()))
        return false;
    const std::string text = ClipboardText();
    net::Endpoint a, b;
    if (!net::ReadRoomCode(text, 0, a, b)) {
        Sound(keyboard, kErrorSound);
        return false;
    }
    SetKeyboardText(keyboard, text);
    LOG_INFO("Online menu: room code pasted");
    return true;
}

// A room code's letters read as digits (S, D, O, B: 5, 0, 0, 8), which it has none of.
bool IsCodeLetterRefused(wchar_t c)
{
    return c == L'S' || c == L'D' || c == L'O' || c == L'B';
}

template <int I>
char __fastcall KeyEvent(uint8_t* key, void*, int event, int value)
{
    if (event == kY) {
        const uint8_t* item = *reinterpret_cast<uint8_t**>(key + 8);
        const uint8_t* parent = item ? *reinterpret_cast<uint8_t* const*>(item + 0x28) : nullptr;
        uint8_t* keyboard = parent ? *reinterpret_cast<uint8_t* const*>(parent + 0x80) : nullptr;
        if (keyboard && *reinterpret_cast<uint32_t**>(keyboard) == g_KeyboardVtable) {
            Paste(keyboard);
            return 1;
        }
    }
    const char result = reinterpret_cast<ItemEventFn>(uintptr_t(kKeyEvents[I]))(key, nullptr, event, value);
    if (I == 0 && !g_KeyboardForName) { // a room code has no S, D, O or B (read as 5, 0, 8): those keys type nothing
        const uint8_t* item = *reinterpret_cast<uint8_t**>(key + 8);
        const uint8_t* parent = item ? *reinterpret_cast<uint8_t* const*>(item + 0x28) : nullptr;
        uint8_t* keyboard = parent ? *reinterpret_cast<uint8_t* const*>(parent + 0x80) : nullptr;
        if (keyboard && *reinterpret_cast<uint32_t**>(keyboard) == g_KeyboardVtable) {
            const std::string text = KeyboardText(keyboard);
            std::string kept;
            for (char c : text)
                if (!IsCodeLetterRefused(wchar_t(c)))
                    kept += c;
            if (kept != text) {
                SetKeyboardText(keyboard, kept);
                Sound(keyboard, kErrorSound);
            }
        }
    }
    return result;
}
constexpr ItemEventFn kKeyHooks[] = { &KeyEvent<0>, &KeyEvent<1>, &KeyEvent<2>, &KeyEvent<3> };

// A letter key's draw (vtable 0x5D5FA0 slot 3, 0x2D0F90): for a room code, the letters read as digits (S, D, O, B)
// are disabled, as the game disables Back, Space and Done on an empty text (the key's handler +0x20). The key's
// letter: its item's label (+0x44: a u16 length and the characters).
constexpr uint32_t kKeyDraw = 0x002D0F90;

void DisableKeyRaw(uint8_t* key)
{
    __try {
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(key + 8);
        const uint8_t* parent = item ? *reinterpret_cast<uint8_t* const*>(item + 0x28) : nullptr;
        const uint8_t* keyboard = parent ? *reinterpret_cast<uint8_t* const*>(parent + 0x80) : nullptr;
        if (!keyboard || *reinterpret_cast<uint32_t* const*>(keyboard) != g_KeyboardVtable)
            return; // the game's own Codes keyboard: every key
        const uint16_t* label = *reinterpret_cast<const uint16_t* const*>(item + 0x44);
        if (label && label[0] == 1)
            key[0x20] = !g_KeyboardForName && IsCodeLetterRefused(wchar_t(label[1])) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

char __fastcall KeyDraw(uint8_t* key, void*, uint32_t value)
{
    DisableKeyRaw(key);
    return reinterpret_cast<DrawFn>(uintptr_t(kKeyDraw))(key, nullptr, value);
}

// Done: joins with the code typed (back to the online screen), or refuses it.
void __fastcall KeyboardDone(uint8_t* keyboard, void*)
{
    if (g_KeyboardForName) { // Online Settings: the name
        net::SetOnlinePlayerName(KeyboardText(keyboard));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kKeyboardClear))(keyboard, nullptr);
        UpdateTexts();
        GoToShellScreen(kScreenNames[kSettingsScreen]);
        return;
    }
    if (!Idle(net::OnlineStatus()))
        return;
    const std::string code = KeyboardText(keyboard);
    net::Endpoint a, b;
    if (!net::ReadRoomCode(code, 0, a, b)) {
        Sound(keyboard, kErrorSound);
        LOG_INFO("Online menu: room code \"%s\" refused", code.c_str());
        return;
    }
    g_Typed = code.substr(0, 5) + "-" + code.substr(5, 5) + "-" + code.substr(10);
    g_Note.clear();
    LOG_INFO("Online menu: joining with %s", g_Typed.c_str());
    if (!net::OnlineJoin(code))
        g_Note = Shorten(net::OnlineStatus().message, 40);
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kKeyboardClear))(keyboard, nullptr);
    UpdateTexts();
    GoToShellScreen(kScreenNames[g_Note.empty() ? kRoom : kOnline]);
}

void OpenKeyboard(bool forName)
{
    g_KeyboardForName = forName;
    SetMenuText("IDS_PORT_ONLINE_ENTER_CODE", forName ? L"Enter your Name" : L"Enter the Room Code");
    GoToShellScreen(kCodeScreen);
}

uint8_t* MakeKeyboard()
{
    uint8_t* keyboard = reinterpret_cast<uint8_t*(__cdecl*)()>(uintptr_t(kMakeCodesKeyboard))();
    if (keyboard)
        *reinterpret_cast<uint32_t**>(keyboard) = g_KeyboardVtable;
    return keyboard;
}
uint8_t* __fastcall MakeKeyboardSlot(uint8_t*, void*) { return MakeKeyboard(); }

// --- The screens -----------------------------------------------------------------------------------------------------
// A top-level item without children: where it starts and where the next item starts.
bool LeafItem(const std::vector<uint8_t>& data, const std::string& name, size_t& begin, size_t& end)
{
    const std::vector<uint8_t> item = MenuString("screenItem");
    std::vector<uint8_t> what = item;
    const std::vector<uint8_t> n = MenuString(name);
    what.insert(what.end(), n.begin(), n.end());
    begin = MenuFind(data, what, 0, data.size());
    if (begin == std::string::npos)
        return false;
    end = MenuFind(data, item, begin + 1, data.size());
    return end != std::string::npos;
}

// An item's x (after "screenItem", its name and a u32).
void SetItemX(std::vector<uint8_t>& data, size_t item, float x)
{
    std::memcpy(data.data() + ItemY(data, item) - 4, &x, 4);
}

// The code screen: the game's Codes screen (its keyboard the port's), and in its footer a fourth prompt, Y Paste: a
// copy of the Back prompt (its ButtonIcon's icon, 82 bytes after its name, item +0x84: 0 A, 1 Y, 2 B, 3 X; the
// texture interfc\front_end\s_buttons_xbox_<letter>, 0x2C4C90) and of its text. The
// prompts are spread again: Scroll, Select, Paste, Back.
bool BuildCodeScreen(std::vector<uint8_t>& data)
{
    if (!ReadDiscResource(kCodesFile, data))
        return false;
    size_t end = 64;
    bool ok = MenuReplaceString(data, 0, end, "codes", kCodeScreen);
    end = data.size();
    ok = ok && MenuReplaceString(data, 0, end, "IDS_SHELL_ENTER_CODE", "IDS_PORT_ONLINE_ENTER_CODE");
    end = data.size();
    ok = ok && MenuReplaceString(data, 0, end, "CodesKeyboard", "OnlineKeyboard");
    if (!ok) {
        LOG_WARN("Online menu: the Codes screen is not as expected; no code screen");
        return false;
    }
    size_t iconBegin, iconEnd, textBegin, textEnd;
    // The prompt hangs the front end's loading as made so far (2026-10-31: the shell stays on LOADING once it is in
    // the screen); until that is understood it is only made for tests (SWROTS_TEST_PASTEMODE=y, icon0, icon3, text).
    char mode[16] = "";
    if (!GetEnvironmentVariableA("SWROTS_TEST_PASTEMODE", mode, sizeof(mode)))
        return true;
    if (!LeafItem(data, "backbutton", iconBegin, iconEnd) || !LeafItem(data, "cancel", textBegin, textEnd) ||
        iconBegin + 18 + 10 + 82 >= iconEnd) {
        LOG_WARN("Online menu: the Codes screen's prompts are not as expected; no Paste prompt");
        return true;
    }
    std::vector<uint8_t> icon(data.begin() + iconBegin, data.begin() + iconEnd);
    std::vector<uint8_t> text(data.begin() + textBegin, data.begin() + textEnd);
    size_t e = icon.size();
    MenuReplaceString(icon, 0, e, "backbutton", "pastebutton");
    icon[18 + 11 + 82] = std::strcmp(mode, "icon0") == 0 ? 0 : std::strcmp(mode, "icon3") == 0 ? 3 : 1; // Y
    e = text.size();
    MenuReplaceString(text, 0, e, "cancel", "pastetext");
    e = text.size();
    if (!MenuReplaceString(text, 0, e, "IDS_BACK", "IDS_PORT_ONLINE_PASTE"))
        return true;
    // the new prompt after the Back text; then the places, from the right: Back 485/518, Paste 375/408, Select 265/298
    std::vector<uint8_t> both;
    if (std::strcmp(mode, "text") != 0)
        both = icon;
    if (std::strcmp(mode, "icon3") != 0)
        both.insert(both.end(), text.begin(), text.end());
    data.insert(data.begin() + textEnd, both.begin(), both.end());
    const struct { const char* name; float x; } places[] = { { "selectbutton", 262 }, { "select", 295 },
        { "pastebutton", 375 }, { "pastetext", 408 }, { "backbutton", 488 }, { "cancel", 521 } };
    for (const auto& p : places) {
        size_t b, en;
        if (LeafItem(data, p.name, b, en))
            SetItemX(data, b, p.x);
    }
    // the screen's item count: after its name and its first item's
    uint32_t nameLength, firstLength, items;
    std::memcpy(&nameLength, data.data(), 4);
    std::memcpy(&firstLength, data.data() + 4 + nameLength, 4);
    const size_t countAt = 4 + nameLength + 4 + firstLength;
    std::memcpy(&items, data.data() + countAt, 4);
    items += std::strcmp(mode, "text") == 0 || std::strcmp(mode, "icon3") == 0 ? 1 : 2;
    std::memcpy(data.data() + countAt, &items, 4);
    return true;
}

bool GenerateScreens(const std::string& lowerName, std::vector<uint8_t>& data)
{
    if (lowerName == kScreenFile)
        return BuildSettingsScreen(data, kScreen, Rows(kOnline), "IDS_PORT_ONLINE_TITLE", "OnlineValue", "OnlineButton");
    if (lowerName == kSettingsFileName)
        return BuildSettingsScreen(data, kScreenNames[kSettingsScreen], Rows(kSettingsScreen), "IDS_PORT_ONLINE_SETTINGS",
            "OnlineValue", "OnlineButton");
    if (lowerName == kRoomFileName)
        return BuildSettingsScreen(data, kScreenNames[kRoom], Rows(kRoom), "IDS_PORT_ROOM_TITLE", "OnlineValue",
            "OnlineButton");
    if (lowerName == kCodeFile)
        return BuildCodeScreen(data);
    return false;
}

// Diagnostics while online: the Versus select's events and the pad each comes from (JediList, vtable 0x5D68B8 slot 4,
// 0x2D54A0: thiscall (event, pad); player 1's pad is [[0x7F33F0]+0x414], the others' events go to player 2).
constexpr uint32_t kJediListVtable = 0x005D68B8, kJediListEvent = 0x002D54A0;
char __fastcall JediListEvent(uint8_t* list, void*, int event, int pad)
{
    static int logged = 0;
    if (net::OnlineActive() && logged < 300) {
        ++logged;
        int p1 = -1;
        if (uint8_t* shellState = *reinterpret_cast<uint8_t**>(uintptr_t(0x007F33F0)))
            p1 = *reinterpret_cast<int*>(shellState + 0x414);
        LOG_INFO("Online menu: select event 0x%X from pad %d (player 1's pad %d)", event, pad, p1);
    }
    return reinterpret_cast<char(__fastcall*)(uint8_t*, void*, int, int)>(uintptr_t(kJediListEvent))(list, nullptr,
        event, pad);
}

bool SlotIs(uint32_t vtable, int slot, uint32_t function)
{
    return *reinterpret_cast<const uint32_t*>(uintptr_t(vtable + 4 * slot)) == function;
}

} // namespace

bool OnlineRefusesMainMenuEvent(uint8_t*, int)
{
    return false; // the main menu stays whole (online play has its own screens)
}

bool OnlineShowLost(const std::string& why)
{
    const bool left = why.find("left") != std::string::npos;
    SetMenuText("IDS_PORT_LOST_TEXT", left ? L"The other player left the room." :
        L"The connection to the other player was lost.");
    g_LostPopup = true;
    return GoToShellScreen(kScreenNames[kOnline]);
}

bool OnlineStartVersus()
{
    return GoToVersusSelect();
}

bool OnlineFollowsScreen(uint32_t key)
{
    return key == MenuKey(kScreenNames[kRoom]) || key == MenuKey("select_jedi") || key == 0x8DC51DC9u ||
        key == MenuKey("select_arena");
}

bool OnlineGoToScreen(uint32_t key)
{
    if (key == MenuKey(kScreenNames[kRoom]))
        return GoToShellScreen(kScreenNames[kRoom]);
    if (key == MenuKey("select_jedi") || key == 0x8DC51DC9u)
        return GoToVersusSelect();
    if (key == MenuKey("select_arena"))
        return GoToShellScreen("select_arena");
    return false;
}

void PatchMainMenuOnline(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> copy = data;
    if (PatchRows(copy))
        data.swap(copy);
}

void InstallOnlineMenu()
{
    if (!SlotIs(kCodesKeyboardVtable, kInitSlot, kCodesKeyboardInit) ||
        !SlotIs(kCodesKeyboardVtable, kDrawSlot, kKeyboardDraw) ||
        !SlotIs(kCodesKeyboardVtable, kDoneSlot, kCodesKeyboardDone) ||
        !SlotIs(kCodesKeyboardVtable, kClearSlot, kKeyboardClear) ||
        !SlotIs(kCodesKeyboardVtable, kMakeSlot, kMakeCodesKeyboard) ||
        !SlotIs(kKeyVtables[0], kEventSlot, kKeyEvents[0]) || !SlotIs(kKeyVtables[1], kEventSlot, kKeyEvents[1]) ||
        !SlotIs(kKeyVtables[2], kEventSlot, kKeyEvents[2]) || !SlotIs(kKeyVtables[3], kEventSlot, kKeyEvents[3])) {
        LOG_WARN("Online menu: the code keyboard is not as expected; no Online entry");
        return;
    }
    if (SlotIs(kKeyVtables[0], 3, kKeyDraw)) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&KeyDraw));
        PatchBytes(kKeyVtables[0] + 3 * 4, &hook, 4);
    }
    if (SlotIs(kJediListVtable, 4, kJediListEvent)) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&JediListEvent));
        PatchBytes(kJediListVtable + 4 * 4, &hook, 4);
    }
    if (SlotIs(kIntroVtable, 0, kIntroInit)) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&IntroInit));
        PatchBytes(kIntroVtable, &hook, 4);
    }
    std::memcpy(g_ValueVtable, reinterpret_cast<const void*>(uintptr_t(kOnOffVtable)), sizeof g_ValueVtable);
    g_OnOffDraw = reinterpret_cast<DrawFn>(uintptr_t(g_ValueVtable[3]));
    g_OnOffEvent = reinterpret_cast<ItemEventFn>(uintptr_t(g_ValueVtable[4]));
    g_ValueVtable[3] = uint32_t(reinterpret_cast<uintptr_t>(&ValueDraw));
    g_ValueVtable[4] = uint32_t(reinterpret_cast<uintptr_t>(&ValueEvent));
    g_ValueVtable[10] = uint32_t(reinterpret_cast<uintptr_t>(&MakeValueSlot));
    g_ValueVtable[27] = uint32_t(reinterpret_cast<uintptr_t>(&Nothing));
    g_ValueVtable[28] = uint32_t(reinterpret_cast<uintptr_t>(&Nothing));
    std::memcpy(g_ButtonVtable, reinterpret_cast<const void*>(uintptr_t(kButtonVtable)), sizeof g_ButtonVtable);
    g_ButtonEvent = reinterpret_cast<ItemEventFn>(uintptr_t(g_ButtonVtable[4]));
    g_ButtonVtable[4] = uint32_t(reinterpret_cast<uintptr_t>(&ButtonEvent));
    g_ButtonDraw = reinterpret_cast<DrawFn>(uintptr_t(g_ButtonVtable[3]));
    g_ButtonVtable[3] = uint32_t(reinterpret_cast<uintptr_t>(&ButtonDraw));
    g_ButtonVtable[10] = uint32_t(reinterpret_cast<uintptr_t>(&MakeButtonSlot));
    std::memcpy(g_KeyboardVtable, reinterpret_cast<const void*>(uintptr_t(kCodesKeyboardVtable)), sizeof g_KeyboardVtable);
    g_KeyboardVtable[kInitSlot] = uint32_t(reinterpret_cast<uintptr_t>(&KeyboardInit));
    g_KeyboardVtable[kDrawSlot] = uint32_t(reinterpret_cast<uintptr_t>(&KeyboardDraw));
    g_KeyboardVtable[kDoneSlot] = uint32_t(reinterpret_cast<uintptr_t>(&KeyboardDone));
    g_KeyboardVtable[kMakeSlot] = uint32_t(reinterpret_cast<uintptr_t>(&MakeKeyboardSlot));
    for (size_t i = 0; i < std::size(kKeyVtables); ++i) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(kKeyHooks[i]));
        PatchBytes(kKeyVtables[i] + 4 * kEventSlot, &hook, 4);
    }

    SetMenuText(kRowText, L"Online");
    SetMenuText("IDS_PORT_ONLINE_TITLE", L"Online");
    SetMenuText("IDS_PORT_ONLINE_HOST", L"Host Game");
    SetMenuText("IDS_PORT_ONLINE_JOIN", L"Join Game");
    SetMenuText("IDS_PORT_ONLINE_SETTINGS", L"Online Settings");
    SetMenuText("IDS_PORT_ONLINE_STATUS", L"Status");
    SetMenuText("IDS_PORT_ONLINE_NAME", L"Name");
    SetMenuText("IDS_PORT_ROOM_TITLE", L"Room");
    SetMenuText("IDS_PORT_ROOM_MODE", L"Game Mode");
    SetMenuText("IDS_PORT_ROOM_ROUNDS", L"Rounds to Win");
    SetMenuText("IDS_PORT_ROOM_ARENA", L"Arena");
    SetMenuText("IDS_PORT_ROOM_REMATCH", L"Auto Rematch");
    SetMenuText("IDS_PORT_ROOM_P1", L"Player 1");
    SetMenuText("IDS_PORT_ROOM_P2", L"Player 2");
    SetMenuText("IDS_PORT_ROOM_CODE", L"Room Code");
    SetMenuText("IDS_PORT_ROOM_START", L"Start");
    SetMenuText("IDS_PORT_ROOM_LEAVE", L"Leave Room");
    SetMenuText("IDS_PORT_LOST_TITLE", L"Connection Lost");
    SetMenuText("IDS_PORT_LOST_TEXT", L"The connection to the other player was lost.");
    SetMenuText("IDS_PORT_ONLINE_PASTE", L"Paste");
    SetMenuText("IDS_PORT_ONLINE_ENTER_CODE", L"Enter the Room Code");
    g_PasteDown = false;
    UpdateTexts();
    AddShellHandler("OnlineValue", &MakeValue);
    AddShellHandler("OnlineButton", &MakeButton);
    AddShellHandler("OnlineKeyboard", &MakeKeyboard);
    AddShellScreen(kScreenFile);
    AddShellScreen(kSettingsFileName);
    AddShellScreen(kRoomFileName);
    AddShellScreen(kCodeFile);
    RegisterResourceGenerator(&GenerateScreens);
}

} // namespace swrots::game
