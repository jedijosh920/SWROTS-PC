// Custom Missions in the game's main menu (see missionsmenu.h).
//
// The main menu (interfc\front_end\xml\main_menu.xbl_xml) has under Single Player a submenu of two rows, Story
// Missions ("adventure", to the screen select_mission) and Bonus Missions ("challenge", to select_bonus). A third row
// is added, a copy of Bonus Missions, switching to the port's screen select_custom: a copy of select_bonus made when
// asked for (a resource generator) with its own title, the Briefing prompt's text moved into the empty band under the
// mission's details to show its description, the Y button out of sight and room in its picture sets for every
// mission (roster.cpp, AddMissionPictures). Its list is the game's own BonusList handler (vtable 0x5D6758); while the
// row is the one chosen (the main menu's button event, hvvmenu.cpp) the port's hooks on its slots make it list the
// missions:
//   0x14 build (0x2D42D0): the game's entries (16 bytes: TString map, wide text (length first), TString "Player:X /
//        Location:Y", bool unlocked; added by 0x2D29A0, thiscall on +0x58 {cap, count +0x5C, data +0x60}) are
//        replaced by one a mission, then the list is reset again (0x2C6650: cursor +0x20 to 0, the cursor's update);
//   0x7C a row's draw (0x2C8F80, thiscall (context, x, row)): its picture is SmallArenaTextSet[row] (item +0x3C, the
//        set's records at [[item+8]+0x2C]+0xC): the mission's place (5 + row) is swapped in for the draw;
//   0x8C the cursor's update (0x2D3BE0): the large picture (item +0x40, record +0x1C), the player's name (+0x4C, a
//        text item: +0x1C its characters) and head (+0x50), the location (+0x28) and planet (+0x44) are the mission's;
//   0x88 chosen (0x2D35E0, the game's launch): starts the mission (StartMission);
//   0x10 events (0x2D34A0): Y (0x2C, the briefing) does nothing.
// A mission's pictures are its own (mods\missions\<id>\picture_small.stx 128x32, picture_large.stx 256x128,
// planet.stx 128x128), else its map's (mods\maps\<map>\arena_small.stx, arena_large.stx, planet.stx), else none (the
// place is left empty); its player's head is the game's select-screen head of that fighter or a custom character's
// own (interfc\front_end\s_selduel_<costume>head.stx), else none.

#include "game/missionsmenu.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "game/customchars.h"
#include "game/custommaps.h"
#include "game/menus.h"
#include "game/missions.h"
#include "game/resources.h"
#include "game/roster.h"

namespace swrots::game {

namespace {

constexpr const char* kRowName = "custommissions";
constexpr const char* kRowText = "IDS_PORT_CUSTOM_MISSIONS";
constexpr const char* kScreen = "select_custom";
constexpr const char* kScreenFile = "interfc\\front_end\\xml\\select_custom.xml";
constexpr const char* kBonusFile = "interfc\\front_end\\xml\\select_bonus.xml";
constexpr const char* kTitleText = "IDS_PORT_SELECT_CUSTOM_MISSION";
constexpr const char* kDescriptionText = "IDS_PORT_MISSION_DESCRIPTION";
constexpr const char* kPlayerText = "IDS_PORT_MISSION_PLAYER";
constexpr const char* kLocationText = "IDS_PORT_MISSION_LOCATION";
constexpr float kRowStep = 24.0f; // the submenu's rows
// the description: where the Briefing prompt's text goes (the screen's 640x480 coordinates: the band under the
// mission's details, from the large picture's left edge)
constexpr float kDescriptionX = 52.0f, kDescriptionY = 346.0f;
constexpr size_t kDescriptionLength = 56; // the characters that fit

constexpr uint32_t kBonusVtable = 0x005D6758;
constexpr uint32_t kEventSlot = 0x10, kBuildSlot = 0x14, kRowSlot = 0x7C, kChosenSlot = 0x88, kCursorSlot = 0x8C;
constexpr uint32_t kEvent = 0x002D34A0, kBuild = 0x002D42D0, kRow = 0x002C8F80, kChosen = 0x002D35E0,
    kCursor = 0x002D3BE0;
constexpr uint32_t kListReset = 0x002C6650;
// the shell ([0x7F637C], when its flags +0xC have 0x40000) finds a screen's item by name: thiscall (name, 0, 0)
constexpr uint32_t kShell = 0x007F637C, kFindItem = 0x002F5A00;
constexpr uint32_t kEntryAdd = 0x002D29A0;
constexpr uint32_t kStringInit = 0x00225100, kStringFree = 0x00225230;
constexpr int kY = 0x2C;
// the game's places in the picture sets (the missions' follow)
constexpr int kSmallPictures = 5, kLargePictures = 6, kHeads = 6, kPlanets = 5;

using ListFn = void(__fastcall*)(uint8_t*, void*);
using RowFn = void(__fastcall*)(uint8_t*, void*, uint32_t, float, int);
using EventFn = bool(__fastcall*)(uint8_t*, void*, int, int);

bool g_Hooked = false;     // the list's hooks are in (this start)
bool g_Custom = false;     // the bonus mission list lists the missions (the Custom Missions row was chosen)
size_t g_Listed = 0;       // the missions the list has (its rows; the screen's picture sets were made for these)
size_t g_ScreenMissions = 0; // the missions the screen's picture sets have places for (made once a session)

std::wstring Wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring(text.begin(), text.end());
    std::wstring wide(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

std::string Lower(std::string text)
{
    for (char& c : text)
        c = char(tolower(static_cast<unsigned char>(c)));
    return text;
}

// The game's fighters: their select-screen head (front end) and name.
struct GameFighter {
    const char* className;
    const char* head;
    const char* name;
};
constexpr GameFighter kFighters[] = {
    { "IAnakin", "s_selduel_anakinhead", "Anakin Skywalker" },
    { "IObiwan", "s_selduel_obihead", "Obi-Wan Kenobi" },
    { "IDooku", "s_selduel_dookuhead", "Count Dooku" },
    { "IGrievous", "s_selduel_grievoushead", "General Grievous" },
    { "IMace", "s_selduel_macehead", "Mace Windu" },
    { "ISerra", "s_selduel_serrahead", "Serra Keto" },
    { "ICinDrallig", "s_selduel_cinhead", "Cin Drallig" },
    { "IVader", "s_selduel_vaderhead", "Darth Vader" },
    { "IOldObiwan", "s_selduel_oldobihead", "Obi-Wan Kenobi" },
    { "IYoda", "s_selduel_yodahead", "Yoda" },
    { "IPalpatine", nullptr, "Darth Sidious" },
};
// the head a custom character's picture is drawn over (any the shell has: the cut Grievous bodyguard's)
constexpr const char* kStandInHead = "s_selduel_gbghead";

const GameFighter* FindFighter(const std::string& className)
{
    for (const GameFighter& fighter : kFighters)
        if (_stricmp(fighter.className, className.c_str()) == 0)
            return &fighter;
    return nullptr;
}

// What the screen shows of a mission.
struct MissionLook {
    std::string head;       // the HeadSet place's picture (a front-end picture), empty: none
    std::string headFile;   // a custom character's head, drawn over it
    std::string player, location;
    std::string small, large, planet; // their files (mods-relative, without .stx), empty: none
};

std::string Picture(const MissionInfo& mission, const char* own, const char* map)
{
    std::string file = mission.folder + "\\" + own;
    if (ModFileExists(file + ".stx"))
        return file;
    if (!mission.map.empty() && map) {
        file = "maps\\" + mission.map + "\\" + map;
        if (ModFileExists(file + ".stx"))
            return file;
    }
    return {};
}

MissionLook LookOf(const MissionInfo& mission)
{
    MissionLook look;
    if (const CustomCharacter* custom = FindCustomCharacter(mission.player.c_str())) {
        look.player = custom->name;
        const std::string file = "interfc\\front_end\\s_selduel_" + Lower(custom->costume) + "head";
        if (custom->builtIn || ModFileExists(file + ".stx")) {
            look.head = kStandInHead;
            look.headFile = file;
        }
    } else if (const GameFighter* fighter = FindFighter(mission.player)) {
        look.player = fighter->name;
        look.head = fighter->head ? fighter->head : "";
    } else {
        look.player = mission.player.size() > 1 && mission.player[0] == 'I' ? mission.player.substr(1) : mission.player;
    }
    const CustomMap* map = mission.map.empty() ? nullptr : FindCustomMap(mission.map);
    look.location = map ? map->name : "Developer Map";
    look.small = Picture(mission, "picture_small", "arena_small");
    look.large = Picture(mission, "picture_large", "arena_large");
    look.planet = Picture(mission, "planet", "planet");
    return look;
}

// --- The main menu's row --------------------------------------------------------------------------------------------

bool PatchSinglePlayer(std::vector<uint8_t>& data)
{
    size_t story, storyEnd, bonus, bonusEnd, existing, existingEnd;
    if (!MenuRowBlock(data, "adventure", story, storyEnd) || !MenuRowBlock(data, "challenge", bonus, bonusEnd) ||
        MenuRowBlock(data, kRowName, existing, existingEnd) || story < 4) {
        LOG_WARN("Missions menu: the main menu is not as expected; no Custom Missions entry");
        return false;
    }
    uint32_t children;
    std::memcpy(&children, data.data() + story - 4, 4);
    if (children != 2) {
        LOG_WARN("Missions menu: the Single Player submenu has %u rows, not 2; no Custom Missions entry", children);
        return false;
    }
    // the submenu's own bytes follow its last row: Bonus Missions' row ends where its highlight bar (as long as
    // Story Missions') does
    const std::vector<uint8_t> item = MenuString("screenItem");
    const size_t storyBar = MenuFind(data, item, story + 1, storyEnd);
    const size_t bonusBar = MenuFind(data, item, bonus + 1, bonusEnd);
    if (storyBar == std::string::npos || bonusBar == std::string::npos ||
        bonusBar + (storyEnd - storyBar) > bonusEnd) {
        LOG_WARN("Missions menu: the submenu's rows are not as expected; no Custom Missions entry");
        return false;
    }
    bonusEnd = bonusBar + (storyEnd - storyBar);
    const size_t nameAt = item.size();
    // the new row: Bonus Missions', one row lower; down Story Missions (the top), up Bonus Missions
    std::vector<uint8_t> row(data.begin() + bonus, data.begin() + bonusEnd);
    float x, y;
    MenuItemPosition(row, &x, &y, false);
    y += kRowStep;
    MenuItemPosition(row, &x, &y, true);
    size_t rowEnd = row.size();
    const std::vector<uint8_t> storyString = MenuString("adventure");
    bool ok = MenuReplaceString(row, 0, rowEnd, "challenge", kRowName) &&
              MenuReplaceString(row, nameAt + 8, rowEnd, "IDS_CHALLENGE_MISSIONS", kRowText);
    const size_t down = ok ? MenuFind(row, storyString, nameAt + 8, rowEnd) : std::string::npos;
    ok = down != std::string::npos &&
         MenuReplaceString(row, down + storyString.size(), rowEnd, "adventure", "challenge") &&
         MenuReplaceString(row, nameAt + 8, rowEnd, "select_bonus", kScreen);
    // after the screen: the button's id (Bonus Missions' 1 asks the profile whether the bonus missions are open;
    // Story Missions' 0)
    const std::vector<uint8_t> screen = MenuString(kScreen);
    const size_t screenAt = ok ? MenuFind(row, screen, nameAt + 8, rowEnd) : std::string::npos;
    if (!ok || screenAt == std::string::npos || screenAt + screen.size() + 4 > rowEnd) {
        LOG_WARN("Missions menu: the Bonus Missions row is not as expected; no Custom Missions entry");
        return false;
    }
    const uint32_t id = 0;
    std::memcpy(row.data() + screenAt + screen.size(), &id, 4);
    // Bonus Missions' down and Story Missions' up go to the new row (the later edits first: they move nothing
    // before them)
    size_t bonusLinks = bonusEnd;
    if (!MenuReplaceString(data, bonus + nameAt + 12, bonusLinks, "adventure", kRowName)) {
        LOG_WARN("Missions menu: the Bonus Missions row's links are not as expected; no Custom Missions entry");
        return false;
    }
    data.insert(data.begin() + bonusLinks, row.begin(), row.end());
    // Story Missions' links: down "challenge", then up "challenge": the second becomes the new row
    size_t storyLinks = storyEnd;
    const std::vector<uint8_t> bonusString = MenuString("challenge");
    const size_t storyDown = MenuFind(data, bonusString, story + nameAt + 12, storyLinks);
    if (storyDown == std::string::npos ||
        !MenuReplaceString(data, storyDown + bonusString.size(), storyLinks, "challenge", kRowName)) {
        LOG_WARN("Missions menu: the Story Missions row's links are not as expected; no Custom Missions entry");
        return false;
    }
    children += 1;
    std::memcpy(data.data() + story - 4, &children, 4);
    LOG_INFO("Missions menu: the main menu has a Custom Missions entry (%zu mission(s))", Missions().size());
    return true;
}

// --- The screen -----------------------------------------------------------------------------------------------------

// An item's start ("screenItem" and its name) in `data`, or npos.
size_t ItemAt(const std::vector<uint8_t>& data, const std::string& name)
{
    std::vector<uint8_t> what = MenuString("screenItem");
    const std::vector<uint8_t> n = MenuString(name);
    what.insert(what.end(), n.begin(), n.end());
    return MenuFind(data, what, 0, data.size());
}

bool MoveItem(std::vector<uint8_t>& data, const std::string& name, float x, float y)
{
    const size_t at = ItemAt(data, name);
    if (at == std::string::npos || at + 64 > data.size())
        return false;
    std::vector<uint8_t> block(data.begin() + at, data.begin() + at + 64);
    if (!MenuItemPosition(block, &x, &y, true))
        return false;
    std::memcpy(data.data() + at, block.data(), block.size());
    return true;
}

bool MakeScreen(std::vector<uint8_t>& data)
{
    if (!ReadDiscResource(kBonusFile, data)) {
        LOG_WARN("Missions menu: no select_bonus screen on the disc");
        return false;
    }
    size_t end = data.size();
    const size_t briefing = ItemAt(data, "briefing");
    bool ok = MenuReplaceString(data, 0, end, "select_bonus", kScreen) &&
              MenuReplaceString(data, 0, end, "IDS_SHELL_SELECT_BONUS_MISSION", kTitleText) &&
              briefing != std::string::npos;
    // the Briefing prompt's text shows the description, under the details; its button is out of sight
    end = data.size();
    ok = ok && MenuReplaceString(data, ItemAt(data, "briefing"), end, "IDS_SHELL_BRIEFING", kDescriptionText) &&
         MoveItem(data, "briefing", kDescriptionX, kDescriptionY) && MoveItem(data, "briefingbutton", 2000.0f, 2000.0f);
    // and draws like the details' texts (the location's): the two bytes its item differs in after the text id
    const std::vector<uint8_t> id = MenuString(kDescriptionText);
    const size_t idAt = ok ? MenuFind(data, id, ItemAt(data, "briefing"), data.size()) : std::string::npos;
    ok = ok && idAt != std::string::npos && idAt + id.size() + 30 <= data.size() && data[idAt + id.size()] == 5 &&
         data[idAt + id.size() + 29] == 0;
    if (ok) {
        data[idAt + id.size()] = 0;
        data[idAt + id.size() + 29] = 1;
    }
    if (!ok) {
        LOG_WARN("Missions menu: the select_bonus screen is not as expected; no Custom Missions screen");
        return false;
    }
    std::vector<std::string> heads;
    for (const MissionInfo& mission : Missions())
        heads.push_back(LookOf(mission).head);
    if (!AddMissionPictures(data, heads))
        return false;
    g_ScreenMissions = heads.size();
    return true;
}

bool GenerateScreen(const std::string& lowerName, std::vector<uint8_t>& data)
{
    return lowerName == kScreenFile && MakeScreen(data);
}

// --- The list -------------------------------------------------------------------------------------------------------

uint8_t** SetRecords(uint8_t* item, int& count)
{
    uint8_t* data = item ? *reinterpret_cast<uint8_t**>(item + 8) : nullptr;
    uint8_t* set = data ? *reinterpret_cast<uint8_t**>(data + 0x2C) : nullptr;
    count = set ? *reinterpret_cast<int*>(set + 8) : 0;
    return set ? *reinterpret_cast<uint8_t***>(set + 0xC) : nullptr;
}

// A set's record for a mission's place (after the game's `first`), or null.
uint8_t* MissionRecord(uint8_t* item, int first, int mission)
{
    int count;
    uint8_t** records = SetRecords(item, count);
    return records && first + mission < count ? records[first + mission] : nullptr;
}

// (in capitals: the details' font has wide-set small letters)
void SetText(uint8_t* item, const char* id, const std::string& text)
{
    std::wstring wide = Wide(text);
    CharUpperBuffW(wide.data(), DWORD(wide.size()));
    SetMenuText(id, wide);
    const wchar_t* buffer = MenuTextBuffer(id);
    if (item && buffer)
        *reinterpret_cast<const wchar_t**>(item + 0x1C) = buffer + 1;
}

uint8_t* ShellItem(const char* name)
{
    uint8_t* shell = *reinterpret_cast<uint8_t**>(uintptr_t(kShell));
    if (!shell || !(*reinterpret_cast<uint32_t*>(shell + 0xC) & 0x40000))
        return nullptr;
    return reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*, int, int)>(uintptr_t(kFindItem))(shell,
        nullptr, name, 0, 0);
}

// A description as the band under the details fits it: one line of up to kDescriptionLength characters, a longer
// one cut at a word with "...".
std::string OneLine(std::string text)
{
    if (text.size() <= kDescriptionLength)
        return text;
    size_t cut = text.rfind(' ', kDescriptionLength - 3);
    if (cut == std::string::npos || cut < kDescriptionLength / 2)
        cut = kDescriptionLength - 3;
    text.resize(cut);
    while (!text.empty() && (text.back() == ' ' || text.back() == ',' || text.back() == '.'))
        text.pop_back();
    return text + "...";
}

// The mission of a row, or null.
const MissionInfo* RowMission(int row)
{
    const std::vector<MissionInfo>& missions = Missions();
    return g_Custom && row >= 0 && size_t(row) < g_Listed && size_t(row) < missions.size() &&
        size_t(row) < g_ScreenMissions ? &missions[size_t(row)] : nullptr;
}

void __fastcall BuildHook(uint8_t* list, void*)
{
    reinterpret_cast<ListFn>(uintptr_t(kBuild))(list, nullptr);
    if (!g_Custom)
        return;
    const auto stringFree = reinterpret_cast<void(__fastcall*)(void*, void*)>(uintptr_t(kStringFree));
    const auto stringInit = reinterpret_cast<void(__fastcall*)(void*, void*)>(uintptr_t(kStringInit));
    int& count = *reinterpret_cast<int*>(list + 0x5C);
    uint8_t* entries = *reinterpret_cast<uint8_t**>(list + 0x60);
    for (int i = 0; entries && i < count; ++i) {
        stringFree(entries + i * 16 + 8, nullptr);
        stringFree(entries + i * 16, nullptr);
    }
    count = 0;
    const std::vector<MissionInfo>& missions = Missions();
    g_Listed = 0;
    for (size_t i = 0; i < missions.size() && i < g_ScreenMissions; ++i) {
        const std::string id = "IDS_PORT_MISSION_" + std::to_string(i);
        SetMenuText(id.c_str(), Wide(missions[i].name));
        struct {
            uint32_t map;
            const wchar_t* text;
            uint32_t spare;
            uint8_t unlocked, pad[3];
        } entry = {};
        stringInit(&entry.map, nullptr);
        stringInit(&entry.spare, nullptr);
        entry.text = MenuTextBuffer(id.c_str());
        entry.unlocked = 1;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kEntryAdd))(list + 0x58, nullptr,
            &entry);
        stringFree(&entry.spare, nullptr);
        stringFree(&entry.map, nullptr);
        ++g_Listed;
    }
    LOG_INFO("Missions menu: %zu mission(s) on the Custom Missions screen", g_Listed);
    reinterpret_cast<ListFn>(uintptr_t(kListReset))(list, nullptr);
}

void __fastcall RowHook(uint8_t* list, void*, uint32_t context, float x, int row)
{
    const MissionInfo* mission = RowMission(row);
    int count;
    uint8_t** records = SetRecords(*reinterpret_cast<uint8_t**>(list + 0x3C), count);
    const int place = kSmallPictures + row;
    if (!mission || !records || row >= count || place >= count) {
        reinterpret_cast<RowFn>(uintptr_t(kRow))(list, nullptr, context, x, row);
        return;
    }
    // the row's draw takes the set's entry of its row: the mission's place stands there for it
    std::swap(records[row], records[place]);
    DrawRecordWith(records[row], LookOf(*mission).small);
    reinterpret_cast<RowFn>(uintptr_t(kRow))(list, nullptr, context, x, row);
    std::swap(records[row], records[place]);
}

void __fastcall CursorHook(uint8_t* list, void*)
{
    reinterpret_cast<ListFn>(uintptr_t(kCursor))(list, nullptr);
    const int row = *reinterpret_cast<int*>(list + 0x20);
    const MissionInfo* mission = RowMission(row);
    if (!mission)
        return;
    const MissionLook look = LookOf(*mission);
    const auto picture = [&](uint32_t field, int first, const std::string& file) {
        uint8_t* item = *reinterpret_cast<uint8_t**>(list + field);
        uint8_t* record = MissionRecord(item, first, row);
        if (!record)
            return;
        *reinterpret_cast<uint8_t**>(item + 0x1C) = record;
        DrawRecordWith(record, file);
    };
    picture(0x40, kLargePictures, look.large);
    picture(0x44, kPlanets, look.planet);
    if (!look.head.empty()) {
        uint8_t* item = *reinterpret_cast<uint8_t**>(list + 0x50);
        if (uint8_t* record = MissionRecord(item, kHeads, row)) {
            *reinterpret_cast<uint8_t**>(item + 0x1C) = record;
            if (!look.headFile.empty())
                DrawRecordWith(record, look.headFile);
        }
    } else {
        picture(0x50, kHeads, std::string()); // none
    }
    SetText(*reinterpret_cast<uint8_t**>(list + 0x4C), kPlayerText, look.player);
    SetText(*reinterpret_cast<uint8_t**>(list + 0x28), kLocationText, look.location);
    SetText(ShellItem("briefing"), kDescriptionText, OneLine(mission->description));
}

void __fastcall ChosenHook(uint8_t* list, void*)
{
    if (!g_Custom) {
        reinterpret_cast<ListFn>(uintptr_t(kChosen))(list, nullptr);
        return;
    }
    const MissionInfo* mission = RowMission(*reinterpret_cast<int*>(list + 0x20));
    if (!mission)
        return;
    std::string error;
    if (StartMission(mission->id, error))
        LOG_INFO("Missions menu: %s chosen", mission->id.c_str());
    else
        LOG_WARN("Missions menu: %s cannot start: %s", mission->id.c_str(), error.c_str());
}

bool __fastcall EventHook(uint8_t* list, void*, int event, int value)
{
    if (g_Custom && event == kY)
        return true; // no briefing
    return reinterpret_cast<EventFn>(uintptr_t(kEvent))(list, nullptr, event, value);
}

} // namespace

void PatchMissionsMainMenu(std::vector<uint8_t>& data)
{
    if (!g_Hooked || Missions().empty())
        return;
    std::vector<uint8_t> copy = data;
    if (PatchSinglePlayer(copy))
        data.swap(copy);
}

void NoteMenuRowChosen(uint32_t key)
{
    const bool custom = key == MenuKey(kRowName);
    if (custom != g_Custom)
        LOG_INFO("Missions menu: %s", custom ? "Custom Missions chosen" : "the bonus mission list is the game's");
    g_Custom = custom;
}

void InstallMissionsMenu()
{
    g_Hooked = false;
    g_Custom = false;
    g_Listed = 0;
    SetMenuText(kRowText, L"Custom Missions");
    SetMenuText(kTitleText, L"Select Custom Mission");
    SetMenuText(kDescriptionText, L"");
    SetMenuText(kPlayerText, L"");
    SetMenuText(kLocationText, L"");
    AddShellScreen(kScreenFile);
    RegisterResourceGenerator(&GenerateScreen);
    const auto slotIs = [](uint32_t slot, uint32_t function) {
        return *reinterpret_cast<const uint32_t*>(uintptr_t(kBonusVtable + slot)) == function;
    };
    if (!slotIs(kEventSlot, kEvent) || !slotIs(kBuildSlot, kBuild) || !slotIs(kRowSlot, kRow) ||
        !slotIs(kChosenSlot, kChosen) || !slotIs(kCursorSlot, kCursor)) {
        LOG_WARN("Missions menu: the bonus mission screen's code is not as expected; no Custom Missions screen");
        return;
    }
    const auto patch = [](uint32_t slot, const void* hook) {
        const uint32_t value = uint32_t(reinterpret_cast<uintptr_t>(hook));
        PatchBytes(kBonusVtable + slot, &value, 4);
    };
    patch(kEventSlot, reinterpret_cast<const void*>(&EventHook));
    patch(kBuildSlot, reinterpret_cast<const void*>(&BuildHook));
    patch(kRowSlot, reinterpret_cast<const void*>(&RowHook));
    patch(kChosenSlot, reinterpret_cast<const void*>(&ChosenHook));
    patch(kCursorSlot, reinterpret_cast<const void*>(&CursorHook));
    g_Hooked = true;
}

} // namespace swrots::game
