// Quit Game in the game's menus (see quitmenu.h).
//
// The rows. Compiled menus (.xbl_xml) are a screen's name, the item it starts on, its number of top-level items,
// then the items: "screenItem", the name, a u32, the position (x, y), ..., the strings of the item's settings (for a
// row: its text id, its down and up neighbours, [a screen,] a u32 id, its action), then its children.
//   - main menu (main_menu.xbl_xml): Single Player, Multi Player, Settings and Features are MainMenuButton rows (made
//     by 0x2CCA90, vtable 0x5D5410), 24 apart from y 179, each with a highlight bar as its child. A fifth row, a copy
//     of Settings 24 below Features, is added between Features and Single Player (the rows wrap round); in the file
//     it goes after Features' submenu, which follows its row.
//   - the screen after the title (new_game.xbl_xml): New Game and Load Game are NewGameNavigation rows (made by
//     0x2D9C50, vtable 0x5D71E0), 53 apart from y 207, without children. Its id (the u32 after its screen: 1 New
//     Game, 0 Load Game) is what the row does when chosen (0x2DA280) and which four highlight items it lights when
//     focused (looked up by name as it starts, 0x2D9CC0 slot 0, into +0x20..+0x2C; lit through their slot 15 by the
//     row's slot 7): New Game's and Load Game's own. All three rows and their highlights are raised 40 so the
//     third clears the logos at the bottom. A third row, a copy of Load Game 53 lower with an id of
//     neither (9), comes with copies of Load Game's four highlight items, 53 lower too; its start (slot 0) looks
//     them up after the game's own start found none.
// Each new row's action is a handler of the port's own: a copy of its row's vtable with events (slot 4: A asks),
// a copy (slot 10) and the popup's answers (slots 8 and 9) of its own.
//
// The question is the screen's own Yes/No popup (main menu: the item "popup"; the screen after the title:
// "YesNoPopup"; a YesNoPopup, vtable 0x5D9E08), opened as the Settings screen's Restore Defaults row opens its own
// (0x2D8180):
//   - the shell's handler of an item of a screen by name (0x2F5A00, thiscall (name, screen or null for the current
//     one, 0) on the shell 0x7F637C; a row's screen is its item's +0x24);
//   - its slot 27 (0x2E7B20, thiscall (owner, 0)) opens it for an owner (kept at +0xA4);
//   - 0x2E8B80 sets its title and 0x2E99E0 its message (thiscall (text)), texts as the string table holds them (the
//     length first).
// The popup's A picks its highlighted answer (+0xB0: 0 No, which the port sets as it opens; 1 Yes) into +0x86 and closes; once
// closed (slot 24, 0x2E6C80) it calls its owner's slot 8 for Yes and slot 9 for No. The rows' own slots 8 and 9
// answer the screens' other questions (the main menu's B: back to the title; the memory unit's), so the port's quit
// only while its own question is open and otherwise do what the rows always did.

#include "game/quitmenu.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/window.h"
#include "game/menus.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

constexpr const char* kRowName = "quitgame";
constexpr const char* kRowText = "IDS_PORT_QUIT_GAME";
constexpr const char* kTitleText = "IDS_PORT_QUIT_GAME_TITLE";
constexpr const char* kMessageText = "IDS_PORT_QUIT_GAME_MESSAGE";
constexpr const char* kMenuAction = "QuitGameMenuButton";
constexpr const char* kTitleAction = "QuitGameNavigation";
constexpr const char* kNewGame = "interfc\\front_end\\xml\\new_game.xml";
constexpr float kMenuStep = 24.0f;  // the main menu's rows
constexpr float kTitleStep = 53.0f; // New Game and Load Game
constexpr uint32_t kTitleRowId = 9; // neither New Game (1) nor Load Game (0)
constexpr float kTitleRaise = 40.0f; // the three rows and their highlights go up, clear of the logos below
constexpr const char* kTitleItems[] = { "newgame", "loadgame", "newgameveil_a", "newgameveil_b", "newgameblueveil_a",
    "newgameblueveil_b", "loadgameveil_a", "loadgameveil_b", "loadgameblueveil_a", "loadgameblueveil_b" };
constexpr const char* kVeils[] = { "loadgameblueveil_a", "loadgameblueveil_b", "loadgameveil_a", "loadgameveil_b" };
constexpr const char* kQuitVeils[] = { "quitgameblueveil_a", "quitgameblueveil_b", "quitgameveil_a",
    "quitgameveil_b" };

constexpr int kVtableSlots = 32;
constexpr uint32_t kNothing = 0x00081C10;
// the main menu's rows (MainMenuButton)
constexpr uint32_t kMenuButtonVtable = 0x005D5410;
constexpr uint32_t kMenuButtonEvent = 0x002CC7E0; // slot 4
constexpr uint32_t kMenuButtonYes = 0x002CBE30;   // slot 8: back to the title
constexpr uint32_t kMakeMenuButton = 0x002CCA90;  // slot 10
// the screen after the title's rows (NewGameNavigation)
constexpr uint32_t kNavigationVtable = 0x005D71E0;
constexpr int kNavigationSlots = 27;
constexpr uint32_t kNavigationStart = 0x002D9CC0; // slot 0
constexpr uint32_t kNavigationEvent = 0x002DA280; // slot 4
constexpr uint32_t kNavigationYes = 0x002DA380;   // slot 8
constexpr uint32_t kNavigationNo = 0x002D9C40;    // slot 9
constexpr uint32_t kMakeNavigation = 0x002D9C50;  // slot 10
constexpr uint32_t kNavigationVeils = 0x20;       // four handlers
constexpr uint32_t kShowSlot = 15;

// the main menu's intro (MainMenuIntro, made by 0x2CCB60, vtable 0x5D55F0): its start (slot 0, 0x2CCBB0) lists the
// four rows' handlers at +0x3C (appended by 0x13130, thiscall (&handler)) and brings them in one by one
constexpr uint32_t kIntroVtable = 0x005D55F0;
constexpr uint32_t kIntroStart = 0x002CCBB0;
constexpr uint32_t kIntroRows = 0x3C;
constexpr uint32_t kListAppend = 0x00013130;

constexpr uint32_t kShell = 0x007F637C;
constexpr uint32_t kShellFlags = 0x0C, kShellReady = 0x40000;
constexpr uint32_t kFindHandler = 0x002F5A00;
constexpr uint32_t kPopupOpenSlot = 27;
constexpr uint32_t kPopupSetTitle = 0x002E8B80;
constexpr uint32_t kPopupSetMessage = 0x002E99E0;
constexpr uint32_t kPopupAnswer = 0xB0; // the highlighted answer: 1 Yes, 0 No
constexpr uint32_t kSoundSlot = 11; // thiscall (sound): the menus' sounds (1: the one a chosen row plays)
constexpr int kChosen = 0x23;

uint32_t g_MenuVtable[kVtableSlots], g_NavigationVtable[kVtableSlots];
bool g_Asking = false; // a popup is open with the port's question

using EventFn = char(__fastcall*)(uint8_t*, void*, int, int);
using PlainFn = void(__fastcall*)(uint8_t*, void*);

uint8_t* Shell()
{
    uint8_t* shell = *reinterpret_cast<uint8_t**>(uintptr_t(kShell));
    return shell && (*reinterpret_cast<const uint32_t*>(shell + kShellFlags) & kShellReady) ? shell : nullptr;
}

// The handler of an item of a row's screen, by name.
uint8_t* FindHandler(uint8_t* row, const char* name)
{
    uint8_t* shell = Shell();
    const uint8_t* item = *reinterpret_cast<uint8_t* const*>(row + 8);
    void* screen = item ? *reinterpret_cast<void* const*>(item + 0x24) : nullptr; // null: the current screen
    return shell ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*, void*, int)>(
                       uintptr_t(kFindHandler))(shell, nullptr, name, screen, 0)
                 : nullptr;
}

bool IsQuitRow(uint8_t* row)
{
    const uint8_t* item = *reinterpret_cast<uint8_t* const*>(row + 8);
    return item && *reinterpret_cast<const uint32_t*>(item + 4) == MenuKey(kRowName);
}

bool Ask(uint8_t* owner)
{
    const wchar_t* title = MenuTextBuffer(kTitleText);
    const wchar_t* message = MenuTextBuffer(kMessageText);
    uint8_t* popup = FindHandler(owner, "popup");
    if (!popup)
        popup = FindHandler(owner, "YesNoPopup");
    if (!popup || !title || !message) {
        LOG_WARN("Quit menu: the screen has no Yes/No popup; Quit Game does nothing");
        return false;
    }
    const uint32_t* vtable = *reinterpret_cast<uint32_t* const*>(owner);
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(uintptr_t(vtable[kSoundSlot]))(owner, nullptr, 1);
    const uint32_t* popupVtable = *reinterpret_cast<uint32_t* const*>(popup);
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*, int)>(uintptr_t(popupVtable[kPopupOpenSlot]))(
        popup, nullptr, owner, 0);
    using SetTextFn = void(__fastcall*)(uint8_t*, void*, const wchar_t*);
    reinterpret_cast<SetTextFn>(uintptr_t(kPopupSetTitle))(popup, nullptr, title);
    reinterpret_cast<SetTextFn>(uintptr_t(kPopupSetMessage))(popup, nullptr, message);
    popup[kPopupAnswer] = 0; // No highlighted first (the popup keeps the last answer it showed)
    g_Asking = true;
    LOG_INFO("Quit menu: asked whether to quit");
    return true;
}

void Quit()
{
    g_Asking = false;
    LOG_INFO("Quit menu: quitting the game");
    LogFlush();
    HWND window = GameWindow();
    HWND frame = window ? GetAncestor(window, GA_ROOT) : nullptr;
    if (frame)
        PostMessageW(frame, WM_CLOSE, 0, 0); // as the window's close button: the frame closes the process
    else
        ExitProcess(0);
}

// The main menu's row.
char __fastcall MenuEvent(uint8_t* self, void*, int event, int value)
{
    if (event == kChosen && Ask(self))
        return 1;
    return reinterpret_cast<EventFn>(uintptr_t(kMenuButtonEvent))(self, nullptr, event, value);
}

void __fastcall MenuYes(uint8_t* self, void*)
{
    if (g_Asking)
        Quit();
    else // the main menu's other question (back to the title)
        reinterpret_cast<PlainFn>(uintptr_t(kMenuButtonYes))(self, nullptr);
}

void __fastcall MenuNo(uint8_t* self, void*)
{
    if (g_Asking) {
        g_Asking = false;
        LOG_INFO("Quit menu: staying");
    } else {
        reinterpret_cast<PlainFn>(uintptr_t(kNothing))(self, nullptr);
    }
}

uint8_t* MakeMenuButton()
{
    uint8_t* handler = reinterpret_cast<uint8_t*(__cdecl*)()>(uintptr_t(kMakeMenuButton))();
    if (handler)
        *reinterpret_cast<uint32_t**>(handler) = g_MenuVtable;
    return handler;
}

uint8_t* __fastcall MakeMenuButtonSlot(uint8_t*, void*) { return MakeMenuButton(); }

// The main menu's intro: the Quit Game row comes in after the other four, as they do (without it, it is there from
// the start).
void __fastcall IntroStart(uint8_t* self, void*)
{
    reinterpret_cast<PlainFn>(uintptr_t(kIntroStart))(self, nullptr);
    uint8_t* row = FindHandler(self, kRowName);
    if (row)
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t**)>(uintptr_t(kListAppend))(
            self + kIntroRows, nullptr, &row);
}

// The screen after the title's row.
void __fastcall NavigationStart(uint8_t* self, void*)
{
    reinterpret_cast<PlainFn>(uintptr_t(kNavigationStart))(self, nullptr);
    if (!IsQuitRow(self))
        return;
    for (int i = 0; i < 4; ++i) {
        uint8_t* veil = FindHandler(self, kQuitVeils[i]);
        *reinterpret_cast<uint8_t**>(self + kNavigationVeils + 4 * i) = veil;
        if (veil)
            reinterpret_cast<PlainFn>(uintptr_t((*reinterpret_cast<uint32_t* const*>(veil))[kShowSlot]))(veil, nullptr);
    }
}

char __fastcall NavigationEvent(uint8_t* self, void*, int event, int value)
{
    if (event == kChosen && IsQuitRow(self)) {
        Ask(self);
        return 1;
    }
    return reinterpret_cast<EventFn>(uintptr_t(kNavigationEvent))(self, nullptr, event, value);
}

void __fastcall NavigationYes(uint8_t* self, void*)
{
    if (g_Asking)
        Quit();
    else
        reinterpret_cast<PlainFn>(uintptr_t(kNavigationYes))(self, nullptr);
}

void __fastcall NavigationNo(uint8_t* self, void*)
{
    if (g_Asking) {
        g_Asking = false;
        LOG_INFO("Quit menu: staying");
    } else {
        reinterpret_cast<PlainFn>(uintptr_t(kNavigationNo))(self, nullptr);
    }
}

uint8_t* MakeNavigation()
{
    uint8_t* handler = reinterpret_cast<uint8_t*(__cdecl*)()>(uintptr_t(kMakeNavigation))();
    if (handler)
        *reinterpret_cast<uint32_t**>(handler) = g_NavigationVtable;
    return handler;
}

uint8_t* __fastcall MakeNavigationSlot(uint8_t*, void*) { return MakeNavigation(); }

// --- The menus' files ---------------------------------------------------------------------------------------------

// An item: where it starts and where the next item (its first child, if it has one) starts.
bool ItemBlock(const std::vector<uint8_t>& data, const std::string& name, size_t& begin, size_t& end)
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

// The screen's number of top-level items: after a u32 of the file's own, the screen's name and its first item's name.
bool AddItems(std::vector<uint8_t>& data, uint32_t more)
{
    uint32_t nameLength, firstLength;
    if (data.size() < 12)
        return false;
    std::memcpy(&nameLength, data.data() + 4, 4);
    if (nameLength > 64 || data.size() < 12 + size_t(nameLength))
        return false;
    std::memcpy(&firstLength, data.data() + 8 + nameLength, 4);
    const size_t at = 12 + size_t(nameLength) + firstLength;
    if (firstLength > 64 || data.size() < at + 4)
        return false;
    uint32_t count;
    std::memcpy(&count, data.data() + at, 4);
    count += more;
    std::memcpy(data.data() + at, &count, 4);
    return true;
}

bool Lower(std::vector<uint8_t>& block, float step)
{
    float x, y;
    if (!MenuItemPosition(block, &x, &y, false))
        return false;
    y += step;
    return MenuItemPosition(block, &x, &y, true);
}

// Moves an item (its block, from ItemBlock) up or down.
bool MoveItem(std::vector<uint8_t>& data, const std::string& name, float dy)
{
    size_t begin, end;
    if (!ItemBlock(data, name, begin, end))
        return false;
    std::vector<uint8_t> block(data.begin() + begin, data.begin() + end);
    if (!Lower(block, dy))
        return false;
    std::copy(block.begin(), block.end(), data.begin() + begin);
    return true;
}

bool PatchMainMenu(std::vector<uint8_t>& data)
{
    size_t single, singleEnd, settings, settingsEnd, features, featuresEnd, existing, existingEnd;
    if (!MenuRowBlock(data, "singleplayer", single, singleEnd) || !MenuRowBlock(data, "settings", settings, settingsEnd) ||
        !MenuRowBlock(data, "features", features, featuresEnd) || MenuRowBlock(data, kRowName, existing, existingEnd) ||
        !(single < settings && settings < features)) {
        LOG_WARN("Quit menu: the main menu is not as expected; no Quit Game entry");
        return false;
    }
    const size_t nameAt = MenuString("screenItem").size();
    // the new row: Settings' with its bar, below Features; after its name: its text, down, up, its screen, its id,
    // its action
    std::vector<uint8_t> row(data.begin() + settings, data.begin() + settingsEnd);
    float x, y, fx, fy;
    MenuItemPosition(row, &x, &y, false);
    std::vector<uint8_t> featuresRow(data.begin() + features, data.begin() + featuresEnd);
    MenuItemPosition(featuresRow, &fx, &fy, false);
    y = fy + kMenuStep;
    MenuItemPosition(row, &x, &y, true);
    size_t rowEnd = row.size();
    bool ok = MenuReplaceString(row, 0, rowEnd, "settings", kRowName) &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "IDS_SETTINGS", kRowText) &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "features", "singleplayer") &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "multiplayer", "features") &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "MainMenuButton", kMenuAction);
    if (!ok) {
        LOG_WARN("Quit menu: the Settings row is not as expected; no Quit Game entry");
        return false;
    }
    // The row goes after Features' submenu (a row's submenu follows it): before the first item after the submenu
    // (the first of the decorations, "darktwobars")
    size_t decoration, decorationEnd;
    if (!ItemBlock(data, "darktwobars", decoration, decorationEnd) || decoration < featuresEnd) {
        LOG_WARN("Quit menu: the main menu's items are not as expected; no Quit Game entry");
        return false;
    }
    // the new row first (after the others), then Features' down and Single Player's up go to it (the later edit
    // first: it moves nothing before it)
    data.insert(data.begin() + decoration, row.begin(), row.end());
    size_t featuresLinks = featuresEnd;
    if (!MenuReplaceString(data, features + nameAt + 8, featuresLinks, "singleplayer", kRowName)) {
        LOG_WARN("Quit menu: the Features row's links are not as expected; no Quit Game entry");
        return false;
    }
    size_t singleLinks = singleEnd;
    if (!MenuReplaceString(data, single + nameAt + 8, singleLinks, "features", kRowName) || !AddItems(data, 1)) {
        LOG_WARN("Quit menu: the Single Player row's links are not as expected; no Quit Game entry");
        return false;
    }
    LOG_INFO("Quit menu: the main menu has a Quit Game entry");
    return true;
}

void PatchNewGame(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> copy = data;
    size_t newGame, newGameEnd, load, loadEnd, existing, existingEnd;
    if (!ItemBlock(copy, "newgame", newGame, newGameEnd) || !ItemBlock(copy, "loadgame", load, loadEnd) ||
        ItemBlock(copy, kRowName, existing, existingEnd) || newGame > load) {
        LOG_WARN("Quit menu: the New Game screen is not as expected; no Quit Game entry");
        return;
    }
    const size_t nameAt = MenuString("screenItem").size();
    // the new row: Load Game's, lower; after its name: its text, down (New Game), up (New Game), its screen, its id,
    // its action
    std::vector<uint8_t> row(copy.begin() + load, copy.begin() + loadEnd);
    size_t rowEnd = row.size();
    const std::vector<uint8_t> screen = MenuString("load_device");
    const std::vector<uint8_t> newGameString = MenuString("newgame");
    bool ok = Lower(row, kTitleStep) && MenuReplaceString(row, 0, rowEnd, "loadgame", kRowName) &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "IDS_LOAD_GAME", kRowText);
    // down "newgame" stays (the rows wrap round), up "newgame" becomes Load Game
    const size_t rowDown = ok ? MenuFind(row, newGameString, nameAt + 8, rowEnd) : std::string::npos;
    ok = rowDown != std::string::npos &&
         MenuReplaceString(row, rowDown + newGameString.size(), rowEnd, "newgame", "loadgame") &&
         MenuReplaceString(row, nameAt + 8, rowEnd, "NewGameNavigation", kTitleAction);
    const size_t screenAt = ok ? MenuFind(row, screen, nameAt + 8, rowEnd) : std::string::npos;
    if (!ok || screenAt == std::string::npos || screenAt + screen.size() + 4 > rowEnd) {
        LOG_WARN("Quit menu: the Load Game row is not as expected; no Quit Game entry");
        return;
    }
    std::memcpy(row.data() + screenAt + screen.size(), &kTitleRowId, 4);
    // its highlight items: Load Game's, as much lower
    std::vector<uint8_t> veils;
    for (int i = 0; i < 4; ++i) {
        size_t begin, end;
        if (!ItemBlock(copy, kVeils[i], begin, end)) {
            LOG_WARN("Quit menu: the New Game screen's highlights are not as expected; no Quit Game entry");
            return;
        }
        std::vector<uint8_t> veil(copy.begin() + begin, copy.begin() + end);
        size_t veilEnd = veil.size();
        if (!Lower(veil, kTitleStep) || !MenuReplaceString(veil, 0, veilEnd, kVeils[i], kQuitVeils[i])) {
            LOG_WARN("Quit menu: the New Game screen's highlights are not as expected; no Quit Game entry");
            return;
        }
        veils.insert(veils.end(), veil.begin(), veil.end());
    }
    // Load Game's down and New Game's up go to the new row (the later one first), the row after Load Game, the
    // highlights before the rows (after the screen's other highlights, as the game's own are)
    size_t loadLinks = loadEnd;
    if (!MenuReplaceString(copy, load + nameAt + 8, loadLinks, "newgame", kRowName)) {
        LOG_WARN("Quit menu: the Load Game row's links are not as expected; no Quit Game entry");
        return;
    }
    copy.insert(copy.begin() + loadLinks, row.begin(), row.end());
    size_t newGameLinks = newGameEnd;
    // New Game's: down "loadgame", then up "loadgame": the second becomes the new row
    const std::vector<uint8_t> loadString = MenuString("loadgame");
    const size_t down = MenuFind(copy, loadString, newGame + nameAt + 8, newGameLinks);
    if (down == std::string::npos ||
        !MenuReplaceString(copy, down + loadString.size(), newGameLinks, "loadgame", kRowName)) {
        LOG_WARN("Quit menu: the New Game row's links are not as expected; no Quit Game entry");
        return;
    }
    copy.insert(copy.begin() + newGame, veils.begin(), veils.end());
    bool moved = MoveItem(copy, kRowName, -kTitleRaise);
    for (const char* name : kTitleItems)
        moved = MoveItem(copy, name, -kTitleRaise) && moved;
    for (const char* name : kQuitVeils)
        moved = MoveItem(copy, name, -kTitleRaise) && moved;
    if (!moved || !AddItems(copy, 5)) {
        LOG_WARN("Quit menu: the New Game screen's header is not as expected; no Quit Game entry");
        return;
    }
    data.swap(copy);
    LOG_INFO("Quit menu: the New Game screen has a Quit Game entry");
}

} // namespace

void PatchQuitMainMenu(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> copy = data;
    if (PatchMainMenu(copy))
        data.swap(copy);
}

void InstallQuitMenu()
{
    g_Asking = false;
    const uint32_t* menu = reinterpret_cast<const uint32_t*>(uintptr_t(kMenuButtonVtable));
    const uint32_t* navigation = reinterpret_cast<const uint32_t*>(uintptr_t(kNavigationVtable));
    if (menu[4] != kMenuButtonEvent || menu[8] != kMenuButtonYes || menu[9] != kNothing ||
        menu[10] != kMakeMenuButton || navigation[0] != kNavigationStart || navigation[4] != kNavigationEvent ||
        navigation[8] != kNavigationYes || navigation[9] != kNavigationNo || navigation[10] != kMakeNavigation) {
        LOG_WARN("Quit menu: the menus' code is not as expected; no Quit Game entries");
        return;
    }
    std::memcpy(g_MenuVtable, menu, sizeof g_MenuVtable);
    g_MenuVtable[4] = uint32_t(reinterpret_cast<uintptr_t>(&MenuEvent));
    g_MenuVtable[8] = uint32_t(reinterpret_cast<uintptr_t>(&MenuYes));
    g_MenuVtable[9] = uint32_t(reinterpret_cast<uintptr_t>(&MenuNo));
    g_MenuVtable[10] = uint32_t(reinterpret_cast<uintptr_t>(&MakeMenuButtonSlot));
    std::memset(g_NavigationVtable, 0, sizeof g_NavigationVtable);
    std::memcpy(g_NavigationVtable, navigation, kNavigationSlots * 4);
    g_NavigationVtable[0] = uint32_t(reinterpret_cast<uintptr_t>(&NavigationStart));
    g_NavigationVtable[4] = uint32_t(reinterpret_cast<uintptr_t>(&NavigationEvent));
    g_NavigationVtable[8] = uint32_t(reinterpret_cast<uintptr_t>(&NavigationYes));
    g_NavigationVtable[9] = uint32_t(reinterpret_cast<uintptr_t>(&NavigationNo));
    g_NavigationVtable[10] = uint32_t(reinterpret_cast<uintptr_t>(&MakeNavigationSlot));
    SetMenuText(kRowText, L"Quit Game");
    SetMenuText(kTitleText, L"Quit Game");
    SetMenuText(kMessageText, L"Are you sure you want to quit the game?");
    AddShellHandler(kMenuAction, &MakeMenuButton);
    AddShellHandler(kTitleAction, &MakeNavigation);
    RegisterResourcePatch(kNewGame, &PatchNewGame);
    const uint32_t introSlot = kIntroVtable;
    if (*reinterpret_cast<const uint32_t*>(uintptr_t(introSlot)) == kIntroStart) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&IntroStart));
        PatchBytes(introSlot, &hook, 4);
    } else {
        LOG_WARN("Quit menu: the main menu's intro is not as expected; Quit Game shows without it");
    }
}

} // namespace swrots::game
