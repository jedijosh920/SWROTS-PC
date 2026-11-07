// Online play in the game's main menu (see onlinemenu.h).
//
// The main menu (main_menu.xbl_xml) has four top-level rows, MainMenuButton items 24 apart from y 179: Single Player,
// Multi Player, Settings, Features (each a text id, its down and up neighbours, and for Settings the screen it opens),
// with a veil behind each row and a fifth veil (debugveil, y 277) spare. A row "online" is put between Multi Player and
// Settings: a copy of Multi Player's (its bar and submenu, after Heroes vs Villains' entries there if any), its submenu
// four entries of the port's (Host Game, Join Game, Public Rooms, Online Settings); Settings and Features move one row
// down, and the neighbours are linked through it.
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
#include "game/characters.h"
#include "game/hvvmenu.h"
#include "game/menus.h"
#include "game/resources.h"
#include "input/controls.h"
#include "kernel/kernel.h"
#include "net/lobby.h"
#include "net/session.h"
#include "net/transport.h"

namespace swrots::game {

namespace {

constexpr const char* kRowName = "online";
constexpr const char* kRowText = "IDS_PORT_ONLINE";
constexpr const char* kCodeScreen = "online_code";
constexpr const char* kCodeFile = "interfc\\front_end\\xml\\online_code.xml";
constexpr const char* kCodesFile = "interfc\\front_end\\xml\\codes.xml";
constexpr const char* kSettingsFile = "interfc\\front_end\\xml\\settings.xml";

// settings.ini, next to the game
std::wstring SettingsIni()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ini = path;
    return ini.substr(0, ini.find_last_of(L"\\/")) + L"\\settings.ini";
}
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

// A length-prefixed string of the menu's format at `at`: its text (false when there is none that looks like one).
bool ReadMenuString(const std::vector<uint8_t>& data, size_t at, std::string& text)
{
    if (at + 4 > data.size())
        return false;
    uint32_t n;
    std::memcpy(&n, data.data() + at, 4);
    if (n == 0 || n > 64 || at + 4 + n > data.size())
        return false;
    text.assign(reinterpret_cast<const char*>(data.data()) + at + 4, n);
    for (char c : text)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

// A copy of a submenu entry (MainMenuSwitchScreenButton and its bar) as another. An entry: "screenItem", its name, its
// place (x, y), ..., its text id, 17 bytes, then its down and up neighbours' names and its screen's (then its id).
bool MakeEntry(const std::vector<uint8_t>& from, const char* name, const char* text, const char* down, const char* up,
    const char* screen, float y, std::vector<uint8_t>& out)
{
    const size_t nameAt = MenuString("screenItem").size();
    std::string oldName, oldText;
    if (!ReadMenuString(from, nameAt, oldName))
        return false;
    std::vector<uint8_t> entry = from;
    size_t end = entry.size();
    if (!MenuReplaceString(entry, 0, end, oldName, name))
        return false;
    // the text id: the first IDS_ string after the name
    const std::vector<uint8_t> ids = { 'I', 'D', 'S', '_' };
    size_t textAt = std::string::npos;
    for (size_t i = nameAt + 4; i + 8 < entry.size(); ++i)
        if (std::memcmp(entry.data() + i + 4, ids.data(), 4) == 0 && ReadMenuString(entry, i, oldText)) {
            textAt = i;
            break;
        }
    if (textAt == std::string::npos || !MenuReplaceString(entry, textAt, end, oldText, text))
        return false;
    const size_t linksAt = textAt + 4 + std::strlen(text) + 17;
    std::string oldDown, oldUp, oldScreen;
    if (!ReadMenuString(entry, linksAt, oldDown) || !ReadMenuString(entry, linksAt + 4 + oldDown.size(), oldUp) ||
        !ReadMenuString(entry, linksAt + 8 + oldDown.size() + oldUp.size(), oldScreen))
        return false;
    const size_t linksEnd = linksAt + 12 + oldDown.size() + oldUp.size() + oldScreen.size();
    std::vector<uint8_t> links;
    for (const char* t : { down, up, screen }) {
        const std::vector<uint8_t> str = MenuString(t);
        links.insert(links.end(), str.begin(), str.end());
    }
    entry.erase(entry.begin() + linksAt, entry.begin() + linksEnd);
    entry.insert(entry.begin() + linksAt, links.begin(), links.end());
    float x, oldY;
    if (!MenuItemPosition(entry, &x, &oldY, false))
        return false;
    MenuItemPosition(entry, &x, &y, true);
    out.insert(out.end(), entry.begin(), entry.end());
    return true;
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
    // The new row: Multi Player's whole (its row, bar, submenu and entries, up to Settings: Versus, Cooperative, and
    // the port's Heroes vs Villains and Battlefront when there), as Online with a submenu of four: Host Game, Join
    // Game, Public Rooms, Online Settings (their choice is the port's own: SwitchEvent). The submenu ends with bytes
    // of its own after its last entry's bar (each entry's bar as long as Versus's).
    std::vector<uint8_t> tree(data.begin() + multi, data.begin() + settings);
    const std::vector<uint8_t> item = MenuString("screenItem");
    size_t versus, versusEnd, sub, subEnd;
    if (!MenuRowBlock(tree, "versus", versus, versusEnd) || !MenuRowBlock(tree, "multiplayersubmenu", sub, subEnd) ||
        sub >= versus) {
        LOG_WARN("Online menu: the Multi Player row is not as expected; no Online entry");
        return false;
    }
    const size_t versusBar = MenuFind(tree, item, versus + 1, versusEnd);
    size_t lastBar = std::string::npos;
    for (size_t at = MenuFind(tree, item, versus, tree.size()); at != std::string::npos;
         at = MenuFind(tree, item, at + 1, tree.size()))
        lastBar = at;
    if (versusBar == std::string::npos || lastBar == std::string::npos ||
        lastBar + (versusEnd - versusBar) > tree.size()) {
        LOG_WARN("Online menu: the Multi Player submenu is not as expected; no Online entry");
        return false;
    }
    const std::vector<uint8_t> tail(tree.begin() + lastBar + (versusEnd - versusBar), tree.end());
    const std::vector<uint8_t> versusBlock(tree.begin() + versus, tree.begin() + versusEnd);
    float x, y0;
    if (!MenuItemPosition(const_cast<std::vector<uint8_t>&>(versusBlock), &x, &y0, false))
        return false;
    static const char* const kEntries[4][5] = {
        { "mmonlinehost", "IDS_PORT_ONLINE_HOST", "mmonlinejoin", "mmonlinesettings", "online_room" },
        { "mmonlinejoin", "IDS_PORT_ONLINE_JOIN", "mmonlinepublic", "mmonlinehost", "online_code" },
        { "mmonlinepublic", "IDS_PORT_ONLINE_PUBLIC", "mmonlinesettings", "mmonlinejoin", "online_public" },
        { "mmonlinesettings", "IDS_PORT_ONLINE_SETTINGS", "mmonlinehost", "mmonlinepublic", "online_settings" },
    };
    std::vector<uint8_t> entries;
    for (int i = 0; i < 4; ++i)
        if (!MakeEntry(versusBlock, kEntries[i][0], kEntries[i][1], kEntries[i][2], kEntries[i][3], kEntries[i][4],
                y0 + kRowStep * float(i), entries)) {
            LOG_WARN("Online menu: the Versus entry is not as expected; no Online entry");
            return false;
        }
    std::vector<uint8_t> row(tree.begin(), tree.begin() + versus);
    // the submenu's children: the u32 before its first one
    uint32_t children = 4;
    std::memcpy(row.data() + row.size() - 4, &children, 4);
    size_t rowEnd = row.size();
    bool ok = MenuReplaceString(row, 0, rowEnd, "multiplayer", kRowName) &&
              MenuReplaceString(row, nameAt + 4, rowEnd, "IDS_MULTI_PLAYER", kRowText) &&
              MenuReplaceString(row, nameAt + 4, rowEnd, "singleplayer", "multiplayer") && // up (down: Settings)
              MenuReplaceString(row, nameAt + 4, rowEnd, "multiplayersubmenu", "onlinesubmenu");
    if (!ok) {
        LOG_WARN("Online menu: the Multi Player row's strings are not as expected; no Online entry");
        return false;
    }
    MoveItem(row, 0, kRowStep); // Multi Player's place, one row lower (its children's places are its own)
    row.insert(row.end(), entries.begin(), entries.end());
    row.insert(row.end(), tail.begin(), tail.end());
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
    LOG_INFO("Online menu: the main menu has an Online entry and its submenu");
    return true;
}

// The game's own popup on the main menu (for "connection lost"): the Settings screen's DefaultPopup item (it uses no
// texture set), put after the main menu's last item (its own YesNoPopup). A screen: its name, its default focus, its
// items (their count, then each), its transition controllers (the same; none on the main menu), its texture sets
// (the same): the item goes before the transition controllers' count, the word before the texture sets' count.
bool AddMainMenuPopup(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> settings;
    if (!ReadDiscResource(kSettingsFile, settings))
        return false;
    const std::vector<uint8_t> item = MenuString("screenItem");
    std::vector<uint8_t> popupTag = item;
    const std::vector<uint8_t> popupName = MenuString("DefaultPopup");
    popupTag.insert(popupTag.end(), popupName.begin(), popupName.end());
    const size_t popup = MenuFind(settings, popupTag, 0, settings.size());
    const size_t popupEnd = popup == std::string::npos ? popup : MenuFind(settings, item, popup + 1, settings.size());
    const size_t menuSets = MenuFind(data, MenuString("textureSet"), 0, data.size());
    uint32_t transitions = 1;
    if (menuSets != std::string::npos && menuSets >= 8)
        std::memcpy(&transitions, data.data() + menuSets - 8, 4);
    if (popupEnd == std::string::npos || menuSets == std::string::npos || menuSets < 8 || transitions != 0 ||
        MenuFind(data, popupTag, 0, data.size()) != std::string::npos ||
        std::memcmp(settings.data(), data.data(), 4) != 0) // the same make: the same first word
        return false;
    // the popup's own block (no child, no texture set)
    if (popupEnd - popup > 400 || MenuFind(settings, MenuString("textureSet"), popup, popupEnd) != std::string::npos)
        return false;
    const std::vector<uint8_t> block(settings.begin() + popup, settings.begin() + popupEnd);
    data.insert(data.begin() + (menuSets - 8), block.begin(), block.end());
    uint32_t nameLength, firstLength, items;
    std::memcpy(&nameLength, data.data() + 4, 4);
    std::memcpy(&firstLength, data.data() + 4 + 4 + nameLength, 4);
    const size_t countAt = 4 + 4 + nameLength + 4 + firstLength;
    std::memcpy(&items, data.data() + countAt, 4);
    ++items;
    std::memcpy(data.data() + countAt, &items, 4);
    LOG_INFO("Online menu: the main menu has the game's popup");
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

// --- The rows of the screens ------------------------------------------------------------------------------------------
// "online_settings": the player's name. "online_room" (both players, from Host Game or a join on): the mode, the arena,
// Room Settings, the two players, the room code, Ready (both players ready: the match starts) and Leave.
// "online_roomsettings": the room's name, rounds, rematch and Public Room (the host's; the guest sees them).
// "online_public": the public rooms.
enum Screen { kOnline, kSettingsScreen, kRoom, kPublicScreen, kRoomSettingsScreen, kScreenCount };
// (The Online screen is gone: Online is the main menu's submenu; its back and its popups are the main menu's.)
constexpr const char* kScreenNames[kScreenCount] = { "main_menu", "online_settings", "online_room", "online_public",
    "online_roomsettings" };
constexpr const char* kPublicFileName = "interfc\\front_end\\xml\\online_public.xml";
constexpr const char* kSettingsFileName = "interfc\\front_end\\xml\\online_settings.xml";
constexpr const char* kRoomFileName = "interfc\\front_end\\xml\\online_room.xml";
constexpr const char* kRoomSettingsFileName = "interfc\\front_end\\xml\\online_roomsettings.xml";

enum Row {
    kHost, kJoin, kSettings, kStatus,
    kName,
    kMode, kRoomSettings, kCode, kLeave, kGap, kPlayer1, kPlayer2,
    kRoomName, kPublic, kArena, kRounds, kTime, kRematch, // Rounds, Time Limit and Auto Rematch: Versus only (VersusRow)
    kPubStatus, kPub0, kPubLast = kPub0 + 7,
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
    { "roomsettings", "IDS_PORT_ROOM_SETTINGS", true, kRoom },
    { "roomcode", "IDS_PORT_ROOM_CODE", false, kRoom },
    { "roomleave", "IDS_PORT_ROOM_LEAVE", true, kRoom },
    { "roomgap", "IDS_PORT_ROOM_GAP", false, kRoom },  // a blank row, then the players as a table: name, connection, Ready
    { "roomp1", "IDS_PORT_ROOM_P1", false, kRoom },
    { "roomp2", "IDS_PORT_ROOM_P2", false, kRoom },
    { "roomname", "IDS_PORT_ROOM_NAME", false, kRoomSettingsScreen },
    { "roompublic", "IDS_PORT_ROOM_PUBLIC", false, kRoomSettingsScreen },
    { "roomarena", "IDS_PORT_ROOM_ARENA", false, kRoomSettingsScreen },
    { "roomrounds", "IDS_PORT_ROOM_ROUNDS", false, kRoomSettingsScreen },
    { "roomtime", "IDS_PORT_ROOM_TIME", false, kRoomSettingsScreen }, // a round's time limit
    { "roomrematch", "IDS_PORT_ROOM_REMATCH", false, kRoomSettingsScreen },
    { "pubstatus", "IDS_PORT_PUB_STATUS", false, kPublicScreen },
    { "pubroom1", "IDS_PORT_PUB_ROOM1", false, kPublicScreen },
    { "pubroom2", "IDS_PORT_PUB_ROOM2", false, kPublicScreen },
    { "pubroom3", "IDS_PORT_PUB_ROOM3", false, kPublicScreen },
    { "pubroom4", "IDS_PORT_PUB_ROOM4", false, kPublicScreen },
    { "pubroom5", "IDS_PORT_PUB_ROOM5", false, kPublicScreen },
    { "pubroom6", "IDS_PORT_PUB_ROOM6", false, kPublicScreen },
    { "pubroom7", "IDS_PORT_PUB_ROOM7", false, kPublicScreen },
    { "pubroom8", "IDS_PORT_PUB_ROOM8", false, kPublicScreen },
};
static_assert(sizeof(kRowDefs) / sizeof(kRowDefs[0]) == kRowCount, "a row each");

// Public Rooms: the lobby's last list (the rows show its first eight).
std::vector<net::PublicRoom> g_PubRooms;
std::string g_PendingJoin; // the room code a Yes in the popup joins
std::string g_PendingJoinName; // and its name

std::vector<ShellScreenRow> Rows(Screen screen)
{
    std::vector<ShellScreenRow> rows;
    for (const RowDef& r : kRowDefs)
        if (r.screen == screen)
            rows.push_back({ r.name, r.text, r.button });
    return rows;
}

// The room's name by default: the player's ("MITORI'S ROOM"); [Online] RoomName once the host has named a room.
std::string DefaultRoomName()
{
    return net::OnlinePlayerName() + "'S ROOM";
}

std::string SavedRoomName()
{
    wchar_t name[64] = L"";
    GetPrivateProfileStringW(L"Online", L"RoomName", L"", name, 64, SettingsIni().c_str());
    std::string out;
    for (const wchar_t* c = name; *c && out.size() < 23; ++c)
        if (*c >= 0x20 && *c < 0x7F)
            out += char(*c);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? DefaultRoomName() : out;
}

// The host's room named (never empty: the default name then), and the name kept for the next rooms.
void SetRoomName(const std::string& typed)
{
    std::string name = typed;
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    const bool custom = !name.empty() && name != DefaultRoomName();
    WritePrivateProfileStringW(L"Online", L"RoomName", custom ? std::wstring(name.begin(), name.end()).c_str() : nullptr,
        SettingsIni().c_str());
    if (!custom)
        name = DefaultRoomName();
    net::OnlineRoomSettings room = net::OnlineStatus().room;
    strncpy_s(room.name, name.c_str(), _TRUNCATE);
    net::SetOnlineRoom(room);
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
constexpr uint8_t kTimeChoices[] = { 0, 99, 60, 30 }; // seconds (0: none)

bool Idle(const net::OnlineStatusInfo& s)
{
    return s.phase == net::OnlinePhase::Off || s.phase == net::OnlinePhase::Ended;
}

// The room's players' table (the rows' texts, and what PlayerRowExtras draws over them): whether the player is there,
// their Ready, and for the other player the connection's time (-1: none).
struct PlayerRow {
    bool present = false;
    bool ready = false;
    int pingMs = -1;
};
PlayerRow g_PlayerRows[2];
// Room in the texts for the pictures: the box before a name, the bars before the time.
const std::string kBoxRoom = "   ";
const std::string kBarsRoom = "     ";
constexpr int kQualityBars = 5;

// The connection's quality as bars lit (of five).
int QualityBars(int pingMs)
{
    return pingMs < 0 ? 0 : pingMs < 30 ? 5 : pingMs < 60 ? 4 : pingMs < 100 ? 3 : pingMs < 160 ? 2 : 1;
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
    SetMenuText(ValueId(kMode).c_str(), s.room.mode == 1 ? L"COOPERATIVE" : L"VERSUS");
    SetMenuText(ValueId(kRounds).c_str(), Wide(std::to_string(s.room.rounds)));
    static const wchar_t* const kArenaTexts[] = { L"HOST PICKS", L"RANDOM", L"LOSER PICKS", L"WINNER PICKS" };
    SetMenuText(ValueId(kArena).c_str(), kArenaTexts[s.room.arena < std::size(kArenaTexts) ? s.room.arena : 0]);
    SetMenuText(ValueId(kRematch).c_str(), s.room.rematch ? L"ON" : L"OFF");
    SetMenuText(ValueId(kPublic).c_str(), s.room.open ? L"YES" : L"NO");
    if (!Idle(s) && s.room.name[0])
        SetMenuText("IDS_PORT_ROOM_TITLE", Wide(Shorten(s.room.name, 24)));
    else
        SetMenuText("IDS_PORT_ROOM_TITLE", L"Room");
    SetMenuText(ValueId(kRoomName).c_str(), Wide(Shorten(s.room.name[0] ? std::string(s.room.name) : DefaultRoomName(), 22)));
    SetMenuText(ValueId(kTime).c_str(), s.room.time ? Wide(std::to_string(s.room.time) + " SECONDS").c_str() : L"INFINITE");
    SetMenuText("IDS_PORT_LEGEND_READY", s.localReady ? L"Not Ready" : L"Ready");
    // Public Rooms
    std::string lobbyStatus;
    net::LobbyRooms(g_PubRooms, lobbyStatus);
    const size_t shown = std::min<size_t>(g_PubRooms.size(), kPubLast - kPub0 + 1);
    SetMenuText(ValueId(kPubStatus).c_str(), Wide(!lobbyStatus.empty() ? lobbyStatus :
        g_PubRooms.empty() ? std::string("NO PUBLIC ROOM") : std::to_string(g_PubRooms.size()) +
        (g_PubRooms.size() == 1 ? " ROOM" : " ROOMS")));
    for (int row = kPub0; row <= kPubLast; ++row) {
        const size_t i = size_t(row - kPub0);
        if (i < shown) {
            const net::PublicRoom& r = g_PubRooms[i];
            SetMenuText(kRowDefs[row].text, Wide(Shorten(r.name, 17)));
            SetMenuText(ValueId(row).c_str(), Wide(std::to_string(r.players) + "/" + std::to_string(r.slots) + "  " +
                Shorten(r.mode, 8) + "  " + Shorten(r.arena, 18)));
        } else {
            SetMenuText(kRowDefs[row].text, L"");
            SetMenuText(ValueId(row).c_str(), L"");
        }
    }
    const std::string me = Shorten(s.localName.empty() ? net::OnlinePlayerName() : s.localName, 15);
    const std::string other = Shorten(s.remoteName, 15);
    // the players' table under the menu: each player's Ready (the game's box, left of the name: PlayerRowExtras) and
    // name, the other player's connection (its bars, then its time)
    PlayerRow& r1 = g_PlayerRows[0];
    PlayerRow& r2 = g_PlayerRows[1];
    r1 = r2 = PlayerRow();
    if (s.host) {
        r1 = { true, s.localReady, -1 };
        r2 = { s.guestHere, s.remoteReady, s.guestHere ? s.pingMs : -1 };
    } else {
        r1 = { !other.empty(), s.remoteReady, other.empty() ? -1 : s.pingMs };
        r2 = { true, s.localReady, -1 };
    }
    const auto name = [](const PlayerRow& r, const std::string& n) { return r.present ? kBoxRoom + n : n; };
    const auto value = [](const PlayerRow& r) {
        return r.pingMs >= 0 ? kBarsRoom + std::to_string(r.pingMs) + " MS" : std::string();
    };
    const std::string n1 = name(r1, s.host ? me : other.empty() ? "..." : other), v1 = value(r1);
    const std::string n2 = name(r2, s.host ? (s.guestHere ? other : "WAITING...") : me), v2 = value(r2);
    SetMenuText(kRowDefs[kPlayer1].text, Wide(n1));
    SetMenuText(ValueId(kPlayer1).c_str(), Wide(v1));
    SetMenuText(kRowDefs[kPlayer2].text, Wide(n2));
    SetMenuText(ValueId(kPlayer2).c_str(), Wide(v2));
    SetMenuText(kRowDefs[kGap].text, L"");
    SetMenuText(ValueId(kGap).c_str(), L"");
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

// A row shown in the room's game mode: Rounds to Win, Time Limit and Auto Rematch are Versus's alone.
bool VersusRow(int row)
{
    return (row != kRounds && row != kTime && row != kRematch) || net::OnlineStatus().room.mode == 0;
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
    case kMode: case kRoomName: case kArena: case kPublic: return !s.host || idle; // the host's alone
    case kRounds: case kTime: case kRematch: return !s.host || idle || !VersusRow(row);
    case kRoomSettings: return idle;
    case kPlayer1: case kPlayer2: case kGap: return true;
    case kCode: return !(s.host && !idle && !s.roomCode.empty());
    case kLeave: return false;
    case kPubStatus: return true;
    default:
        if (row >= kPub0 && row <= kPubLast)
            return size_t(row - kPub0) >= g_PubRooms.size();
        return false;
    }
}

// Rows shown dimmed: the disabled ones, but not the rows that only tell.
bool RowDimmed(int row)
{
    if (row == kMode || row == kRoomName || row == kRounds || row == kTime || row == kArena || row == kRematch ||
        row == kPublic) { // the guest sees the host's settings, not dimmed
        const net::OnlineStatusInfo s = net::OnlineStatus();
        if (!s.host && !Idle(s))
            return false;
    }
    return RowDisabled(row) && row != kStatus && row != kPlayer1 && row != kPlayer2 && row != kGap && row != kMode &&
        row != kPubStatus;
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
uint32_t g_ValueVtable[kVtableSlots], g_ButtonVtable[kVtableSlots], g_KeyboardVtable[kVtableSlots];
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

// A row that cannot be chosen is never drawn as the cursor's (the screen's first focus, a list without rooms): its
// handler's focus mark (+0x18: 1 from its slot 5 on focus, 0 from slot 7) cleared for its draw.
template <typename Draw>
char DrawUnfocused(uint8_t* handler, bool unfocused, Draw draw)
{
    if (!unfocused || !handler[0x18])
        return draw();
    handler[0x18] = 0;
    const char result = draw();
    handler[0x18] = 1;
    return result;
}

// A row's label as its text is now: an item takes its label once, as its screen comes ([item+0x44]: the length, then
// the characters); the rows whose label changes (the players' names, the public rooms') are drawn with the port's
// text buffer in its place, their own given back after.
template <typename Draw>
char DrawLabelled(uint8_t* handler, int row, Draw draw)
{
    uint8_t* item = *reinterpret_cast<uint8_t**>(handler + 8);
    const wchar_t* text = MenuTextBuffer(kRowDefs[row].text);
    if (!item || !text)
        return draw();
    const void* own = *reinterpret_cast<const void**>(item + 0x44);
    *reinterpret_cast<const void**>(item + 0x44) = text;
    const char result = draw();
    *reinterpret_cast<const void**>(item + 0x44) = own;
    return result;
}

// The room's title as its name: the screen's "title" item pointed at the port's text (its buffers are never freed).
void TitleAsRoomName(uint8_t* handler)
{
    __try {
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(handler + 8);
        uint8_t* items = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
        uint8_t* title = items ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(
            uintptr_t(kFindItem))(items, nullptr, "title") : nullptr;
        const wchar_t* text = MenuTextBuffer("IDS_PORT_ROOM_TITLE");
        if (title && text)
            *reinterpret_cast<const void**>(title + 0x44) = text;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Room Settings in another mode than Versus: its Versus rows (the last three) and their veils out of sight, as if the
// screen had three rows fewer: each item put below the screen (+0xE6: y in 1/32 pixels; its children, the rows' bars,
// placed from it) and see-through (+0x97: its colour's alpha), both given back for Versus.
struct HiddenItem {
    uint8_t* item = nullptr;
    int16_t y = 0;
    bool hidden = false; // put away by the port (its place and alpha its own again once shown)
};
HiddenItem g_HiddenItems[6];

void ShowItem(uint8_t* handler, int slot, const char* name, bool shown)
{
    __try {
        const uint8_t* own = *reinterpret_cast<uint8_t* const*>(handler + 8);
        uint8_t* items = own ? *reinterpret_cast<uint8_t* const*>(own + 0x24) : nullptr;
        uint8_t* item = items ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(
            uintptr_t(kFindItem))(items, nullptr, name) : nullptr;
        if (!item)
            return;
        HiddenItem& h = g_HiddenItems[slot];
        if (h.item != item) { // this screen's item (the screen made again): as the game has it
            h.item = item;
            h.hidden = false;
        }
        if (!shown && !h.hidden) {
            h.y = *reinterpret_cast<int16_t*>(item + 0xE6);
            h.hidden = true;
        }
        if (!shown) {
            *reinterpret_cast<int16_t*>(item + 0xE6) = int16_t(h.y + 1000 * 32 > 32767 ? 32767 : h.y + 1000 * 32);
            item[0x97] = 0;
        } else if (h.hidden) { // back as it was (the menus' items are opaque once their screen is in)
            *reinterpret_cast<int16_t*>(item + 0xE6) = h.y;
            item[0x97] = 0xFF;
            h.hidden = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void PlaceVersusRows(uint8_t* handler)
{
    const bool versus = net::OnlineStatus().room.mode == 0;
    static const char* const kItems[6] = { "roomrounds", "roomtime", "roomrematch", "mvolumeveil", "evolumeveil",
        "dvolumeveil" };
    for (int i = 0; i < 6; ++i)
        ShowItem(handler, i, kItems[i], versus);
}

// --- The room's pictures --------------------------------------------------------------------------------------------
// The game's own: the pause screen's objective boxes (interfc\front_end\s_pause_obj_off, _on: a hollow box, a filled
// one; in the levels' PAKs, not the menus') for the players' Ready, and the menus' row stroke (s_main_bar_off, a
// glowing blue bar) for the connection's bars. Each is copied from the disc's PAKs to cache\online and loaded through
// the game's texture interface from there (a disc path of the port's own, interfc\port_online\<name>, as the
// prompts' key caps), then drawn with the interface's quad (slot 5: as a ButtonIcon draws its icon, 0x2C4DB0: the
// corners in the menus' 640 x 480, the picture, its colour as four floats (alpha first), then 0, 1, 0, 0, 1.0, 1.0, 0).
enum Picture { kBoxOff, kBoxOn, kBar, kPictureCount };
constexpr const char* kPictureNames[kPictureCount] = { "s_pause_obj_off", "s_pause_obj_on", "s_main_bar_off" };
void* g_Pictures[kPictureCount] = {};
bool g_PicturesTried[kPictureCount] = {};
constexpr uint32_t kMenuGraphics = 0x0068D9A0; // [+0x4C]: the texture interface the menus draw with (vtable 0x561A30)
constexpr int kQuadSlot = 5;

void* RoomPicture(Picture which)
{
    if (g_PicturesTried[which])
        return g_Pictures[which];
    g_PicturesTried[which] = true;
    const std::string name = kPictureNames[which];
    std::vector<uint8_t> stx;
    if (!ReadDiscResource("interfc\\front_end\\" + name + ".stx", stx) || stx.size() < 0x40 ||
        std::memcmp(stx.data(), "STX", 3) != 0) {
        LOG_WARN("Online menu: the picture %s is not on the disc", name.c_str());
        return nullptr;
    }
    const std::wstring folder = kernel::CachePath() + L"\\online";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring file = folder + L"\\" + std::wstring(name.begin(), name.end()) + L".stx";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARN("Online menu: %ls could not be written", file.c_str());
        return nullptr;
    }
    DWORD written = 0;
    WriteFile(h, stx.data(), DWORD(stx.size()), &written, nullptr);
    CloseHandle(h);
    if (written != stx.size())
        return nullptr;
    kernel::SetDiscFileOverride("\\interfc\\port_online\\" + name + ".stx", file);
    g_Pictures[which] = LoadPictureFile(("interfc\\port_online\\" + name).c_str());
    LOG_INFO("Online menu: the picture %s %s", name.c_str(), g_Pictures[which] ? "loaded" : "did not load");
    return g_Pictures[which];
}

// A picture over the corners (x0, y0)-(x1, y1) of the menus' 640 x 480, in white times `light` (1: as it is), `alpha`.
void DrawPicture(Picture which, float x0, float y0, float x1, float y1, float light, float alpha)
{
    void* picture = RoomPicture(which);
    if (!picture)
        return;
    __try {
        uint8_t* graphics = *reinterpret_cast<uint8_t**>(uintptr_t(kMenuGraphics));
        void* api = graphics ? *reinterpret_cast<void**>(graphics + 0x4C) : nullptr;
        if (!api)
            return;
        using QuadFn = uint32_t(__fastcall*)(void*, void*, const float*, const float*, void*, float, float, float, float,
            uint32_t, uint32_t, uint32_t, uint32_t, float, float, uint32_t);
        const auto quad = reinterpret_cast<QuadFn>((*reinterpret_cast<void* const* const*>(api))[kQuadSlot]);
        const float topLeft[2] = { x0, y0 }, bottomRight[2] = { x1, y1 };
        quad(api, nullptr, topLeft, bottomRight, picture, alpha, light, light, light, 0, 1, 0, 0, 1.0f, 1.0f, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// An item's place in the menus' 640 x 480 (+0xE4, +0xE6: x and y in 1/32 pixels).
bool ItemPlace(uint8_t* handler, float& x, float& y)
{
    __try {
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(handler + 8);
        if (!item)
            return false;
        x = float(*reinterpret_cast<const int16_t*>(item + 0xE4)) / 32.0f;
        y = float(*reinterpret_cast<const int16_t*>(item + 0xE6)) / 32.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Over a player's row of the room (its text made room for them: kBoxRoom, kBarsRoom): their Ready as the game's box
// left of the name (filled when ready), and for the other player the connection as five bars before its time, those
// it does not reach greyed.
constexpr float kValueColumn = 312.0f; // the rows' values, from the screen's left (the Settings screen's)
void PlayerRowExtras(uint8_t* handler, int row)
{
    const PlayerRow& r = g_PlayerRows[row == kPlayer1 ? 0 : 1];
    float x, y;
    if (!r.present || !ItemPlace(handler, x, y))
        return;
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        LOG_INFO("Online menu: the players' row %d at %.1f, %.1f", row == kPlayer1 ? 1 : 2, x, y);
    }
    const float box = 22.0f, middle = y + 11.0f;
    DrawPicture(r.ready ? kBoxOn : kBoxOff, x - 3.0f, middle - box / 2, x - 3.0f + box, middle + box / 2, 1.0f, 1.0f);
    const int lit = QualityBars(r.pingMs);
    if (r.pingMs < 0)
        return;
    // the picture's stroke is its middle sixth across, most of its height: each bar's quad a little wider than its gap
    const float bottom = y + 19.0f;
    for (int i = 0; i < kQualityBars; ++i) {
        const float h = 7.0f + 2.5f * float(i), left = kValueColumn + 5.0f * float(i) - 6.0f;
        const bool on = i < lit;
        DrawPicture(kBar, left, bottom - h * 1.25f, left + 16.0f, bottom + h * 0.08f, on ? 1.0f : 0.35f, on ? 1.0f : 0.55f);
    }
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
    } else if ((screen == kRoom || screen == kRoomSettingsScreen) && !inRoom) { // the room is over: the main menu (the game's popup when the other left)
        g_LastRedirect = GetTickCount64();
        if (s.phase == net::OnlinePhase::Ended && s.lost && !s.matchOn)
            OnlineShowLost(s.message);
        else
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

// The HUD's opacity: its vitals' fade (the in-level HUD's Vitals handlers, vtable 0x5A80A0; their slot 2, 0x25CD10,
// steps a fade each frame: its time +0x10 times its rate +0xC, at most 1, as the alpha of their drawing's colour,
// [handler+4] +0xB), read as the game sets it.
constexpr uint32_t kVitalsFadeSlot = 0x005A80A8, kVitalsFade = 0x0025CD10;
using VitalsFadeFn = char(__fastcall*)(uint8_t*, void*, float);
float g_HudAlpha = -1.0f;
ULONGLONG g_HudAlphaAt = 0;

char __fastcall VitalsFade(uint8_t* handler, void*, float seconds)
{
    const char result = reinterpret_cast<VitalsFadeFn>(uintptr_t(kVitalsFade))(handler, nullptr, seconds);
    __try {
        const uint8_t* draw = *reinterpret_cast<uint8_t* const*>(handler + 4);
        if (draw) {
            g_HudAlpha = float(draw[0xB]) / 255.0f;
            g_HudAlphaAt = GetTickCount64();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

float HudItemAlpha()
{
    return g_HudAlpha >= 0 && GetTickCount64() - g_HudAlphaAt < 500 ? g_HudAlpha : -1.0f;
}

// A notice's owner (the popup gives its owner's item the focus back as it closes, then calls the owner's slot 8 for A,
// slot 9 for B): a stand-in for the row that showed it, its item that row's, its slots doing nothing but noting the
// popup closed (the rows' own would act: the main menu's slot 8 starts a new game, a settings control's saves). While
// the notice is up, the port's rows give it the events they get (NoticeTakes), so that it closes whatever has the focus.
struct NoticeOwner {
    const uint32_t* vtable;
    uint32_t unused;
    uint8_t* item;
    uint8_t rest[0x240];
};
uint32_t g_NoticeOwnerVtable[kVtableSlots];
NoticeOwner g_NoticeOwner = {};
uint8_t* g_OpenNotice = nullptr;   // the notice up, or null
uint32_t g_OpenNoticeScreen = 0;   // on that screen

void __fastcall NoticeClosed(uint8_t*, void*)
{
    g_OpenNotice = nullptr;
    LOG_INFO("Online menu: the notice closed");
}

uint8_t* NoticeOwnerFor(uint8_t* owner)
{
    if (!g_NoticeOwnerVtable[0]) {
        std::memcpy(g_NoticeOwnerVtable, g_ValueVtable, sizeof g_NoticeOwnerVtable);
        g_NoticeOwnerVtable[8] = g_NoticeOwnerVtable[9] = uint32_t(reinterpret_cast<uintptr_t>(&NoticeClosed));
    }
    g_NoticeOwner.vtable = g_NoticeOwnerVtable;
    g_NoticeOwner.item = owner ? *reinterpret_cast<uint8_t**>(owner + 8) : nullptr;
    return reinterpret_cast<uint8_t*>(&g_NoticeOwner);
}

// A row's event while a notice is up: the notice's (true: taken).
bool NoticeTakes(int event, int value)
{
    if (!g_OpenNotice)
        return false;
    if (LastShellScreen() != g_OpenNoticeScreen) { // its screen is gone: so is it
        g_OpenNotice = nullptr;
        return false;
    }
    if (event == kChosen || event == kB) {
        __try {
            reinterpret_cast<ItemEventFn>((*reinterpret_cast<void* const* const*>(g_OpenNotice))[4])(g_OpenNotice,
                nullptr, event, value);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_OpenNotice = nullptr;
        }
    }
    return true;
}

bool ShowPopup(uint8_t* owner, const char* titleId, const char* messageId, const char* name = "DefaultPopup")
{
    // a keyboard (the code screen's) owns its popup as the game's Codes keyboard does; any other owner a stand-in
    const bool keyboard = owner && *reinterpret_cast<uint32_t**>(owner) == g_KeyboardVtable;
    __try {
        uint8_t* shell = *reinterpret_cast<uint8_t**>(uintptr_t(kShellObject));
        if (!shell)
            return false;
        uint8_t* popup = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*, void*, int)>(
            uintptr_t(kFindPopup))(shell, nullptr, name, nullptr, 0);
        if (!popup)
            return false;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kPopupTitle))(popup, nullptr,
            GameText(titleId));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kPopupMessage))(popup, nullptr,
            GameText(messageId));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(
            (*reinterpret_cast<void* const* const*>(popup))[0x58 / 4])(popup, nullptr,
            keyboard ? owner : NoticeOwnerFor(owner));
        if (!keyboard) {
            g_OpenNotice = popup;
            g_OpenNoticeScreen = LastShellScreen();
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Online menu: the popup could not be shown");
        return false;
    }
}

// A note of the menu's own (a code refused, a room that could not be made) in the game's popup.
bool ShowNotice(uint8_t* owner, const std::string& text, const char* name = "DefaultPopup")
{
    SetMenuText("IDS_PORT_NOTICE_TEXT", Wide(text));
    return ShowPopup(owner, "IDS_PORT_NOTICE_TITLE", "IDS_PORT_NOTICE_TEXT", name);
}

// The game's Yes/No popup (the screen's YesNoPopup item): shown with its slot 27 (+0x6C: (owner, 0)); once closed it
// calls its owner's slot 8 (+0x20) for Yes, slot 9 (+0x24) for No (0x2E6C80).
bool ShowYesNo(uint8_t* owner, const char* titleId, const char* messageId, bool yesFirst = false)
{
    __try {
        uint8_t* shell = *reinterpret_cast<uint8_t**>(uintptr_t(kShellObject));
        uint8_t* popup = shell ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*, void*, int)>(
            uintptr_t(kFindPopup))(shell, nullptr, "YesNoPopup", nullptr, 0) : nullptr;
        if (!popup)
            return false;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*, int)>(
            (*reinterpret_cast<void* const* const*>(popup))[0x6C / 4])(popup, nullptr, owner, 0);
        (void)yesFirst; // the game's popup opens on No (its own choice)
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kPopupTitle))(popup, nullptr,
            GameText(titleId));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kPopupMessage))(popup, nullptr,
            GameText(messageId));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN("Online menu: the Yes/No popup could not be shown");
        return false;
    }
}

// The game's popup over the room while the match is being set up (both games then restart for it, the last picture
// held on screen meanwhile: the HUD): the guest's "Joining" once its join is under way, the host's "<guest> is
// joining" once its guest is in.
enum class KeyboardFor { Code, Name, RoomName };
bool GoToVersusSelect();
void OpenKeyboard(KeyboardFor what);

bool g_JoinNotice = false;      // the guest: a join just asked (its popup where it was asked: Public Rooms, the code)
bool g_JoinWatch = false;       // and its end watched there (a join that fails: the game's popup says why)
bool g_GuestNoticeShown = false; // the host: its popup shown for this guest
void ShowRoomNotice(uint8_t* handler, const char* popupName = "DefaultPopup")
{
    const net::OnlineStatusInfo s = net::OnlineStatus();
    if (g_JoinWatch && (s.matchOn || s.host)) {
        g_JoinWatch = false;
    } else if (g_JoinWatch && s.phase == net::OnlinePhase::Ended) {
        g_JoinWatch = g_JoinNotice = false;
        LOG_INFO("Online menu: the join failed: %s", s.message.c_str());
        ShowNotice(handler, Shorten(s.message.empty() ? std::string("The room could not be joined.") : s.message, 60),
            popupName);
        return;
    }
    const bool settingUp = s.phase == net::OnlinePhase::Connecting || s.phase == net::OnlinePhase::Preparing ||
        s.phase == net::OnlinePhase::Starting;
    if (!s.host && g_JoinNotice && settingUp) {
        g_JoinNotice = false;
        const std::string room = !s.room.name[0] ? g_PendingJoinName : std::string(s.room.name);
        SetMenuText("IDS_PORT_JOINING_TEXT", Wide(room.empty() ? std::string("Joining the room...") :
            "Joining " + Shorten(room, 24) + "..."));
        ShowPopup(handler, "IDS_PORT_JOINING_TITLE", "IDS_PORT_JOINING_TEXT", popupName);
    }
    if (!settingUp && s.phase != net::OnlinePhase::Playing)
        g_JoinNotice = g_JoinNotice && s.phase == net::OnlinePhase::Off; // a join that ended: nothing to say
    if (s.host && s.guestHere && !g_GuestNoticeShown &&
        (s.phase == net::OnlinePhase::Preparing || s.phase == net::OnlinePhase::Starting)) {
        g_GuestNoticeShown = true;
        SetMenuText("IDS_PORT_JOINING_TEXT", Wide(Shorten(s.remoteName.empty() ? std::string("A player") : s.remoteName,
            16) + " is joining your room..."));
        ShowPopup(handler, "IDS_PORT_GUEST_TITLE", "IDS_PORT_JOINING_TEXT");
    }
    if (s.host && (s.phase == net::OnlinePhase::Waiting || Idle(s)))
        g_GuestNoticeShown = false;
}

// The room's footer: Ready's prompt (X: the room's screen has it after Select, ReadyPrompt) says what X does now
// (Ready, or Not Ready once ready): its text item pointed at the port's text, as the title. X itself is read from
// the pad as the room is drawn (a press, not a hold): Ready or not, once both games are in the room.
constexpr int kReadySound = 0x0B, kRefusedSound = 0x10, kSoundSlotAll = 11; // the shell's sounds (the handlers' slot 11)
ULONGLONG g_ReadySeenAt = 0;
bool g_ReadyHeld = true;

// An item of the handler's screen (by name) given the port's text as its label.
void LabelAsText(uint8_t* handler, const char* itemName, const char* textId)
{
    __try {
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(handler + 8);
        uint8_t* items = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
        uint8_t* text = items ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(
            uintptr_t(kFindItem))(items, nullptr, itemName) : nullptr;
        const wchar_t* label = MenuTextBuffer(textId);
        if (text && label)
            *reinterpret_cast<const void**>(text + 0x44) = label;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void HandlerSound(uint8_t* handler, int id)
{
    __try {
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(
            (*reinterpret_cast<void* const* const*>(handler))[kSoundSlotAll])(handler, nullptr, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ReadyLegend(uint8_t* handler)
{
    LabelAsText(handler, "readytext", "IDS_PORT_LEGEND_READY");
    input::GamePad pad = {};
    input::ReadLivePad(0, pad);
    const bool down = pad.analog[2] > 0x40 && !g_OpenNotice; // X (not under a notice)
    const ULONGLONG now = GetTickCount64();
    if (now - g_ReadySeenAt > 300) // the room just came: a press already held is not one
        g_ReadyHeld = true;
    g_ReadySeenAt = now;
    if (down && !g_ReadyHeld) {
        const net::OnlineStatusInfo s = net::OnlineStatus();
        const bool live = s.matchOn && s.phase == net::OnlinePhase::Playing;
        if (live) {
            net::SetOnlineReady(!s.localReady);
            UpdateTexts();
        }
        LOG_INFO("Online menu: X: %s", live ? (s.localReady ? "not ready" : "ready") : "not in a match's room yet");
        HandlerSound(handler, live ? kReadySound : kRefusedSound);
    }
    g_ReadyHeld = down;
}

// A value row shows its text as the control's On text (the control shows On for the profile's subtitles setting,
// +0x2A, which it is given for the draw).
char __fastcall ValueDraw(uint8_t* handler, void*, uint32_t value)
{
    const int row = RowOf(handler);
    if (row < 0 || kRowDefs[row].button)
        return g_OnOffDraw(handler, nullptr, value);
    if (row == kPubStatus)
        net::LobbyWantList(); // the list kept fresh while Public Rooms is up
    if (row == kStatus || row == kName || row == kMode || row == kPubStatus || row == kRoomName) // once a frame on each screen
        UpdateTexts();
    if (row == kStatus || row == kMode || row == kRoomName)
        KeepOnRightScreen(row);
    if ((row == kMode || row == kRoomName) && net::OnlineTakeStart()) { // both players ready: the host's game starts
        if (net::OnlineStatus().room.mode == 0) {
            LOG_INFO("Online menu: both players ready: Versus");
            GoToVersusSelect();
        } else {
            LOG_INFO("Online menu: both players ready: Cooperative (not playable online yet)");
            ShowNotice(handler, "Cooperative is not playable online yet.");
        }
    }
    if (row == kPubStatus)
        ShowRoomNotice(handler);
    if (row == kMode && net::OnlineTakeDrawEnd())
        ShowNotice(handler, "Three draws: the match is over.");
    if (row == kMode) {
        ShowRoomNotice(handler);
        TitleAsRoomName(handler);
        ReadyLegend(handler);
    }
    if (row == kRoomName)
        PlaceVersusRows(handler);
    if (!VersusRow(row)) // not in this game mode: nothing of it drawn
        return 1;
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
    if (row == kGap) // the blank row before the players: nothing drawn
        return 1;
    const std::string text = ValueId(row);
    SetOnOffTexts(text.c_str(), text.c_str());
    const char result = DrawUnfocused(handler, RowDisabled(row), [&] {
        return DrawDimmed(handler, RowDimmed(row), [&] {
            return DrawLabelled(handler, row, [&] { return g_OnOffDraw(handler, nullptr, value); });
        });
    });
    SetOnOffTexts(nullptr, nullptr);
    if (profile)
        profile[0x2A] = saved;
    if (row == kPlayer1 || row == kPlayer2)
        PlayerRowExtras(handler, row);
    return result;
}



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
    if (row >= 0 && (kRowDefs[row].screen == kSettingsScreen || kRowDefs[row].screen == kPublicScreen)) {
        GoToShellScreen(kScreenNames[kOnline]);
        return 1;
    }
    if (row >= 0 && kRowDefs[row].screen == kRoomSettingsScreen) {
        GoToShellScreen(kScreenNames[Idle(net::OnlineStatus()) ? kOnline : kRoom]);
        return 1;
    }
    return reinterpret_cast<ItemEventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
}

char __fastcall ValueEvent(uint8_t* handler, void*, int event, int value)
{
    if (NoticeTakes(event, value))
        return 1;
    if (event == kB)
        return Back(handler, event, value);
    if (event != kLeft && event != kRight && event != kChosen)
        return g_OnOffEvent(handler, nullptr, event, value);
    const int row = RowOf(handler);
    if (row < 0 || RowDisabled(row)) {
        static int refused = 0;
        if (row >= 0 && refused < 20 && kRowDefs[row].screen == kRoomSettingsScreen) {
            ++refused;
            LOG_INFO("Online menu: %s refused (%s)", kRowDefs[row].name,
                net::OnlineStatus().host ? "not now" : "the host's setting");
        }
        return 1;
    }
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
        OpenKeyboard(KeyboardFor::Name);
        return 1;
    }
    if (row == kRoomName && event == kChosen) {
        OpenKeyboard(KeyboardFor::RoomName);
        return 1;
    }
    if (row >= kPub0 && row <= kPubLast) { // a public room: join it (the game's Yes/No first)
        if (event != kChosen || size_t(row - kPub0) >= g_PubRooms.size())
            return 1;
        const net::PublicRoom r = g_PubRooms[size_t(row - kPub0)];
        if (r.players >= r.slots) {
            ShowNotice(handler, "This room is full.");
            return 1;
        }
        g_PendingJoin = r.code;
        g_PendingJoinName = r.name;
        SetMenuText("IDS_PORT_JOIN_TEXT", Wide("Join " + Shorten(r.name, 24) + "?"));
        if (!ShowYesNo(handler, "IDS_PORT_JOIN_TITLE", "IDS_PORT_JOIN_TEXT", true))
            g_PendingJoin.clear();
        return 1;
    }
    net::OnlineRoomSettings room = s.room;
    const int step = event == kLeft ? -1 : 1;
    if (row == kMode) {
        room.mode = room.mode ? 0 : 1; // Versus, Cooperative
    } else if (row == kRounds) {
        int at = 0;
        for (int i = 0; i < int(std::size(kRoundChoices)); ++i)
            if (kRoundChoices[i] == room.rounds)
                at = i;
        at = (at + step + int(std::size(kRoundChoices))) % int(std::size(kRoundChoices));
        room.rounds = kRoundChoices[at];
    } else if (row == kTime) {
        int at = 0;
        for (int i = 0; i < int(std::size(kTimeChoices)); ++i)
            if (kTimeChoices[i] == room.time)
                at = i;
        at = (at + step + int(std::size(kTimeChoices))) % int(std::size(kTimeChoices));
        room.time = kTimeChoices[at];
    } else if (row == kArena) {
        room.arena = uint8_t((room.arena + step + 4) % 4); // the host, random, the loser, the winner
    } else if (row == kRematch) {
        room.rematch = room.rematch ? 0 : 1;
    } else if (row == kPublic) {
        room.open = room.open ? 0 : 1; // listed in Public Rooms; the choice kept for the next rooms
        WritePrivateProfileStringW(L"Online", L"PublicRoom", room.open ? L"1" : L"0", SettingsIni().c_str());
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
    if (NoticeTakes(event, value))
        return 1;
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
        OpenKeyboard(KeyboardFor::Code);
        return 1;
    case kSettings:
        GoToShellScreen(kScreenNames[kSettingsScreen]);
        return 1;
    case kRoomSettings:
        GoToShellScreen(kScreenNames[kRoomSettingsScreen]);
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
    return DrawUnfocused(handler, RowDisabled(row), [&] {
        return DrawDimmed(handler, RowDimmed(row), [&] { return g_ButtonDraw(handler, nullptr, value); });
    });
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
bool g_PasteDown = false;
KeyboardFor g_KeyboardFor = KeyboardFor::Code; // what the keyboard types: a room code, the player's name, the room's
bool g_KeyboardForName = false;                 // a name (the player's or the room's), not a room code

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
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == ' ' && g_KeyboardForName) ||
            (c == '\'' && g_KeyboardFor == KeyboardFor::RoomName))
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
    if (g_KeyboardFor == KeyboardFor::Name) {
        const std::string name = net::OnlinePlayerName();
        SetKeyboardText(keyboard, name == "NONAME" ? std::string() : name);
    } else if (g_KeyboardFor == KeyboardFor::RoomName) {
        const net::OnlineStatusInfo s = net::OnlineStatus();
        SetKeyboardText(keyboard, s.room.name[0] ? std::string(s.room.name) : DefaultRoomName());
    }
}

// The keys of a room code's letters read as digits are disabled, as the game disables Back, Space and Done on an
// empty text (their handler's +0x20): each key is an item of the keyboard's screen named by its letter, found as the
// keyboard finds those three (kFindItem: the screen's items [item+0x24], name), its handler at +0x80. A letter key's
// draw (0x2D0F90) has no disabled look of its own (the wide buttons' 0x2D1280 has): its text, drawn in its item's
// colour (+0x94, RGBA), is dimmed as theirs is, and its own colour given back for a name.
std::vector<std::pair<uint8_t*, uint32_t>> g_KeyColours; // the dimmed keys' items and their own colours
ULONGLONG g_CodeKeyboardAt = 0;                          // the code keyboard last drawn (the paste legend: the HUD)
bool g_PastePrompt = false; // the code screen has the game's own Paste prompt (else the HUD draws one)

bool KeyColourRaw(uint8_t* key, uint32_t& colour, bool write)
{
    __try {
        uint32_t& field = *reinterpret_cast<uint32_t*>(key + 0x94);
        if (write)
            field = colour;
        else
            colour = field;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DimKey(uint8_t* key, bool dim)
{
    auto it = std::find_if(g_KeyColours.begin(), g_KeyColours.end(), [&](const auto& e) { return e.first == key; });
    if (!dim) {
        if (it != g_KeyColours.end())
            KeyColourRaw(key, it->second, true);
        return;
    }
    if (it == g_KeyColours.end()) {
        uint32_t own = 0;
        if (!KeyColourRaw(key, own, false))
            return;
        g_KeyColours.push_back({ key, own });
        it = g_KeyColours.end() - 1;
    }
    const uint32_t own = it->second;
    auto part = [&](int shift, float k) { return uint32_t(float((own >> shift) & 0xFF) * k) << shift; };
    uint32_t dimmed = part(0, 0.45f) | part(8, 0.45f) | part(16, 0.45f) | part(24, 0.55f);
    KeyColourRaw(key, dimmed, true);
}

int FindRefusedKeysRaw(uint8_t* keyboard, uint8_t* (&found)[4])
{
    int count = 0;
    __try {
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(keyboard + 8);
        uint8_t* items = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
        if (!items)
            return 0;
        for (const char letter : { 'S', 'D', 'O', 'B' }) {
            const char name[2] = { letter, 0 };
            uint8_t* key = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(uintptr_t(kFindItem))(
                items, nullptr, name);
            uint8_t* handler = key ? *reinterpret_cast<uint8_t**>(key + 0x80) : nullptr;
            if (handler)
                handler[0x20] = g_KeyboardForName ? 0 : 1;
            if (key)
                found[count++] = key;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

void DisableRefusedKeys(uint8_t* keyboard)
{
    uint8_t* keys[4] = {};
    const int n = FindRefusedKeysRaw(keyboard, keys);
    for (int i = 0; i < n; ++i)
        DimKey(keys[i], !g_KeyboardForName);
}

// Ctrl+V: the clipboard's code (its letters and digits) as the text.
void __fastcall KeyboardDraw(uint8_t* keyboard, void*, uint32_t value)
{
    const bool down = !g_KeyboardForName && GameWindowActive() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState('V') & 0x8000);
    if (down && !g_PasteDown)
        Paste(keyboard);
    g_PasteDown = down;
    if (!g_KeyboardForName) {
        g_CodeKeyboardAt = GetTickCount64();
        ShowRoomNotice(keyboard, "popup"); // a join asked here: its popup (the code screen's own)
    }
    DisableRefusedKeys(keyboard);
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
    if (event == kB) { // back: to the main menu (a code) or Online Settings (a name), not to the Codes screen's own
        const uint8_t* item = *reinterpret_cast<uint8_t**>(key + 8);
        const uint8_t* parent = item ? *reinterpret_cast<uint8_t* const*>(item + 0x28) : nullptr;
        uint8_t* keyboard = parent ? *reinterpret_cast<uint8_t* const*>(parent + 0x80) : nullptr;
        if (keyboard && *reinterpret_cast<uint32_t**>(keyboard) == g_KeyboardVtable) {
            GoToShellScreen(kScreenNames[g_KeyboardFor == KeyboardFor::Name ? kSettingsScreen :
                g_KeyboardFor == KeyboardFor::RoomName ? kRoomSettingsScreen : kOnline]);
            return 1;
        }
    }
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


// Done: joins with the code typed (back to the online screen), or refuses it.
void __fastcall KeyboardDone(uint8_t* keyboard, void*)
{
    if (g_KeyboardFor == KeyboardFor::Name) { // Online Settings: the name
        net::SetOnlinePlayerName(KeyboardText(keyboard));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kKeyboardClear))(keyboard, nullptr);
        UpdateTexts();
        GoToShellScreen(kScreenNames[kSettingsScreen]);
        return;
    }
    if (g_KeyboardFor == KeyboardFor::RoomName) { // Room Settings: the room's name (never empty)
        SetRoomName(KeyboardText(keyboard));
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kKeyboardClear))(keyboard, nullptr);
        UpdateTexts();
        GoToShellScreen(kScreenNames[Idle(net::OnlineStatus()) ? kOnline : kRoomSettingsScreen]);
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
        g_Note = Shorten(net::OnlineStatus().message, 60);
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kKeyboardClear))(keyboard, nullptr);
    UpdateTexts();
    if (g_Note.empty()) { // the game's popup here while joining (both games then restart, to the room)
        g_PendingJoinName.clear();
        g_JoinNotice = g_JoinWatch = true;
    } else {
        ShowNotice(keyboard, g_Note, "popup"); // the code screen's own popup (its item "popup")
    }
}

// The keyboard set for a room code, the player's name or the room's (its screen opened by the caller).
void PrepareKeyboard(KeyboardFor what)
{
    g_KeyboardFor = what;
    g_KeyboardForName = what != KeyboardFor::Code;
    SetMenuText("IDS_PORT_ONLINE_ENTER_CODE", what == KeyboardFor::Name ? L"Enter your Name" :
        what == KeyboardFor::RoomName ? L"Enter the Room Name" : L"Enter the Room Code");
}

void OpenKeyboard(KeyboardFor what)
{
    PrepareKeyboard(what);
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
// A screen's top-level item count raised by `added`: the word just before its first item ("screenItem", its tag).
bool AddScreenItems(std::vector<uint8_t>& data, uint32_t added)
{
    const size_t first = MenuFind(data, MenuString("screenItem"), 0, data.size());
    if (first == std::string::npos || first < 4)
        return false;
    uint32_t items;
    std::memcpy(&items, data.data() + first - 4, 4);
    LOG_INFO("Online menu: the screen's items: %u at %zu, now %u", items, first - 4, items + added);
    items += added;
    std::memcpy(data.data() + first - 4, &items, 4);
    return true;
}

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
    // the footer's prompts spread for a fourth (Scroll, Select, Paste, Back): Paste the game's own (below) or, without
    // it, the HUD's in its place (OnlinePasteSlot)
    const struct { const char* name; float x; } spread[] = { { "selectbutton", 262 }, { "select", 295 },
        { "backbutton", 488 }, { "cancel", 521 } };
    for (const auto& p : spread) {
        size_t b, en;
        if (LeafItem(data, p.name, b, en))
            SetItemX(data, b, p.x);
    }
    // The game's own Paste prompt (Y; the key bound to Y's cap with the keyboard's prompts: prompts.cpp), Back's icon
    // and text copied (the screen's item count: the word before its first item). SWROTS_TEST_PASTEMODE: test modes
    // (text, icon..., -end, dup...; none: no prompt, the HUD's).
    char mode[16] = "y";
    if (GetEnvironmentVariableA("SWROTS_TEST_PASTEMODE", mode, sizeof(mode)) && std::strcmp(mode, "none") == 0)
        return true;
    if (std::strncmp(mode, "dup", 3) == 0) { // tests: an exact copy of an item (dup: Back's prompt after it; dup-veil:
                                             // the screen's first item after it), the count one more
        size_t b, en;
        if (!LeafItem(data, std::strcmp(mode, "dup-veil") == 0 ? "screenveil" : "backbutton", b, en))
            return true;
        const std::vector<uint8_t> copy(data.begin() + b, data.begin() + en);
        data.insert(data.begin() + en, copy.begin(), copy.end());
        if (!AddScreenItems(data, 1))
            return true;
        uint32_t items = 0;
        LOG_INFO("Online menu: test: %s (%zu bytes copied, %u items)", mode, copy.size(), items);
        return true;
    }
    if (!LeafItem(data, "backbutton", iconBegin, iconEnd) || !LeafItem(data, "cancel", textBegin, textEnd) ||
        iconBegin + 18 + 10 + 82 >= iconEnd) {
        LOG_WARN("Online menu: the Codes screen's prompts are not as expected; no Paste prompt");
        return true;
    }
    std::vector<uint8_t> icon(data.begin() + iconBegin, data.begin() + iconEnd);
    std::vector<uint8_t> text(data.begin() + textBegin, data.begin() + textEnd);
    size_t e = icon.size();
    MenuReplaceString(icon, 0, e, "backbutton", "pastebutton");
    icon[18 + 11 + 82] = std::strcmp(mode, "icon0") == 0 ? 0 : std::strcmp(mode, "icon3") == 0 ? 3 :
        std::strcmp(mode, "icon2") == 0 ? 2 : 1; // Y
    e = text.size();
    MenuReplaceString(text, 0, e, "cancel", "pastetext");
    e = text.size();
    if (!MenuReplaceString(text, 0, e, "IDS_BACK", "IDS_PORT_ONLINE_PASTE"))
        return true;
    // the new prompt after the Back text; then the places, from the right: Back 485/518, Paste 375/408, Select 265/298
    // test modes: y (the icon and the text), text (the text alone), icon / icon0 / icon2 / icon3 (the icon alone)
    const bool iconOnly = std::strncmp(mode, "icon", 4) == 0, textOnly = std::strncmp(mode, "text", 4) == 0;
    std::vector<uint8_t> both;
    if (!textOnly)
        both = icon;
    if (!iconOnly)
        both.insert(both.end(), text.begin(), text.end());
    // "-end" modes: after the screen's last item (before the transition controllers' count) instead of after Back's
    size_t at = textEnd;
    if (std::strstr(mode, "-end")) {
        const size_t sets = MenuFind(data, MenuString("textureSet"), 0, data.size());
        uint32_t transitions = 1;
        if (sets != std::string::npos && sets >= 8)
            std::memcpy(&transitions, data.data() + sets - 8, 4);
        if (sets == std::string::npos || transitions != 0)
            return true;
        at = sets - 8;
    }
    data.insert(data.begin() + at, both.begin(), both.end());
    g_PastePrompt = !iconOnly && !textOnly;
    const struct { const char* name; float x; } places[] = { { "selectbutton", 262 }, { "select", 295 },
        { "pastebutton", 375 }, { "pastetext", 408 }, { "backbutton", 488 }, { "cancel", 521 } };
    for (const auto& p : places) {
        size_t b, en;
        if (LeafItem(data, p.name, b, en))
            SetItemX(data, b, p.x);
    }
    AddScreenItems(data, iconOnly || textOnly ? 1 : 2);
    return true;
}

// The room's footer: a third prompt, X Ready (its text the port's: ReadyLegend), a copy of Back's icon (its icon X, 3)
// and text after Back's; the prompts spread again: Scroll, Select, Ready, Back (Not Ready the longest).
void ReadyPrompt(std::vector<uint8_t>& data)
{
    size_t iconBegin, iconEnd, textBegin, textEnd;
    if (!LeafItem(data, "backbutton", iconBegin, iconEnd) || !LeafItem(data, "back", textBegin, textEnd)) {
        LOG_WARN("Online menu: the room's prompts are not as expected; no Ready prompt");
        return;
    }
    std::vector<uint8_t> icon(data.begin() + iconBegin, data.begin() + iconEnd);
    std::vector<uint8_t> text(data.begin() + textBegin, data.begin() + textEnd);
    size_t e = icon.size();
    if (!MenuReplaceString(icon, 0, e, "backbutton", "readybutton") || icon.size() <= 18 + 11 + 82)
        return;
    icon[18 + 11 + 82] = 3; // X
    e = text.size();
    if (!MenuReplaceString(text, 0, e, "back", "readytext"))
        return;
    e = text.size();
    if (!MenuReplaceString(text, 0, e, "IDS_BACK", "IDS_PORT_LEGEND_READY"))
        return;
    std::vector<uint8_t> both = icon;
    both.insert(both.end(), text.begin(), text.end());
    data.insert(data.begin() + textEnd, both.begin(), both.end());
    const struct { const char* name; float x; } places[] = { { "selectbutton", 230 }, { "select", 263 },
        { "readybutton", 340 }, { "readytext", 373 }, { "backbutton", 488 }, { "back", 521 } }; // room for "Not Ready"
    for (const auto& p : places) {
        size_t b, en;
        if (LeafItem(data, p.name, b, en))
            SetItemX(data, b, p.x);
    }
    AddScreenItems(data, 2);
}

bool GenerateScreens(const std::string& lowerName, std::vector<uint8_t>& data)
{
    if (lowerName == kSettingsFileName)
        return BuildSettingsScreen(data, kScreenNames[kSettingsScreen], Rows(kSettingsScreen), "IDS_PORT_ONLINE_SETTINGS",
            "OnlineValue", "OnlineButton");
    if (lowerName == kPublicFileName) // opened on the first room (the status row above it is never the cursor's)
        return BuildSettingsScreen(data, kScreenNames[kPublicScreen], Rows(kPublicScreen), "IDS_PORT_PUBLIC_TITLE",
            "OnlineValue", "OnlineButton", kRowDefs[kPub0].name);
    if (lowerName == kRoomFileName) {
        if (!BuildSettingsScreen(data, kScreenNames[kRoom], Rows(kRoom), "IDS_PORT_ROOM_TITLE", "OnlineValue",
                "OnlineButton", kRowDefs[kRoomSettings].name))
            return false;
        ReadyPrompt(data);
        return true;
    }
    if (lowerName == kRoomSettingsFileName)
        return BuildSettingsScreen(data, kScreenNames[kRoomSettingsScreen], Rows(kRoomSettingsScreen),
            "IDS_PORT_ROOM_SETTINGS", "OnlineValue", "OnlineButton");
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

void OnlineOpenCodeKeyboard()
{
    OpenKeyboard(KeyboardFor::Code);
}

float GameHudAlpha()
{
    const float alpha = HudItemAlpha();
    static int logged = 0;
    static float last = -2.0f;
    if (alpha != last && logged < 40) { // (the fades' steps, the first ones)
        ++logged;
        LOG_INFO("Online HUD: the game's HUD alpha %.2f", alpha);
    }
    last = alpha;
    return alpha < 0 ? 1.0f : alpha;
}

bool OnlinePastePromptBuilt()
{
    return g_PastePrompt;
}

bool OnlineCodeKeyboardShown()
{
    return g_CodeKeyboardAt && GetTickCount64() - g_CodeKeyboardAt < 250;
}

bool OnlineShowLost(const std::string& why)
{
    const bool left = why.find("left") != std::string::npos;
    const bool otherIdle = why.find("other player was removed") != std::string::npos;
    const bool idle = !otherIdle && why.find("inactivity") != std::string::npos;
    SetMenuText("IDS_PORT_LOST_TEXT", otherIdle ? L"The other player was removed from the room for inactivity." :
        idle ? L"You were removed from the room after five minutes without any action." :
        left ? L"The other player left the room." : L"The connection to the other player was lost.");
    SetMenuText("IDS_PORT_LOST_TITLE", idle ? L"Removed from the Room" : L"Connection Lost");
    g_LostPopup = true; // shown by the Online row once the main menu is up (MainButtonDraw)
    if (LastShellScreen() == MenuKey(kScreenNames[kOnline]))
        return true;
    return GoToShellScreen(kScreenNames[kOnline]);
}

bool OnlineStartVersus()
{
    return GoToVersusSelect();
}

bool OnlineFollowsScreen(uint32_t key)
{
    if (key == MenuKey(kScreenNames[kRoom]) && (LastShellScreen() == MenuKey(kScreenNames[kRoomSettingsScreen]) ||
                                                 LastShellScreen() == MenuKey(kCodeScreen)))
        return false; // the guest looks at the room's settings: it stays there while the host is in the room
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

// --- The main menu's Online submenu ----------------------------------------------------------------------------------
// Its entries are MainMenuSwitchScreenButtons (vtable 0x5D5580; slot 4, 0x2CBEC0: thiscall (event, value), 0x23 A):
// their A is the port's (Host Game makes the room, Join Game opens the code keyboard, Public Rooms, Online Settings).
// The Online row (a MainMenuButton, vtable 0x5D5410; slot 3, 0x2CC550, its draw: thiscall (value)) shows the game's
// popup pending once the main menu is up (the other player gone, the connection lost), as its owner.
constexpr uint32_t kSwitchVtable = 0x005D5580, kSwitchEvent = 0x002CBEC0;
constexpr uint32_t kMainButtonVtable = 0x005D5410, kMainButtonDraw = 0x002CC550;
uint32_t g_PrevSwitchEvent = kSwitchEvent, g_PrevMainButtonDraw = kMainButtonDraw; // the slots as they were
uint32_t g_PrevMainButtonEvent = 0;

char __fastcall MainButtonEvent(uint8_t* handler, void*, int event, int value)
{
    if (NoticeTakes(event, value)) // the main menu's rows: a notice over it closes first
        return 1;
    return reinterpret_cast<ItemEventFn>(uintptr_t(g_PrevMainButtonEvent))(handler, nullptr, event, value);
}

uint32_t ItemKey(uint8_t* handler)
{
    const uint8_t* item = handler ? *reinterpret_cast<uint8_t* const*>(handler + 8) : nullptr;
    return item ? *reinterpret_cast<const uint32_t*>(item + 4) : 0;
}


// A room made (true: the room screen to open, by the caller when `open` is false); else the game's popup says why.
bool HostGame(uint8_t* owner, bool open = true)
{
    g_Note.clear();
    g_Copied.clear();
    g_Typed.clear();
    LOG_INFO("Online menu: Host Game");
    const bool hosting = net::OnlineHost();
    if (hosting) { // the last Public Room choice ([Online] PublicRoom), the room's name ([Online] RoomName)
        net::OnlineRoomSettings room = net::OnlineStatus().room;
        room.open = GetPrivateProfileIntW(L"Online", L"PublicRoom", 0, SettingsIni().c_str()) ? 1 : 0;
        strncpy_s(room.name, SavedRoomName().c_str(), _TRUNCATE);
        net::SetOnlineRoom(room);
    }
    if (hosting && open)
        GoToShellScreen(kScreenNames[kRoom]);
    else if (!hosting)
        ShowNotice(owner, Shorten(net::OnlineStatus().message, 60));
    UpdateTexts();
    return hosting;
}

// The Yes/No popup's answer to a value row (its owner): a public room joined; else the settings control's own.
constexpr uint32_t kControlYes = 0x002D7C60, kControlNo = 0x002D7B80;

void __fastcall PopupYes(uint8_t* handler, void*)
{
    if (g_PendingJoin.empty()) {
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kControlYes))(handler, nullptr);
        return;
    }
    const std::string code = g_PendingJoin;
    g_PendingJoin.clear();
    g_Typed = code.substr(0, 5) + "-" + code.substr(5, 5) + "-" + code.substr(10);
    g_Note.clear();
    LOG_INFO("Online menu: joining the public room %s", g_Typed.c_str());
    if (net::OnlineJoin(code)) // the game's popup here while joining (both games then restart, to the room)
        g_JoinNotice = g_JoinWatch = true;
    else
        ShowNotice(handler, Shorten(net::OnlineStatus().message, 60));
}

void __fastcall PopupNo(uint8_t* handler, void*)
{
    if (!g_PendingJoin.empty()) {
        g_PendingJoin.clear();
        return;
    }
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(uintptr_t(kControlNo))(handler, nullptr);
}

char __fastcall SwitchEvent(uint8_t* handler, void*, int event, int value)
{
    if (NoticeTakes(event, value))
        return 1;
    // The entries' screens are opened by the game's own choice (as Versus's: the main menu then closes the submenu
    // when it is left), after what each needs first.
    if (event == kChosen) {
        const uint32_t key = ItemKey(handler);
        if (key == MenuKey("mmonlinehost") && !HostGame(handler, false))
            return 1; // no room: the popup says why, the menu stays
        if (key == MenuKey("mmonlinejoin")) {
            LOG_INFO("Online menu: Join Game");
            PrepareKeyboard(KeyboardFor::Code);
        }
        if (key == MenuKey("mmonlinepublic"))
            net::LobbyWantList();
    }
    return reinterpret_cast<ItemEventFn>(uintptr_t(g_PrevSwitchEvent))(handler, nullptr, event, value);
}

char __fastcall MainButtonDraw(uint8_t* handler, void*, uint32_t value)
{
    if (g_LostPopup && ItemKey(handler) == MenuKey(kRowName) && LastShellScreen() == MenuKey(kScreenNames[kOnline])) {
        g_LostPopup = false;
        ShowPopup(handler, "IDS_PORT_LOST_TITLE", "IDS_PORT_LOST_TEXT");
    }
    return reinterpret_cast<DrawFn>(uintptr_t(g_PrevMainButtonDraw))(handler, nullptr, value);
}

void PatchMainMenuOnline(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> copy = data;
    if (!PatchRows(copy))
        return;
    std::vector<uint8_t> withPopup = copy;
    if (AddMainMenuPopup(withPopup))
        copy.swap(withPopup);
    else
        LOG_WARN("Online menu: no popup of the game's on the main menu");
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
    if (SlotIs(kJediListVtable, 4, kJediListEvent)) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&JediListEvent));
        PatchBytes(kJediListVtable + 4 * 4, &hook, 4);
    }
    if (*reinterpret_cast<const uint32_t*>(uintptr_t(kVitalsFadeSlot)) == kVitalsFade) { // the HUD's fade (GameHudAlpha)
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&VitalsFade));
        PatchBytes(kVitalsFadeSlot, &hook, 4);
    } else {
        LOG_WARN("Online menu: the HUD's vitals are not as expected; the duel's names do not fade with it");
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
    if (g_ValueVtable[8] == kControlYes && g_ValueVtable[9] == kControlNo) {
        g_ValueVtable[8] = uint32_t(reinterpret_cast<uintptr_t>(&PopupYes));
        g_ValueVtable[9] = uint32_t(reinterpret_cast<uintptr_t>(&PopupNo));
    }
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
    { // the main menu's buttons: after the port's other hooks there (Heroes vs Villains'), which are called on
        g_PrevSwitchEvent = *reinterpret_cast<const uint32_t*>(uintptr_t(kSwitchVtable + 4 * 4));
        g_PrevMainButtonEvent = *reinterpret_cast<const uint32_t*>(uintptr_t(kMainButtonVtable + 4 * 4));
        const uint32_t mainEvent = uint32_t(reinterpret_cast<uintptr_t>(&MainButtonEvent));
        PatchBytes(kMainButtonVtable + 4 * 4, &mainEvent, 4);
        g_PrevMainButtonDraw = *reinterpret_cast<const uint32_t*>(uintptr_t(kMainButtonVtable + 4 * 3));
        const uint32_t event = uint32_t(reinterpret_cast<uintptr_t>(&SwitchEvent));
        const uint32_t draw = uint32_t(reinterpret_cast<uintptr_t>(&MainButtonDraw));
        PatchBytes(kSwitchVtable + 4 * 4, &event, 4);
        PatchBytes(kMainButtonVtable + 4 * 3, &draw, 4);
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
    SetMenuText("IDS_PORT_ROOM_PUBLIC", L"Public Room");
    SetMenuText("IDS_PORT_PUBLIC_TITLE", L"Public Rooms");
    SetMenuText("IDS_PORT_PUB_STATUS", L"Public Rooms");
    SetMenuText("IDS_PORT_JOIN_TITLE", L"Join Room");
    SetMenuText("IDS_PORT_JOIN_TEXT", L"");
    SetMenuText("IDS_PORT_ROOM_P1", L"Player 1");
    SetMenuText("IDS_PORT_ROOM_P2", L"Player 2");
    SetMenuText("IDS_PORT_ROOM_CODE", L"Room Code");
    SetMenuText("IDS_PORT_ROOM_TIME", L"Time Limit");
    SetMenuText("IDS_PORT_LEGEND_READY", L"Ready");
    SetMenuText("IDS_PORT_ROOM_SETTINGS", L"Room Settings");
    SetMenuText("IDS_PORT_ROOM_NAME", L"Room Name");
    SetMenuText("IDS_PORT_JOINING_TITLE", L"Joining Room");
    SetMenuText("IDS_PORT_GUEST_TITLE", L"Player Joining");
    SetMenuText("IDS_PORT_ROOM_LEAVE", L"Leave Room");
    SetMenuText("IDS_PORT_LOST_TITLE", L"Connection Lost");
    SetMenuText("IDS_PORT_LOST_TEXT", L"The connection to the other player was lost.");
    SetMenuText("IDS_PORT_ONLINE_PASTE", L"Paste");
    SetMenuText("IDS_PORT_ONLINE_PUBLIC", L"Public Rooms");
    SetMenuText("IDS_PORT_NOTICE_TITLE", L"Online");
    SetMenuText("IDS_PORT_NOTICE_TEXT", L"");
    SetMenuText("IDS_PORT_ONLINE_ENTER_CODE", L"Enter the Room Code");
    g_PasteDown = false;
    for (int i = 0; i < kPictureCount; ++i) { // the last start's pictures are gone with it
        g_Pictures[i] = nullptr;
        g_PicturesTried[i] = false;
    }
    UpdateTexts();
    AddShellHandler("OnlineValue", &MakeValue);
    AddShellHandler("OnlineButton", &MakeButton);
    AddShellHandler("OnlineKeyboard", &MakeKeyboard);
    AddShellScreen(kSettingsFileName);
    AddShellScreen(kRoomFileName);
    AddShellScreen(kPublicFileName);
    AddShellScreen(kRoomSettingsFileName);
    net::StartLobby(); // a public room's announcements
    AddShellScreen(kCodeFile);
    RegisterResourceGenerator(&GenerateScreens);
}

} // namespace swrots::game
