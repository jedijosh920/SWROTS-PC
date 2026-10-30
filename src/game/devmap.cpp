// The developer test map in the game: Grievous Survival's level file and the cave's model are replaced as the
// game reads them from the level's PAK (resource patches), when this start of the game asks for the dev map.

#include "game/devmap.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "game/characters.h"
#include "game/freecam.h"
#include "game/custommaps.h"
#include "game/hvv.h"
#include "game/resources.h"
#include "kernel/kernel.h"

namespace swrots::game {

namespace {

constexpr const char* kLevelArgument = "map:u107_uta_grievoussurvival";
// (with the boot flow's switches a direct start needs: no licence movies, no direct-launch check, which otherwise
// sends a start from the debug menu through the intro movies and title first)
constexpr const char* kDevMapArgument = "devmap:1 playlicensemovie:0 allowskipmovie:1 detectDirectLaunch:0";
// A Versus duel on the dev map: the game's Versus starts its Utapau arena (a duel level; its PAK brings the duel's
// fighters, sounds, cameras and HUD art), whose level file is served as the dev map built for a duel.
constexpr const char* kDuelArena = "levels\\u107_uta_grievous_01.slp";
constexpr const char* kDuelArgument = "map:u107_uta_grievous_01";

bool g_Active = false;
DevMapLayout g_Layout;
bool g_HasLayout = false;
const CustomMap* g_Map = nullptr; // a map of a mod's own, from `custommap:<id>`
std::string g_MusicServed;        // the disc's music file its track is served for
bool g_Duel = false;              // a Versus duel on the dev map (`versus:1`)
std::string g_Arguments; // this start's launch arguments, or else its Default_Xbox.cfg
std::vector<uint32_t> g_Fallen; // the instance ids of the characters a KillHeight= fall has killed (DevMapFrame)

void BuildLevel(std::vector<uint8_t>& data)
{
    if (!g_Active)
        return;
    std::string report;
    DevMapLayout layout = g_Layout;
    if (g_Map)
        ApplyCustomMap(*g_Map, layout);
    if (BuildDevMapLevel(data, report, g_HasLayout || g_Map ? &layout : nullptr))
        LOG_INFO("Dev map: level built (%zu bytes): %s", data.size(), report.c_str());
    else
        LOG_WARN("Dev map: the level file is not the one expected (%s); Grievous Survival loads as it is",
            report.c_str());
}

// The duel arena's level file: the dev map (from Grievous Survival's level file, read from its PAK) built as a duel.
void BuildDuel(std::vector<uint8_t>& data)
{
    if (!g_Active || !g_Duel)
        return;
    std::vector<uint8_t> level;
    if (!ReadDiscResource(kDevMapSource, level)) {
        LOG_WARN("Dev map: Grievous Survival's level file is not on the disc; the arena loads as it is");
        return;
    }
    DevMapLayout layout = g_Layout;
    layout.versus = true;
    layout.duelArena = data; // its duel camera
    if (g_Map)
        ApplyCustomMap(*g_Map, layout);
    std::string report;
    if (!BuildDevMapLevel(level, report, &layout)) {
        LOG_WARN("Dev map: the duel could not be built (%s); the arena loads as it is", report.c_str());
        return;
    }
    LOG_INFO("Dev map: the duel's arena built (%zu bytes): %s", level.size(), report.c_str());
    data.swap(level);
}

// The duel's camera on a map with walls: the arena's rail was laid out for its open floor. The game's camera (the
// rail, the intro's shots) stays while it sees both fighters from above the floor; where it does not (a wall or the
// map's furniture between, below their feet's height), the port's own takes over for at least a second: side-on to
// the line between the fighters, as far as they are apart, on the side that sees them, nearer them where a wall stands
// behind it (the map's collision boxes), looking at their middle.
constexpr float kDuelEyeHeight = 75, kDuelFocusHeight = 45, kDuelLowest = 30;
constexpr float kDuelEyeRoom = 25; // the room the eye keeps from a wall
constexpr float kDuelNearest = 70; // and from a fighter
ULONGLONG g_DuelOwnUntil = 0;
float g_DuelEye[3] = {};
bool g_DuelEyeSet = false;
float g_DuelSide = 1;

void LookAt(const float eye[3], const float target[3], float m[16])
{
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f)
        v /= fl > 0 ? fl : 1;
    float r[3] = { f[2], 0, -f[0] };
    const float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    for (float& v : r)
        v /= rl > 0 ? rl : 1;
    const float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    const float rows[16] = { r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, eye[0], eye[1], eye[2], 1 };
    std::copy(std::begin(rows), std::end(rows), m);
}

// How far from `focus` along `direction` (unit, flat) the eye can stand and still see `a` and `b` (the walls).
float ClearDistance(const float focus[3], const float direction[3], float wanted, const float a[3], const float b[3])
{
    float best = 0;
    for (int i = 1; i <= 16; ++i) {
        const float d = wanted * float(i) / 16;
        const float eye[3] = { focus[0] + direction[0] * d, focus[1] + direction[1] * d, focus[2] + direction[2] * d };
        if (!CustomMapLineOfSight(*g_Map, focus, eye) || !CustomMapLineOfSight(*g_Map, a, eye) ||
            !CustomMapLineOfSight(*g_Map, b, eye))
            break;
        best = d;
    }
    return best;
}

bool DuelCamera(float m[16], float)
{
    uint8_t* player = PlayerObject();
    if (!g_Map || !player)
        return false;
    auto position = [](uint8_t* c) { return reinterpret_cast<const float*>(c + 0x150 + 48); }; // transform's row 4
    const float* p = position(player);
    const float* q = nullptr;
    float nearest = 1e30f;
    for (uint8_t* c : LevelCharacters()) { // the opponent: the nearest other character standing
        if (c == player || *reinterpret_cast<const float*>(c + 0x130) <= 0)
            continue;
        const float* o = position(c);
        const float d = std::hypot(o[0] - p[0], o[2] - p[2]);
        if (d < nearest) {
            nearest = d;
            q = o;
        }
    }
    const float floorY = q ? std::min(p[1], q[1]) : p[1];
    const float a[3] = { p[0], p[1] + kDuelFocusHeight, p[2] };
    const float b[3] = { q ? q[0] : p[0], (q ? q[1] : p[1]) + kDuelFocusHeight, q ? q[2] : p[2] };
    const float focus[3] = { (a[0] + b[0]) / 2, floorY + kDuelFocusHeight, (a[2] + b[2]) / 2 };
    const float eye[3] = { m[12], m[13], m[14] };
    // a little behind the game's eye (its forward row reversed): an eye right at a wall shows the wall's inside
    const float behind[3] = { eye[0] - m[8] * kDuelEyeRoom, eye[1] - m[9] * kDuelEyeRoom, eye[2] - m[10] * kDuelEyeRoom };
    const ULONGLONG now = GetTickCount64();
    const bool gameGood = eye[1] > floorY + kDuelLowest && CustomMapLineOfSight(*g_Map, a, eye) &&
        CustomMapLineOfSight(*g_Map, b, eye) && CustomMapLineOfSight(*g_Map, focus, behind) &&
        // not at a fighter's shoulder (the game fades a character that near the eye)
        std::hypot(eye[0] - a[0], eye[2] - a[2]) > kDuelNearest && std::hypot(eye[0] - b[0], eye[2] - b[2]) > kDuelNearest;
    if (gameGood && now >= g_DuelOwnUntil) {
        g_DuelEyeSet = false;
        return false;
    }
    if (!gameGood)
        g_DuelOwnUntil = now + 1000;
    // the port's: side-on to the fighters' line
    float dx = b[0] - a[0], dz = b[2] - a[2];
    const float apart = std::hypot(dx, dz);
    if (apart < 1) {
        dx = 0;
        dz = 1;
    } else {
        dx /= apart;
        dz /= apart;
    }
    const float wanted = std::clamp(apart * 0.7f + 150, 180.0f, 400.0f);
    float best = -1, bestSide = g_DuelSide;
    for (float side : { g_DuelSide, -g_DuelSide }) { // the side it was on first: no swapping while it sees them
        const float direction[3] = { -dz * side, (kDuelEyeHeight - kDuelFocusHeight) / wanted, dx * side };
        const float clear = ClearDistance(focus, direction, wanted, a, b);
        if (clear > best + (side == g_DuelSide ? 0 : 40)) {
            best = clear;
            bestSide = side;
        }
    }
    g_DuelSide = bestSide;
    const float d = std::max(best - kDuelEyeRoom, 60.0f); // off the wall it found
    const float target[3] = { focus[0] - dz * bestSide * d, focus[1] + (kDuelEyeHeight - kDuelFocusHeight) * d / wanted,
        focus[2] + dx * bestSide * d };
    if (!g_DuelEyeSet) {
        std::copy(target, target + 3, g_DuelEye);
        g_DuelEyeSet = true;
    }
    for (int k = 0; k < 3; ++k) // eased: no jumps
        g_DuelEye[k] += (target[k] - g_DuelEye[k]) * 0.15f;
    if (!CustomMapLineOfSight(*g_Map, focus, g_DuelEye)) // easing through a wall: a cut instead
        std::copy(target, target + 3, g_DuelEye);
    LookAt(g_DuelEye, focus, m);
    return true;
}

void BuildFloor(std::vector<uint8_t>& data)
{
    if (g_Active)
        data = DevMapFloorModel();
}

// The loading screen (0x42C40) draws one 640x256 picture, the textures <prefix>a, b and c (256, 256 and 128 wide),
// the prefix being the mission list's `loadScreenPrefix:`; the level's PAK has them. Grievous Survival's (Grievous on
// Utapau; only its PAK has them) is no picture of the dev map: they are served black (the game has no plain one: an
// empty prefix asks for load_back_v2_a/b/c, which no PAK has), or as a custom map's own (mods\maps\<id>\load_a/b/c.stx).
const char* const kMapLoadPictures[3] = { "interfc\\front_end\\loading\\load_back_uta_griev_a.stx",
    "interfc\\front_end\\loading\\load_back_uta_griev_b.stx", "interfc\\front_end\\loading\\load_back_uta_griev_c.stx" };
// the Utapau duel arena's (the dev map's duel loads as it: Utapau without Grievous)
const char* const kDuelLoadPictures[3] = { "interfc\\front_end\\loading\\load_back_uta_plain_a.stx",
    "interfc\\front_end\\loading\\load_back_uta_plain_b.stx", "interfc\\front_end\\loading\\load_back_uta_plain_c.stx" };
std::vector<uint8_t> g_LoadPictures[3]; // this start's custom map's, when it has all three

constexpr size_t kStxHeader = 0x184, kStxName = 0x40, kStxNameLength = 0x28, kStxKind = 8;

void ServeLoadPicture(std::vector<uint8_t>& data, const std::vector<uint8_t>& own)
{
    if (!g_Active || data.size() <= kStxHeader)
        return;
    if (own.size() > kStxHeader) {
        std::vector<uint8_t> out = own;
        std::memcpy(out.data() + kStxName, data.data() + kStxName, kStxNameLength); // the name inside: the one asked for
        data.swap(out);
        return;
    }
    // black: DXT1 blocks all zero; 8-byte-alpha blocks (DXT3/5) opaque alpha (all ones), then black colour
    const uint32_t kind = *reinterpret_cast<const uint32_t*>(data.data() + kStxKind);
    const size_t block = kind == 4 ? 16 : 8;
    for (size_t at = kStxHeader; at + block <= data.size(); at += block) {
        std::memset(data.data() + at, 0, block);
        if (block == 16)
            std::memset(data.data() + at, 0xFF, 8);
    }
}
void ServeLoadA(std::vector<uint8_t>& data) { ServeLoadPicture(data, g_LoadPictures[0]); }
void ServeLoadB(std::vector<uint8_t>& data) { ServeLoadPicture(data, g_LoadPictures[1]); }
void ServeLoadC(std::vector<uint8_t>& data) { ServeLoadPicture(data, g_LoadPictures[2]); }

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& data)
{
    data.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart < (8 << 20)) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr);
        data.resize(read);
    }
    CloseHandle(file);
    return !data.empty();
}

std::string ReadText(const std::wstring& path)
{
    std::string text;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart < (1 << 20)) {
        text.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr);
        text.resize(read);
    }
    CloseHandle(file);
    return text;
}

} // namespace

void InstallDevMap(const std::wstring& gameData, const std::wstring& mods)
{
    // the launch data decides when there is any (a level change, StartDevMap); else the configuration the game
    // starts with (mods\ overrides the disc's)
    g_Arguments = kernel::LaunchArguments();
    if (g_Arguments.empty()) {
        g_Arguments = ReadText(mods + L"\\Default_Xbox.cfg");
        if (g_Arguments.empty())
            g_Arguments = ReadText(gameData + L"\\Default_Xbox.cfg");
    }
    std::string value;
    g_Active = BootArgument("devmap", value) && value == "1";
    g_Layout = {};
    g_HasLayout = false;
    if (g_MusicServed.size()) // the last start's map's track: the disc's own again
        kernel::SetDiscFileOverride(g_MusicServed, L"");
    g_MusicServed.clear();
    g_Map = nullptr;
    g_Fallen.clear();
    g_Duel = g_Active && BootArgument("versus", value) && value == "1";
    if (g_Active && BootArgument("custommap", value)) {
        g_Map = FindCustomMap(value);
        if (g_Map && !g_Map->music.empty()) { // its track in the slot's place, for this start only
            g_MusicServed = "\\audio\\xbox\\" + g_Map->music + ".hwx";
            kernel::SetDiscFileOverride(g_MusicServed, g_Map->musicFile);
        }
        if (!g_Map)
            LOG_WARN("Dev map: there is no map %s under mods\\maps (the dev map's floor instead)", value.c_str());
    }
    for (auto& picture : g_LoadPictures)
        picture.clear();
    if (g_Map) {
        static const wchar_t* const kOwn[3] = { L"\\load_a.stx", L"\\load_b.stx", L"\\load_c.stx" };
        bool all = true;
        for (int i = 0; i < 3; ++i)
            all = ReadFileBytes(mods + L"\\maps\\" + std::wstring(g_Map->id.begin(), g_Map->id.end()) + kOwn[i],
                g_LoadPictures[i]) && all;
        if (!all)
            for (auto& picture : g_LoadPictures)
                picture.clear();
    }
    static const ResourcePatch kServe[3] = { &ServeLoadA, &ServeLoadB, &ServeLoadC };
    for (int i = 0; i < 3; ++i) {
        RegisterResourcePatch(kMapLoadPictures[i], kServe[i]);
        RegisterResourcePatch(kDuelLoadPictures[i], kServe[i]);
    }
    // the dev map by itself (no mode on it): the player is Anakin, not Grievous Survival's Grievous
    SetLevelPlayerDefault(g_Active && !g_Duel && !BootArgument("hvv", value) ? "IAnakin" : nullptr);
    RegisterResourcePatch(kDevMapSource, &BuildLevel);
    RegisterResourcePatch(kDuelArena, &BuildDuel);
    if (g_Duel && g_Map)
        SetCameraAdjuster(&DuelCamera);
    RegisterResourcePatch(kDevMapFloorModel, &BuildFloor);
    if (g_Active)
        LOG_INFO("Dev map: %s loads as the developer map%s%s", g_Duel ? "a Versus duel" : "Grievous Survival",
            g_Map ? ", on " : "", g_Map ? g_Map->name.c_str() : "");
}

bool DevMapActive() { return g_Active; }

// A fall below a custom map's KillHeight= (its pits): the character's instant kill (ICharacter's slot 0xC8, 0x152D30),
// as the game's own falls into the void, once a fall: its instance id (+4) is kept in g_Fallen until it is back above
// the line or gone from the level (its body lies below the line for a while, and a kill a frame was hundreds a fall).
// In Heroes vs Villains and Battlefront only while the fight is on: the match's guard on the player's falls is in
// place then; before it (the line-up, the intro) a kill would be the game's own, with its Game Over.
void DevMapFrame()
{
    if (!g_Active || !g_Map || !g_Map->hasKillHeight || (HvvActive() && !HvvFighting()))
        return;
    constexpr uint32_t kInstanceId = 0x4, kHealth = 0x130, kDead = 0x12C, kTransform = 0x150, kInstantKillSlot = 0xC8;
    const float killY = kDevMapFloorY + g_Map->killHeight - g_Map->floor;
    std::vector<uint32_t> present;
    for (uint8_t* c : LevelCharacters()) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(c + kInstanceId);
        present.push_back(id);
        const auto fallen = std::find(g_Fallen.begin(), g_Fallen.end(), id);
        if (reinterpret_cast<const float*>(c + kTransform + 48)[1] >= killY) {
            if (fallen != g_Fallen.end()) // back above the line (brought back): a new fall kills again
                g_Fallen.erase(fallen);
            continue;
        }
        if (fallen != g_Fallen.end() || *reinterpret_cast<const float*>(c + kHealth) <= 0 || c[kDead])
            continue;
        g_Fallen.push_back(id);
        { // where, in the map's own coordinates (map.ini's x z)
            const float* at = reinterpret_cast<const float*>(c + kTransform + 48);
            const float cy = std::cos(g_Map->yaw), sy = std::sin(g_Map->yaw);
            const float dx = at[0] - kDevMapMiddleX, dz = at[2] - kDevMapMiddleZ;
            LOG_INFO("Dev map: character %08X fell below KillHeight= (%.0f) at the map's %.0f %.0f: its instant kill",
                id, killY, g_Map->middle[0] + cy * dx - sy * dz, g_Map->middle[1] + sy * dx + cy * dz);
        }
        using InstantKill = void(__fastcall*)(uint8_t*, void*, uint32_t);
        const auto kill = reinterpret_cast<InstantKill>((*reinterpret_cast<void* const* const*>(c))[kInstantKillSlot / 4]);
        kill(c, nullptr, 0);
    }
    // the removed ones (their ids may be given again)
    g_Fallen.erase(std::remove_if(g_Fallen.begin(), g_Fallen.end(), [&](uint32_t id) {
        return std::find(present.begin(), present.end(), id) == present.end();
    }), g_Fallen.end());
}

const CustomMap* DevMapCustomMap() { return g_Active ? g_Map : nullptr; }

bool BootArgument(const char* name, std::string& value)
{
    std::istringstream words(g_Arguments);
    std::string word;
    const size_t length = strlen(name);
    while (words >> word) {
        if (word.size() > length && word[length] == ':' && _strnicmp(word.c_str(), name, length) == 0) {
            value = word.substr(length + 1);
            return true;
        }
    }
    return false;
}

bool BootIsVersusDuel()
{
    // the mission list's Duel levels
    static const char* const kArenas[] = { "u103_sep_throneroom_01", "u103_sep_throneroom_02", "u107_uta_grievous_01",
        "u107_uta_grievous_02", "u108_cor_palpatine_01", "u108_cor_palpatine_02", "u110_cor_training_00",
        "u110_cor_training_01", "u110_cor_training_02", "u140_mus_theduel_01", "u140_mus_theduel_02",
        "u150_mus_theduel_01", "u150_mus_theduel_02", "u170_ep4_deathstar_02" };
    std::string value;
    if (g_Duel || (BootArgument("v_versusMode", value) && value == "1"))
        return true;
    if (BootArgument("map", value))
        for (const char* arena : kArenas)
            if (_stricmp(value.c_str(), arena) == 0)
                return true;
    return false;
}

bool BootIsMatch()
{
    std::string value;
    return BootIsVersusDuel() || (g_Active && BootArgument("hvv", value));
}

void SetDevMapLayout(const DevMapLayout& layout)
{
    g_Layout = layout;
    g_HasLayout = true;
}

void StartDuel(const std::string& first, const std::string& second, const std::string& map, int rounds,
    const std::string& more)
{
    kernel::LaunchGame(std::string(kDuelArgument) + " " + kDevMapArgument + " versus:1" +
        (map.empty() ? "" : " custommap:" + map) + " v_versusMode:1 v_c1:" + first + " v_c2:" + second +
        " v_p1:0 v_p2:-1 v_roundcount:" + std::to_string(rounds > 0 ? rounds : 3) + more);
}

void StartDevMap(const std::string& more)
{
    kernel::LaunchGame(std::string(kLevelArgument) + " " + kDevMapArgument + (more.empty() ? "" : " " + more));
}

} // namespace swrots::game
