#pragma once

#include <string>
#include <vector>

#include "game/devmapbuild.h"

namespace swrots::game {

// Maps of mods' own (docs/modding/custom-maps.md): a folder under mods\maps\ with a map.ini makes a map the dev map
// can be (its model in the place of the floor's, its collision boxes), played on with Heroes vs Villains (its two
// lines, its spawns) or walked in (`devmap <map>`). The map's own coordinates are turned and moved so that the
// middle between its lines is the dev map's middle and the heroes' line faces the villains' along +z.

struct CustomMap {
    std::string id;            // its folder's name, e.g. "cantina"
    std::string name;          // as shown, e.g. "MOS EISLEY CANTINA"
    // the modes that offer it (Modes=, all when left out): Versus' Select Arena screen, Heroes vs Villains' map row,
    // Battlefront's (which also needs its command posts)
    bool versus = true, hvv = true, battlefront = true;
    std::string modelFolder;   // under meshes\, e.g. "maps\cantina\"
    std::string modelName;     // e.g. "cantina"
    uint32_t vertices = 0;     // the model's vertex count (its baked light has one entry per vertex)
    std::string skyFolder, skyName; // its sky's model (Sky=), drawn as the level's sky backdrop; empty: none
    float floor = 0;           // the height of the model's floor (its own y)
    bool hasKillHeight = false;
    float killHeight = 0;      // a fighter below it (its own y) dies, as in a fall into the void (KillHeight=)
    float heroes[2] = {}, villains[2] = {}; // the lines' middles (x, z)
    float lineGap = 150;
    std::vector<DevMapBox> boxes;           // in the map's own coordinates (cy from its y 0), yaw in radians
    std::vector<uint8_t> brushes;           // its Brushes file: BSP trees in the level's format, its own coordinates
    std::vector<std::pair<float, float>> spawns;
    // Battlefront ([Battlefront]): its command posts and where each team starts (its own coordinates, x z)
    std::vector<std::pair<float, float>> commandPosts;
    bool hasStarts = false;
    float cloneStart[2] = {}, droidStart[2] = {};
    bool hasBounds = false;
    float bounds[4] = {};      // x0 z0 x1 z1
    // its light ([Lighting]): negative: the dev map's
    float ambient[3] = { -1, -1, -1 }, characterAmbient[3] = { -1, -1, -1 };
    float lightScale = 1.0f;
    std::vector<uint32_t> bakedLight; // a colour a vertex, from its Baked file
    std::string music;         // its track: a music slot of the game's (e.g. mus_m13_templebattle01_lp), empty: none
    std::wstring musicFile;    // the track's file (<map folder>\<slot>.hwx)
    // its place in the dev map: own coordinates (x, z) -> the dev map's
    float yaw = 0, middle[2] = {};
    // its collision boxes in the dev map (centre, half size, turn about y), for the sight lines below
    struct Wall {
        float centre[3], half[3], yaw;
    };
    std::vector<Wall> walls;
};

// Each start of the game image, before the dev map: reads mods\maps\*\map.ini.
void InstallCustomMaps(const std::wstring& mods);

const std::vector<CustomMap>& CustomMaps();
// The maps a mode offers (Modes=), in CustomMaps' order.
enum class MapMode { Versus, HeroesVsVillains, Battlefront };
std::vector<const CustomMap*> CustomMapsFor(MapMode mode);
// A map by its id (case-insensitive), or null.
const CustomMap* FindCustomMap(const std::string& id);

// A point of the map's own (x, z) in the dev map.
void CustomMapToWorld(const CustomMap& map, float x, float z, float& worldX, float& worldZ);
// The map's turn in the dev map (radians, added to a yaw of its own).
float CustomMapYaw(const CustomMap& map);
// Its bounds in the dev map (x0, z0, x1, z1), the dev map's floor when it has none.
void CustomMapBounds(const CustomMap& map, float (&bounds)[4]);

// Whether the line between two points of the dev map passes none of the map's collision boxes (the cameras and the
// overhead info keep out of its walls).
bool CustomMapLineOfSight(const CustomMap& map, const float a[3], const float b[3]);

// Puts the map in a dev map layout: its model, where it stands, its collision boxes.
void ApplyCustomMap(const CustomMap& map, DevMapLayout& layout);

} // namespace swrots::game
