// The game's in-game menus, extended (see menus.h).

#include "game/menus.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "core/patch.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

// The game's string hash (cdecl (const char*) -> key): item, screen and text id names are keyed by it.
constexpr uint32_t kStringHash = 0x00222BA0;

// --- Texts ---
// The menus look a text id up in the language's string table (0x23B6E0, thiscall (TString* out, const key*),
// a binary search; `out` gets the text, or null when the table has no such id). A text is wide characters,
// its length first. An id of the
// port's own gets the port's text (a buffer kept for the session, so a menu item keeps showing what is
// written into it later).
constexpr uint32_t kTableFind = 0x0023B6E0;
constexpr uint8_t kTableFindBytes[] = { 0x53, 0x8B, 0x5C, 0x24, 0x0C }; // push ebx; mov ebx, [esp + 0xC]
constexpr size_t kTextCapacity = 96;
using TableFindFn = void*(__fastcall*)(uint8_t*, void*, uint32_t*, const uint32_t*);
TableFindFn g_OriginalTableFind = nullptr;
std::mutex g_TextLock;
std::unordered_map<uint32_t, std::unique_ptr<wchar_t[]>> g_Texts;

void* __fastcall TableFindHook(uint8_t* table, void*, uint32_t* out, const uint32_t* key)
{
    void* result = g_OriginalTableFind(table, nullptr, out, key);
    if (out && !*out && key) {
        std::lock_guard<std::mutex> lock(g_TextLock);
        auto it = g_Texts.find(*key);
        if (it != g_Texts.end())
            *out = uint32_t(reinterpret_cast<uintptr_t>(it->second.get()));
    }
    return result;
}

// --- Actions ---
// The in-game menus' actions are handlers registered by name: the HUD's setup (IVaderHUD 0x2A8600) appends
// {name, prototype} pairs to its list at +0x268 (0x157300); an item's action string picks the prototype,
// whose vtable slot 10 makes the item's own.
constexpr uint32_t kHudSetup = 0x002A8600;
constexpr uint8_t kHudSetupBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x0C };
constexpr uint32_t kHudHandlers = 0x268;
constexpr uint32_t kAddHandler = 0x00157300; // thiscall (const {name, prototype}*)
using HudSetupFn = void(__fastcall*)(uint8_t*, void*);
HudSetupFn g_OriginalHudSetup = nullptr;
struct Handler {
    std::string name;
    MenuHandlerMaker make;
};
std::vector<Handler> g_Handlers;

void __fastcall HudSetupHook(uint8_t* hud, void*)
{
    g_OriginalHudSetup(hud, nullptr);
    for (const Handler& handler : g_Handlers) {
        struct {
            const char* name;
            uint8_t* prototype;
        } entry = { handler.name.c_str(), handler.make() };
        if (entry.prototype)
            reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const void*)>(uintptr_t(kAddHandler))(hud + kHudHandlers,
                nullptr, &entry);
    }
}

// --- Screens ---
// gameinfo\guilist.txt (in each level's PAK) lists the in-game menus' screens, one compiled file a line; the
// GUI manager (IGuiManParse, 0xD5270) makes a screen of each.
constexpr char kGuiList[] = "gameinfo\\guilist.txt";
std::vector<std::string> g_Screens;

void PatchGuiList(std::vector<uint8_t>& data)
{
    std::string text(data.begin(), data.end());
    for (const std::string& screen : g_Screens) {
        // The level loads only what it declares: the screen (made by a generator, and decoded from memory
        // when the menus ask for it) is declared to it.
        constexpr int kMenuType = 31;
        if (!DeclareGeneratedResource(screen, kMenuType))
            LOG_WARN("Menus: %s could not be declared", screen.c_str());
        if (text.find(screen) != std::string::npos)
            continue;
        if (!text.empty() && text.back() != '\n')
            text += "\r\n";
        text += screen + "\r\n";
    }
    data.assign(text.begin(), text.end());
}

uint8_t* Trampoline(uint32_t address, const uint8_t* bytes, uint32_t length)
{
    uint8_t* stub = AllocStub(length + 5);
    std::memcpy(stub, bytes, length);
    stub[length] = 0xE9;
    const int32_t rel = int32_t(address + length) - int32_t(uintptr_t(stub) + length + 5);
    std::memcpy(stub + length + 1, &rel, 4);
    return stub;
}

} // namespace

uint32_t MenuKey(const char* name)
{
    return reinterpret_cast<uint32_t(__cdecl*)(const char*)>(uintptr_t(kStringHash))(name);
}

void SetMenuText(const char* id, const std::wstring& text)
{
    const uint32_t key = MenuKey(id);
    std::lock_guard<std::mutex> lock(g_TextLock);
    std::unique_ptr<wchar_t[]>& buffer = g_Texts[key];
    if (!buffer) {
        buffer.reset(new wchar_t[kTextCapacity]);
        buffer[0] = 0;
    }
    // As the string table has them: the length first, then the characters.
    const size_t length = std::min(text.size(), kTextCapacity - 2);
    buffer[0] = wchar_t(length);
    std::memcpy(buffer.get() + 1, text.data(), length * sizeof(wchar_t));
    buffer[length + 1] = 0;
}

void AddMenuHandler(const char* name, MenuHandlerMaker make)
{
    for (const Handler& handler : g_Handlers)
        if (handler.name == name)
            return;
    g_Handlers.push_back({ name, make });
}

void AddMenuScreen(const std::string& lowerXmlName)
{
    if (std::find(g_Screens.begin(), g_Screens.end(), lowerXmlName) == g_Screens.end())
        g_Screens.push_back(lowerXmlName);
}

void InstallMenus()
{
    const auto matches = [](uint32_t address, const uint8_t* bytes, size_t length) {
        return std::memcmp(reinterpret_cast<const void*>(uintptr_t(address)), bytes, length) == 0;
    };
    if (!matches(kTableFind, kTableFindBytes, sizeof(kTableFindBytes)) ||
        !matches(kHudSetup, kHudSetupBytes, sizeof(kHudSetupBytes))) {
        LOG_WARN("Menus: the menu code is not as expected; no menus of the port's own");
        return;
    }
    // The stubs live in the port's memory and survive reboots; the jumps are patched into every new image.
    static uint8_t* tableStub = Trampoline(kTableFind, kTableFindBytes, sizeof(kTableFindBytes));
    static uint8_t* hudStub = Trampoline(kHudSetup, kHudSetupBytes, sizeof(kHudSetupBytes));
    g_OriginalTableFind = reinterpret_cast<TableFindFn>(tableStub);
    g_OriginalHudSetup = reinterpret_cast<HudSetupFn>(hudStub);
    PatchJump(kTableFind, reinterpret_cast<const void*>(&TableFindHook));
    PatchJump(kHudSetup, reinterpret_cast<const void*>(&HudSetupHook));
    RegisterResourcePatch(kGuiList, &PatchGuiList);
}

std::vector<uint8_t> MenuString(const std::string& text)
{
    std::vector<uint8_t> out(4 + text.size());
    const uint32_t length = uint32_t(text.size());
    std::memcpy(out.data(), &length, 4);
    std::memcpy(out.data() + 4, text.data(), text.size());
    return out;
}

size_t MenuFind(const std::vector<uint8_t>& data, const std::vector<uint8_t>& what, size_t from, size_t to)
{
    to = std::min(to, data.size());
    for (size_t i = from; i + what.size() <= to; ++i)
        if (std::memcmp(data.data() + i, what.data(), what.size()) == 0)
            return i;
    return std::string::npos;
}

bool MenuReplaceString(std::vector<uint8_t>& data, size_t begin, size_t& end, const std::string& from, const std::string& to)
{
    const std::vector<uint8_t> a = MenuString(from), b = MenuString(to);
    const size_t at = MenuFind(data, a, begin, end);
    if (at == std::string::npos)
        return false;
    data.erase(data.begin() + at, data.begin() + at + a.size());
    data.insert(data.begin() + at, b.begin(), b.end());
    end = end - a.size() + b.size();
    return true;
}

bool MenuRowBlock(const std::vector<uint8_t>& data, const std::string& name, size_t& begin, size_t& end)
{
    const std::vector<uint8_t> item = MenuString("screenItem");
    std::vector<uint8_t> what = item;
    const std::vector<uint8_t> n = MenuString(name);
    what.insert(what.end(), n.begin(), n.end());
    begin = MenuFind(data, what, 0, data.size());
    if (begin == std::string::npos)
        return false;
    const size_t child = MenuFind(data, item, begin + 1, data.size());
    end = child == std::string::npos ? std::string::npos : MenuFind(data, item, child + 1, data.size());
    return end != std::string::npos;
}

bool MenuItemPosition(std::vector<uint8_t>& block, float* x, float* y, bool write)
{
    // "screenItem", the name, a u32, then x and y.
    const size_t nameAt = MenuString("screenItem").size();
    if (block.size() < nameAt + 4)
        return false;
    uint32_t nameLength;
    std::memcpy(&nameLength, block.data() + nameAt, 4);
    const size_t at = nameAt + 4 + nameLength + 4;
    if (block.size() < at + 8)
        return false;
    if (write) {
        std::memcpy(block.data() + at, x, 4);
        std::memcpy(block.data() + at + 4, y, 4);
    } else {
        std::memcpy(x, block.data() + at, 4);
        std::memcpy(y, block.data() + at + 4, 4);
    }
    return true;
}

} // namespace swrots::game
