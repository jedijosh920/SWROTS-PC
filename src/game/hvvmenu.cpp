// Heroes vs Villains in the game's main menu (see hvvmenu.h).
//
// The main menu (interfc\front_end\xml\main_menu.xbl_xml, asked for as main_menu.xml) has under Multi Player a
// submenu (MainMenuSubMenu) of two rows, Versus and Cooperative: MainMenuSwitchScreenButton items, each a text id,
// its down and up neighbours and a screen to switch to (select_jedi, select_challenge). A third row is added, a copy
// of Cooperative, switching to the port's screen hvv_setup (the match's rules), whose Line-up row goes to hvv_lineup
// (a row a fighter: choosing one opens the Versus character select to pick it); the port knows it was this row that
// was chosen from the button's event (vtable 0x5D5580 slot 4, 0x2CBEC0; event 0x23 "chosen" switches the screen
// through 0xD4070).

#include "game/hvvmenu.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>

#include "core/log.h"
#include "core/patch.h"
#include "game/customchars.h"
#include "game/custommaps.h"
#include "game/devmap.h"
#include "kernel/kernel.h"
#include "game/hvv.h"
#include "game/menus.h"
#include "game/resources.h"
#include "game/roster.h"
#include "input/controls.h"

namespace swrots::game {

namespace {

constexpr const char* kMainMenu = "interfc\\front_end\\xml\\main_menu.xml";
constexpr const char* kRowName = "hvv";
constexpr const char* kRowText = "IDS_PORT_HVV";
constexpr const char* kBfRowName = "battlefront"; // Battlefront's row, under Heroes vs Villains
constexpr const char* kBfRowText = "IDS_PORT_BF";
constexpr const char* kBfScreen = "bf_setup";
constexpr const char* kBfFile = "interfc\\front_end\\xml\\bf_setup.xml";
constexpr const char* kSetupScreen = "hvv_setup";
constexpr const char* kLineupScreen = "hvv_lineup";
constexpr const char* kSetupFile = "interfc\\front_end\\xml\\hvv_setup.xml";
constexpr const char* kLineupFile = "interfc\\front_end\\xml\\hvv_lineup.xml";
constexpr const char* kSettingsFile = "interfc\\front_end\\xml\\settings.xml";
constexpr float kRowStep = 24.0f; // the submenu's rows

constexpr uint32_t kSwitchButtonVtable = 0x005D5580;
constexpr uint32_t kEventSlot = 4;
constexpr uint32_t kSwitchButtonEvent = 0x002CBEC0;
constexpr int kChosen = 0x23;

bool g_HvvMode = false;  // in the Heroes vs Villains screens (from its main menu row on)
int g_PickSlot = -1;     // the line-up's place the character select picks for, or -1

// The submenu's child count: the u32 right before its first child (the Versus row).
bool PatchMainMenu(std::vector<uint8_t>& data)
{
    size_t versus, versusEnd, coop, coopEnd, existing, existingEnd;
    if (!MenuRowBlock(data, "versus", versus, versusEnd) || !MenuRowBlock(data, "cooperative", coop, coopEnd) ||
        MenuRowBlock(data, kRowName, existing, existingEnd) || versus < 4) {
        LOG_WARN("HvV menu: the main menu is not as expected; no Heroes vs Villains entry");
        return false;
    }
    uint32_t children;
    std::memcpy(&children, data.data() + versus - 4, 4);
    if (children != 2) {
        LOG_WARN("HvV menu: the Multi Player submenu has %u rows, not 2; no Heroes vs Villains entry", children);
        return false;
    }
    // The submenu ends with 8 bytes of its own after its last row: Cooperative's row ends where its highlight bar
    // (as long as Versus's) does.
    const std::vector<uint8_t> item = MenuString("screenItem");
    const size_t versusBar = MenuFind(data, item, versus + 1, versusEnd);
    const size_t coopBar = MenuFind(data, item, coop + 1, coopEnd);
    if (versusBar == std::string::npos || coopBar == std::string::npos ||
        coopBar + (versusEnd - versusBar) > coopEnd) {
        LOG_WARN("HvV menu: the submenu's rows are not as expected; no Heroes vs Villains entry");
        return false;
    }
    coopEnd = coopBar + (versusEnd - versusBar);
    const size_t nameAt = MenuString("screenItem").size();
    // the new row: Cooperative's, one row lower, Versus's screen
    std::vector<uint8_t> row(data.begin() + coop, data.begin() + coopEnd);
    std::vector<uint8_t> bfRow = row; // Battlefront's: the same, one more row lower
    float x, y;
    MenuItemPosition(row, &x, &y, false);
    y += kRowStep;
    MenuItemPosition(row, &x, &y, true);
    {
        float bx, by;
        MenuItemPosition(bfRow, &bx, &by, false);
        by += 2 * kRowStep;
        MenuItemPosition(bfRow, &bx, &by, true);
    }
    size_t rowEnd = row.size();
    // after the text id: down (stays Versus: back to the top), then up (Cooperative)
    const std::vector<uint8_t> versusString = MenuString("versus");
    bool ok = MenuReplaceString(row, 0, rowEnd, "cooperative", kRowName) &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "IDS_COOPERATIVE", kRowText);
    const size_t rowDown = ok ? MenuFind(row, versusString, nameAt + 8, rowEnd) : std::string::npos;
    ok = rowDown != std::string::npos &&
         MenuReplaceString(row, rowDown + versusString.size(), rowEnd, "versus", "cooperative") &&
         MenuReplaceString(row, nameAt + 8, rowEnd, "select_challenge", kSetupScreen);
    // after the screen: the button's id (+0x84 at run time; Versus 2, Cooperative 3, which asks the profile
    // whether the co-op missions are open): Versus's (the character select it leads to is the Versus one)
    const std::vector<uint8_t> screen = MenuString(kSetupScreen);
    const size_t screenAt = ok ? MenuFind(row, screen, nameAt + 8, rowEnd) : std::string::npos;
    if (!ok || screenAt == std::string::npos || screenAt + screen.size() + 4 > rowEnd) {
        LOG_WARN("HvV menu: the Cooperative row is not as expected; no Heroes vs Villains entry");
        return false;
    }
    const uint32_t versusId = 2;
    std::memcpy(row.data() + screenAt + screen.size(), &versusId, 4);
    // Heroes vs Villains' down: Battlefront; Battlefront's: down Versus (the top), up Heroes vs Villains
    size_t rowLinks = row.size();
    ok = MenuReplaceString(row, nameAt + 8, rowLinks, "versus", kBfRowName);
    size_t bfEnd = bfRow.size();
    ok = ok && MenuReplaceString(bfRow, 0, bfEnd, "cooperative", kBfRowName) &&
         MenuReplaceString(bfRow, nameAt + 8, bfEnd, "IDS_COOPERATIVE", kBfRowText);
    const size_t bfDown = ok ? MenuFind(bfRow, versusString, nameAt + 8, bfEnd) : std::string::npos;
    ok = bfDown != std::string::npos &&
         MenuReplaceString(bfRow, bfDown + versusString.size(), bfEnd, "versus", kRowName) &&
         MenuReplaceString(bfRow, nameAt + 8, bfEnd, "select_challenge", kBfScreen);
    const std::vector<uint8_t> bfScreen = MenuString(kBfScreen);
    const size_t bfScreenAt = ok ? MenuFind(bfRow, bfScreen, nameAt + 8, bfEnd) : std::string::npos;
    if (!ok || bfScreenAt == std::string::npos || bfScreenAt + bfScreen.size() + 4 > bfEnd) {
        LOG_WARN("HvV menu: the rows are not as expected; no Battlefront entry");
        return false;
    }
    std::memcpy(bfRow.data() + bfScreenAt + bfScreen.size(), &versusId, 4);
    row.insert(row.end(), bfRow.begin(), bfRow.end());
    // Cooperative's down and Versus's up go to the new row (the later edits first: they move nothing before them)
    size_t coopLinks = coopEnd;
    if (!MenuReplaceString(data, coop + nameAt + 12, coopLinks, "versus", kRowName)) {
        LOG_WARN("HvV menu: the Cooperative row's links are not as expected; no Heroes vs Villains entry");
        return false;
    }
    data.insert(data.begin() + coopLinks, row.begin(), row.end());
    size_t versusLinks = versusEnd;
    // Versus's links: down "cooperative", then up "cooperative": the second becomes the new row
    const std::vector<uint8_t> coopString = MenuString("cooperative");
    const size_t down = MenuFind(data, coopString, versus + nameAt + 8, versusLinks);
    if (down == std::string::npos ||
        !MenuReplaceString(data, down + coopString.size(), versusLinks, "cooperative", kBfRowName)) {
        LOG_WARN("HvV menu: the Versus row's links are not as expected; no Heroes vs Villains entry");
        return false;
    }
    children += 2;
    std::memcpy(data.data() + versus - 4, &children, 4);
    LOG_INFO("HvV menu: the main menu has Heroes vs Villains and Battlefront entries");
    return true;
}

void PatchMainMenuResource(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> copy = data;
    if (PatchMainMenu(copy))
        data.swap(copy);
}

using EventFn = bool(__fastcall*)(uint8_t*, void*, int, int);

// The submenu's buttons: the row chosen (an item's data +4 is its name key) puts the select screen in Heroes vs
// Villains mode or takes it out.
bool __fastcall SwitchButtonEvent(uint8_t* self, void*, int event, int value)
{
    if (event == kChosen) {
        uint8_t* item = *reinterpret_cast<uint8_t**>(self + 8);
        const uint32_t key = item ? *reinterpret_cast<const uint32_t*>(item + 4) : 0;
        if (key == MenuKey(kRowName)) {
            g_HvvMode = true;
            g_PickSlot = -1;
            LOG_INFO("HvV menu: Heroes vs Villains chosen");
        } else if (key == MenuKey(kBfRowName)) {
            g_HvvMode = false;
            LOG_INFO("HvV menu: Battlefront chosen");
        } else if (key) {
            g_HvvMode = false;
        }
    }
    return reinterpret_cast<EventFn>(uintptr_t(kSwitchButtonEvent))(self, nullptr, event, value);
}

// --- The character select for a line-up's place ---------------------------------------------------------------------
// The select screen (JediList, vtable 0x5D68B8) passes player 1's events to 0x2D51B0 (thiscall (event), from
// 0x2D54B6): left/right (0x1F/0x20) move the cursor (+0x2C, a slot), A (0x23) picks (+0x28 set; then player 1 picks
// the opponent), B (0x26) takes the pick back. Picking for the line-up (g_PickSlot), A takes the fighter under the
// cursor for that place and B leaves it as it was; both go back to the line-up.
constexpr uint32_t kPlayer1EventCall = 0x002D54B6;
constexpr uint32_t kPlayer1Event = 0x002D51B0;
constexpr uint32_t kSelectVtable = 0x005D68B8;
constexpr uint32_t kSelectFrameSlot = 3;
constexpr uint32_t kSelectFrame = 0x002D56F0;
constexpr uint32_t kSelectSound = 0x2C; // vfunc, thiscall (sound id): 0x13 the pick's
constexpr int kLeft = 0x1F, kRight = 0x20, kUp = 0x21, kDown = 0x22, kB = 0x26;
constexpr int kX = 0x2B, kY = 0x2C;
constexpr int kWhite = 0x2A, kBlack = 0x29; // LB and RB on a PC pad
constexpr int kLeftTrigger = 0x28, kRightTrigger = 0x27;
constexpr int kMaxPerSide = 5; // the setup screen's team sizes, 1v1 to 5v5 (the console takes up to 8 a side)
constexpr int kLivesChoices[] = { 5, 10, 20, 35, 50, 75, 99 };
constexpr int kHealthChoices[] = { 150, 250, 400, 600, 1000 };

ULONGLONG g_SelectSeen = 0;
uint8_t* g_SelectScreen = nullptr;
int g_LivesChoice = 3, g_HealthChoice = 1;
bool g_Intro = true;
int g_PerSide = 4;
// the line-up: kMaxPerSide heroes, then kMaxPerSide villains (the first g_PerSide of each play)
std::string g_Lineup[2 * kMaxPerSide];
int g_MapChoice = 0; // 0 the dev map's floor, then the custom maps
// The Versus arenas (the missionlist's Duel levels): a duel's launch names one of them.
constexpr const char* kDuelArenas[] = { "u103_sep_throneroom_01", "u103_sep_throneroom_02", "u107_uta_grievous_01",
    "u107_uta_grievous_02", "u108_cor_palpatine_01", "u108_cor_palpatine_02", "u110_cor_training_00",
    "u110_cor_training_01", "u110_cor_training_02", "u140_mus_theduel_01", "u140_mus_theduel_02",
    "u150_mus_theduel_01", "u150_mus_theduel_02", "u170_ep4_deathstar_02" };

// --- Custom maps on the arena screen ---------------------------------------------------------------------------------
// The arena list (ArenaList, vtable 0x5D63D8): slot 5 (0x2D2AB0) builds its entries from the mission list's Duel rows
// (+0x5C {cap, count +0x60, data +0x64} of 16-byte entries {TString map, wide text (length first), TString, bool
// unlocked}, added by 0x2D29A0 (thiscall on +0x5C, the entry copied); +0x58 Random's row); the port adds one a custom
// map, the duel's level (u107_uta_grievous_01) with the map's name, after the open arenas and Random: the cursor
// (0x2C69F0) does not move onto a locked row, and the game opens its arenas in order (the locked ones last). A row's
// picture is SmallArenaTextSet[row] (0x2D2460: item +0x3C, set while it draws; a custom row's place in the set is
// swapped in for that draw), the large one LargeArenaTextSet[...] (0x2D2500, slot 0x8C: the cursor +0x20, the picture
// item +0x40, its record +0x1C), which the port points at its place for a custom row (PatchArenaScreen adds them
// after the game's); their pictures are the map's own (mods\maps\<id>\arena_small.stx 128x32, arena_large.stx 256x128) when
// it has them. Chosen (slot 0x88, 0x2D21A0), a custom row makes the duel's launch the dev map's duel on that map
// (VersusLaunch).
constexpr uint32_t kArenaVtable = 0x005D63D8;
constexpr uint32_t kArenaBuildSlot = 0x14, kArenaChosenSlot = 0x88, kArenaCursorSlot = 0x8C, kArenaRowSlot = 0x7C;
constexpr uint32_t kArenaBuild = 0x002D2AB0, kArenaChosen = 0x002D21A0, kArenaCursor = 0x002D2500, kArenaRow = 0x002D2460;
constexpr uint32_t kArenaAdd = 0x002D29A0;
constexpr uint32_t kStringInit = 0x00225100, kStringAssign = 0x00225A10, kStringFree = 0x00225230;
constexpr const char* kArenaLevel = "u107_uta_grievous_01";
int g_FirstCustomRow = -1; // the arena list's first custom map's row, -1 none
int g_VersusMap = 0;

std::wstring Wide(const std::string& text);

using ArenaFn = void(__fastcall*)(uint8_t*, void*);
using ArenaRowFn = void(__fastcall*)(uint8_t*, void*, uint32_t, float, int);

std::string MapPicture(const CustomMap& map, const char* name)
{
    const std::string file = "maps\\" + map.id + "\\" + name;
    return ModFileExists(file + ".stx") ? file : std::string();
}

void __fastcall ArenaBuildHook(uint8_t* list, void*)
{
    reinterpret_cast<ArenaFn>(uintptr_t(kArenaBuild))(list, nullptr);
    const std::vector<const CustomMap*> maps = CustomMapsFor(MapMode::Versus);
    g_FirstCustomRow = -1;
    if (maps.empty())
        return;
    // their rows: before the first locked one (Random's comes before it too), else last
    const int before = *reinterpret_cast<int*>(list + 0x60);
    int at = before;
    for (int i = 0; i < before; ++i)
        if (!(*reinterpret_cast<uint8_t**>(list + 0x64))[i * 16 + 0xC]) {
            at = i;
            break;
        }
    for (size_t i = 0; i < maps.size(); ++i) {
        const std::string id = "IDS_PORT_ARENA_" + std::to_string(i);
        SetMenuText(id.c_str(), Wide(maps[i]->name));
        struct {
            uint32_t map;
            const wchar_t* text;
            uint32_t spare;
            uint8_t unlocked, pad[3];
        } entry = {};
        reinterpret_cast<void(__fastcall*)(uint32_t*, void*)>(uintptr_t(kStringInit))(&entry.map, nullptr);
        reinterpret_cast<void(__fastcall*)(uint32_t*, void*)>(uintptr_t(kStringInit))(&entry.spare, nullptr);
        reinterpret_cast<void(__fastcall*)(uint32_t*, void*, const char*)>(uintptr_t(kStringAssign))(&entry.map, nullptr,
            kArenaLevel);
        entry.text = MenuTextBuffer(id.c_str());
        entry.unlocked = 1;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kArenaAdd))(list + 0x5C, nullptr, &entry);
        reinterpret_cast<void(__fastcall*)(uint32_t*, void*)>(uintptr_t(kStringFree))(&entry.map, nullptr);
        reinterpret_cast<void(__fastcall*)(uint32_t*, void*)>(uintptr_t(kStringFree))(&entry.spare, nullptr);
    }
    // appended; moved to their place (the entries are plain 16 bytes: their strings are pooled pointers)
    const int count = *reinterpret_cast<int*>(list + 0x60), added = count - before;
    uint8_t* entries = *reinterpret_cast<uint8_t**>(list + 0x64);
    if (added > 0 && at < before) {
        std::vector<uint8_t> moved(entries + before * 16, entries + count * 16);
        std::memmove(entries + (at + added) * 16, entries + at * 16, size_t(before - at) * 16);
        std::memcpy(entries + at * 16, moved.data(), moved.size());
        int& random = *reinterpret_cast<int*>(list + 0x58);
        if (random != 100 && random >= at)
            random += added;
    }
    g_FirstCustomRow = at;
    LOG_INFO("HvV menu: %d custom map(s) on the arena screen from row %d", added, g_FirstCustomRow);
    // the remembered cursor (the launch's +0x80) may name a row of another start's list
    int& cursor = *reinterpret_cast<int*>(list + 0x20);
    if (cursor < 0 || cursor >= count)
        cursor = 0;
}

// The custom map of an arena row, or null.
const CustomMap* RowMap(int row)
{
    const std::vector<const CustomMap*> maps = CustomMapsFor(MapMode::Versus);
    const int k = g_FirstCustomRow < 0 ? -1 : row - g_FirstCustomRow;
    return k >= 0 && k < int(maps.size()) ? maps[size_t(k)] : nullptr;
}

uint8_t* SetRecord(uint8_t* item, int index)
{
    uint8_t* data = item ? *reinterpret_cast<uint8_t**>(item + 8) : nullptr;
    uint8_t* set = data ? *reinterpret_cast<uint8_t**>(data + 0x2C) : nullptr;
    if (!set || index >= *reinterpret_cast<int*>(set + 8))
        return nullptr;
    return (*reinterpret_cast<uint8_t***>(set + 0xC))[index];
}

void __fastcall ArenaCursorHook(uint8_t* list, void*)
{
    reinterpret_cast<ArenaFn>(uintptr_t(kArenaCursor))(list, nullptr);
    const int row = *reinterpret_cast<int*>(list + 0x20);
    const CustomMap* map = RowMap(row);
    uint8_t* item = *reinterpret_cast<uint8_t**>(list + 0x40);
    if (!map || !item)
        return;
    uint8_t* record = SetRecord(item, 16 + (row - g_FirstCustomRow)); // after the game's 16 (locked, 14, Random)
    if (!record)
        return;
    *reinterpret_cast<uint8_t**>(item + 0x1C) = record;
    const std::string picture = MapPicture(*map, "arena_large");
    if (!picture.empty())
        DrawRecordWith(record, picture);
}

void __fastcall ArenaRowHook(uint8_t* list, void*, uint32_t context, float x, int row)
{
    const CustomMap* map = RowMap(row);
    uint8_t* item = *reinterpret_cast<uint8_t**>(list + 0x3C);
    uint8_t* data = item ? *reinterpret_cast<uint8_t**>(item + 8) : nullptr;
    uint8_t* set = data ? *reinterpret_cast<uint8_t**>(data + 0x2C) : nullptr;
    const int place = 15 + (row - g_FirstCustomRow); // after the game's 15 (14 arenas, Random)
    if (!map || !set || row >= *reinterpret_cast<int*>(set + 8) || place >= *reinterpret_cast<int*>(set + 8)) {
        reinterpret_cast<ArenaRowFn>(uintptr_t(kArenaRow))(list, nullptr, context, x, row);
        return;
    }
    // the row's draw takes the set's entry of its row: the custom map's place stands there for it
    uint8_t** records = *reinterpret_cast<uint8_t***>(set + 0xC);
    std::swap(records[row], records[place]);
    const std::string picture = MapPicture(*map, "arena_small");
    if (!picture.empty())
        DrawRecordWith(records[row], picture);
    reinterpret_cast<ArenaRowFn>(uintptr_t(kArenaRow))(list, nullptr, context, x, row);
    std::swap(records[row], records[place]);
}

void __fastcall ArenaChosenHook(uint8_t* list, void*)
{
    const CustomMap* map = RowMap(*reinterpret_cast<int*>(list + 0x20));
    g_VersusMap = 0;
    if (map)
        for (size_t i = 0; i < CustomMaps().size(); ++i)
            if (&CustomMaps()[i] == map)
                g_VersusMap = int(i) + 1;
    reinterpret_cast<ArenaFn>(uintptr_t(kArenaChosen))(list, nullptr);
}

const CustomMap* VersusMap()
{
    const auto& maps = CustomMaps();
    return g_VersusMap > 0 && g_VersusMap <= int(maps.size()) ? &maps[size_t(g_VersusMap - 1)] : nullptr;
}

// The game's launch of a Versus duel (map: one of its arenas) with a custom map chosen: the dev map's duel on it.
void VersusLaunch(std::string& arguments)
{
    const CustomMap* map = VersusMap();
    if (!map || arguments.find("hvv:") != std::string::npos)
        return;
    std::string words = " " + arguments + " ";
    std::string lower = words;
    for (char& c : lower)
        c = char(tolower(static_cast<unsigned char>(c)));
    for (const char* arena : kDuelArenas) {
        const std::string token = std::string(" map:") + arena + " ";
        const size_t at = lower.find(token);
        if (at == std::string::npos)
            continue;
        words.replace(at, token.size(), " map:u107_uta_grievous_01 ");
        arguments = words.substr(1, words.size() - 2) + " devmap:1 versus:1 custommap:" + map->id;
        LOG_INFO("Versus: the duel on %s", map->name.c_str());
        return;
    }
}

// The chosen map's id (empty: the dev map's floor).
std::string MapChoice()
{
    const std::vector<const CustomMap*> maps = CustomMapsFor(MapMode::HeroesVsVillains);
    return g_MapChoice > 0 && g_MapChoice <= int(maps.size()) ? maps[size_t(g_MapChoice - 1)]->id : std::string();
}

// --- The setup and line-up screens ---------------------------------------------------------------------------------
// Both are the front end's Settings screen (settings.xbl_xml: nine rows from y 138, 24 apart, each with a veil item
// behind it, then the footer) with rows of the port's own:
//   hvv_setup: Team Size, Lives, Health, Intro, Map: copies of the Subtitles row (an On/Off control,
//     SettingsScreenOnOffControl) with the action "HvvValue", showing the value as its On text; Line-up, Start: copies
//     of the Codes row (SettingsScreenButton) with the action "HvvButton". Codes and Restore Defaults go.
//   hvv_lineup: ten rows, "HvvButton" too: the team's heroes, then its villains (SlotOfRow); the rows past them are not
//     drawn and the others' links (the item's up and down neighbours, the keys of their names at +0x18 and +0x1C) are
//     set for the team size as they draw.
// A row's name tells the handlers which it is (handler +8 the item, its +4 the name's key).
struct ScreenRow {
    std::string name, textId;
    bool button;
};

std::vector<ScreenRow> SetupRows()
{
    return { { "hvvteam", "IDS_PORT_HVV_TEAM", false }, { "hvvlives", "IDS_PORT_HVV_LIVES", false },
        { "hvvhealth", "IDS_PORT_HVV_HEALTH", false }, { "hvvintro", "IDS_PORT_HVV_INTRO", false },
        { "hvvmap", "IDS_PORT_HVV_MAP", false }, { "hvvlineup", "IDS_PORT_HVV_LINEUP", true },
        { "hvvstart", "IDS_PORT_HVV_START", true } };
}

// Battlefront's setup: Map, Side, Troopers, Tickets, Heroes, Start.
std::vector<ScreenRow> BfRows()
{
    return { { "bfmap", "IDS_PORT_BF_MAP", false }, { "bfside", "IDS_PORT_BF_SIDE", false },
        { "bftroops", "IDS_PORT_BF_TROOPS", false }, { "bftickets", "IDS_PORT_BF_TICKETS", false },
        { "bfheroes", "IDS_PORT_BF_HEROES", false }, { "bfstart", "IDS_PORT_HVV_START", true } };
}

std::vector<ScreenRow> LineupRows()
{
    std::vector<ScreenRow> rows;
    for (int i = 0; i < 2 * kMaxPerSide; ++i)
        rows.push_back({ "hvvslot" + std::to_string(i), "IDS_PORT_HVV_SLOT" + std::to_string(i), true });
    return rows;
}

constexpr const char* kSettingsRows[] = { "controller", "vibration", "subtitles", "musicvolume", "effectsvolume",
    "dialoguevolume", "difficulty", "codes", "restoredefaults" };
constexpr const char* kSettingsVeils[] = { "controllerveil", "vibrationveil", "subtitlesveil", "mvolumeveil",
    "evolumeveil", "dvolumeveil", "difficultyveil", "codesveil", "restoreveil" };
constexpr float kSettingsStep = 24.0f;

// A top-level item without children (a veil): where it starts and where the next item starts.
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

// A row from a template row: its name, text, neighbours and action (the template's in the order they come: its name,
// text id, down, up, [the Codes row's screen,] action).
bool MakeRow(std::vector<uint8_t>& row, const std::string& templateName, const std::string& textId,
    const std::string& down, const std::string& up, const std::string& action, const ScreenRow& r, const std::string& rowDown,
    const std::string& rowUp, bool button)
{
    const size_t nameAt = MenuString("screenItem").size();
    size_t end = row.size();
    bool ok = MenuReplaceString(row, 0, end, templateName, r.name) &&
              MenuReplaceString(row, nameAt + 4, end, textId, r.textId) &&
              MenuReplaceString(row, nameAt + 4, end, down, rowDown) &&
              MenuReplaceString(row, nameAt + 4, end, up, rowUp);
    if (ok && button) // the Codes row's screen: none of the port's rows goes to it
        ok = MenuReplaceString(row, nameAt + 4, end, templateName, r.name);
    return ok && MenuReplaceString(row, nameAt + 4, end, action, button ? "HvvButton" : "HvvValue");
}

bool BuildScreen(std::vector<uint8_t>& data, const char* screenName, const std::vector<ScreenRow>& rows,
    const char* titleId)
{
    if (!ReadDiscResource(kSettingsFile, data))
        return false;
    size_t valueBegin, valueEnd, buttonBegin, buttonEnd, veilBegin, veilEnd;
    if (!MenuRowBlock(data, "subtitles", valueBegin, valueEnd) || !MenuRowBlock(data, "codes", buttonBegin, buttonEnd) ||
        !ItemBlock(data, "restoreveil", veilBegin, veilEnd)) {
        LOG_WARN("HvV menu: the Settings screen is not as expected; no %s screen", screenName);
        return false;
    }
    const std::vector<uint8_t> valueRow(data.begin() + valueBegin, data.begin() + valueEnd);
    const std::vector<uint8_t> buttonRow(data.begin() + buttonBegin, data.begin() + buttonEnd);
    const std::vector<uint8_t> veil(data.begin() + veilBegin, data.begin() + veilEnd);
    const int count = int(rows.size()), settingsRows = int(std::size(kSettingsRows));
    int itemDelta = 0;
    // more rows than Settings has: copies after its last row, and a veil each after its last veil (their places set
    // below with the others)
    for (int i = settingsRows; i < count; ++i) {
        size_t begin, end;
        if (!MenuRowBlock(data, kSettingsRows[settingsRows - 1], begin, end))
            return false;
        data.insert(data.begin() + end, buttonRow.begin(), buttonRow.end());
        if (!ItemBlock(data, kSettingsVeils[settingsRows - 1], begin, end))
            return false;
        std::vector<uint8_t> v = veil;
        float x, y;
        MenuItemPosition(v, &x, &y, false);
        y += kSettingsStep * float(i - settingsRows + 1);
        MenuItemPosition(v, &x, &y, true);
        size_t vEnd = v.size();
        MenuReplaceString(v, 0, vEnd, kSettingsVeils[settingsRows - 1], "hvvveil" + std::to_string(i));
        data.insert(data.begin() + end, v.begin(), v.end());
        itemDelta += 2;
    }
    // the rows, from the last back (a change moves only what follows it): the first nine replace Settings' rows in
    // place, the extra copies are replaced from the end
    std::vector<std::pair<size_t, size_t>> blocks;
    {
        size_t at = 0;
        for (int i = 0; i < std::max(count, settingsRows); ++i) {
            size_t begin, end;
            const std::string name = i < settingsRows ? kSettingsRows[i] : "codes";
            if (i < settingsRows) {
                if (!MenuRowBlock(data, name, begin, end))
                    return false;
            } else { // the copies: the next "codes" row after the last
                const std::vector<uint8_t> item = MenuString("screenItem");
                std::vector<uint8_t> what = item;
                const std::vector<uint8_t> n = MenuString("codes");
                what.insert(what.end(), n.begin(), n.end());
                begin = MenuFind(data, what, at, data.size());
                if (begin == std::string::npos)
                    return false;
                const size_t child = MenuFind(data, item, begin + 1, data.size());
                end = child == std::string::npos ? std::string::npos : MenuFind(data, item, child + 1, data.size());
                if (end == std::string::npos)
                    return false;
            }
            at = end;
            blocks.push_back({ begin, end });
        }
    }
    float x = 106.0f, firstY = 138.0f;
    for (int i = int(blocks.size()) - 1; i >= 0; --i) {
        const size_t begin = blocks[size_t(i)].first, end = blocks[size_t(i)].second;
        if (i >= count) { // a Settings row the screen does not need (its veil goes after: the veils come first)
            data.erase(data.begin() + begin, data.begin() + end);
            itemDelta -= 1;
            continue;
        }
        const ScreenRow& r = rows[size_t(i)];
        std::vector<uint8_t> row = r.button ? buttonRow : valueRow;
        const std::string down = rows[size_t((i + 1) % count)].name, up = rows[size_t((i + count - 1) % count)].name;
        const bool ok = r.button ? MakeRow(row, "codes", "IDS_SHELL_CODES", "restoredefaults", "difficulty",
                                       "SettingsScreenButton", r, down, up, true)
                                 : MakeRow(row, "subtitles", "IDS_SHELL_SUBTITLES", "musicvolume", "vibration",
                                       "SettingsScreenOnOffControl", r, down, up, false);
        if (!ok) {
            LOG_WARN("HvV menu: the Settings screen's rows are not as expected; no %s screen", screenName);
            return false;
        }
        float y = firstY + kSettingsStep * float(i);
        MenuItemPosition(row, &x, &y, true);
        data.erase(data.begin() + begin, data.begin() + end);
        data.insert(data.begin() + begin, row.begin(), row.end());
    }
    for (int i = count; i < settingsRows; ++i) {
        size_t vb, ve;
        if (ItemBlock(data, kSettingsVeils[i], vb, ve)) {
            data.erase(data.begin() + vb, data.begin() + ve);
            itemDelta -= 1;
        }
    }
    // the screen's name and its first item, then its number of items (after them), then the title
    size_t end = 64;
    if (!MenuReplaceString(data, 0, end, "settings", screenName) ||
        !MenuReplaceString(data, 0, end, "controller", rows[0].name)) {
        LOG_WARN("HvV menu: the Settings screen's header is not as expected; no %s screen", screenName);
        return false;
    }
    const size_t countAt = 4 + MenuString(screenName).size() + MenuString(rows[0].name).size();
    uint32_t items;
    std::memcpy(&items, data.data() + countAt, 4);
    items = uint32_t(int(items) + itemDelta);
    std::memcpy(data.data() + countAt, &items, 4);
    end = data.size();
    if (!MenuReplaceString(data, 0, end, "IDS_SETTINGS", titleId))
        LOG_WARN("HvV menu: the Settings screen's title is not as expected");
    return true;
}

bool GenerateScreens(const std::string& lowerName, std::vector<uint8_t>& data)
{
    bool ok = false;
    if (lowerName == kSetupFile)
        ok = BuildScreen(data, kSetupScreen, SetupRows(), "IDS_PORT_HVV_TITLE");
    else if (lowerName == kLineupFile)
        ok = BuildScreen(data, kLineupScreen, LineupRows(), "IDS_PORT_HVV_LINEUP_TITLE");
    else if (lowerName == kBfFile)
        ok = BuildScreen(data, kBfScreen, BfRows(), "IDS_PORT_BF_TITLE");
    return ok;
}

std::string MapChoice();

// A line-up row's place in g_Lineup: the team's heroes, then its villains; -1 past them.
int SlotOfRow(int row)
{
    if (row < g_PerSide)
        return row;
    return row < 2 * g_PerSide ? kMaxPerSide + row - g_PerSide : -1;
}

std::wstring Wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

// Battlefront's choices: its map (the dev map's floor, then the custom maps with command posts), side, troopers a
// side, tickets, heroes.
constexpr int kBfTroopChoices[] = { 4, 6, 8 };
constexpr int kBfTicketChoices[] = { 50, 100, 150, 200, 300 };
int g_BfMap = 0, g_BfSide = 0, g_BfTroops = 1, g_BfTickets = 1;
bool g_BfHeroes = true;

std::vector<const CustomMap*> BfMaps()
{
    std::vector<const CustomMap*> maps;
    for (const CustomMap* m : CustomMapsFor(MapMode::Battlefront))
        if (!m->commandPosts.empty())
            maps.push_back(m);
    return maps;
}

std::string BfMapChoice()
{
    const std::vector<const CustomMap*> maps = BfMaps();
    return g_BfMap > 0 && g_BfMap <= int(maps.size()) ? maps[size_t(g_BfMap - 1)]->id : std::string();
}

// The heroes each side can call: the game's, the built-in commanders, and the custom characters of that side.
void BfHeroes(std::vector<std::string>& republic, std::vector<std::string>& separatists)
{
    republic = { "IAnakin", "IObiwan", "IYoda", "ICody" };
    separatists = { "IGrievous", "IDooku", "IDroidCommander" };
    for (const CustomCharacter& custom : CustomCharacters()) {
        if (custom.builtIn)
            continue;
        std::vector<std::string>& pool = custom.side == 0 ? republic : separatists;
        if (pool.size() < 5)
            pool.push_back(custom.className);
    }
    if (separatists.size() < 5)
        separatists.push_back("IVader");
}

// The rows' texts: a value row's name is its text, its value the On text the control shows; a line-up row is the
// place and the fighter.
void UpdateTexts()
{
    {
        const std::string map = BfMapChoice();
        const CustomMap* custom = map.empty() ? nullptr : FindCustomMap(map);
        SetMenuText("IDS_PORT_BF_V_MAP", Wide(custom ? custom->name : "DEVELOPER MAP"));
        SetMenuText("IDS_PORT_BF_V_SIDE", g_BfSide == 0 ? L"REPUBLIC (CLONES)" : L"SEPARATISTS (DROIDS)");
        SetMenuText("IDS_PORT_BF_V_TROOPS", Wide(std::to_string(kBfTroopChoices[g_BfTroops]) + " A SIDE"));
        SetMenuText("IDS_PORT_BF_V_TICKETS", Wide(std::to_string(kBfTicketChoices[g_BfTickets])));
    }
    SetMenuText("IDS_PORT_HVV_V_TEAM", Wide(std::to_string(g_PerSide) + " VS " + std::to_string(g_PerSide)));
    SetMenuText("IDS_PORT_HVV_V_LIVES", Wide(std::to_string(kLivesChoices[g_LivesChoice])));
    SetMenuText("IDS_PORT_HVV_V_HEALTH", Wide(std::to_string(kHealthChoices[g_HealthChoice])));
    const std::string map = MapChoice();
    const CustomMap* custom = map.empty() ? nullptr : FindCustomMap(map);
    SetMenuText("IDS_PORT_HVV_V_MAP", Wide(custom ? custom->name : "DEVELOPER MAP"));
    for (int row = 0; row < 2 * kMaxPerSide; ++row) {
        const int slot = SlotOfRow(row);
        const std::string id = "IDS_PORT_HVV_SLOT" + std::to_string(row);
        if (slot < 0) {
            SetMenuText(id.c_str(), L"");
            continue;
        }
        const int side = slot / kMaxPerSide, place = slot % kMaxPerSide;
        SetMenuText(id.c_str(), Wide((side ? "VILLAIN " : "HERO ") + std::to_string(place + 1) + "   " +
            HvvDisplayName(g_Lineup[slot]) + (slot == 0 ? "  (YOU)" : "")));
    }
}

int RowOf(uint8_t* handler, const std::vector<ScreenRow>& rows)
{
    const uint8_t* item = *reinterpret_cast<uint8_t**>(handler + 8);
    if (!item)
        return -1;
    const uint32_t key = *reinterpret_cast<const uint32_t*>(item + 4);
    for (size_t i = 0; i < rows.size(); ++i)
        if (key == MenuKey(rows[i].name.c_str()))
            return int(i);
    return -1;
}

// The value rows: the game's On/Off control (made by 0x2D9690, vtable 0x5D7000; its value a byte at +0x23C; it shows
// the profile's subtitles setting, profile +0x2A, for a row made from Subtitles) with a vtable of the port's own: its
// draw (slot 3) shows the row's value as its On text, its events (slot 4) change the value (left/right, A), B goes
// back as any item does (0x2C62D0); slot 10 makes another, slots 27/28 (defaults, apply) do nothing.
constexpr uint32_t kMakeOnOff = 0x002D9690;
constexpr uint32_t kOnOffVtable = 0x005D7000;
constexpr uint32_t kMakeButton = 0x002D93F0; // SettingsScreenButton
constexpr uint32_t kButtonVtable = 0x005D6E10;
constexpr int kVtableSlots = 32;
constexpr uint32_t kItemEvent = 0x002C62D0;
constexpr uint32_t kProfiles = 0x007E8F24;
constexpr uint32_t kCurrentProfile = 0x0024CC50;
using DrawFn = char(__fastcall*)(uint8_t*, void*, uint32_t);
using ItemEventFn = char(__fastcall*)(uint8_t*, void*, int, int);
uint32_t g_ValueVtable[kVtableSlots], g_ButtonVtable[kVtableSlots];
DrawFn g_OnOffDraw = nullptr;
ItemEventFn g_OnOffEvent = nullptr, g_ButtonEvent = nullptr;

uint8_t* CurrentProfile()
{
    uint8_t* profiles = *reinterpret_cast<uint8_t**>(uintptr_t(kProfiles));
    return profiles ? reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(uintptr_t(kCurrentProfile))(profiles, nullptr)
                    : nullptr;
}

char __fastcall BfValueDraw(uint8_t* handler, uint32_t value, int row)
{
    static const char* const kTexts[] = { "IDS_PORT_BF_V_MAP", "IDS_PORT_BF_V_SIDE", "IDS_PORT_BF_V_TROOPS",
        "IDS_PORT_BF_V_TICKETS", nullptr };
    const bool on = row == 4 ? g_BfHeroes : true;
    handler[0x23C] = on ? 1 : 0;
    uint8_t* profile = CurrentProfile();
    const uint8_t saved = profile ? profile[0x2A] : 0;
    if (profile)
        profile[0x2A] = on ? 1 : 0;
    if (row < int(std::size(kTexts)) && kTexts[row])
        SetOnOffTexts(kTexts[row], kTexts[row]);
    const char result = g_OnOffDraw(handler, nullptr, value);
    SetOnOffTexts(nullptr, nullptr);
    if (profile)
        profile[0x2A] = saved;
    return result;
}

char __fastcall ValueDraw(uint8_t* handler, void*, uint32_t value)
{
    if (const int bfRow = RowOf(handler, BfRows()); bfRow >= 0 && bfRow < 5)
        return BfValueDraw(handler, value, bfRow);
    static const char* const kValueTexts[] = { "IDS_PORT_HVV_V_TEAM", "IDS_PORT_HVV_V_LIVES", "IDS_PORT_HVV_V_HEALTH",
        nullptr, "IDS_PORT_HVV_V_MAP" };
    const int row = RowOf(handler, SetupRows());
    if (row < 0 || row > 4)
        return g_OnOffDraw(handler, nullptr, value);
    const bool on = row == 3 ? g_Intro : true;
    handler[0x23C] = on ? 1 : 0;
    uint8_t* profile = CurrentProfile();
    const uint8_t saved = profile ? profile[0x2A] : 0;
    if (profile)
        profile[0x2A] = on ? 1 : 0;
    if (kValueTexts[row])
        SetOnOffTexts(kValueTexts[row], kValueTexts[row]);
    const char result = g_OnOffDraw(handler, nullptr, value);
    SetOnOffTexts(nullptr, nullptr);
    if (profile)
        profile[0x2A] = saved;
    return result;
}

void Step(int row, int by)
{
    const auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    switch (row) {
    case 0: g_PerSide = wrap(g_PerSide - 1 + by, kMaxPerSide) + 1; break;
    case 1: g_LivesChoice = wrap(g_LivesChoice + by, int(std::size(kLivesChoices))); break;
    case 2: g_HealthChoice = wrap(g_HealthChoice + by, int(std::size(kHealthChoices))); break;
    case 3: g_Intro = !g_Intro; break;
    case 4: g_MapChoice = wrap(g_MapChoice + by, int(CustomMapsFor(MapMode::HeroesVsVillains).size()) + 1); break;
    default: return;
    }
    UpdateTexts();
}

void BfStep(int row, int by)
{
    const auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    switch (row) {
    case 0: g_BfMap = wrap(g_BfMap + by, int(BfMaps().size()) + 1); break;
    case 1: g_BfSide = 1 - g_BfSide; break;
    case 2: g_BfTroops = wrap(g_BfTroops + by, int(std::size(kBfTroopChoices))); break;
    case 3: g_BfTickets = wrap(g_BfTickets + by, int(std::size(kBfTicketChoices))); break;
    case 4: g_BfHeroes = !g_BfHeroes; break;
    default: return;
    }
    UpdateTexts();
}

char __fastcall ValueEvent(uint8_t* handler, void*, int event, int value)
{
    if (const int bfRow = RowOf(handler, BfRows()); bfRow >= 0 && event != kB &&
        (event == kLeft || event == kRight || event == kChosen)) {
        g_OnOffEvent(handler, nullptr, event, value);
        BfStep(bfRow, event == kLeft ? -1 : 1);
        return 1;
    }
    const int row = RowOf(handler, SetupRows());
    if (event == kB)
        return reinterpret_cast<ItemEventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
    if (row >= 0 && (event == kLeft || event == kRight || event == kChosen)) {
        g_OnOffEvent(handler, nullptr, event, value); // its sound (its value is the port's, set as it draws)
        Step(row, event == kLeft ? -1 : 1);
        return 1;
    }
    return g_OnOffEvent(handler, nullptr, event, value);
}

void StartMatch()
{
    std::vector<std::string> lineup;
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < g_PerSide; ++i)
            lineup.push_back(g_Lineup[side * kMaxPerSide + i]);
    std::string error;
    if (!StartHvv(lineup, g_PerSide, 0, kLivesChoices[g_LivesChoice], kHealthChoices[g_HealthChoice], g_Intro,
            MapChoice(), error))
        LOG_WARN("HvV menu: the match cannot start: %s", error.c_str());
    else
        LOG_INFO("HvV menu: the match starts");
}

void StartBattlefrontMatch()
{
    BattlefrontSetup setup;
    setup.map = BfMapChoice();
    setup.side = g_BfSide;
    setup.troops = kBfTroopChoices[g_BfTroops];
    setup.tickets = kBfTicketChoices[g_BfTickets];
    setup.heroes = g_BfHeroes;
    BfHeroes(setup.republicHeroes, setup.separatistHeroes);
    std::string error;
    if (!StartBattlefront(setup, error))
        LOG_WARN("HvV menu: Battlefront cannot start: %s", error.c_str());
    else
        LOG_INFO("HvV menu: Battlefront starts");
}

char __fastcall ButtonEvent(uint8_t* handler, void*, int event, int value)
{
    if (RowOf(handler, BfRows()) == 5 && event == kChosen) {
        StartBattlefrontMatch();
        return 1;
    }
    const int setupRow = RowOf(handler, SetupRows());
    const int row = setupRow < 0 ? RowOf(handler, LineupRows()) : -1;
    const int slot = row >= 0 ? SlotOfRow(row) : -1;
    if (event == kB) {
        if (row >= 0) { // the line-up: back to the setup
            GoToShellScreen(kSetupScreen);
            return 1;
        }
        return reinterpret_cast<ItemEventFn>(uintptr_t(kItemEvent))(handler, nullptr, event, value);
    }
    if (event != kChosen)
        return g_ButtonEvent(handler, nullptr, event, value);
    if (setupRow == 5) {
        GoToShellScreen(kLineupScreen);
        return 1;
    }
    if (setupRow == 6) {
        StartMatch();
        return 1;
    }
    if (slot >= 0) {
        g_PickSlot = slot;
        GoToShellScreen("select_jedi");
        return 1;
    }
    return 1;
}

// The line-up rows' draw: a row past the team isn't drawn; the others lead to each other for the team size.
DrawFn g_ButtonDraw = nullptr;

char __fastcall ButtonDraw(uint8_t* handler, void*, uint32_t value)
{
    const int row = RowOf(handler, LineupRows());
    if (row < 0)
        return g_ButtonDraw(handler, nullptr, value);
    if (SlotOfRow(row) < 0)
        return 0;
    uint32_t* item = *reinterpret_cast<uint32_t**>(handler + 8);
    const int rows = 2 * g_PerSide;
    const std::string up = "hvvslot" + std::to_string((row + rows - 1) % rows);
    const std::string down = "hvvslot" + std::to_string((row + 1) % rows);
    item[0x18 / 4] = MenuKey(up.c_str());
    item[0x1C / 4] = MenuKey(down.c_str());
    return g_ButtonDraw(handler, nullptr, value);
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

// The pause screen during a match: Restart HvV and Quit HvV, their questions in the match's words.
uint32_t MatchTexts(uint32_t key)
{
    static const char* const kPairs[][2] = { { "IDS_PAUSE_RESTART_MISSION", "IDS_PORT_HVV_RESTART" },
        { "IDS_PAUSE_QUIT_MISSION", "IDS_PORT_HVV_QUIT" }, { "IDS_PAUSE_RESTART", "IDS_PORT_HVV_RESTART" },
        { "IDS_PAUSE_QUIT", "IDS_PORT_HVV_QUIT" }, { "IDS_PAUSE_RESTART_MESSAGE", "IDS_PORT_HVV_RESTART_MESSAGE" },
        { "IDS_PAUSE_QUIT_MESSAGE", "IDS_PORT_HVV_QUIT_MESSAGE" } };
    static uint32_t keys[std::size(kPairs)][2];
    if (!keys[0][0])
        for (size_t i = 0; i < std::size(kPairs); ++i)
            for (int k = 0; k < 2; ++k)
                keys[i][k] = MenuKey(kPairs[i][k]);
    if (!HvvActive())
        return key;
    static const char* const kBattlefront[] = { "IDS_PORT_BF_RESTART", "IDS_PORT_BF_QUIT", "IDS_PORT_BF_RESTART",
        "IDS_PORT_BF_QUIT", "IDS_PORT_HVV_RESTART_MESSAGE", "IDS_PORT_HVV_QUIT_MESSAGE" };
    for (size_t i = 0; i < std::size(keys); ++i)
        if (keys[i][0] == key)
            return BattlefrontActive() ? MenuKey(kBattlefront[i]) : keys[i][1];
    return key;
}

void InstallScreens()
{
    SetMenuText("IDS_PORT_HVV_RESTART", L"Restart HvV");
    SetMenuText("IDS_PORT_HVV_QUIT", L"Quit HvV");
    SetMenuText("IDS_PORT_BF_RESTART", L"Restart Battle");
    SetMenuText("IDS_PORT_BF_QUIT", L"Quit Battle");
    SetMenuText("IDS_PORT_HVV_RESTART_MESSAGE", L"Are you sure you want to restart the match? Its score will be lost.");
    SetMenuText("IDS_PORT_HVV_QUIT_MESSAGE", L"Are you sure you want to quit the match? Its score will be lost.");
    SetMenuTextRedirect(&MatchTexts);
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
    SetMenuText("IDS_PORT_HVV_TITLE", L"Heroes vs Villains");
    SetMenuText("IDS_PORT_HVV_LINEUP_TITLE", L"Line-up");
    SetMenuText("IDS_PORT_HVV_TEAM", L"Team Size");
    SetMenuText("IDS_PORT_HVV_LIVES", L"Lives");
    SetMenuText("IDS_PORT_HVV_HEALTH", L"Health");
    SetMenuText("IDS_PORT_HVV_INTRO", L"Intro");
    SetMenuText("IDS_PORT_HVV_MAP", L"Map");
    SetMenuText("IDS_PORT_HVV_LINEUP", L"Line-up");
    SetMenuText("IDS_PORT_HVV_START", L"Start");
    SetMenuText("IDS_PORT_BF_TITLE", L"Battlefront");
    SetMenuText("IDS_PORT_BF_MAP", L"Map");
    SetMenuText("IDS_PORT_BF_SIDE", L"Side");
    SetMenuText("IDS_PORT_BF_TROOPS", L"Troopers");
    SetMenuText("IDS_PORT_BF_TICKETS", L"Tickets");
    SetMenuText("IDS_PORT_BF_HEROES", L"Heroes");
    // the line-up's defaults, once (kept across the game's restarts, as the rules)
    if (g_Lineup[0].empty()) {
        // Versus fighters only (the select screen offers no others); a custom villain fourth when there is one
        static const char* const kDefaults[2 * kMaxPerSide] = { "IObiwan", "IYoda", "IMace", "ISerra", "ICinDrallig",
            "IVader", "IDooku", "IGrievous", "IAnakin", "IVader" };
        for (int i = 0; i < 2 * kMaxPerSide; ++i)
            g_Lineup[i] = kDefaults[i];
        for (const CustomCharacter& custom : CustomCharacters())
            if (custom.side == 1) {
                g_Lineup[kMaxPerSide + 3] = custom.className;
                break;
            }
    }
    UpdateTexts();
    AddShellHandler("HvvValue", &MakeValue);
    AddShellHandler("HvvButton", &MakeButton);
    AddShellScreen(kSetupFile);
    AddShellScreen(kLineupFile);
    AddShellScreen(kBfFile);
    RegisterResourceGenerator(&GenerateScreens);
}

using Player1EventFn = bool(__fastcall*)(uint8_t*, void*, int);
using FrameFn = void(__fastcall*)(uint8_t*, void*, uint32_t);

void Sound(uint8_t* screen, int id)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int)>(
        (*reinterpret_cast<void* const* const*>(screen))[kSelectSound / 4])(screen, nullptr, id);
}

bool __fastcall Player1EventHook(uint8_t* screen, void*, int event)
{
    g_SelectScreen = screen;
    if (!g_HvvMode || g_PickSlot < 0)
        return reinterpret_cast<Player1EventFn>(uintptr_t(kPlayer1Event))(screen, nullptr, event);
    if (event == kChosen) {
        const int slot = *reinterpret_cast<const int*>(screen + 0x2C);
        if (const char* className = SelectSlotClass(slot)) {
            g_Lineup[g_PickSlot] = className;
        } else { // Random: any fighter
            const std::vector<std::string> classes = HvvFighterClasses();
            if (!classes.empty())
                g_Lineup[g_PickSlot] = classes[size_t(std::rand()) % classes.size()];
        }
        Sound(screen, 0x13);
        UpdateTexts();
        g_PickSlot = -1;
        GoToShellScreen(kLineupScreen);
        return true;
    }
    if (event == kB) {
        g_PickSlot = -1;
        GoToShellScreen(kLineupScreen);
        return true;
    }
    if (event == kLeft || event == kRight)
        return reinterpret_cast<Player1EventFn>(uintptr_t(kPlayer1Event))(screen, nullptr, event);
    return true; // nothing else while picking for the line-up
}

void __fastcall SelectFrameHook(uint8_t* screen, void*, uint32_t arg)
{
    g_SelectScreen = screen;
    g_SelectSeen = GetTickCount64();
    if (!DrawSelectStrip(screen, arg))
        reinterpret_cast<FrameFn>(uintptr_t(kSelectFrame))(screen, nullptr, arg);
}

// --- Screen changes (for scripted menu tests' screen marks) ---------------------------------------------------------
// The front end changes screens through its transition object (0x2D1B80, thiscall (const TString* name): the name's
// pooled text is latched) and switches a screen's children (0xD4070, thiscall (const key*): the name's hash).
constexpr uint32_t kTransition = 0x002D1B80;
constexpr uint8_t kTransitionBytes[] = { 0x33, 0xC0, 0x56, 0x8B, 0x74, 0x24, 0x08 }; // xor eax, eax; push esi; mov esi, [esp+8]
constexpr uint32_t kChildSwitch = 0x000D4070;
constexpr uint8_t kChildSwitchBytes[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x2C }; // push esi; mov esi, ecx; mov eax, [esi+0x2C]
using ScreenFn = uint32_t(__fastcall*)(void*, void*, const uint32_t*);
ScreenFn g_Transition = nullptr, g_ChildSwitch = nullptr;

// A mark names the screen (its key is the name's hash), or gives the key itself ("0x..."): the transition's names are
// not always the screens' (the Versus row's select screen is 0x8DC51DC9).
bool ScreenMatches(const char* name, uint32_t key)
{
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
        return std::strtoul(name, nullptr, 16) == key;
    return MenuKey(name) == key;
}

uint32_t __fastcall TransitionHook(void* self, void*, const uint32_t* name)
{
    if (name) {
        LOG_DEBUG("Menus: screen 0x%08X", *name);
        input::NoteMenuScreen(&ScreenMatches, *name);
    }
    return g_Transition(self, nullptr, name);
}

uint32_t __fastcall ChildSwitchHook(void* self, void*, const uint32_t* key)
{
    if (key) {
        LOG_DEBUG("Menus: child screen 0x%08X", *key);
        input::NoteMenuScreen(&ScreenMatches, *key);
    }
    return g_ChildSwitch(self, nullptr, key);
}

uint8_t* Trampoline(uint32_t address, const uint8_t* bytes, uint32_t length)
{
    uint8_t* stub = AllocStub(length + 5);
    std::memcpy(stub, bytes, length);
    stub[length] = 0xE9;
    const int32_t back = int32_t(address + length) - int32_t(uintptr_t(stub) + length + 5);
    std::memcpy(stub + length + 1, &back, 4);
    return stub;
}

} // namespace

void InstallHvvMenu()
{
    InstallScreens();
    // once (the port's memory outlives the game's reboots); the jumps into every new image
    static uint8_t* transition = Trampoline(kTransition, kTransitionBytes, sizeof kTransitionBytes);
    static uint8_t* childSwitch = Trampoline(kChildSwitch, kChildSwitchBytes, sizeof kChildSwitchBytes);
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kTransition)), kTransitionBytes, sizeof kTransitionBytes) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kChildSwitch)), kChildSwitchBytes, sizeof kChildSwitchBytes) == 0) {
        g_Transition = reinterpret_cast<ScreenFn>(transition);
        g_ChildSwitch = reinterpret_cast<ScreenFn>(childSwitch);
        PatchJump(kTransition, reinterpret_cast<const void*>(&TransitionHook));
        PatchJump(kChildSwitch, reinterpret_cast<const void*>(&ChildSwitchHook));
    }
    g_HvvMode = false;
    g_PickSlot = -1;
    g_SelectSeen = 0;
    g_SelectScreen = nullptr;
    SetMenuText(kRowText, L"Heroes vs Villains");
    SetMenuText(kBfRowText, L"Battlefront");
    RegisterResourcePatch(kMainMenu, &PatchMainMenuResource);
    const uint32_t slot = kSwitchButtonVtable + kEventSlot * 4;
    const uint32_t frameSlot = kSelectVtable + kSelectFrameSlot * 4;
    uint8_t call[5] = { 0xE8 };
    const int32_t rel = int32_t(kPlayer1Event) - int32_t(kPlayer1EventCall + 5);
    std::memcpy(call + 1, &rel, 4);
    if (*reinterpret_cast<const uint32_t*>(uintptr_t(slot)) != kSwitchButtonEvent ||
        *reinterpret_cast<const uint32_t*>(uintptr_t(frameSlot)) != kSelectFrame ||
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kPlayer1EventCall)), call, 5) != 0) {
        LOG_WARN("HvV menu: the menus' code is not as expected; no Heroes vs Villains entry");
        return;
    }
    const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&SwitchButtonEvent));
    PatchBytes(slot, &hook, 4);
    const uint32_t frame = uint32_t(reinterpret_cast<uintptr_t>(&SelectFrameHook));
    PatchBytes(frameSlot, &frame, 4);
    PatchCall(kPlayer1EventCall, reinterpret_cast<const void*>(&Player1EventHook), 5);
    const auto slotIs = [](uint32_t slot, uint32_t function) {
        return *reinterpret_cast<const uint32_t*>(uintptr_t(kArenaVtable + slot)) == function;
    };
    if (slotIs(kArenaBuildSlot, kArenaBuild) && slotIs(kArenaChosenSlot, kArenaChosen) &&
        slotIs(kArenaCursorSlot, kArenaCursor) && slotIs(kArenaRowSlot, kArenaRow)) {
        const auto patch = [](uint32_t slot, const void* hook) {
            const uint32_t value = uint32_t(reinterpret_cast<uintptr_t>(hook));
            PatchBytes(kArenaVtable + slot, &value, 4);
        };
        patch(kArenaBuildSlot, reinterpret_cast<const void*>(&ArenaBuildHook));
        patch(kArenaChosenSlot, reinterpret_cast<const void*>(&ArenaChosenHook));
        patch(kArenaCursorSlot, reinterpret_cast<const void*>(&ArenaCursorHook));
        patch(kArenaRowSlot, reinterpret_cast<const void*>(&ArenaRowHook));
    } else {
        LOG_WARN("HvV menu: the arena screen's code is not as expected; no custom maps on it");
    }
    kernel::SetLaunchFilter(&VersusLaunch);
}

} // namespace swrots::game
