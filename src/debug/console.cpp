#include "debug/console.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "core/log.h"
#include "core/patch.h"
#include "d3d/recorder.h"
#include "game/freecam.h"
#include "game/game.h"
#include "core/settings.h"
#include "game/characters.h"
#include "game/coop.h"
#include "game/versus.h"
#include "kernel/kernel.h"

namespace swrots::debug {

namespace {

constexpr size_t kMaxLines = 4000;

std::mutex g_Lock;
std::deque<ConsoleLine> g_Lines;
size_t g_Dropped = 0; // lines removed from the front, so indexes stay stable
std::vector<std::string> g_Queue;
bool g_Cleared = false;

void AddLines(LineKind kind, const char* text)
{
    // One log line per text line.
    std::lock_guard<std::mutex> lock(g_Lock);
    const char* p = text;
    while (true) {
        const char* end = p + strcspn(p, "\r\n");
        if (end != p || *end == '\0')
            g_Lines.push_back({ kind, std::string(p, end) });
        if (*end == '\0')
            break;
        p = end + 1;
        if (*p == '\0')
            break;
    }
    while (g_Lines.size() > kMaxLines) {
        g_Lines.pop_front();
        ++g_Dropped;
    }
}

void Print(LineKind kind, const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    AddLines(kind, text);
    if (kind == LineKind::Error)
        LOG_WARN("Console: %s", text); // the log has what the console showed, for reports
    else
        LOG_INFO("Console: %s", text);
}

// --- The engine's print slots (empty stubs in the Xbox build) ------------------------------------

// The engine prints either engine strings (TString: a pointer to the characters)
// or plain C strings; tell them apart by whether the first bytes read as text.
const char* TextOf(const void* arg)
{
    if (!arg)
        return nullptr;
    __try {
        auto* s = static_cast<const unsigned char*>(arg);
        bool text = true;
        for (int i = 0; i < 4 && s[i]; ++i)
            text &= (s[i] >= 0x20 && s[i] < 0x7F) || s[i] == '\t' || s[i] == '\n' || s[i] == '\r';
        if (text)
            return static_cast<const char*>(arg);
        const char* deref = *static_cast<const char* const*>(arg);
        volatile char probe = deref ? deref[0] : 0;
        (void)probe;
        return deref;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void EnginePrint(const void* text)
{
    const char* s = TextOf(text);
    if (!s)
        return;
    const size_t n = strlen(s);
    constexpr char kInvalid[] = ": invalid command";
    const bool error = n >= sizeof(kInvalid) - 1 && strcmp(s + n - (sizeof(kInvalid) - 1), kInvalid) == 0;
    AddLines(error ? LineKind::Error : LineKind::Output, s);
}

void __fastcall ConsolePrint(void* self, void* edx, const void* text)
{
    (void)self;
    (void)edx;
    EnginePrint(text);
}

// --- Engine objects -----------------------------------------------------------------------------

template <typename Fn> Fn Slot(const void* object, uint32_t offset)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(object))[offset / 4]);
}

// The engine's console object; also registers it as the global console the commands use.
uint8_t* Console()
{
    auto* holder = *reinterpret_cast<uint8_t**>(uintptr_t(game::kLaunchSettingsHolder));
    auto* console = holder ? *reinterpret_cast<uint8_t**>(holder + game::kConsoleInHolder) : nullptr;
    if (!console || *reinterpret_cast<uint32_t*>(console) != game::kConsoleVtable)
        return nullptr;
    auto** global = reinterpret_cast<uint8_t**>(uintptr_t(game::kConsole));
    if (*global != console) {
        *global = console;
        LOG_INFO("Console %p registered as the global console", console);
    }
    return console;
}

struct Entry {
    std::string name;
    uint8_t* value;
};

// The entries of one of the console's hash tables (registry or commands), sorted by name.
std::vector<Entry> TableEntries(uint8_t* owner)
{
    std::vector<Entry> entries;
    const uint32_t count = *reinterpret_cast<uint32_t*>(owner + 8);
    if (count == 0 || count > 1024)
        return entries;
    auto** buckets = reinterpret_cast<uint8_t**>(owner + game::kHashBuckets);
    for (uint32_t b = 0; b < count; ++b)
        for (uint8_t* node = buckets[b]; node; node = *reinterpret_cast<uint8_t**>(node + 8))
            if (const char* name = *reinterpret_cast<const char**>(node + 4))
                entries.push_back({ name, *reinterpret_cast<uint8_t**>(node) });
    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return entries;
}

uint8_t* FindVariable(uint8_t* registry, const char* name)
{
    return Slot<uint8_t*(__fastcall*)(void*, void*, const char*)>(registry, game::kRegistryFindSlot)(registry, nullptr, name);
}

// Variables can point at fields of game objects that are gone (e.g. between levels); reading
// them is guarded, like the console's own display would not be.
bool SafeType(uint8_t* var, char* out, size_t size)
{
    __try {
        const char* type = Slot<const char*(__fastcall*)(void*, void*)>(var, 0x08)(var, nullptr);
        strncpy_s(out, size, type ? type : "?", _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The value as the engine formats it (Get into an engine string).
bool SafeValue(uint8_t* var, char* out, size_t size)
{
    __try {
        uint32_t text = 0; // TString: one pointer
        reinterpret_cast<void(__fastcall*)(void*, void*)>(uintptr_t(game::kTStringCtor))(&text, nullptr);
        Slot<void(__fastcall*)(void*, void*, void*)>(var, 0x00)(var, nullptr, &text);
        strncpy_s(out, size, text ? reinterpret_cast<const char*>(uintptr_t(text)) : "", _TRUNCATE);
        reinterpret_cast<void(__fastcall*)(void*, void*)>(uintptr_t(game::kTStringDtor))(&text, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string VariableType(uint8_t* var)
{
    char text[64];
    return SafeType(var, text, sizeof(text)) ? text : "?";
}

std::string VariableValue(uint8_t* var)
{
    char text[512];
    return SafeValue(var, text, sizeof(text)) ? text : "(unreadable)";
}

void SetVariable(uint8_t* registry, const char* name, const char* value)
{
    Slot<void(__fastcall*)(void*, void*, const char*, const char*, int)>(registry, game::kRegistrySetSlot)(
        registry, nullptr, name, value, 1);
}

// --- Port-side commands ---------------------------------------------------------------------------
// The Xbox build's help, listvars and set do nothing (their table walks and the registry's set are
// stubs); these do the same through the parts that work.

std::vector<std::string> Words(const std::string& line)
{
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isspace(static_cast<unsigned char>(line[i])))
            ++i;
        size_t start = i;
        while (i < line.size() && !isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i > start)
            words.push_back(line.substr(start, i - start));
    }
    return words;
}

bool Contains(const std::string& text, const std::string& part)
{
    auto it = std::search(text.begin(), text.end(), part.begin(), part.end(),
        [](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); });
    return it != text.end();
}

void Help(uint8_t* console)
{
    Print(LineKind::Port, "Console commands (the port):");
    Print(LineKind::Output, "  set <variable> <value>   (also <variable>=<value>)");
    Print(LineKind::Output, "  get <variable>");
    Print(LineKind::Output, "  toggle <variable>        (on/off variables)");
    Print(LineKind::Output, "  listvars [text]          all variables, or those whose name contains text");
    Print(LineKind::Output, "  duelist [<slot> <class>] versus select slots, or put a class in one");
    Print(LineKind::Output, "  player [<class>|- [<costume>] [skin <set>] [mesh <mesh>|off]|off]");
    Print(LineKind::Output, "                           play as a character, costume or mesh (- = the level's own class)");
    Print(LineKind::Output, "  saber [red|green|blue|purple|<r> <g> <b>|off]  the player's saber colour, its own only");
    Print(LineKind::Output, "  spawn <class> [<costume>] [skin <set>] [mesh <mesh>] [ally|enemy|neutral|riot]");
    Print(LineKind::Output, "                           [scale <size>]: a character in front of the player");
    Print(LineKind::Output, "  scale [<size>] [spawned]  your size (or the last spawned character's), 1 being its own");
    Print(LineKind::Output, "  infiniteforce [on|off]   your Force stays full");
    Print(LineKind::Output, "  coop [on|off|input ..|death ..|camera ..|friendlyfire ..|boss ..|player2 ..|storysafety ..]  two players in story missions");
    Print(LineKind::Output, "  memory                   the game's memory use, and the characters spawned");
    Print(LineKind::Output, "  despawn                  remove the characters you spawned");
    Print(LineKind::Output, "  restart                  restart the mission");
    Print(LineKind::Output, "  autorestart [on|off]     whether player changes apply at once (live, or by restarting)");
    Print(LineKind::Output, "  variants <class>         a character class's costumes");
    Print(LineKind::Output, "  meshes [text]            the character meshes on the disc (those containing text)");
    Print(LineKind::Output, "  unlockprofile            unlock everything in the signed-in profile (it is saved with it)");
    Print(LineKind::Output, "  freecam [on|off]         free camera: fly the view, the player stands still");
    Print(LineKind::Output, "  screenshot [name]        save the game's picture (without this menu) to screenshots");
    Print(LineKind::Output, "  clear                    empties this window");
    Print(LineKind::Output, "  help");
    Print(LineKind::Port, "Game commands:");
    std::string line = " ";
    for (const Entry& e : TableEntries(console + game::kConsoleCommands)) {
        if (line.size() + e.name.size() > 90) {
            Print(LineKind::Output, "%s", line.c_str());
            line = " ";
        }
        line += " " + e.name;
    }
    Print(LineKind::Output, "%s", line.c_str());
}

void ListVars(uint8_t* console, const std::string& filter)
{
    int shown = 0;
    for (const Entry& e : TableEntries(console + game::kConsoleRegistry)) {
        if (!filter.empty() && !Contains(e.name, filter))
            continue;
        Print(LineKind::Output, "%-28s [%s] %s", e.name.c_str(), VariableType(e.value).c_str(),
            VariableValue(e.value).c_str());
        ++shown;
    }
    Print(LineKind::Port, "%d variable(s)", shown);
}

void Get(uint8_t* registry, const std::string& name)
{
    uint8_t* var = FindVariable(registry, name.c_str());
    if (!var) {
        Print(LineKind::Error, "%s: unknown variable", name.c_str());
        return;
    }
    Print(LineKind::Output, "%s [%s] %s", name.c_str(), VariableType(var).c_str(), VariableValue(var).c_str());
}

void Set(uint8_t* registry, const std::string& name, const std::string& value)
{
    uint8_t* var = FindVariable(registry, name.c_str());
    if (!var) {
        Print(LineKind::Error, "%s: unknown variable", name.c_str());
        return;
    }
    SetVariable(registry, name.c_str(), value.c_str());
    Print(LineKind::Output, "%s = %s", name.c_str(), VariableValue(var).c_str());
}

void Toggle(uint8_t* registry, const std::string& name)
{
    uint8_t* var = FindVariable(registry, name.c_str());
    if (!var || VariableType(var) != "boolean") {
        Print(LineKind::Error, "%s: not an on/off variable", name.c_str());
        return;
    }
    Set(registry, name, VariableValue(var) == "true" ? "false" : "true");
}

void Duelist(const std::vector<std::string>& words)
{
    if (words.size() == 3) {
        int slot = atoi(words[1].c_str());
        if (slot < 0 || slot >= game::kDuelistCount) {
            Print(LineKind::Error, "slot must be 0-%d", game::kDuelistCount - 1);
            return;
        }
        if (!game::SetDuelist(slot, words[2].c_str())) {
            Print(LineKind::Error, "%s: not a class the game knows", words[2].c_str());
            return;
        }
    } else if (words.size() != 1) {
        Print(LineKind::Error, "duelist [<slot> <class>]");
        return;
    }
    for (int i = 0; i < game::kDuelistCount; ++i)
        Print(LineKind::Output, "  %d  %s", i, game::Duelist(i));
}

void ShowPlayer()
{
    const char* current = game::PlayerClass();
    const std::string variant = game::PlayerVariantChoice(), mesh = game::PlayerMesh(), skin = game::PlayerSkin();
    if (!current && variant.empty() && mesh.empty()) {
        Print(LineKind::Output, "  player: each level's own");
        return;
    }
    Print(LineKind::Output, "  player: %s%s%s%s%s%s%s", current ? current : "the level's own class",
        variant.empty() ? "" : ", costume ", variant.c_str(), skin.empty() ? "" : ", texture set ", skin.c_str(),
        mesh.empty() ? "" : ", mesh ", mesh.c_str());
}

void Restart()
{
    if (!game::PlayerInLevel()) {
        Print(LineKind::Error, "no mission is running");
        return;
    }
    Print(LineKind::Output, "  restarting the mission...");
    kernel::RestartMission();
}

// After a change of character: the mission restarts with it, or it waits for the next level start.
// After a change of character: during a mission it happens at once, the new character taking the old
// one's place; when that cannot be done, the mission restarts with it (or, with autorestart off, it waits
// for the next level start).
void PlayerChanged()
{
    ShowPlayer();
    if (game::PlayerInLevel() && !game::PlayerObject()) {
        // A level without a story player (Versus creates its fighters its own way): nothing to change
        // or restart into.
        Print(LineKind::Output, "  (from the next story level: not in Versus)");
        return;
    }
    std::string error;
    if (game::PlayerInLevel() && game::RestartOnChange() && game::ReplacePlayer(error)) {
        Print(LineKind::Output, "  changed at once");
        return;
    }
    if (!error.empty())
        Print(LineKind::Output, "  not at once (%s): restarting instead", error.c_str());
    if (game::PlayerInLevel() && game::RestartOnChange())
        Restart();
    else if (game::PlayerInLevel())
        Print(LineKind::Output, "  (from the next level start: restart to see it, or autorestart on)");
    else
        Print(LineKind::Output, "  (from the next level start)");
}

// player [<class>|- [<costume>] [skin <set>] [mesh <mesh>|off] | mesh <mesh>|off | off]
void Player(const std::vector<std::string>& words)
{
    if (words.size() == 1) {
        ShowPlayer();
        const game::PlayerInfo now = game::CurrentPlayer();
        if (now.valid)
            Print(LineKind::Output, "  now: %s%s%s, health %.0f / %.0f%s, at %.0f %.0f %.0f", now.className.c_str(),
                now.costume.empty() ? "" : " ", now.costume.c_str(), now.health, now.maxHealth,
                now.hasPower ? (", Force " + std::to_string(int(now.power))).c_str() : "", now.position[0],
                now.position[1], now.position[2]);
        return;
    }
    if (_stricmp(words[1].c_str(), "off") == 0 && words.size() == 2) {
        game::SetPlayerClass(nullptr);
        PlayerChanged();
        return;
    }
    // The mesh, from "mesh <mesh>" anywhere after the class.
    std::string mesh, skin;
    bool meshGiven = false;
    std::vector<std::string> rest;
    for (size_t i = 1; i < words.size(); ++i) {
        if (_stricmp(words[i].c_str(), "skin") == 0) {
            if (i + 1 >= words.size()) {
                Print(LineKind::Error, "skin <set>: a texture set's number or name (see variants <class>)");
                return;
            }
            skin = words[++i];
        } else if (_stricmp(words[i].c_str(), "mesh") == 0) {
            if (i + 1 >= words.size()) {
                Print(LineKind::Error, "mesh <mesh>: a mesh name (see meshes), or off");
                return;
            }
            meshGiven = true;
            if (_stricmp(words[i + 1].c_str(), "off") != 0) {
                std::string error;
                mesh = game::ResolveMesh(words[i + 1], error);
                if (mesh.empty()) {
                    Print(LineKind::Error, "%s", error.c_str());
                    return;
                }
            }
            ++i;
        } else {
            rest.push_back(words[i]);
        }
    }
    if (!rest.empty() && rest[0] == "-") {
        // Each level's own class (and so its own costume).
        if (rest.size() > 1 || !skin.empty()) {
            Print(LineKind::Error, "a costume or texture set needs a class: player <class> <costume> skin <set>");
            return;
        }
        game::SetPlayerClass(nullptr);
    } else if (!rest.empty()) {
        const char* name = game::RegisteredClassName(rest[0].c_str());
        if (!name) {
            Print(LineKind::Error, "%s: not a class the game knows", rest[0].c_str());
            return;
        }
        if (rest.size() > 2) {
            Print(LineKind::Error, "player <class> [<costume>] [mesh <mesh>]");
            return;
        }
        std::string variant = rest.size() == 2 ? rest[1] : "";
        if (!variant.empty() && game::FindVariantList(name) && game::ClassVariantIndex(name, variant) < 0) {
            Print(LineKind::Error, "%s has no costume '%s' (see variants %s)", name, variant.c_str(), name);
            return;
        }
        if (!skin.empty() && game::ClassTextureSetIndex(name, skin) < 0) {
            Print(LineKind::Error, "%s has no texture set '%s' (see variants %s)", name, skin.c_str(), name);
            return;
        }
        if (!game::ClassHasBody(name)) {
            Print(LineKind::Error, "%s was cut from the game: none of its costumes is on the disc (see variants %s), "
                "and it crashes the game as the player", name, name);
            return;
        }
        game::SetPlayerClass(name);
        game::SetPlayerVariant(variant);
        game::SetPlayerSkin(skin);
        if (!meshGiven)
            game::SetPlayerMesh("");
    }
    if (meshGiven) {
        game::SetPlayerMesh(mesh);
        if (!mesh.empty() && !game::CharacterMeshes("").empty()) {
            bool onDisc = false;
            for (const std::string& m : game::CharacterMeshes(mesh))
                onDisc = onDisc || m == mesh;
            if (!onDisc)
                Print(LineKind::Output, "  %s is not on the disc: a loose copy under mods\\meshes\\chars\\ must provide it",
                    mesh.c_str());
        }
    }
    PlayerChanged();
}

void Saber(const std::vector<std::string>& words)
{
    if (words.size() == 2 && _stricmp(words[1].c_str(), "off") == 0) {
        game::SetPlayerSaberColor(nullptr);
        Print(LineKind::Output, "  saber: the game's colours (from the next level start)");
        return;
    }
    if (words.size() > 1) {
        std::string spec = words[1];
        for (size_t i = 2; i < words.size(); ++i)
            spec += " " + words[i];
        float rgb[3];
        if (!game::ParseSaberColor(spec, rgb)) {
            Print(LineKind::Error, "saber red|green|blue|purple, saber <r> <g> <b> (0-255), or saber off");
            return;
        }
        game::SetPlayerSaberColor(rgb);
    }
    float rgb[3];
    if (game::PlayerSaberColor(rgb))
        Print(LineKind::Output, "  saber: %d %d %d, the player's only (Jedi-like characters: those with sabers)",
            int(rgb[0] * 255 + 0.5f), int(rgb[1] * 255 + 0.5f), int(rgb[2] * 255 + 0.5f));
    else
        Print(LineKind::Output, "  saber: the game's colours");
}

// spawn <class> [<costume>] [skin <set>] [mesh <mesh>] [ally|enemy] [scale <size>]. A body of another class's is worn
// as a private copy (see game::PrivateBodyName), so the characters wearing the original keep theirs.
void Spawn(const std::vector<std::string>& words)
{
    std::string costume, skin, mesh;
    game::SpawnSide side = game::SpawnSide::Default;
    float scale = 0.0f;
    std::vector<std::string> rest;
    for (size_t i = 1; i < words.size(); ++i) {
        if (_stricmp(words[i].c_str(), "scale") == 0 && i + 1 < words.size()) {
            scale = float(atof(words[++i].c_str()));
            continue;
        }
        static const struct { const char* name; game::SpawnSide side; } kSides[] = { { "ally", game::SpawnSide::Ally },
            { "enemy", game::SpawnSide::Enemy }, { "neutral", game::SpawnSide::Neutral }, { "riot", game::SpawnSide::Riot } };
        bool isSide = false;
        for (const auto& s : kSides)
            if (_stricmp(words[i].c_str(), s.name) == 0) {
                side = s.side;
                isSide = true;
            }
        if (isSide)
            continue;
        const bool isSkin = _stricmp(words[i].c_str(), "skin") == 0;
        const bool isMesh = _stricmp(words[i].c_str(), "mesh") == 0;
        if ((isSkin || isMesh) && i + 1 < words.size()) {
            (isSkin ? skin : mesh) = words[i + 1];
            ++i;
        } else {
            rest.push_back(words[i]);
        }
    }
    if (rest.empty() || rest.size() > 2) {
        Print(LineKind::Error, "spawn <class> [<costume>] [skin <set>] [mesh <mesh>] [ally|enemy|neutral|riot] [scale <size>]");
        return;
    }
    const char* name = game::RegisteredClassName(rest[0].c_str());
    if (name && rest.size() == 2 && game::FindVariantList(name) && game::ClassVariantIndex(name, rest[1]) < 0) {
        Print(LineKind::Error, "%s has no costume '%s' (see variants %s)", name, rest[1].c_str(), name);
        return;
    }
    if (name && !skin.empty() && game::ClassTextureSetIndex(name, skin) < 0) {
        Print(LineKind::Error, "%s has no texture set '%s' (see variants %s)", name, skin.c_str(), name);
        return;
    }
    std::string error;
    if (game::SpawnCharacter(rest[0].c_str(), rest.size() == 2 ? rest[1] : "", skin, mesh, side, error)) {
        if (scale >= 0.05f && scale <= 20.0f)
            game::SetCharacterScale(game::LastSpawnedObject(), scale);
        Print(LineKind::Output, "  spawned %s", name);
    }
    else
        Print(LineKind::Error, "%s", error.c_str());
}

// peek <hex offset> [count] [spawned]: the player's (or the last spawned character's) dwords from
// there, as hex and as floats (research).
bool ReadDword(uintptr_t a, uint32_t& v);

void Peek(const std::vector<std::string>& words)
{
    // "spawned": the last spawned character; "at <hex address>" as the 4th word: any address.
    const bool spawned = words.size() > 3 && _stricmp(words[3].c_str(), "spawned") == 0;
    const bool at = words.size() > 4 && _stricmp(words[3].c_str(), "at") == 0;
    uint8_t* player = spawned ? game::LastSpawnedObject() : game::PlayerObject();
    if (at) {
        // "at <hex>" an address; "at *<hex>" the pointer stored there.
        const bool deref = words[4][0] == '*';
        uint32_t address = uint32_t(strtoul(words[4].c_str() + (deref ? 1 : 0), nullptr, 16));
        if (deref && !ReadDword(address, address))
            address = 0;
        player = reinterpret_cast<uint8_t*>(uintptr_t(address));
    }
    if (!player || words.size() < 2) {
        Print(LineKind::Error, player ? "peek <hex offset> [count] [spawned | at <hex address>]" : "no mission is running");
        return;
    }
    const uint32_t offset = uint32_t(strtoul(words[1].c_str(), nullptr, 16)) & ~3u;
    const int count = words.size() > 2 ? std::clamp(atoi(words[2].c_str()), 1, 2048) : 8;
    for (int i = 0; i < count; ++i) {
        uint32_t value = 0;
        __try {
            value = *reinterpret_cast<const uint32_t*>(player + offset + i * 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Print(LineKind::Error, "  +%X: unreadable", offset + i * 4);
            return;
        }
        float f;
        std::memcpy(&f, &value, 4);
        Print(LineKind::Output, "  +%03X: %08X  %g", offset + i * 4, value, f);
    }
}

void InfiniteForceCommand(const std::vector<std::string>& words)
{
    if (words.size() == 2 && (_stricmp(words[1].c_str(), "on") == 0 || _stricmp(words[1].c_str(), "off") == 0))
        game::SetInfiniteForce(_stricmp(words[1].c_str(), "on") == 0);
    else if (words.size() != 1) {
        Print(LineKind::Error, "infiniteforce [on|off]");
        return;
    }
    Print(LineKind::Output, "  infinite Force: %s", game::InfiniteForce() ? "on" : "off");
}

// coop [on|off | input auto|keyboard|controllers | death respawn|gameover | camera shared|player1 |
// friendlyfire on|off | boss on|off | player2 <class>|auto | storysafety on|off]: co-op's settings (saved to
// settings.ini), and what it is doing.
void Coop(const std::vector<std::string>& words)
{
    Settings& s = EditSettings();
    auto is = [&](size_t i, const char* text) { return words.size() > i && _stricmp(words[i].c_str(), text) == 0; };
    if (words.size() == 2 && (is(1, "on") || is(1, "off"))) {
        s.coop = is(1, "on");
    } else if (words.size() == 3 && is(1, "input") && (is(2, "auto") || is(2, "keyboard") || is(2, "controllers"))) {
        s.coopInput = is(2, "auto") ? 0 : is(2, "keyboard") ? 1 : 2;
    } else if (words.size() == 3 && is(1, "death") && (is(2, "respawn") || is(2, "gameover"))) {
        s.coopDeath = is(2, "gameover") ? 1 : 0;
    } else if (words.size() == 3 && is(1, "boss") && (is(2, "on") || is(2, "off"))) {
        s.coopBoss = is(2, "on");
    } else if (words.size() == 3 && is(1, "friendlyfire") && (is(2, "on") || is(2, "off"))) {
        s.coopFriendlyFire = is(2, "on");
    } else if (words.size() == 3 && is(1, "camera") && (is(2, "player1") || is(2, "shared"))) {
        s.coopCamera = is(2, "player1") ? 1 : 0;
    } else if (words.size() == 3 && is(1, "storysafety") && (is(2, "on") || is(2, "off"))) {
        s.coopStorySafety = is(2, "on");
    } else if (words.size() == 3 && is(1, "player2")) {
        if (is(2, "auto")) {
            s.coopPlayer2.clear();
        } else if (const char* name = game::RegisteredClassName(words[2].c_str()); name && game::ClassHasBody(name)) {
            s.coopPlayer2 = name;
        } else {
            Print(LineKind::Error, "%s: no such character class (see `characters`)", words[2].c_str());
            return;
        }
    } else if (words.size() != 1) {
        Print(LineKind::Error, "coop [on|off | input auto|keyboard|controllers | death respawn|gameover | camera shared|player1 | friendlyfire on|off | boss on|off |");
        Print(LineKind::Error, "      player2 <class>|auto | storysafety on|off]");
        return;
    }
    if (words.size() > 1)
        SaveSettings();
    static const char* const kInputs[] = { "auto (keyboard alone with one controller)", "keyboard is player 1",
        "two controllers" };
    Print(LineKind::Output, "  co-op %s; input: %s; when player 2 dies: %s; camera: %s; friendly fire %s; boss: %s; story safety %s; "
        "player 2 without a companion: %s", s.coop ? "on" : "off", kInputs[s.coopInput], s.coopDeath ? "game over" : "they come back",
        s.coopCamera ? "follows player 1" : "shared", s.coopFriendlyFire ? "on" : "off",
        s.coopBoss ? "player 2 plays it" : "the game's",
        s.coopStorySafety ? "on" : "off", s.coopPlayer2.empty() ? "automatic" : s.coopPlayer2.c_str());
    const game::CoopState state = game::GetCoopState();
    if (state.playing)
        Print(LineKind::Output, "  player 2 plays %s %s (health %.0f / %.0f, %.0f from player 1)",
            state.boss ? "the boss" : state.spawned ? "the spawned" : "the companion", state.player2.c_str(), state.health, state.maxHealth, state.distance);
    else if (s.coop)
        Print(LineKind::Output, "  player 2 is not playing: %s", state.reason.empty() ? "-" : state.reason.c_str());
    if (state.cutscene)
        Print(LineKind::Output, "  a cutscene is playing");
}

void Memory()
{
    const kernel::MemoryUsage m = kernel::QueryMemoryUsage();
    Print(LineKind::Output, "  memory: Xbox %.1f / %.0f MiB, other %.1f MiB, %zu pool blocks; %d spawned in this level",
        m.contiguousUsed / 1048576.0, m.contiguousSize / 1048576.0, m.virtualCommitted / 1048576.0, m.poolBlocks,
        game::SpawnedCount());
}

// team [spawned] [<hex teams>]: the player's (or last spawned character's) teams (research).
void Team(const std::vector<std::string>& words)
{
    size_t next = 1;
    const bool spawned = words.size() > next && _stricmp(words[next].c_str(), "spawned") == 0;
    if (spawned)
        ++next;
    uint8_t* character = spawned ? game::LastSpawnedObject() : game::PlayerObject();
    uint32_t teams = 0;
    if (words.size() > next && !game::SetCharacterTeams(character, uint32_t(strtoul(words[next].c_str(), nullptr, 16)))) {
        Print(LineKind::Error, "no such character, or it has no AI data");
        return;
    }
    if (!game::CharacterTeams(character, teams)) {
        Print(LineKind::Error, "no such character, or it has no AI data");
        return;
    }
    Print(LineKind::Output, "  %s teams: %08X", spawned ? "spawned" : "player", teams);
    // Research: the AI data's first fields ("Enemies" +0x20, "Preferred Enemies" +0x38).
    const uint8_t* ai = character ? *reinterpret_cast<uint8_t* const*>(character + 0xA00) : nullptr;
    for (uint32_t off = 0; ai && off < 0x60; off += 0x10) {
        const auto* w = reinterpret_cast<const uint32_t*>(ai + off);
        Print(LineKind::Output, "  ai+%02X: %08X %08X %08X %08X", off, w[0], w[1], w[2], w[3]);
    }
}

// characters: every character in the level, with its side (the AI's "Target Player": an enemy of
// yours), teams, behaviour (the AI's controller), health, distance and what it is attacking.
struct CharacterSummary {
    char type[40];
    char target[40];
    uint32_t teams;
    int controller;
    bool targetsPlayer;
    float health;
    float distance;
};

void CopyName(char* out, size_t size, uint8_t* object)
{
    const auto typeName = reinterpret_cast<const char*(__fastcall*)(uint8_t*, void*)>((*reinterpret_cast<void* const* const*>(object))[3]);
    const char* name = typeName(object, nullptr);
    strncpy_s(out, size, name ? name : "?", _TRUNCATE);
}

// Reads one character's summary; false when it cannot be read (one being removed).
bool Summarize(uint8_t* c, const uint8_t* player, CharacterSummary& out)
{
    __try {
        CopyName(out.type, sizeof(out.type), c);
        const uint8_t* ai = *reinterpret_cast<uint8_t* const*>(c + 0xA00);
        out.teams = *reinterpret_cast<const uint32_t*>(ai + 0x214);
        out.controller = *reinterpret_cast<const int32_t*>(ai + 0x08);
        out.targetsPlayer = ai[0x50] != 0;
        out.health = *reinterpret_cast<const float*>(c + 0x130);
        const float* m = reinterpret_cast<const float*>(c + 0x150);
        const float* pm = player ? reinterpret_cast<const float*>(player + 0x150) : m;
        const float dx = m[12] - pm[12], dz = m[14] - pm[14];
        out.distance = std::sqrt(dx * dx + dz * dz);
        strcpy_s(out.target, "-");
        // The AI controller's target (character +0x9FC, +0x41C).
        if (const uint8_t* brain = *reinterpret_cast<uint8_t* const*>(c + 0x9FC)) {
            if (uint8_t* target = *reinterpret_cast<uint8_t* const*>(brain + 0x41C)) {
                // An AI's target can be stale (a character gone): named only when it is still an object (its
                // vtable in the game's .rdata).
                const uint32_t vtable = IsBadReadPtr(target, 4) ? 0 : *reinterpret_cast<const uint32_t*>(target);
                if (target == player)
                    strcpy_s(out.target, "you");
                else if (vtable >= 0x0055F660 && vtable < 0x00612BC8)
                    CopyName(out.target, sizeof(out.target), target);
                else
                    strcpy_s(out.target, "(gone)");
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* ControllerName(int controller)
{
    static const char* const kNames[] = { "class", "pursue", "attack", "idle", "patrol", "roam", "stalk", "wall", "goto",
        "giveitem", "?", "?", "?", "?", "follow", "runaway" };
    return controller >= 0 && controller < int(std::size(kNames)) ? kNames[controller] : "?";
}

void Characters()
{
    uint8_t* player = game::PlayerObject();
    int i = 0, listed = 0;
    for (uint8_t* c : game::LevelCharacters()) {
        CharacterSummary s{};
        if (!Summarize(c, player, s))
            continue;
        ++listed;
        Print(LineKind::Output, "  %2d %08X %-20s %-6s teams %04X %-8s health %5.0f dist %6.0f target %s", i++,
            uint32_t(uintptr_t(c)), s.type, c == player ? "you" : s.targetsPlayer ? "enemy" : "ally", s.teams,
            ControllerName(s.controller), s.health, s.distance, s.target);
    }
    Print(LineKind::Output, "  %d character(s)", listed);
}

// scale [<factor>] [spawned]: your size (or the last spawned character's), 1 being its own.
void Scale(const std::vector<std::string>& words)
{
    size_t next = 1;
    float factor = 0.0f;
    if (words.size() > next && _stricmp(words[next].c_str(), "spawned") != 0)
        factor = float(atof(words[next++].c_str()));
    const bool spawned = words.size() > next && _stricmp(words[next].c_str(), "spawned") == 0;
    uint8_t* character = spawned ? game::LastSpawnedObject() : game::PlayerObject();
    if (factor != 0.0f && (factor < 0.05f || factor > 20.0f || !game::SetCharacterScale(character, factor))) {
        Print(LineKind::Error, character ? "scale: 0.05 to 20" : "no such character");
        return;
    }
    float scale = 0.0f;
    if (!game::CharacterScale(character, scale)) {
        Print(LineKind::Error, "no such character");
        return;
    }
    Print(LineKind::Output, "  %s scale: %.2f", spawned ? "spawned" : "your", scale);
}

// findrefs [spawned]: where the game keeps pointers to the player (or the last spawned character):
// every aligned dword in its memory and static data pointing at the character or into it (within
// 0x1200 bytes), with the object around it found by its vtable (research: what a live swap must move).
bool IsVtable(uint32_t value)
{
    if (value < 0x0055F660 || value >= 0x00612BC8 || (value & 3))
        return false;
    const uint32_t first = *reinterpret_cast<const uint32_t*>(uintptr_t(value));
    return first >= 0x00011000 && first < 0x004FEB00;
}

bool ReadDword(uintptr_t a, uint32_t& v)
{
    __try {
        v = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The nearest vtable pointer at or before `a` (within 0x1000 bytes, not before `base`), and its type.
void OwnerOf(uintptr_t a, uintptr_t base, uintptr_t& owner, uint32_t& vtable, const char*& type)
{
    owner = 0;
    vtable = 0;
    type = nullptr;
    __try {
        for (uintptr_t o = a & ~uintptr_t(3); o + 0x1000 > a && o >= base; o -= 4) {
            const uint32_t w = *reinterpret_cast<const uint32_t*>(o);
            if (IsVtable(w)) {
                owner = o;
                vtable = w;
                break;
            }
        }
        // No type name: calling a vtable's slot is only safe for real objects, and a match may not be
        // one (name the vtables from the symbol map instead).
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        type = nullptr;
    }
}

void FindRefs(const std::vector<std::string>& words)
{
    const bool spawned = words.size() > 1 && _stricmp(words[1].c_str(), "spawned") == 0;
    const bool replaced = words.size() > 1 && _stricmp(words[1].c_str(), "old") == 0;
    uint8_t* target = spawned ? game::LastSpawnedObject() : replaced ? game::ReplacedPlayerObject() : game::PlayerObject();
    // "field <hex offset>" (last two words): the object the character points to there instead.
    if (target && words.size() >= 3 && _stricmp(words[words.size() - 2].c_str(), "field") == 0) {
        const uint32_t off = uint32_t(strtoul(words.back().c_str(), nullptr, 16));
        uint32_t v = 0;
        target = ReadDword(uintptr_t(target) + off, v) ? reinterpret_cast<uint8_t*>(uintptr_t(v)) : nullptr;
    }
    if (!target) {
        Print(LineKind::Error, "no such character");
        return;
    }
    const uintptr_t lo = uintptr_t(target), hi = lo + 0x1200;
    std::vector<std::pair<uintptr_t, size_t>> regions = kernel::GameMemoryRegions();
    std::sort(regions.begin(), regions.end());
    regions.erase(std::unique(regions.begin(), regions.end()), regions.end());
    regions.push_back({ 0x00612BE0, 0x0096E000 - 0x00612BE0 }); // the image's .data and .bss
    int shown = 0, total = 0;
    for (const auto& [base, size] : regions) {
        for (uintptr_t a = base; a + 4 <= base + size; a += 4) {
            uint32_t v;
            if (!ReadDword(a, v))
                break;
            if (v < lo || v >= hi || (a >= lo && a < hi))
                continue;
            ++total;
            if (shown >= 300)
                continue;
            ++shown;
            uintptr_t owner;
            uint32_t vtable;
            const char* type;
            OwnerOf(a, base, owner, vtable, type);
            Print(LineKind::Output, "  %08X -> +%03X   in %08X+%03X vtable %08X %s", uint32_t(a), uint32_t(v - lo),
                uint32_t(owner), uint32_t(a - owner), vtable, type ? type : "");
            if (!vtable && v == lo) {
                // No object around it: the data before and after, to tell what holds it.
                char line[200] = "     around:";
                for (int k = -6; k <= 6; ++k) {
                    uint32_t w = 0;
                    ReadDword(a + k * 4, w);
                    char word[12];
                    sprintf_s(word, k == 0 ? " [%08X]" : " %08X", w);
                    strcat_s(line, word);
                }
                Print(LineKind::Output, "%s", line);
            }
        }
    }
    Print(LineKind::Output, "  %d reference(s) to %s %08X", total, spawned ? "spawned" : "player", uint32_t(lo));
}

void AutoRestart(const std::vector<std::string>& words)
{
    if (words.size() == 2 && (_stricmp(words[1].c_str(), "on") == 0 || _stricmp(words[1].c_str(), "off") == 0))
        game::SetRestartOnChange(_stricmp(words[1].c_str(), "on") == 0);
    else if (words.size() != 1) {
        Print(LineKind::Error, "autorestart [on|off]");
        return;
    }
    Print(LineKind::Output, "  autorestart: %s", game::RestartOnChange() ? "on (player changes restart the mission)" :
        "off (player changes apply from the next level start)");
}

void Variants(const std::vector<std::string>& words)
{
    if (words.size() != 2) {
        Print(LineKind::Error, "variants <class>");
        return;
    }
    const char* name = game::RegisteredClassName(words[1].c_str());
    if (!name) {
        Print(LineKind::Error, "%s: not a class the game knows", words[1].c_str());
        return;
    }
    const std::vector<game::Variant> variants = game::ClassVariants(name);
    if (variants.empty()) {
        Print(LineKind::Output, "  %s: no costume list found", name);
        return;
    }
    Print(LineKind::Output, "  %s's costumes (player %s <name or number>):", name, name);
    for (size_t i = 0; i < variants.size(); ++i)
        Print(LineKind::Output, "  %2zu  %-24s %-36s%s", i, variants[i].name, variants[i].mesh,
            variants[i].onDisc ? "" : "  (not on the disc)");
    const std::vector<std::string> sets = game::ClassTextureSets(name);
    if (!sets.empty()) {
        Print(LineKind::Output, "  texture sets (player %s <costume> skin <number or name>):", name);
        Print(LineKind::Output, "   0  (plain)");
        for (size_t i = 0; i < sets.size(); ++i)
            Print(LineKind::Output, "  %2zu  %s", i + 1, sets[i].c_str());
    }
}

void Meshes(const std::vector<std::string>& words)
{
    const std::vector<std::string> meshes = game::CharacterMeshes(words.size() > 1 ? words[1] : "");
    for (const std::string& mesh : meshes)
        Print(LineKind::Output, "  %s", mesh.c_str());
    Print(LineKind::Output, "  %zu mesh(es); use one with player mesh <mesh> or player <class> mesh <mesh>", meshes.size());
}

void FreeCamera(const std::vector<std::string>& words)
{
    if (words.size() == 1) {
        game::SetFreeCamera(!game::FreeCameraOn());
    } else if (words.size() == 2 && (_stricmp(words[1].c_str(), "on") == 0 || _stricmp(words[1].c_str(), "off") == 0)) {
        game::SetFreeCamera(_stricmp(words[1].c_str(), "on") == 0);
    } else {
        Print(LineKind::Error, "freecam [on|off]");
        return;
    }
    if (game::FreeCameraOn()) {
        Print(LineKind::Output, "  free camera on (close this menu to fly): mouse / right stick look, W A S D / left stick move,");
        Print(LineKind::Output, "  E Q / RB LB up and down, Shift / RT faster, Alt / LT slower, wheel / D-pad base speed");
    } else {
        Print(LineKind::Output, "  free camera off");
    }
}

// Returns false when the line is for the game's own console.
bool RunPortCommand(uint8_t* console, const std::string& line)
{
    std::vector<std::string> words = Words(line);
    if (words.empty())
        return true;
    std::string command = words[0];
    for (char& c : command)
        c = char(tolower(static_cast<unsigned char>(c)));
    uint8_t* registry = console ? console + game::kConsoleRegistry : nullptr;

    // name=value / name:value, as in vars_xbox.cfg
    if (words.size() == 1) {
        size_t sep = words[0].find_first_of("=:");
        if (sep != std::string::npos && sep > 0) {
            Print(LineKind::Output, "> %s", line.c_str());
            Set(registry, words[0].substr(0, sep), words[0].substr(sep + 1));
            return true;
        }
    }
    if (command == "clear" || command == "cls") {
        ClearConsole();
        return true;
    }
    if (command != "help" && command != "listvars" && command != "get" && command != "set" && command != "toggle" &&
        command != "duelist" && command != "player" && command != "variants" && command != "meshes" &&
        command != "restart" && command != "autorestart" && command != "unlockprofile" &&
        command != "freecam" && command != "saber" && command != "spawn" && command != "peek" &&
        command != "infiniteforce" && command != "memory" && command != "team" &&
        command != "findrefs" && command != "despawn" && command != "characters" && command != "scale" &&
        command != "screenshot" && command != "coop")
        return false;
    Print(LineKind::Output, "> %s", line.c_str());
    if (command == "duelist") {
        Duelist(words);
    } else if (command == "player") {
        Player(words);
    } else if (command == "variants") {
        Variants(words);
    } else if (command == "meshes") {
        Meshes(words);
    } else if (command == "restart") {
        Restart();
    } else if (command == "autorestart") {
        AutoRestart(words);
    } else if (command == "freecam") {
        FreeCamera(words);
    } else if (command == "saber") {
        Saber(words);
    } else if (command == "spawn") {
        Spawn(words);
    } else if (command == "peek") {
        Peek(words);
    } else if (command == "infiniteforce") {
        InfiniteForceCommand(words);
    } else if (command == "memory") {
        Memory();
    } else if (command == "team") {
        Team(words);
    } else if (command == "findrefs") {
        FindRefs(words);
    } else if (command == "characters") {
        Characters();
    } else if (command == "scale") {
        Scale(words);
    } else if (command == "coop") {
        Coop(words);
    } else if (command == "screenshot") {
        d3d::RequestScreenshot(words.size() > 1 ? words[1] : "");
        Print(LineKind::Output, "  saved at the next frame, without this menu, to the screenshots folder");
    } else if (command == "despawn") {
        Print(LineKind::Output, "  %d spawned character(s) removed", game::RemoveSpawned());
    } else if (command == "unlockprofile") {
        // The game's own developer command (TVaderGameOptions), not registered in the retail build.
        reinterpret_cast<void(__cdecl*)()>(uintptr_t(game::kUnlockProfile))();
        Print(LineKind::Output, "  unlocked: story, fighters, arenas, bonus missions, concept art (saved with the profile)");
    } else if (command == "help") {
        Help(console);
    } else if (command == "listvars") {
        ListVars(console, words.size() > 1 ? words[1] : "");
    } else if (command == "get") {
        if (words.size() < 2)
            Print(LineKind::Error, "get <variable>");
        else
            Get(registry, words[1]);
    } else if (command == "toggle") {
        if (words.size() < 2)
            Print(LineKind::Error, "toggle <variable>");
        else
            Toggle(registry, words[1]);
    } else { // set
        std::string name = words.size() > 1 ? words[1] : "";
        std::string value = words.size() > 2 ? words[2] : "";
        size_t sep = name.find_first_of("=:");
        if (words.size() == 2 && sep != std::string::npos) {
            value = name.substr(sep + 1);
            name = name.substr(0, sep);
        }
        if (name.empty() || value.empty())
            Print(LineKind::Error, "set <variable> <value>");
        else
            Set(registry, name, value);
    }
    return true;
}

} // namespace

void InstallConsoleHooks()
{
    const void* print = reinterpret_cast<const void*>(&ConsolePrint);
    PatchBytes(game::kConsoleVtable + game::kConsolePrintSlot, &print, 4);
    PatchBytes(game::kConsoleVtable + game::kConsolePrintTextSlot, &print, 4);
}

void AddConsoleLine(LineKind kind, const char* text)
{
    if (text)
        AddLines(kind, text);
}

bool ReadGameVariable(const char* name, std::string& value)
{
    uint8_t* console = Console();
    uint8_t* var = console ? FindVariable(console + game::kConsoleRegistry, name) : nullptr;
    char text[128];
    if (!var || !SafeValue(var, text, sizeof(text)))
        return false;
    value = text;
    return true;
}

bool WriteGameVariable(const char* name, const std::string& value)
{
    uint8_t* console = Console();
    if (!console || !FindVariable(console + game::kConsoleRegistry, name))
        return false;
    SetVariable(console + game::kConsoleRegistry, name, value.c_str());
    return true;
}

void QueueConsoleCommand(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    g_Queue.push_back(line);
}

void RunQueuedConsoleCommands()
{
    std::vector<std::string> queue;
    {
        std::lock_guard<std::mutex> lock(g_Lock);
        queue.swap(g_Queue);
    }
    for (const std::string& line : queue) {
        uint8_t* console = Console();
        if (!console) {
            // The port's own character commands need no game console (it is made with the game's menus,
            // and gone while a level loads).
            const std::vector<std::string> words = Words(line);
            static const char* const kStandalone[] = { "player", "variants", "meshes", "restart", "autorestart",
                "duelist", "freecam", "saber", "spawn", "peek", "infiniteforce", "memory", "team", "findrefs", "despawn", "characters", "scale", "screenshot", "coop", "clear", "cls" };
            const bool standalone = !words.empty() && std::any_of(std::begin(kStandalone), std::end(kStandalone),
                [&](const char* c) { return _stricmp(words[0].c_str(), c) == 0; });
            if (standalone) {
                LOG_INFO("Console: %s", line.c_str());
                RunPortCommand(nullptr, line);
            } else {
                AddLines(LineKind::Port, "The game's console does not exist yet.");
            }
            continue;
        }
        LOG_INFO("Console: %s", line.c_str());
        if (RunPortCommand(console, line))
            continue;
        using ExecuteFn = void(__fastcall*)(void* self, void* edx, const char* line, int echo);
        Slot<ExecuteFn>(console, game::kConsoleExecuteSlot)(console, nullptr, line.c_str(), 1);
    }
}

size_t CopyConsoleLines(size_t first, std::vector<ConsoleLine>& out)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    size_t total = g_Dropped + g_Lines.size();
    size_t start = first < g_Dropped ? g_Dropped : first;
    for (size_t i = start; i < total; ++i)
        out.push_back(g_Lines[i - g_Dropped]);
    return total;
}

void ClearConsole()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    g_Dropped += g_Lines.size();
    g_Lines.clear();
    g_Cleared = true;
}

bool ConsoleCleared()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    bool cleared = g_Cleared;
    g_Cleared = false;
    return cleared;
}

} // namespace swrots::debug
