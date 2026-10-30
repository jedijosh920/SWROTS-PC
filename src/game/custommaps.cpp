// Maps of mods' own (see custommaps.h).

#include "game/custommaps.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/log.h"

namespace swrots::game {

namespace {

constexpr float kPi = 3.14159265f;
// The dev map's floor (devmapbuild.cpp's space), for a map without bounds.
constexpr float kFloorBounds[4] = { 1500.0f, 2700.0f, 24000.0f, 13500.0f };

std::vector<CustomMap> g_Maps;
std::wstring g_Mods;

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(file);
    return ok;
}

std::string Profile(const std::wstring& file, const wchar_t* section, const wchar_t* key)
{
    wchar_t value[2048] = {};
    GetPrivateProfileStringW(section, key, L"", value, 2048, file.c_str());
    std::string out;
    for (const wchar_t* c = value; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

// The numbers in a value, whatever separates them (spaces, commas, semicolons).
std::vector<float> Numbers(const std::string& text)
{
    std::vector<float> out;
    const char* p = text.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {
            char* end = nullptr;
            const float v = std::strtof(p, &end);
            if (end == p) {
                ++p;
                continue;
            }
            out.push_back(v);
            p = end;
        } else {
            ++p;
        }
    }
    return out;
}

std::string Narrow(const wchar_t* text)
{
    std::string out;
    for (const wchar_t* c = text; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

bool ReadMap(const std::wstring& file, const std::string& id, CustomMap& m)
{
    m.id = id;
    m.name = Profile(file, L"Map", L"Name");
    if (m.name.empty())
        m.name = id;
    for (char& c : m.name)
        c = char(toupper(static_cast<unsigned char>(c)));
    // Modes=Versus HvV Battlefront (any of them, separated by spaces or commas): the modes that offer the map
    if (std::string modes = Profile(file, L"Map", L"Modes"); !modes.empty()) {
        for (char& c : modes)
            c = char(c == ',' || c == ';' ? ' ' : tolower(static_cast<unsigned char>(c)));
        m.versus = m.hvv = m.battlefront = false;
        std::istringstream words(modes);
        for (std::string word; words >> word;) {
            if (word == "versus" || word == "duel")
                m.versus = true;
            else if (word == "hvv" || word == "heroesvsvillains")
                m.hvv = true;
            else if (word == "battlefront" || word == "bf")
                m.battlefront = true;
            else
                LOG_WARN("Custom maps: %s: unknown mode %s in Modes (Versus, HvV, Battlefront)", id.c_str(), word.c_str());
        }
    }
    // Model=maps\cantina\cantina: its folder (with the last backslash) and its name
    const std::string model = Profile(file, L"Map", L"Model");
    const size_t slash = model.find_last_of('\\');
    if (model.empty() || slash == std::string::npos || slash + 1 == model.size()) {
        LOG_WARN("Custom maps: %s needs Model (its folder and name under meshes, e.g. maps\\%s\\%s)", id.c_str(),
            id.c_str(), id.c_str());
        return false;
    }
    m.modelFolder = model.substr(0, slash + 1);
    m.modelName = model.substr(slash + 1);
    // Sky=maps\cantina\cantina_sky: a model about the sky's origin (the level's backdrop is some 600 units across)
    const std::string sky = Profile(file, L"Map", L"Sky");
    if (const size_t skySlash = sky.find_last_of('\\'); skySlash != std::string::npos && skySlash + 1 < sky.size()) {
        m.skyFolder = sky.substr(0, skySlash + 1);
        m.skyName = sky.substr(skySlash + 1);
    }
    if (const std::string kill = Profile(file, L"Map", L"KillHeight"); !kill.empty()) {
        m.hasKillHeight = true;
        m.killHeight = std::strtof(kill.c_str(), nullptr);
    }
    m.vertices = uint32_t(std::strtoul(Profile(file, L"Map", L"Vertices").c_str(), nullptr, 10));
    if (!m.vertices) {
        LOG_WARN("Custom maps: %s needs Vertices (its model's vertex count)", id.c_str());
        return false;
    }
    std::vector<float> v = Numbers(Profile(file, L"Map", L"Floor"));
    m.floor = v.empty() ? 0 : v[0];
    const std::vector<float> heroes = Numbers(Profile(file, L"Map", L"HeroesLine"));
    const std::vector<float> villains = Numbers(Profile(file, L"Map", L"VillainsLine"));
    if (heroes.size() >= 2 && villains.size() >= 2) {
        m.heroes[0] = heroes[0];
        m.heroes[1] = heroes[1];
        m.villains[0] = villains[0];
        m.villains[1] = villains[1];
    } else { // lines across the middle of the map's own space
        m.heroes[1] = -350;
        m.villains[1] = 350;
    }
    if (std::hypot(m.villains[0] - m.heroes[0], m.villains[1] - m.heroes[1]) < 1) {
        LOG_WARN("Custom maps: %s's lines are at one place", id.c_str());
        return false;
    }
    v = Numbers(Profile(file, L"Map", L"LineGap"));
    if (!v.empty() && v[0] > 0)
        m.lineGap = v[0];
    v = Numbers(Profile(file, L"Map", L"Spawns"));
    for (size_t i = 0; i + 1 < v.size(); i += 2)
        m.spawns.push_back({ v[i], v[i + 1] });
    // CommandPosts=x z; x z height; ...: a post on a floor of its own (a bridge, a pad) gives that floor's height
    {
        std::stringstream posts(Profile(file, L"Battlefront", L"CommandPosts"));
        std::string one;
        while (std::getline(posts, one, ';')) {
            v = Numbers(one);
            if (v.size() >= 2)
                m.commandPosts.push_back({ v[0], v[1], v.size() >= 3 ? v[2] : m.floor });
        }
    }
    {
        const std::vector<float> clones = Numbers(Profile(file, L"Battlefront", L"CloneStart"));
        const std::vector<float> droids = Numbers(Profile(file, L"Battlefront", L"DroidStart"));
        if (clones.size() >= 2 && droids.size() >= 2) {
            m.hasStarts = true;
            m.cloneStart[0] = clones[0];
            m.cloneStart[1] = clones[1];
            m.droidStart[0] = droids[0];
            m.droidStart[1] = droids[1];
        }
    }
    v = Numbers(Profile(file, L"Map", L"Bounds"));
    if (v.size() >= 4) {
        m.hasBounds = true;
        m.bounds[0] = std::min(v[0], v[2]);
        m.bounds[1] = std::min(v[1], v[3]);
        m.bounds[2] = std::max(v[0], v[2]);
        m.bounds[3] = std::max(v[1], v[3]);
    }
    for (int i = 1;; ++i) {
        const std::wstring key = L"Box" + std::to_wstring(i);
        v = Numbers(Profile(file, L"Collision", key.c_str()));
        if (v.size() < 6)
            break;
        m.boxes.push_back({ v[0], v[1], v[2], v[3], v[4], v[5], v.size() > 6 ? v[6] * kPi / 180 : 0.0f });
    }
    // Brushes=<file in the map's folder>: its collision as convex volumes (BSP trees in the level file's format)
    const std::string brushes = Profile(file, L"Collision", L"Brushes");
    if (!brushes.empty()) {
        const std::wstring path = file.substr(0, file.find_last_of(L'\\') + 1) + std::wstring(brushes.begin(), brushes.end());
        if (!ReadAll(path, m.brushes))
            LOG_WARN("Custom maps: %s's brushes %s are missing", id.c_str(), brushes.c_str());
    }
    // its light: one number (grey) or three (red, green, blue) each
    auto colour = [&](const wchar_t* key, float (&out)[3]) {
        const std::vector<float> c = Numbers(Profile(file, L"Lighting", key));
        if (c.size() >= 3)
            std::copy(c.begin(), c.begin() + 3, out);
        else if (c.size() == 1)
            out[0] = out[1] = out[2] = c[0];
    };
    colour(L"Ambient", m.ambient);
    colour(L"CharacterAmbient", m.characterAmbient);
    v = Numbers(Profile(file, L"Lighting", L"Lights"));
    if (!v.empty() && v[0] >= 0)
        m.lightScale = v[0];
    const std::string baked = Profile(file, L"Lighting", L"Baked");
    if (!baked.empty()) { // in the map's folder, else the model's
        std::vector<uint8_t> data;
        const std::wstring folder = file.substr(0, file.find_last_of(L'\\') + 1);
        const std::wstring name(baked.begin(), baked.end());
        if (!ReadAll(folder + name, data) && !ReadAll(g_Mods + L"\\meshes\\" +
                std::wstring(m.modelFolder.begin(), m.modelFolder.end()) + name, data))
            LOG_WARN("Custom maps: %s's baked light %s is missing", id.c_str(), baked.c_str());
        else if (data.size() != size_t(m.vertices) * 4)
            LOG_WARN("Custom maps: %s's baked light has %zu colours, not one for each of its %u vertices", id.c_str(),
                data.size() / 4, m.vertices);
        else {
            m.bakedLight.resize(m.vertices);
            std::memcpy(m.bakedLight.data(), data.data(), data.size());
            // BakedScale: the baked light dimmed or brightened (its red, green and blue)
            const std::vector<float> scale = Numbers(Profile(file, L"Lighting", L"BakedScale"));
            if (!scale.empty() && scale[0] >= 0 && scale[0] != 1.0f) {
                for (uint32_t& c : m.bakedLight) {
                    uint32_t out = c & 0xFF000000;
                    for (int shift = 0; shift < 24; shift += 8)
                        out |= uint32_t(std::min(255.0f, float((c >> shift) & 0xFF) * scale[0])) << shift;
                    c = out;
                }
            }
        }
    }
    // its music: Music=<a slot of the game's sound table>, the track <map folder>\<slot>.hwx in the slot's format
    // (Xbox ADPCM at the slot's rate and length: tools/audio/hwx.py)
    std::string music = Profile(file, L"Map", L"Music");
    for (char& c : music)
        c = char(tolower(static_cast<unsigned char>(c)));
    if (!music.empty()) {
        const std::wstring track = file.substr(0, file.find_last_of(L'\\') + 1) + std::wstring(music.begin(), music.end()) +
            L".hwx";
        if (music.rfind("mus_", 0) != 0 || GetFileAttributesW(track.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("Custom maps: %s's music %s needs %s.hwx in its folder (a mus_ slot)", id.c_str(), music.c_str(),
                music.c_str());
        } else {
            m.music = music;
            m.musicFile = track;
        }
    }
    // its place: the middle between the lines at the dev map's middle, the heroes' line facing +z
    m.middle[0] = (m.heroes[0] + m.villains[0]) / 2;
    m.middle[1] = (m.heroes[1] + m.villains[1]) / 2;
    const float dx = m.villains[0] - m.heroes[0], dz = m.villains[1] - m.heroes[1];
    m.yaw = std::atan2(-dx, dz);
    for (const DevMapBox& b : m.boxes) {
        CustomMap::Wall w;
        CustomMapToWorld(m, b.cx, b.cz, w.centre[0], w.centre[2]);
        w.centre[1] = kDevMapFloorY + b.cy - m.floor;
        w.half[0] = b.hx;
        w.half[1] = b.hy;
        w.half[2] = b.hz;
        w.yaw = b.yaw + m.yaw;
        m.walls.push_back(w);
    }
    return true;
}

} // namespace

void InstallCustomMaps(const std::wstring& mods)
{
    g_Maps.clear();
    g_Mods = mods;
    WIN32_FIND_DATAW found;
    const std::wstring folder = mods + L"\\maps\\";
    HANDLE find = FindFirstFileW((folder + L"*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.')
            continue;
        const std::wstring file = folder + found.cFileName + L"\\map.ini";
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        CustomMap m;
        if (!ReadMap(file, Narrow(found.cFileName), m))
            continue;
        LOG_INFO("Custom maps: %s (%s), model meshes\\%s%s, %zu collision boxes, %zu bytes of brushes, %zu spawns; "
            "in%s%s%s", m.id.c_str(), m.name.c_str(), m.modelFolder.c_str(), m.modelName.c_str(), m.boxes.size(),
            m.brushes.size(), m.spawns.size(), m.versus ? " Versus" : "", m.hvv ? " HvV" : "",
            m.battlefront ? (m.commandPosts.empty() ? " (Battlefront: no command posts)" : " Battlefront") : "");
        g_Maps.push_back(std::move(m));
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

const std::vector<CustomMap>& CustomMaps() { return g_Maps; }

std::vector<const CustomMap*> CustomMapsFor(MapMode mode)
{
    std::vector<const CustomMap*> maps;
    for (const CustomMap& m : g_Maps)
        if (mode == MapMode::Versus ? m.versus : mode == MapMode::HeroesVsVillains ? m.hvv : m.battlefront)
            maps.push_back(&m);
    return maps;
}

const CustomMap* FindCustomMap(const std::string& id)
{
    for (const CustomMap& m : g_Maps)
        if (_stricmp(m.id.c_str(), id.c_str()) == 0)
            return &m;
    return nullptr;
}

void CustomMapToWorld(const CustomMap& map, float x, float z, float& worldX, float& worldZ)
{
    // turned as a placed object's rows turn it (right c 0 -s, forward s 0 c)
    const float c = std::cos(map.yaw), s = std::sin(map.yaw);
    const float lx = x - map.middle[0], lz = z - map.middle[1];
    worldX = kDevMapMiddleX + c * lx + s * lz;
    worldZ = kDevMapMiddleZ - s * lx + c * lz;
}

float CustomMapYaw(const CustomMap& map) { return map.yaw; }

bool CustomMapLineOfSight(const CustomMap& map, const float a[3], const float b[3])
{
    // a slab test in each box's own frame (the world turned back by its yaw)
    for (const CustomMap::Wall& w : map.walls) {
        const float c = std::cos(w.yaw), s = std::sin(w.yaw);
        const float da[3] = { a[0] - w.centre[0], a[1] - w.centre[1], a[2] - w.centre[2] };
        const float db[3] = { b[0] - w.centre[0], b[1] - w.centre[1], b[2] - w.centre[2] };
        const float p[3] = { c * da[0] - s * da[2], da[1], s * da[0] + c * da[2] };
        const float d[3] = { c * db[0] - s * db[2] - p[0], db[1] - p[1], s * db[0] + c * db[2] - p[2] };
        float t0 = 0, t1 = 1;
        bool hit = true;
        for (int k = 0; k < 3 && hit; ++k) {
            if (std::fabs(d[k]) < 1e-6f) {
                hit = std::fabs(p[k]) <= w.half[k];
                continue;
            }
            float ta = (-w.half[k] - p[k]) / d[k], tb = (w.half[k] - p[k]) / d[k];
            if (ta > tb)
                std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            hit = t0 <= t1;
        }
        if (hit)
            return false;
    }
    return true;
}

void CustomMapBounds(const CustomMap& map, float (&bounds)[4])
{
    if (!map.hasBounds) {
        std::copy(std::begin(kFloorBounds), std::end(kFloorBounds), bounds);
        return;
    }
    bounds[0] = bounds[1] = 1e30f;
    bounds[2] = bounds[3] = -1e30f;
    for (int i = 0; i < 4; ++i) {
        float x, z;
        CustomMapToWorld(map, map.bounds[i & 1 ? 2 : 0], map.bounds[i & 2 ? 3 : 1], x, z);
        bounds[0] = std::min(bounds[0], x);
        bounds[1] = std::min(bounds[1], z);
        bounds[2] = std::max(bounds[2], x);
        bounds[3] = std::max(bounds[3], z);
    }
}

void ApplyCustomMap(const CustomMap& map, DevMapLayout& layout)
{
    layout.modelFolder = map.modelFolder;
    layout.modelName = map.modelName;
    layout.skyFolder = map.skyFolder;
    layout.skyName = map.skyName;
    layout.modelVertices = map.vertices;
    CustomMapToWorld(map, 0, 0, layout.modelX, layout.modelZ);
    layout.modelY = -map.floor;
    layout.modelYaw = map.yaw;
    std::copy(std::begin(map.ambient), std::end(map.ambient), layout.modelAmbient);
    std::copy(std::begin(map.characterAmbient), std::end(map.characterAmbient), layout.characterAmbient);
    layout.lightScale = map.lightScale;
    layout.bakedLight = map.bakedLight;
    layout.music = map.music;
    layout.brushes = map.brushes;
    layout.boxes.clear();
    for (const DevMapBox& b : map.boxes) {
        DevMapBox w = b;
        CustomMapToWorld(map, b.cx, b.cz, w.cx, w.cz);
        w.cy = b.cy - map.floor;
        w.yaw = b.yaw + map.yaw;
        layout.boxes.push_back(w);
    }
}

} // namespace swrots::game
