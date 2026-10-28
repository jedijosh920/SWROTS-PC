#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// The developer test map (docs/research/levels.md): an empty, lit, flat floor under the sky, made from the bonus
// mission Grievous Survival's level file (levels\u107_uta_grievoussurvival.slp) as the game reads it from its PAK.
// No game code: the level file is read and edited as bytes (the format: tools/levels/slp.py, slpscene.py), with
// the same edits as tools/levels/devmap.py (whose default output it matches byte for byte).

// The level file the dev map is made from.
constexpr const char* kDevMapSource = "levels\\u107_uta_grievoussurvival.slp";
// The model the floor takes the place of (the cave's, whose static entry the dev map keeps).
constexpr const char* kDevMapFloorModel = "meshes\\props\\m07\\cave\\m07_cave.msh";

// What a mode puts on the dev map. Characters placed in the level load with it (their classes' sound sets too,
// which characters created later do not get) and wait inactive until the port activates them. Positions are on
// the floor (x, z); `yaw` turns about the up axis (a character faces +z at 0, -z at pi).
struct DevMapCharacter {
    std::string className; // e.g. "IObiwan"
    uint32_t id;           // the object's id, unique in the level (see kDevMapFirstId)
    float x, z, yaw;
    std::string costume;   // its costume ("Mesh Choice", e.g. "hordeBattleDroid": the battle droid's first has no body)
};
// Any other object of the game placed in the level: its class, id, place (y from the floor) and its own properties
// beside ID, Name and Transform (name, the property's bytes as the level file holds them), e.g. a mission's
// objective (ITriggerObjective, "ID string").
struct DevMapObject {
    std::string className;
    uint32_t id;
    float x, y, z, yaw;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> properties;
    // A static's model (IStatic and its kinds, e.g. a force field): its static light record (white, per vertex) is
    // added to the level, its handle given as the object's __lighthandle. The model's path under meshes\ without
    // extension ("meshes\<folder>\<name>"), its vertex count per part as the model has them.
    std::string lightMesh;
    std::vector<uint16_t> lightParts;
};
// A box of collision (a box brush): its centre (y from the floor), half extents and turn about the up axis.
struct DevMapBox {
    float cx, cy, cz, hx, hy, hz, yaw;
};
struct DevMapLayout {
    std::vector<DevMapCharacter> characters;
    bool start = false; // the player's start, else the middle of the floor
    float startX = 0, startZ = 0, startYaw = 0;
    // A map of a mod's own (custommaps.h): its model under meshes\ (folder and name) in the place of the floor's,
    // where it stands on the floor, its vertex count (its baked light), and its collision boxes.
    std::string modelFolder, modelName;
    uint32_t modelVertices = 0;
    float modelX = 0, modelY = 0, modelZ = 0, modelYaw = 0; // modelY: from the floor
    std::vector<DevMapBox> boxes;
    // Its light: the model's ambient (negative: the floor's), its baked light (one colour a vertex, 0xAARRGGBB; with
    // none, white), the characters' ambient (negative: the dev map's) and the point lights' intensity scale.
    float modelAmbient[3] = { -1, -1, -1 };
    std::vector<uint32_t> bakedLight;
    float characterAmbient[3] = { -1, -1, -1 };
    float lightScale = 1.0f;
    // Its music: a track of the game's sound table (e.g. "mus_m13_templebattle01_lp"), played from the start (one of
    // the level's music objects kept and pointed at it); empty: none.
    std::string music;
    // A Versus duel (the game's own mode): the level is a head-to-head arena, the player's start becomes player 1's
    // spawn point and player 2's is added, `duelGap` apart across the middle (player 1 on -z, facing +z).
    bool versus = false;
    float duelGap = 220.0f;
    // The Versus arena's own level file: its duel camera (ICameraControl 'Rail Tracking', its helpers and trigger
    // bound) is taken from it, moved from the arena's fighters onto the duel's.
    std::vector<uint8_t> duelArena;
    // Places the AI can be sent to (x, z on the floor): the level's own goto marker (ICharacterGoto) copied there,
    // one each, with ids from kDevMapFirstMarkerId on; arrived within `markerReach`.
    std::vector<std::pair<float, float>> markers;
    float markerReach = 175.0f;
    std::vector<DevMapObject> objects;
};
// The ids of the objects the dev map adds for a duel.
constexpr uint32_t kDevMapDuelId = 0x00300100;
// The ids the dev map's own objects take (the level's run far below).
constexpr uint32_t kDevMapFirstId = 0x00300001;
constexpr uint32_t kDevMapFirstBoxId = 0x00380001; // the collision boxes' brush handles
constexpr uint32_t kDevMapFirstMarkerId = 0x00390001; // the layout's goto markers
constexpr uint32_t kDevMapFirstLightHandle = 0x0000F000; // the layout's statics' light records
// The floor's middle and its height (the characters stand there).
constexpr float kDevMapMiddleX = 12750.0f, kDevMapMiddleZ = 8100.0f, kDevMapFloorY = -2557.5f;

// Edits `level` (the whole level file) into the dev map, with `layout`'s characters and start when given. False
// (level unchanged) when it is not the file expected; `report` says what was done or why not.
bool BuildDevMapLevel(std::vector<uint8_t>& level, std::string& report, const DevMapLayout* layout = nullptr);

// The dev map's floor model, written from scratch: a flat grid tiled with the cave's floor texture.
std::vector<uint8_t> DevMapFloorModel();

} // namespace swrots::game
