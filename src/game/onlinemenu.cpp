// Online play in the game's main menu (see onlinemenu.h).
//
// The main menu (main_menu.xbl_xml) has four top-level rows, MainMenuButton items 24 apart from y 179: Single Player,
// Multi Player, Settings, Features (each a text id, its down and up neighbours, and for Settings the screen it opens),
// with a veil behind each row and a fifth veil (debugveil, y 277) spare. A row "online" is put between Multi Player and
// Settings: a copy of Settings' (its highlight bar too), opening the port's screen "online"; Settings and Features
// move one row down, and the neighbours are linked through it.
//
// The screen "online" is the Settings screen's (BuildSettingsScreen) with five rows: Host Game, Join Game (buttons),
// Room Code, Status (value rows showing text), Leave. Join Game opens "online_code", the game's code screen
// (codes.xbl_xml: the Settings > Codes keyboard, CodesKeyboard: A-Z, 0-9, Back, Space, Done) with a keyboard of the
// port's own: the game's (made by 0x2D1740, vtable 0x5D61E0) whose Done (slot 29) joins with the code typed, which
// holds 15 letters (its init, slot 0, sets the length to 14: +0x24), and which takes Ctrl+V (the clipboard's code)
// as it draws (slot 3), or Y on any of its keys. Its text is wide characters at +0x28 (16), +0x48 the end, +0x50/+0x54/+0x58 its Back,
// Space and Done keys' handlers (their +0x20 set while the text is empty).
//
// In a match the screens are played on both PCs alike (session.h): what they show and do must not depend on the PC,
// so then they show the same texts on both, Host and Join do nothing and the clipboard is left alone; Leave ends the
// session on both.

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

// --- The screens' texts ----------------------------------------------------------------------------------------------
std::string g_Typed;      // the guest's code, as typed
std::string g_Copied;     // the room code last put on the clipboard
std::string g_Note;       // a note of the menu's own (a code refused)

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

void UpdateTexts()
{
    const net::OnlineStatusInfo s = net::OnlineStatus();
    std::string code = "-", status;
    if (net::OnlineActive()) { // the same on both PCs
        SetMenuText("IDS_PORT_ONLINE_V_CODE", L"-");
        SetMenuText("IDS_PORT_ONLINE_V_STATUS", L"IN A MATCH");
        return;
    }
    switch (s.phase) {
    case net::OnlinePhase::Off: status = g_Note.empty() ? "NOT CONNECTED" : g_Note; break;
    case net::OnlinePhase::Opening: status = "OPENING A ROOM..."; break;
    case net::OnlinePhase::Waiting: status = g_Copied == s.roomCode ? "WAITING (CODE COPIED)" : "WAITING FOR A FRIEND"; break;
    case net::OnlinePhase::Connecting: status = "CALLING THE HOST..."; break;
    case net::OnlinePhase::Preparing: status = s.host ? "SENDING THE PROFILE..." : "GETTING THE PROFILE..."; break;
    case net::OnlinePhase::Starting: status = "STARTING..."; break;
    case net::OnlinePhase::Playing: status = "IN A MATCH"; break;
    case net::OnlinePhase::Ended: status = g_Note.empty() ? "ENDED: " + s.message : g_Note; break;
    }
    if (s.host && !s.roomCode.empty() && s.phase != net::OnlinePhase::Ended)
        code = s.roomCode;
    else if (!s.host && !g_Typed.empty())
        code = g_Typed;
    // the host's code goes to the clipboard as soon as there is one
    if (s.host && s.phase == net::OnlinePhase::Waiting && !s.roomCode.empty() && g_Copied != s.roomCode &&
        CopyToClipboard(s.roomCode)) {
        g_Copied = s.roomCode;
        LOG_INFO("Online menu: room code %s copied to the clipboard", s.roomCode.c_str());
    }
    SetMenuText("IDS_PORT_ONLINE_V_CODE", Wide(code));
    SetMenuText("IDS_PORT_ONLINE_V_STATUS", Wide(Shorten(status, 40)));
}

// --- The online screen's rows ----------------------------------------------------------------------------------------
std::vector<ShellScreenRow> Rows()
{
    return { { "onlinehost", "IDS_PORT_ONLINE_HOST", true }, { "onlinejoin", "IDS_PORT_ONLINE_JOIN", true },
        { "onlinecode", "IDS_PORT_ONLINE_CODE", false }, { "onlinestatus", "IDS_PORT_ONLINE_STATUS", false },
        { "onlineleave", "IDS_PORT_ONLINE_LEAVE", true } };
}
enum Row { kHost, kJoin, kCode, kStatus, kLeave };

constexpr uint32_t kMakeOnOff = 0x002D9690;   // SettingsScreenOnOffControl
constexpr uint32_t kOnOffVtable = 0x005D7000;
constexpr uint32_t kMakeButton = 0x002D93F0;  // SettingsScreenButton
constexpr uint32_t kButtonVtable = 0x005D6E10;
constexpr uint32_t kItemEvent = 0x002C62D0;    // an item's events (B goes back)
constexpr uint32_t kProfiles = 0x007E8F24;
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
    const std::vector<ShellScreenRow> rows = Rows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (key == MenuKey(rows[i].name.c_str()))
            return int(i);
    return -1;
}

uint8_t* CurrentProfile()
{
    uint8_t* profiles = *reinterpret_cast<uint8_t**>(uintptr_t(kProfiles));
    return profiles ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(uintptr_t(kCurrentProfile))(profiles, nullptr)
                    : nullptr;
}

// A value row shows its text as the control's On text (the control shows On for the profile's subtitles setting,
// +0x2A, which it is given for the draw).
char __fastcall ValueDraw(uint8_t* handler, void*, uint32_t value)
{
    const int row = RowOf(handler);
    const char* text = row == kCode ? "IDS_PORT_ONLINE_V_CODE" : row == kStatus ? "IDS_PORT_ONLINE_V_STATUS" : nullptr;
    if (!text)
        return g_OnOffDraw(handler, nullptr, value);
    if (row == kCode)
        UpdateTexts(); // once a frame
    handler[0x23C] = 1;
    uint8_t* profile = CurrentProfile();
    const uint8_t saved = profile ? profile[0x2A] : 0;
    if (profile)
        profile[0x2A] = 1;
    SetOnOffTexts(text, text);
    const char result = g_OnOffDraw(handler, nullptr, value);
    SetOnOffTexts(nullptr, nullptr);
    if (profile)
        profile[0x2A] = saved;
    return result;
}

char __fastcall ValueEvent(uint8_t* handler, void*, int event, int value)
{
    if (event == kB)
        return reinterpret_cast<ItemEventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
    if (event == kLeft || event == kRight || event == kChosen) {
        // A on the room code copies it again (it may have been replaced on the clipboard since)
        if (RowOf(handler) == kCode && event == kChosen && !net::OnlineActive()) {
            const net::OnlineStatusInfo s = net::OnlineStatus();
            if (s.host && !s.roomCode.empty() && CopyToClipboard(s.roomCode))
                g_Copied = s.roomCode;
        }
        return 1; // the value is not the player's to change
    }
    return g_OnOffEvent(handler, nullptr, event, value);
}

char __fastcall ButtonEvent(uint8_t* handler, void*, int event, int value)
{
    const int row = RowOf(handler);
    if (event == kB)
        return reinterpret_cast<ItemEventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
    if (row < 0 || event != kChosen)
        return g_ButtonEvent(handler, nullptr, event, value);
    if (row == kLeave) {
        g_Note.clear();
        net::OnlineLeave(); // in a match: on both PCs alike (both get the same press)
        LOG_INFO("Online menu: Leave");
        return 1;
    }
    if (net::OnlineActive())
        return 1;
    const net::OnlineStatusInfo s = net::OnlineStatus();
    const bool idle = s.phase == net::OnlinePhase::Off || s.phase == net::OnlinePhase::Ended;
    if (row == kHost) {
        if (idle) {
            g_Note.clear();
            g_Copied.clear();
            g_Typed.clear();
            LOG_INFO("Online menu: Host Game");
            if (!net::OnlineHost())
                g_Note = Shorten(net::OnlineStatus().message, 40);
        } else if (s.host && !s.roomCode.empty() && CopyToClipboard(s.roomCode)) {
            g_Copied = s.roomCode;
        }
        UpdateTexts();
        return 1;
    }
    if (row == kJoin && idle) {
        LOG_INFO("Online menu: Join Game");
        GoToShellScreen(kCodeScreen);
        return 1;
    }
    return 1;
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
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
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
}

// Ctrl+V: the clipboard's code (its letters and digits) as the text.
void __fastcall KeyboardDraw(uint8_t* keyboard, void*, uint32_t value)
{
    const bool down = GameWindowActive() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('V') & 0x8000);
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
    if (net::OnlineActive())
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
    return reinterpret_cast<ItemEventFn>(uintptr_t(kKeyEvents[I]))(key, nullptr, event, value);
}
constexpr ItemEventFn kKeyHooks[] = { &KeyEvent<0>, &KeyEvent<1>, &KeyEvent<2>, &KeyEvent<3> };

// Done: joins with the code typed (back to the online screen), or refuses it.
void __fastcall KeyboardDone(uint8_t* keyboard, void*)
{
    if (net::OnlineActive())
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
    GoToShellScreen(kScreen);
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
    if (!ok)
        LOG_WARN("Online menu: the Codes screen is not as expected; no code screen");
    return ok;
}

bool GenerateScreens(const std::string& lowerName, std::vector<uint8_t>& data)
{
    if (lowerName == kScreenFile)
        return BuildSettingsScreen(data, kScreen, Rows(), "IDS_PORT_ONLINE_TITLE", "OnlineValue", "OnlineButton");
    if (lowerName == kCodeFile)
        return BuildCodeScreen(data);
    return false;
}

bool SlotIs(uint32_t vtable, int slot, uint32_t function)
{
    return *reinterpret_cast<const uint32_t*>(uintptr_t(vtable + 4 * slot)) == function;
}

} // namespace

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
    SetMenuText("IDS_PORT_ONLINE_CODE", L"Room Code");
    SetMenuText("IDS_PORT_ONLINE_STATUS", L"Status");
    SetMenuText("IDS_PORT_ONLINE_LEAVE", L"Leave");
    SetMenuText("IDS_PORT_ONLINE_ENTER_CODE", L"Enter the Room Code (Y or Ctrl+V: paste)");
    g_PasteDown = false;
    UpdateTexts();
    AddShellHandler("OnlineValue", &MakeValue);
    AddShellHandler("OnlineButton", &MakeButton);
    AddShellHandler("OnlineKeyboard", &MakeKeyboard);
    AddShellScreen(kScreenFile);
    AddShellScreen(kCodeFile);
    RegisterResourceGenerator(&GenerateScreens);
}

} // namespace swrots::game
