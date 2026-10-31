#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// The game's own menus, extended by the port: texts of its own, actions (menu handlers) of its own and screens
// of its own in the in-game menus (the pause screen and its sub-screens). How the game's menus work is in
// docs/research/coop.md (the pause menu).

// At every boot, after the image is loaded.
void InstallMenus();

// The key the menus give a name (an item's, a screen's, a text id's): the game's string hash (0x222BA0).
uint32_t MenuKey(const char* name);

// A text for a text id the game does not have (an id of the port's own, "IDS_PORT_..."); it can be changed
// at any time and shows from the next frame on.
void SetMenuText(const char* id, const std::wstring& text);
// Such a text as the string table holds one (its length first, then the characters; it stays for the session), or
// null when it was never set.
const wchar_t* MenuTextBuffer(const char* id);
// Texts asked for by one id answered with another's (e.g. the keyboard's wording): `redirect` gets the id's key and
// gives the key to look up (the same for none). Several can be set (each its own ids).
void SetMenuTextRedirect(uint32_t (*redirect)(uint32_t key));

// A menu action of the port's own, for an item's action string: `make` makes a handler (the menu makes one
// for every item that names the action). Registered with the in-game menus' handlers (the HUD's list).
using MenuHandlerMaker = uint8_t* (*)();
void AddMenuHandler(const char* name, MenuHandlerMaker make);

// Adds a screen to the in-game menus (gameinfo\guilist.txt): its compiled file, asked for as
// "interfc\<name>.xml", must come from a resource patch or generator.
void AddMenuScreen(const std::string& lowerXmlName);

// The front end's twins (the shell: title, main menu, Versus...): a menu action of the port's own (registered with
// the shell's handlers), a screen (gameinfo\shelllist.txt; its compiled file, asked for as
// "interfc\front_end\xml\<name>.xml", from a resource generator), and a change of screen (as the game goes from the
// fighters' select to the arena's): false without the shell.
void AddShellHandler(const char* name, MenuHandlerMaker make);
void AddShellScreen(const std::string& lowerXmlName);
bool GoToShellScreen(const char* name);

// The text ids the game's On/Off control (SettingsScreenOnOffControl, draw 0x2D8D70) shows for its two values
// (IDS_SHELL_ON / IDS_SHELL_OFF); a control of the port's own sets its own around its draw (null: the game's).
void SetOnOffTexts(const char* on, const char* off);

// Compiled menus (.xbl_xml): strings are a u32 length and the text; an item is the string "screenItem", its
// name, a u32, its position (x, y floats), ... and its children.
std::vector<uint8_t> MenuString(const std::string& text);
size_t MenuFind(const std::vector<uint8_t>& data, const std::vector<uint8_t>& what, size_t from, size_t to);
// Replaces the first string `from` in [begin, end) with `to`; `end` moves with it. False when not there.
bool MenuReplaceString(std::vector<uint8_t>& data, size_t begin, size_t& end, const std::string& from, const std::string& to);
// A top-level item with one child (a menu row and its highlight bar): where it starts and where the next item
// starts. False when there is no such item.
bool MenuRowBlock(const std::vector<uint8_t>& data, const std::string& name, size_t& begin, size_t& end);
// An item's position (x, y) in a block that starts with the item.
bool MenuItemPosition(std::vector<uint8_t>& block, float* x, float* y, bool write);

// A front-end screen of the port's own made from the game's Settings screen (settings.xbl_xml: rows from y 138, 24
// apart, each with a veil behind it): `rows` in order, a value row a copy of Subtitles (an On/Off control) with the
// action `valueAction`, a button a copy of Codes with `buttonAction`; the title `titleId`. A row's name tells its
// handler which it is (handler +8 the item, its +4 the name's key, MenuKey).
struct ShellScreenRow {
    std::string name, textId;
    bool button;
};
bool BuildSettingsScreen(std::vector<uint8_t>& data, const char* screenName, const std::vector<ShellScreenRow>& rows,
    const char* titleId, const char* valueAction, const char* buttonAction);

} // namespace swrots::game
