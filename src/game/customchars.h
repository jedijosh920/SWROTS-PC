#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game {

// Characters of mods' own (docs/modding/custom-characters.md): a file under mods\characters\ (<name>.ini) makes a new
// character class, e.g. ILuke, that the game creates wherever it creates characters by class name (a level's
// objects, Versus, the port's player choice and spawns, Heroes vs Villains). Its moves, sounds and AI are another
// class's (its base, e.g. IAnakin); its body, costume, saber colour, name and side its own. The class lookup
// (0xE9B60) answers the new name with a copy of the base's class info whose create gives the character the custom
// costume list (its own costume first, then the base's).

struct CustomCharacter {
    std::string className;   // e.g. "ILuke"
    std::string name;        // as shown, e.g. "LUKE SKYWALKER"
    std::string title;       // e.g. "JEDI KNIGHT"
    std::string base;        // the class whose moves it has, e.g. "IAnakin"
    std::string costume;     // its costume's name, e.g. "Luke"
    std::string mesh;        // its body under meshes\chars ("folder\file"), e.g. "luke\luke"
    std::string mesh2;       // player 2's body in a Versus mirror (Mesh2=), the same skeleton; empty: its own
    std::string code;        // its dialogue speaker code (three letters: whose lines it says, e.g. "AKN"); empty: the
                             // base's
    std::string speaker;     // its name in subtitles, e.g. "Luke"
    std::string speakerKey;  // the string id suffix the game looks its subtitle name up by (IDS_SPEAKER_<key>)
    // its own voice ([Voice]): for a dialogue event (BAT battle cry, TNT taunt, KIL kill, FSE first sight, HIT hit or
    // saber lock, ...), the lines it says instead of the base's (their ids, upper case, e.g. "LUK003")
    std::vector<std::pair<std::string, std::vector<std::string>>> voice;
    // player 2's in a Versus mirror (both chose it; [Voice2], the same events), e.g. another JK2 Jedi's; empty: its own
    std::vector<std::pair<std::string, std::vector<std::string>>> voice2;
    bool hasSaber = false;
    float saber[3] = {};     // its saber's colour (0-1), when it has one of its own
    bool hasSaber2 = false;
    float saber2[3] = {};    // player 2's in a Versus mirror (both chose it; Saber2=), as the game's dark Anakin's
    bool hasSaberLeft = false;
    float saberLeft[3] = {}; // its second saber's colour, the one in its left hand (SaberLeft=), on a base with two
    int blades = 0;          // its saber (Blades=): 0 the base's, 1 single, 2 double-bladed, 3 dual (two sabers)
    std::string hilt;        // its saber's hilt (Hilt=): a weapon mesh, "Meshes\Weapons\<folder>\<file>"; empty: the base's
    float scale = 1.0f;      // its size (Scale=), 1 the base's
    bool breathing = true;   // on Vader's class: his breathing (Breathing=0: none)
    float force = -1.0f;     // its Force (Force=, the most it holds; Anakin's is 1000), negative: the base's
    float health = -1.0f;    // its health (Health=; Anakin's 1000, the Jedi Brute's 220), negative: the base's
    // Force powers on a base whose controls have none (ForceFrom=IVader: the Jedi's Force controls, as Vader's; the
    // Jedi Brute's): which ones (ForcePowers=, the Jedi's defaults when empty: push, grip, saber throw, heal)
    bool forceControls = false;
    std::string forceFrom;   // that class (ForceFrom=), e.g. "IVader": its attack table's Force rows for the computer
    int forcePowers = -1;    // bits: 1 push, 2 grip, 4 saber throw, 8 lightning, 16 heal; -1 the defaults
    int intro = -1;          // whose Versus intro and outro it plays (Intro=, a duelist: its animations, line timing and
                             // cameras), an index into the port's duelists; -1 the base's own
    std::string introAnimation; // its intro is this animation of its base's instead (IntroAnimation=, e.g. the Jedi
    float introSpeed = 1.0f;    // Brute's twirl), played as a duelist's intro is (no attack), at this speed
    std::string outroAnimation; // its outro (a round won) likewise (OutroAnimation=, OutroSpeed=)
    float outroSpeed = 1.0f;
    std::string introLine, outroLine; // the line said in them (IntroLine=, OutroLine=: a dialogue line id, e.g. CMC951)
    int side = 0;            // 0 hero, 1 villain (Heroes vs Villains' default side)
    // moves of other duelists on a base of Serra Keto's class (ISerra, revan.cpp): Force lightning instead of her stun
    // (Lightning=1), Count Dooku's grapple throws instead of hers (Grapples=IDooku), the Force Storm special
    // (ForceStorm=1: lightning striking the enemies near it, for a part of its Force)
    bool lightning = false;
    std::string grapples;
    bool forceStorm = false;
    std::string hudFace;     // the HUD portrait shown for it (a donor's path, its picture served: see customchars.cpp)
    std::string face;        // its HUD portrait's path (interfc\hud_b\hud_face_<costume>01), when the mod has one
    std::string duelFace;    // player 2's HUD portrait in Versus (<face>_duel, beside its <texture>_duel look), if any
    std::string source;      // the file it came from
    // a fighter of the port's own, made from the game's assets (no files: Commander Cody): its Versus pictures are the
    // disc's (selectBust, a picture the menus' PAK has) and copies of disc pictures the port writes (generated)
    bool builtIn = false;
    std::string selectBust;  // e.g. "s_selduel_codybust"
    // a texture set of its own (the costume list header's suffixes, +0x14.., one picked by character +0x1EC): its body's
    // textures are asked for with this suffix (e.g. "_cody": hordetrooper_cody.stx), made by a generator; the one named
    // here is declared to the level when it is created (the body's load asks only for declared ones)
    std::string textureSet;
    std::string textureSetFile;
};

// Each start of the game image, before the classes are first looked up: reads mods\characters\*.ini and hooks the
// class lookup.
// `gameData` and `cache`: for a custom character's own voice lines (the dialogue archive served with them added).
void InstallCustomCharacters(const std::wstring& mods, const std::wstring& gameData, const std::wstring& cache);

// The custom characters (this start's), and one by class name (case-insensitive), or null.
const std::vector<CustomCharacter>& CustomCharacters();
const CustomCharacter* FindCustomCharacter(const char* className);

// True when the mods folder has this file (relative, e.g. a custom character's pictures), from the start of the
// game on (before the resource hooks are installed).
bool ModFileExists(const std::string& relative);

// Each frame: what custom characters keep up (their second saber's own colour, SaberLeft=).
void CustomCharactersFrame();

// The custom character a character is (recognised by its costume list, the port's own), or null.
const CustomCharacter* CustomCharacterOf(const uint8_t* character);
// True for Versus player 2 when both players chose the same fighter (its look's texture suffix "_duel").
bool IsMirrorPlayer2(const uint8_t* character);
// The HUD portrait a custom character shows now (player 2's in a Versus mirror, else its own).
const std::string& CustomFaceOf(const uint8_t* character, const CustomCharacter& custom);
// The next line `speaker` starts for `event` (three letters, e.g. "TKE": an event its speaker code has entries for)
// says `lineId` instead (a line of the mounted dialogue archives, e.g. "CLC013"), for the next few seconds; the event is
// then asked as usual (the script's SayEvent, 0x17ACC0, in the character's script turn).
void ForceNextLine(const uint8_t* speaker, const char* event, const char* lineId);

// The duel cameras' name of the duelist whose intro a custom character plays (Intro=), or null for the base's.
const char* IntroCameras(const CustomCharacter& custom);
// The file's contents; false when there is none.
bool ReadModFile(const std::string& relative, std::vector<uint8_t>& data);

} // namespace swrots::game
