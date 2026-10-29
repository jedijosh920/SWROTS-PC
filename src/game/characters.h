#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Characters: which class the player is, in which costume or mesh. At every boot, after the image is
// loaded.
void InstallCharacters();

// The game's own string naming a registered character class (case-insensitive), or null.
const char* RegisteredClassName(const char* name);

// Makes the player a character class (e.g. "ICloneTrooper") from the next level start until the game
// is closed; null goes back to each level's own player (and its costume and mesh). False when the game
// knows no such class; unchecked (before the game has registered its classes, at boot), the name is taken as
// given, in the game's spelling.
bool SetPlayerClass(const char* className, bool checked = true);

// A picture loaded from its file (the disc's, or one under mods\), its path without .stx, e.g.
// "interfc\\front_end\\s_selduel_lukehead": the engine's texture (as the menus and the HUD draw), or null.
void* LoadPictureFile(const char* path);
// The same through the resources (a picture some loaded PAK has, e.g. the front end's): the engine's texture, or null.
void* LoadPicture(const char* path);

// The class set with SetPlayerClass, or null.
const char* PlayerClass();

// This start's level's player when no class is set (the level's own replaced, e.g. the dev map's IAnakin for
// Grievous Survival's Grievous), or null: the level's own. A static string, set before the level starts.
void SetLevelPlayerDefault(const char* className);

// The player's costume from the next level start: a variant's name, a part of its name found in no
// other ("duel" for Anakin_Duel) or its number; empty for the usual one.
void SetPlayerVariant(const std::string& variant);
std::string PlayerVariantChoice();

// The player's mesh from the next level start, under meshes\chars without the extension (e.g.
// "anakinduel\anakinduel"), in place of its costume's; empty for the costume's own.
void SetPlayerMesh(const std::string& mesh);
std::string PlayerMesh();

// The player's texture set from the next level start ("Starting texture set" in the game's level
// data, e.g. the clone trooper's _var01): its number (0 the plain textures) or name; empty for the
// usual one.
void SetPlayerSkin(const std::string& skin);
std::string PlayerSkin();

// A class's texture sets (e.g. "_var01", "_var02"; the plain textures, set 0, not listed), and one by
// number or name (with or without its "_"); -1 when it has none such.
std::vector<std::string> ClassTextureSets(const char* className);
int ClassTextureSetIndex(const char* className, const std::string& spec);

// A character class's costumes, from the game's static lists.
struct Variant {
    const char* name;
    const char* mesh; // under meshes\chars, e.g. "AnakinDuel\AnakinDuel"
    bool onDisc;
};
std::vector<Variant> ClassVariants(const char* className);

// A class's costume by number, name or a unique part of its name; -1 when none (or several) match.
int ClassVariantIndex(const char* className, const std::string& spec);

// False for a class none of whose costumes' models is on the disc: a character cut from the game
// (Commander Cody: only his portraits are left, not his definition or moves), which crashes the game
// as the player whatever body it is given.
bool ClassHasBody(const char* className);

// Spawns a character of a class into the running level, in front of the player and facing it, in a
// costume (empty: its usual one), texture set and body (empty: the costume's) as for the player. It
// takes its class's own AI and teams. False with `error` set when it cannot (no mission running, an
// unknown or cut class).
// Neutral: attacks no one and no one attacks it, until it is hit: then it riots. Riot: everyone's enemy.
enum class SpawnSide { Default, Ally, Enemy, Neutral, Riot };
bool SpawnCharacter(const char* className, const std::string& costume, const std::string& skin,
    const std::string& mesh, SpawnSide side, std::string& error);
// The same at a place (a character's transform: rows right, up, forward and position, 4 floats each; null: in
// front of the player); the character, or null with `error` set.
uint8_t* SpawnCharacterAt(const char* className, const std::string& costume, const std::string& skin,
    const std::string& mesh, SpawnSide side, const float* place, std::string& error);
// Puts a character in the running level on a side, as a spawn's (Ally: the player's). False outside a level.
bool SetCharacterSide(uint8_t* character, SpawnSide side);
// The AI data's "Ignored By AI" (no one sets out for the character); false when it has no AI data.
bool SetIgnoredByAI(uint8_t* character, bool ignored);
const char* SideName(SpawnSide side);

// The running level's player, for display: its class (type name), costume, position and heading
// (degrees about the up axis). `valid` is false outside a level.
struct PlayerInfo {
    bool valid = false;
    std::string className;
    std::string costume;
    float position[3] = {};
    float facing = 0;
    float health = 0, maxHealth = 0;
    bool hasPower = false; // Jedi-like characters have Force power
    float power = 0, maxPower = 0;
};
PlayerInfo CurrentPlayer();
// Every character in the running level (the player, the level's, spawned ones), alive or dead.
std::vector<uint8_t*> LevelCharacters();
// The next LevelCharacters scans again (a character made or taken away).
void InvalidateLevelCharacters();
// A character's size: the object's "Uniform scale" (1 is its own). False when it is not there.
bool CharacterScale(const uint8_t* character, float& scale);
bool SetCharacterScale(uint8_t* character, float scale);
// Sets the running level's player's maximum health and fills it (a clone trooper has a few hits' worth).
void SetPlayerMaxHealth(float health);
// The running level's player object, or null (for research tools such as `peek`).
uint8_t* PlayerObject();
// The character spawned last in the running level, or null (research).
uint8_t* LastSpawnedObject();
// The body a live player change left behind, or null (research).
uint8_t* ReplacedPlayerObject();

// A character's teams (a bit per team A-H, "Team Setting" in the game's level data), from its AI data.
bool CharacterTeams(const uint8_t* character, uint32_t& teams);
bool SetCharacterTeams(uint8_t* character, uint32_t teams);
// Infinite Force: the player's Force kept at its maximum (the game's `god` covers only health).
void SetInfiniteForce(bool on);
bool InfiniteForce();
// Once a frame, on the game thread: keeps up what the switches above ask for.
void PlayerFrame();
// Gives the characters whose activation waited for their setup their first tick once it is done (each frame).
void CharactersFrame();
// Gives the running level's player its maximum health back.
void RefillPlayerHealth();

// Friendly fire between two characters (co-op's players): their blows hurt each other while they stay allies
// (no one else changes sides). Nulls end it.
void SetFriendlyFire(const uint8_t* a, const uint8_t* b);

// The characters spawned in the running level.
int SpawnedCount();
// Removes the characters spawned in the running level (those still there), as the game removes its own
// objects. The number removed.
int RemoveSpawned();
// Removes one spawned character (still there), as RemoveSpawned does. False when it is not one.
bool RemoveSpawnedCharacter(uint8_t* character);

// A character fighting for another (a fighter's reinforcement, e.g. Commander Cody's clones): created and placed at
// `place` (a transform) in any running level, Versus too, on its master's side (its teams; `againstPlayer` when the
// master is the computer's); removed as a spawned character. Null with `error` when it cannot. `costume`: one of
// its class's (empty: the usual).
uint8_t* SpawnHelper(const char* className, const std::string& skin, uint8_t* master, bool againstPlayer,
    const float* place, std::string& error, const std::string& costume = std::string());

// Changes the running level's player at once, where it stands, to the current choice (class, costume,
// texture set, body; the player's own class when none is chosen): a new character takes over the
// controls, the camera and the level's references to the player. False with `error` set when it cannot.
bool ReplacePlayer(std::string& error);
// Resolves every camera focus list again (TCamComPrimaryFocusList, slot 5): their "the player" entries take the
// current player 1. After a player change made while the camera followed something else (a grapple's camera), so the
// camera follows the new player. Returns how many lists.
int RefocusCameraOnPlayer();

// After the port activates a character the level holds (vfunc +0xB4): puts back what its class script had set that
// the activation undid (Grievous's second arms' sabers, switched off with those arms hidden).
void SettleActivated(uint8_t* character);

// The player's saber colour (r, g, b, 0-1), its own only: other characters keep theirs, and power-ups
// no longer change it. Applies at once to a running level's player, and from then on; null goes back
// to the game's colours from the next level start. The pure colours (1,0,0), (0,1,0), (0,0,1),
// (1,0,1) are the game's tuned red, green, blue and purple.
void SetPlayerSaberColor(const float* rgb);
bool PlayerSaberColor(float* rgb);
// A colour as "red", "green", "blue", "purple" or "<r> <g> <b>" (0-255 each).
bool ParseSaberColor(const std::string& spec, float* rgb);

// The game's costume list of a class (its name without the leading I), or null.
const uint8_t* FindVariantList(const char* className);

// The registered character classes that have costumes (all registered classes with `all`), sorted;
// empty until the game has registered them.
std::vector<std::string> CharacterClasses(bool all);

// True once a level's player was created since the last boot (a level is running).
bool PlayerInLevel();

// Whether a change of character restarts the running mission at once ([Debug] AutoRestart, on by
// default); otherwise it applies from the next level start.
void SetRestartOnChange(bool enabled);
// Whether every level loads all of its characters' optional moves (the duel moves among them), so
// that characters in a level not made for them have their whole move set ([Debug] OptionalMoves).
void EnableOptionalMoves(bool enabled);
bool RestartOnChange();

// The character meshes in the disc's PAKs ("folder\file", lower case), those containing `filter`.
std::vector<std::string> CharacterMeshes(const std::string& filter);

// A mesh named by its folder, its file or "folder\file" (with or without meshes\chars and .msh),
// as "folder\file"; empty with `error` set when none or several match. A "folder\file" the disc does
// not have is returned as it is (a loose copy under mods\ may provide it).
std::string ResolveMesh(const std::string& text, std::string& error);

// A character's variant (costume) whose model is on the disc: `preferred` when its model is, else the
// first that is (a class's first variant is not always shipped: the battle droid's plain
// "BattleDroid" model is not, its "hordeBattleDroid" is), else `preferred`. `character` is a created
// character object; its variant list is at +0x1E0.
int VariantOnDisc(const void* character, int preferred);

} // namespace swrots::game
