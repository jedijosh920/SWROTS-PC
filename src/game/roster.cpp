// Versus roster: select-screen slots for fighters beyond the game's nine.
//
// The versus select screen is the menu interfc\front_end\xml\select_jedi.xbl_xml with the handler
// "JediList" (0x2D54D0). Its slots are the game's per-slot tables' indexes: 0-8 the fighters, 9
// Random. The number of cells is the size of the menu's HeadsTextSet less one (its last picture is
// the "locked" one), so a slot is added by adding a picture; the pictures, names and titles of a
// slot are found by its number, and so are its locks, which the profile keeps for slots 2-8 only.
// Extra fighters take slots from 10 (kFirstExtraSlot); for them this file adds the pictures to the
// menu as it loads, the names and titles, and keeps them unlocked without touching the profile.
// The duel reads a slot's class and saber colours from tables of nine, which this file extends;
// the arena select screen that follows shows the fighters' full-body pictures by slot as well.

#include "game/roster.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "game/characters.h"
#include "game/custommaps.h"
#include "game/customchars.h"
#include "game/game.h"
#include "game/menus.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

// Extra fighters show no full-body picture (the disc has none for them), as locked fighters do.
struct ExtraFighter {
    const char* className;
    const char* head;    // interfc\front_end\<head>.stx, the grid cell (64x64)
    const char* bust;    // the bust above the grid (256x128)
    const char* nameId;  // gameinfo\strings text IDs
    const char* titleId;
    const char* cameras; // the duelist whose intro and win cameras it uses when it has none
    float sabers[2][3];  // per player an RGB saber colour, 0 0 0 for the character's own, when it
                         // fights itself (the game's fighters have one for player 2 always)
    const char* meshes;  // the textures' folder, whose player 2 look (the engine's <texture>_duel,
                         // see DarkLookGenerator) is made when the disc has none, or null
    // its own pictures, loaded from their files (paths without .stx; null: none, or the disc's above): the head, the
    // bust, player 1's and player 2's full body (see Pictures below)
    const char* files[4];
};

// Yoda: a planned fighter (the disc has his select head, bust and HUD portrait, and his name in
// every language).
constexpr ExtraFighter kYoda = { "IYoda", "s_selduel_yodahead", "s_selduel_yodabust", "IDS_SHELL_YODA",
    "IDS_SHELL_JEDI_MASTER", "Anakin", { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, "meshes\\chars\\yoda\\" };
constexpr int kFighterCount = 9;
constexpr int kRandomSlot = 9;
constexpr int kFirstExtraSlot = 10;
// Custom characters (customchars.h) with their select pictures join the extras, up to this many in all (Yoda, the
// packs' characters, Commander Cody and the Droid Commander). The bound is player 2's full-body offset, a signed 8-bit
// displacement (see InstallRoster): (kFirstExtraSlot + kMaxExtras + 1) * 4 <= 0x7F.
constexpr int kMaxExtras = 20;

// This start's extras: Yoda, then the custom characters; their texts (owned here: the strings live for the session).
std::vector<ExtraFighter> g_Extras;
std::deque<std::string> g_ExtraStrings;
int g_ExtraCount = 0;
int g_SlotCount = kFirstExtraSlot; // cells on the grid

const ExtraFighter* Extra(int slot)
{
    return slot >= kFirstExtraSlot && slot < g_SlotCount ? &g_Extras[size_t(slot - kFirstExtraSlot)] : nullptr;
}

// --- The menu's picture sets --------------------------------------------------------------------
// A texture set in an .xbl_xml menu: the strings "textureSet" and the set's name (each a u32 length
// and the text), a u32 count, then per picture the strings "texture", folder and name and 28 bytes
// {0, 0, float width, float height, 0, 0, 0}.

struct Picture {
    std::string folder;
    std::string name;
    uint8_t tail[28];
};

bool ReadText(const std::vector<uint8_t>& data, size_t& pos, std::string& text)
{
    if (pos + 4 > data.size())
        return false;
    uint32_t length;
    std::memcpy(&length, &data[pos], 4);
    if (length > 256 || pos + 4 + length > data.size())
        return false;
    text.assign(reinterpret_cast<const char*>(&data[pos + 4]), length);
    pos += 4 + length;
    return true;
}

void WriteText(std::vector<uint8_t>& out, const std::string& text)
{
    uint32_t length = uint32_t(text.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&length), reinterpret_cast<const uint8_t*>(&length) + 4);
    out.insert(out.end(), text.begin(), text.end());
}

std::vector<uint8_t> Marker(const char* setName)
{
    std::vector<uint8_t> marker;
    WriteText(marker, "textureSet");
    WriteText(marker, setName);
    return marker;
}

// Reads a set's pictures; `begin`/`end` delimit its pictures (after the count) in `data`.
bool ReadSet(const std::vector<uint8_t>& data, const char* setName, std::vector<Picture>& pictures, size_t& countAt,
    size_t& end)
{
    std::vector<uint8_t> marker = Marker(setName);
    auto found = std::search(data.begin(), data.end(), marker.begin(), marker.end());
    if (found == data.end())
        return false;
    size_t pos = size_t(found - data.begin()) + marker.size();
    if (pos + 4 > data.size())
        return false;
    countAt = pos;
    uint32_t count;
    std::memcpy(&count, &data[pos], 4);
    pos += 4;
    pictures.clear();
    for (uint32_t i = 0; i < count; ++i) {
        Picture picture;
        std::string kind;
        if (!ReadText(data, pos, kind) || kind != "texture" || !ReadText(data, pos, picture.folder) ||
            !ReadText(data, pos, picture.name) || pos + sizeof(picture.tail) > data.size())
            return false;
        std::memcpy(picture.tail, &data[pos], sizeof(picture.tail));
        pos += sizeof(picture.tail);
        pictures.push_back(picture);
    }
    end = pos;
    return true;
}

void WriteSet(std::vector<uint8_t>& data, size_t countAt, size_t end, const std::vector<Picture>& pictures)
{
    std::vector<uint8_t> out;
    uint32_t count = uint32_t(pictures.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&count), reinterpret_cast<const uint8_t*>(&count) + 4);
    for (const Picture& picture : pictures) {
        WriteText(out, "texture");
        WriteText(out, picture.folder);
        WriteText(out, picture.name);
        out.insert(out.end(), picture.tail, picture.tail + sizeof(picture.tail));
    }
    data.erase(data.begin() + ptrdiff_t(countAt), data.begin() + ptrdiff_t(end));
    data.insert(data.begin() + ptrdiff_t(countAt), out.begin(), out.end());
}

Picture Like(const Picture& model, const std::string& name)
{
    Picture picture = model;
    picture.name = name;
    return picture;
}

Picture Sized(Picture picture, float width, float height)
{
    std::memcpy(picture.tail + 8, &width, 4);
    std::memcpy(picture.tail + 12, &height, 4);
    return picture;
}

// The select screen's sets, with slot numbers as their indexes:
//   HeadsTextSet    fighters 0-8, Random, [extras], Locked (the last)
//   BustsTextSet    fighters 0-8, [unused 9, extras], evil Anakin, Random, Locked (the last three
//                   are taken from the end: player 2's Anakin against Anakin, Random, locked)
//   FullBodyTextSet player 1's 0-8, player 2's 0-8 (the same fighter as player 1 takes its +9
//                   picture), Random (the last); with extras, two blocks of g_SlotCount + 1 and the
//                   player 2 offset patched to match (kBodyOffsetSite).
// `standIns`: the extras' busts in their full-body pictures' place, at the busts' own size (in the
// full-body pictures' 128x256 the engine repeats their edges); otherwise the Random picture (not
// shown, see BodiesHook).
std::vector<Picture> WithExtraBodies(const std::vector<Picture>& bodies, bool standIns)
{
    const Picture random = bodies.back();
    std::vector<Picture> newBodies;
    for (int block = 0; block < 2; ++block) {
        for (int slot = 0; slot < kFighterCount; ++slot)
            newBodies.push_back(bodies[size_t(block * kFighterCount + slot)]);
        // Random's slot (its picture is taken from the end), the extras' and one more: blocks of
        // g_SlotCount + 1, a round 12 with one extra.
        newBodies.push_back(random);
        for (const ExtraFighter& extra : g_Extras)
            newBodies.push_back(standIns && !extra.files[2 + block] ? Sized(Like(random, extra.bust), 256.0f, 128.0f)
                : random);
        newBodies.push_back(random);
    }
    newBodies.push_back(random);
    return newBodies;
}

void PatchSelectScreen(std::vector<uint8_t>& data)
{
    std::vector<Picture> heads, busts, bodies;
    size_t headsAt, headsEnd, bustsAt, bustsEnd, bodiesAt, bodiesEnd;
    // Read back to front: rewriting a set moves what follows it.
    if (!ReadSet(data, "FullBodyTextSet", bodies, bodiesAt, bodiesEnd) || bodies.size() != 2 * kFighterCount + 1 ||
        !ReadSet(data, "BustsTextSet", busts, bustsAt, bustsEnd) || busts.size() != kFighterCount + 3 ||
        !ReadSet(data, "HeadsTextSet", heads, headsAt, headsEnd) || heads.size() != kFighterCount + 2) {
        LOG_WARN("Roster: the select screen's pictures are not as expected; no extra fighters");
        return;
    }

    std::vector<Picture> newBodies = WithExtraBodies(bodies, false);

    std::vector<Picture> newBusts(busts.begin(), busts.begin() + kFighterCount);
    newBusts.push_back(busts[kFighterCount + 1]); // unused (Random's comes from the end)
    for (int i = 0; i < g_ExtraCount; ++i)
        newBusts.push_back(Like(busts[0], g_Extras[size_t(i)].bust));
    newBusts.insert(newBusts.end(), busts.end() - 3, busts.end());

    std::vector<Picture> newHeads(heads.begin(), heads.end() - 1);
    for (int i = 0; i < g_ExtraCount; ++i)
        newHeads.push_back(Like(heads[0], g_Extras[size_t(i)].head));
    newHeads.push_back(heads.back());

    WriteSet(data, bodiesAt, bodiesEnd, newBodies);
    WriteSet(data, bustsAt, bustsEnd, newBusts);
    WriteSet(data, headsAt, headsEnd, newHeads);
    LOG_INFO("Roster: select screen with %d extra fighter(s)", g_ExtraCount);
}

// The arena select screen (select_arena.xbl_xml) shows the chosen fighters' full-body pictures from
// a FullBodyTextSet like the select screen's (0x2D2240; player 2's offset at kArenaBodyOffsetSite),
// and nothing else of them, so extras show their busts there.
void PatchArenaScreen(std::vector<uint8_t>& data)
{
    std::vector<Picture> bodies, small, large;
    size_t bodiesAt, bodiesEnd, smallAt, smallEnd, largeAt, largeEnd;
    // back to front (rewriting a set moves what follows it)
    if (!ReadSet(data, "FullBodyTextSet", bodies, bodiesAt, bodiesEnd) || bodies.size() != 2 * kFighterCount + 1) {
        LOG_WARN("Roster: the arena screen's pictures are not as expected");
        return;
    }
    WriteSet(data, bodiesAt, bodiesEnd, WithExtraBodies(bodies, true));
    // the custom maps' entries (hvvmenu.cpp) take places after the game's own in both arena picture sets: copies of
    // a game arena's pictures, drawn with the map's own (DrawRecordWith)
    const size_t maps = CustomMapsFor(MapMode::Versus).size();
    if (!maps)
        return;
    if (!ReadSet(data, "LargeArenaTextSet", large, largeAt, largeEnd) || large.size() != 16 ||
        !ReadSet(data, "SmallArenaTextSet", small, smallAt, smallEnd) || small.size() != 15) {
        LOG_WARN("Roster: the arena screen's arena pictures are not as expected; no custom maps on it");
        return;
    }
    // (the later set first: LargeArenaTextSet follows SmallArenaTextSet or not, either way the positions are re-read)
    const bool largeFirst = largeAt < smallAt;
    std::vector<Picture> newSmall = small, newLarge = large;
    for (size_t i = 0; i < maps; ++i) {
        newSmall.push_back(small[3]);  // the Utapau arena's (the duel's level)
        newLarge.push_back(large[4]);
    }
    if (largeFirst) {
        WriteSet(data, smallAt, smallEnd, newSmall);
        WriteSet(data, largeAt, largeEnd, newLarge);
    } else {
        WriteSet(data, largeAt, largeEnd, newLarge);
        WriteSet(data, smallAt, smallEnd, newSmall);
    }
}

// --- Player 2's look ------------------------------------------------------------------------------
// The duel gives player 2 the suffix "_duel" (0x27B88D); his character's textures are then
// <texture>_duel where the level has one (0x14F7E0 checks it is declared), as the nine fighters
// have for their duels (e.g. meshes\chars\darthvader\vaderhead_duel.stx). Extra fighters have none
// on the disc, so darker, greyer copies of their textures are made when an extra fights itself
// (the game's evil Anakin is for Anakin against Anakin): STX textures (a 388-byte header with the
// texture's name at 0x40, then DXT1 blocks for format 3 or DXT3/5 blocks for 4, not swizzled) are
// edited block by block -- a block's two endpoint colours changed, its DXT1 transparency mode kept
// -- and named <name>_duel. (Menus find pictures among the textures loaded; loading a generated
// one with a menu crashed the texture's creation, so the menus show the normal bust.)
constexpr char kDuelSuffix[] = "_duel";
constexpr size_t kStxHeader = 0x184;
constexpr size_t kStxName = 0x40;
constexpr size_t kStxNameSize = 0x28;

uint16_t DarkColour(uint16_t colour)
{
    float rgb[3] = { ((colour >> 11) & 31) * 255.0f / 31, ((colour >> 5) & 63) * 255.0f / 63, (colour & 31) * 255.0f / 31 };
    const float grey = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    const float tint[3] = { 0.95f, 0.92f, 1.08f }; // cold
    int out[3];
    for (int i = 0; i < 3; ++i) {
        float c = (rgb[i] + (grey - rgb[i]) * 0.95f) * 0.22f * tint[i]; // nearly grey, much darker
        out[i] = int(std::min(255.0f, std::max(0.0f, c)) + 0.5f);
    }
    return uint16_t(((out[0] * 31 / 255) << 11) | ((out[1] * 63 / 255) << 5) | (out[2] * 31 / 255));
}

// Swaps the endpoints of a DXT colour block: indices 0 <-> 1, and 2 <-> 3 with four colours.
uint32_t SwappedIndices(uint32_t indices, bool fourColours)
{
    uint32_t out = 0;
    for (int i = 0; i < 16; ++i) {
        uint32_t index = (indices >> (2 * i)) & 3;
        index = index < 2 ? index ^ 1 : (fourColours ? index ^ 1 : index);
        out |= index << (2 * i);
    }
    return out;
}

void DarkenColourBlock(uint8_t* block, bool dxt1)
{
    uint16_t c0, c1;
    uint32_t indices;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    std::memcpy(&indices, block + 4, 4);
    uint16_t n0 = DarkColour(c0), n1 = DarkColour(c1);
    if (dxt1 && c0 > c1 && n0 <= n1) { // four colours: c0 > c1 must stay
        if (n0 == n1) {
            if (n0 < 0xFFFF)
                ++n0;
            else
                --n1;
        } else {
            std::swap(n0, n1);
            indices = SwappedIndices(indices, true);
        }
    } else if (dxt1 && c0 <= c1 && n0 > n1) { // three colours and transparent: c0 <= c1 must stay
        std::swap(n0, n1);
        indices = SwappedIndices(indices, false);
    }
    std::memcpy(block, &n0, 2);
    std::memcpy(block + 2, &n1, 2);
    std::memcpy(block + 4, &indices, 4);
}

bool DarkenStx(std::vector<uint8_t>& data)
{
    if (data.size() <= kStxHeader || std::memcmp(data.data(), "STX", 4) != 0)
        return false;
    uint32_t format;
    std::memcpy(&format, &data[8], 4);
    if (format != 3 && format != 4)
        return false;
    const bool dxt1 = format == 3;
    const size_t blockSize = dxt1 ? 8 : 16;
    for (size_t at = kStxHeader; at + blockSize <= data.size(); at += blockSize) // mip levels too
        DarkenColourBlock(&data[at + (dxt1 ? 0 : 8)], dxt1);
    char* name = reinterpret_cast<char*>(&data[kStxName]);
    size_t length = strnlen(name, kStxNameSize);
    if (length + sizeof(kDuelSuffix) > kStxNameSize)
        return false;
    std::memcpy(name + length, kDuelSuffix, sizeof(kDuelSuffix));
    return true;
}

std::string DuelName(const std::string& texture) // meshes\x\y.stx -> meshes\x\y_duel.stx
{
    return texture.substr(0, texture.size() - 4) + kDuelSuffix + ".stx";
}

bool DarkLookGenerator(const std::string& lowerName, std::vector<uint8_t>& data)
{
    const std::string tail = std::string(kDuelSuffix) + ".stx";
    if (lowerName.size() <= tail.size() || lowerName.compare(lowerName.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    const std::string base = lowerName.substr(0, lowerName.size() - tail.size()) + ".stx";
    bool ours = false;
    for (const ExtraFighter& extra : g_Extras) {
        ours = ours || (extra.meshes && base.rfind(extra.meshes, 0) == 0);
    }
    return ours && ReadDiscResource(base, data) && DarkenStx(data);
}

// Declares an extra fighter's player 2 textures in the duel's level, for the engine's check.
void DeclareDarkLook(const ExtraFighter& extra)
{
    if (!extra.meshes)
        return;
    for (const std::string& name : DiscResourceNames(extra.meshes)) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".stx") == 0 && name.find(kDuelSuffix) == std::string::npos &&
            !DiscHasResource(DuelName(name)))
            DeclareGeneratedResource(DuelName(name), 0); // type 0: textures
    }
}

// --- The select screen's code -------------------------------------------------------------------

// Player 2's full-body picture for the fighter player 1 also chose: mov eax, [edx + eax*4 + 0x24]
// (9 pictures on); the blocks are g_SlotCount + 1 long with extras.
constexpr uint32_t kBodyOffsetSite = 0x002D4ECE;
constexpr uint8_t kBodyOffsetBytes[] = { 0x8B, 0x44, 0x82, 0x24 };

// The locks the select screen checks inline: cmp edi, 9 / je <unlocked> / (the profile's byte for
// the slot); jae makes every slot from Random on unlocked, so extras never read the profile. The
// grid's own drawing (0x2D57A4) checks with esi.
constexpr uint32_t kInlineLockSites[] = { 0x002D49A8, 0x002D4A4F, 0x002D4B48, 0x002D4BEF, 0x002D4CD4, 0x002D4DEB,
    0x002D4E95, 0x002D4F6C, 0x002D4FAA };
constexpr uint32_t kGridLockSite = 0x002D57A4;

// The players' full-body pictures (thiscall (), 0x2D4DB0; +0x70 player 1's, +0x74 player 2's
// picture item, its texture at +0x1C, 0 for none as for a locked fighter).
constexpr uint32_t kSetBodies = 0x002D4DB0;
constexpr uint8_t kSetBodiesPrologue[] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7E, 0x2C }; // push esi; mov esi, ecx; push edi; mov edi, [esi+0x2C]
using SetBodiesFn = void(__fastcall*)(uint8_t* screen, void* edx);
SetBodiesFn g_OriginalSetBodies = nullptr;

void __fastcall BodiesHook(uint8_t* screen, void* edx)
{
    g_OriginalSetBodies(screen, edx);
    for (int player = 0; player < 2; ++player) {
        const ExtraFighter* extra = Extra(*reinterpret_cast<int*>(screen + 0x2C + player * 4));
        if (extra && !extra->files[2 + player])
            *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(screen + 0x70 + player * 4) + 0x1C) = nullptr;
    }
}

// Whether a slot is locked for choosing (thiscall (slot), 0x2D4920): true stops the choice.
constexpr uint32_t kIsLocked = 0x002D4920;
constexpr uint8_t kIsLockedPrologue[] = { 0x8B, 0x0D, 0x24, 0x8F, 0x7E, 0x00 }; // mov ecx, [0x7E8F24]
using IsLockedFn = bool(__fastcall*)(void* screen, void* edx, int slot);
IsLockedFn g_OriginalIsLocked = nullptr;

bool __fastcall IsLockedHook(void* screen, void* edx, int slot)
{
    if (Extra(slot))
        return false;
    return g_OriginalIsLocked(screen, edx, slot);
}

// Random (0x2D4CF0): the game counts the unlocked fighters and takes rand() % count (0xA23D0,
// cdecl) as the slot, which assumes the unlocked ones come first and knows only the nine. At both
// calls (each player's), a fighter is chosen from the unlocked ones and the extras instead.
constexpr uint32_t kRandomCalls[] = { 0x002D4CF7, 0x002D4D09 };
constexpr uint32_t kRandomBelow = 0x000A23D0; // cdecl (n): rand() % n
constexpr uint32_t kProfileSource = 0x007E8F24; // [..] thiscall 0x24CC50 -> the profile
constexpr uint32_t kProfile = 0x0024CC50;

int __cdecl RandomFighter(int)
{
    auto* profile = reinterpret_cast<uint8_t*(__fastcall*)(void*, void*)>(uintptr_t(kProfile))(
        *reinterpret_cast<void**>(uintptr_t(kProfileSource)), nullptr);
    int slots[kFirstExtraSlot + kMaxExtras];
    int count = 0;
    for (int slot = 0; slot < kFighterCount; ++slot) {
        if (slot <= 1 || (profile && profile[0x2A8 + slot])) // as IsLocked (0x2D4920)
            slots[count++] = slot;
    }
    for (int i = 0; i < g_ExtraCount; ++i)
        slots[count++] = kFirstExtraSlot + i;
    return slots[reinterpret_cast<int(__cdecl*)(int)>(uintptr_t(kRandomBelow))(count)];
}

// The arena select screen (0x2D2240, from the two slots) shows the fighters' names, titles and
// full-body pictures like the select screen, player 2's offset for the same fighter as player 1 at
// kArenaBodyOffsetSite. Extra fighters show their bust there, stretched (PatchArenaScreen).
constexpr uint32_t kArenaBodyOffsetSite = 0x002D23D2;
constexpr uint8_t kArenaBodyOffsetBytes[] = { 0x8B, 0x74, 0xB5, 0x24 }; // mov esi, [ebp + esi*4 + 0x24]

// --- The duel's tables ----------------------------------------------------------------------------
// The shell puts the chosen fighters' class names in the launch settings ([0x7F33FC] +4 and +8),
// taking a slot's class from the class table (kDuelistClasses, nine fighters and a null; read by
// slot at 0x2C6644). The duel level then (0x27D980) creates both players' fighters of every class in
// the table, noting those whose class names are the chosen ones, and releases the rest; its slots
// are the table's indexes. The roster has two longer tables:
//   g_Classes      the select screen's slots: the nine, Random's (unused), the extras
//   g_DuelClasses  the duel's, made when the duel sets up: the nine and the extras chosen, so an
//                  extra fighter is loaded only for a duel it is in
// Saber colours (0x650C98: per duel slot and player three floats, all 0 for none; the fighter
// keeps a pointer to them) have a longer copy too, with none for the extras.
const char* g_Classes[kFirstExtraSlot + kMaxExtras + 1] = {};
constexpr int kDuelSlotCount = kFighterCount + kMaxExtras;
const char* g_DuelClasses[kDuelSlotCount + 1] = {};
float g_SaberColours[kDuelSlotCount * 2 * 3] = {};

struct TableSite {
    uint32_t address;
    uint8_t bytes[9];
    uint32_t length;
    uint32_t displacementAt;
};
constexpr TableSite kSelectClassSite = { 0x002C6644, { 0x8B, 0x04, 0x85, 0x08, 0x0C, 0x65, 0x00 }, 7, 3 }; // 0x2C6640
constexpr TableSite kDuelClassSites[] = { // the table, but the last: the table + 4
    { 0x0027B7D5, { 0x8B, 0x04, 0x9D, 0x08, 0x0C, 0x65, 0x00 }, 7, 3 }, // fighter creation
    { 0x0027BB8F, { 0x8B, 0x14, 0x9D, 0x08, 0x0C, 0x65, 0x00 }, 7, 3 }, // duel cameras
    { 0x0027D992, { 0xA1, 0x08, 0x0C, 0x65, 0x00 }, 5, 1 },             // matching the chosen names
    { 0x0027D99D, { 0xB8, 0x08, 0x0C, 0x65, 0x00 }, 5, 1 },
    { 0x0027DA0E, { 0xA1, 0x08, 0x0C, 0x65, 0x00 }, 5, 1 },             // creating the fighters
    { 0x0027DBA6, { 0x8B, 0x04, 0xAD, 0x0C, 0x0C, 0x65, 0x00 }, 7, 3 }, // the next class
};

// The duel's fighter setup (thiscall (), 0x27D980).
constexpr uint32_t kDuelSetup = 0x0027D980;
constexpr uint8_t kDuelSetupPrologue[] = { 0x83, 0xEC, 0x60, 0x53, 0x55, 0x56, 0x8B, 0xE9 }; // sub esp, 0x60; push ebx/ebp/esi; mov ebp, ecx
constexpr uint32_t kLaunchSettings = 0x007F33FC;
using DuelSetupFn = void(__fastcall*)(void* manager, void* edx);
DuelSetupFn g_OriginalDuelSetup = nullptr;

// Both players chose `extra`: player 2 then has its player 2 look and saber (as the game's
// evil Anakin for Anakin against Anakin); otherwise it looks as player 1.
bool Mirror(const ExtraFighter& extra)
{
    auto* launch = *reinterpret_cast<uint8_t**>(uintptr_t(kLaunchSettings));
    for (int player = 0; launch && player < 2; ++player) {
        const char* chosen = *reinterpret_cast<const char**>(launch + 4 + player * 4);
        if (!chosen || _stricmp(chosen, extra.className) != 0)
            return false;
    }
    return launch != nullptr;
}

void BuildDuelClasses()
{
    std::memcpy(g_DuelClasses, reinterpret_cast<const void*>(uintptr_t(kDuelistClasses)), kFighterCount * sizeof(g_DuelClasses[0]));
    int count = kFighterCount;
    auto* launch = *reinterpret_cast<uint8_t**>(uintptr_t(kLaunchSettings));
    for (const ExtraFighter& extra : g_Extras) {
        bool inTable = false; // put in a slot of the nine with the duelist command
        for (int slot = 0; slot < kFighterCount; ++slot)
            inTable = inTable || (g_DuelClasses[slot] && _stricmp(g_DuelClasses[slot], extra.className) == 0);
        for (int player = 0; launch && !inTable && player < 2; ++player) {
            const char* chosen = *reinterpret_cast<const char**>(launch + 4 + player * 4);
            if (chosen && _stricmp(chosen, extra.className) == 0) {
                LOG_INFO("Roster: %s fights in duel slot %d", extra.className, count);
                if (Mirror(extra))
                    std::memcpy(&g_SaberColours[count * 2 * 3], extra.sabers, sizeof(extra.sabers));
                else
                    std::memset(&g_SaberColours[count * 2 * 3], 0, sizeof(extra.sabers));
                g_DuelClasses[count++] = extra.className;
                break;
            }
        }
    }
    g_DuelClasses[count] = nullptr;
    // a chosen fighter the duel does not have (a custom character no longer installed, named by a config): the
    // game's own fighter of that player's side stands in (the duel would find no fighter and stop)
    for (int player = 0; launch && player < 2; ++player) {
        const char*& chosen = *reinterpret_cast<const char**>(launch + 4 + player * 4);
        bool known = false;
        for (int slot = 0; slot < count && chosen; ++slot)
            known = known || (g_DuelClasses[slot] && _stricmp(g_DuelClasses[slot], chosen) == 0);
        if (!known && g_DuelClasses[player]) {
            LOG_WARN("Roster: player %d's fighter %s is not installed; %s fights instead", player + 1,
                chosen ? chosen : "(none)", g_DuelClasses[player]);
            chosen = g_DuelClasses[player];
        }
    }
}

// The fighter a player chose: the fighter creation (0x27B7D0, ebx the duel slot) keeps a slot's fighter for the
// player whose chosen name equals the fighter's class name (vfunc +0x40), compared at 0x27B8CD and 0x27B983. A
// custom character's fighter is its base class (customchars.h), so the slot's own class name stands in for it.
constexpr uint32_t kChosenCompareSites[] = { 0x0027B8CD, 0x0027B983 };
constexpr uint32_t kCompareNames = 0x002233F0; // cdecl (a, b) -> 0 when equal

int __cdecl CompareChosen(const char* a, const char* b, int slot)
{
    const auto compare = reinterpret_cast<int(__cdecl*)(const char*, const char*)>(uintptr_t(kCompareNames));
    if (slot < 0 || slot >= kDuelSlotCount || !g_DuelClasses[slot])
        return compare(a, b);
    // which one is the chosen name (the launch settings' own string): it against the slot's class, so a custom
    // character's fighter (an object of its base class: Luke's an IAnakin) is not also taken for its base chosen by
    // the other player (Luke against Anakin was Luke against Luke)
    auto* launch = *reinterpret_cast<uint8_t**>(uintptr_t(kLaunchSettings));
    for (int player = 0; launch && player < 2; ++player) {
        const char* chosen = *reinterpret_cast<const char**>(launch + 4 + player * 4);
        if (chosen && (chosen == a || chosen == b))
            return compare(chosen, g_DuelClasses[slot]);
    }
    const bool customA = FindCustomCharacter(a) != nullptr, customB = FindCustomCharacter(b) != nullptr;
    if (!customA && !customB)
        return compare(a, b);
    return compare(customA ? a : b, g_DuelClasses[slot]);
}

__declspec(naked) void CompareChosenStub()
{
    __asm {
        push ebx                  // the duel slot
        push dword ptr [esp + 0xC]
        push dword ptr [esp + 0xC]
        call CompareChosen
        add esp, 0xC
        ret
    }
}

// The fighter creation gives a fighter its slot's saber colour (0x27B872: mov [edi + 0xF20], ecx, when the slot has a
// colour): only a Jedi has that field (IsA the IJedi type 0x249950, vfunc +4); a clone's object ends before it.
constexpr uint32_t kFighterSaberStore = 0x0027B872;
constexpr uint8_t kFighterSaberStoreBytes[] = { 0x89, 0x8F, 0x20, 0x0F, 0x00, 0x00 };

void __stdcall FighterSaberStore(uint8_t* fighter, const float* colour)
{
    const auto isA = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>(
        (*reinterpret_cast<void* const* const*>(fighter))[1]);
    if (isA(fighter, nullptr, 0x00249950))
        *reinterpret_cast<const float**>(fighter + 0xF20) = colour;
}

__declspec(naked) void FighterSaberStoreStub()
{
    __asm {
        pushad
        push ecx
        push edi
        call FighterSaberStore
        popad
        ret
    }
}

void __fastcall DuelSetupHook(void* manager, void* edx)
{
    BuildDuelClasses();
    for (int slot = 0; g_DuelClasses[slot]; ++slot) { // also in a slot of the nine (duelist)
        for (const ExtraFighter& extra : g_Extras) {
            if (_stricmp(extra.className, g_DuelClasses[slot]) == 0 && Mirror(extra))
                DeclareDarkLook(extra);
        }
    }
    g_OriginalDuelSetup(manager, edx);
}
constexpr TableSite kSaberSites[] = { // displacement: the table + 8, + 4, + 0, + 0
    { 0x0027B843, { 0xF3, 0x0F, 0x10, 0x04, 0x8D, 0xA0, 0x0C, 0x65, 0x00 }, 9, 5 },
    { 0x0027B84C, { 0xF3, 0x0F, 0x58, 0x04, 0x8D, 0x9C, 0x0C, 0x65, 0x00 }, 9, 5 },
    { 0x0027B855, { 0xF3, 0x0F, 0x58, 0x04, 0x8D, 0x98, 0x0C, 0x65, 0x00 }, 9, 5 },
    { 0x0027B865, { 0x8D, 0x0C, 0x8D, 0x98, 0x0C, 0x65, 0x00 }, 7, 3 },
};
constexpr uint32_t kSaberColours = 0x00650C98;

// A slot's name and title (stdcall (TString* out, slot), 0x2C6610 and 0x2C65E0): the text for the
// text ID in a table of ten (0x7F2F2C, 0x7F2F54: the fighters and Random).
constexpr uint32_t kSlotName = 0x002C6610;
constexpr uint32_t kSlotTitle = 0x002C65E0;
constexpr uint32_t kSlotNames = 0x007F2F2C;
constexpr uint32_t kSlotTitles = 0x007F2F54;
constexpr uint32_t kTStringFromText = 0x00222FA0; // cdecl (TString* out, const char*)

void* SlotText(void* out, const void* textId)
{
    auto* services = *reinterpret_cast<uint8_t**>(uintptr_t(kEngineServices));
    auto* strings = *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(services + 0x38) + 0x94);
    using TextFn = void(__fastcall*)(void*, void*, void*, const void*, int);
    (*reinterpret_cast<TextFn* const*>(strings))[1](strings, nullptr, out, textId, 0);
    return out;
}

void* g_TextIds[kMaxExtras][2] = {}; // the extras' text ids (game strings: made anew at every start)

const void* ExtraTextId(int slot, bool title)
{
    void*& id = g_TextIds[slot - kFirstExtraSlot][title ? 1 : 0];
    if (!id) {
        const ExtraFighter* extra = Extra(slot);
        reinterpret_cast<void(__cdecl*)(void**, const char*)>(uintptr_t(kTStringFromText))(
            &id, title ? extra->titleId : extra->nameId);
    }
    return &id;
}

void* __stdcall SlotNameHook(void* out, int slot)
{
    return SlotText(out, Extra(slot) ? ExtraTextId(slot, false) : reinterpret_cast<const void*>(kSlotNames + slot * 4));
}

void* __stdcall SlotTitleHook(void* out, int slot)
{
    return SlotText(out, Extra(slot) ? ExtraTextId(slot, true) : reinterpret_cast<const void*>(kSlotTitles + slot * 4));
}

bool Matches(uint32_t address, const uint8_t* bytes, size_t length)
{
    return std::memcmp(reinterpret_cast<const void*>(uintptr_t(address)), bytes, length) == 0;
}

// Hooks a function whose first `length` bytes are position-independent instructions: they are
// copied to a stub that continues after them, which is returned (the original function).
void* Detour(uint32_t address, const uint8_t* prologue, uint32_t length, const void* hook)
{
    uint8_t* stub = AllocStub(length + 5);
    std::memcpy(stub, prologue, length);
    stub[length] = 0xE9;
    int32_t back = int32_t(address + length) - int32_t(uintptr_t(stub) + length + 5);
    std::memcpy(stub + length + 1, &back, 4);
    PatchJump(address, hook);
    return stub;
}

void PatchDisplacement(const TableSite& site, const void* table)
{
    uint32_t value = uint32_t(reinterpret_cast<uintptr_t>(table));
    PatchBytes(site.address + site.displacementAt, &value, 4);
}

// This start's extras: Yoda, then each custom character (customchars.h) whose select pictures are there
// (interfc\front_end\s_selduel_<costume>head.stx and ...bust.stx, on the disc or under mods\): its name and title
// are texts of the port's own, its intro and win cameras its base's, its saber colour its own for both players.
const char* Keep(const std::string& text)
{
    g_ExtraStrings.push_back(text);
    return g_ExtraStrings.back().c_str();
}

std::wstring Wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

// A custom character's select pictures: the menus' sets name pictures the shared PAK has (a picture under mods\ that
// no PAK has is refused by the menus' pre-cache, 0x210B6B), so its entries name stand-ins the disc has (the cut
// Grievous bodyguard's, kStandInHead/Bust; Random's full body), and its own pictures, loaded from their files, are
// drawn in their place (see Pictures below).
constexpr const char* kStandInHead = "s_selduel_gbghead";
constexpr const char* kStandInBust = "s_selduel_gbgbust";

void BuildExtras()
{
    g_Extras.clear();
    g_Extras.push_back(kYoda);
    for (const CustomCharacter& custom : CustomCharacters()) {
        if (int(g_Extras.size()) >= kMaxExtras) {
            LOG_WARN("Roster: no select-screen slot left for %s", custom.className.c_str());
            break;
        }
        std::string costume = custom.costume;
        for (char& c : costume)
            c = char(tolower(static_cast<unsigned char>(c)));
        const std::string head = "s_selduel_" + costume + "head", bust = "s_selduel_" + costume + "bust";
        const auto present = [](const std::string& picture) {
            const std::string file = "interfc\\front_end\\" + picture + ".stx";
            return ModFileExists(file); // not the disc: its index is made only once the resource hooks are in
        };
        if (!custom.builtIn && (!present(head) || !present(bust))) {
            LOG_INFO("Roster: %s has no select pictures (%s, %s); not on the Versus select screen",
                custom.className.c_str(), head.c_str(), bust.c_str());
            continue;
        }
        std::string upper = custom.className;
        for (char& c : upper)
            c = char(toupper(static_cast<unsigned char>(c)));
        ExtraFighter extra = {};
        extra.className = Keep(custom.className);
        extra.head = kStandInHead;
        extra.bust = kStandInBust;
        extra.files[0] = Keep("interfc\\front_end\\" + head); // a built-in fighter's: generated (customchars.cpp)
        extra.files[1] = Keep("interfc\\front_end\\" + bust);
        if (custom.builtIn && !custom.selectBust.empty()) { // the disc's own, as Yoda's
            extra.bust = Keep(custom.selectBust);
            extra.files[1] = nullptr;
        }
        for (int player = 0; player < 2; ++player) {
            const std::string body = "s_selduel_fb_" + costume + (player ? "_v2" : "_v1");
            if (present(body))
                extra.files[2 + player] = Keep("interfc\\front_end\\" + body);
        }
        extra.nameId = Keep("IDS_PORT_" + upper + "_NAME");
        extra.titleId = Keep("IDS_PORT_" + upper + "_TITLE");
        SetMenuText(extra.nameId, Wide(custom.name));
        SetMenuText(extra.titleId, Wide(custom.title));
        if (const char* cameras = IntroCameras(custom)) // the duelist's whose intro it plays (timed to its animation)
            extra.cameras = cameras;
        else
            extra.cameras = Keep(custom.base.size() > 1 && custom.base[0] == 'I' ? custom.base.substr(1) : custom.base);
        for (int player = 0; player < 2; ++player)
            for (int k = 0; k < 3 && custom.hasSaber; ++k)
                extra.sabers[player][k] = custom.saber[k];
        for (int k = 0; k < 3 && custom.hasSaber2; ++k) // a mirror's player 2
            extra.sabers[1][k] = custom.saber2[k];
        if (custom.hasSaber && !custom.hasSaber2) { // else the other side's: a hero's red, a villain's blue
            static const float kRed[3] = { 1.0f, 0.0f, 0.0f }, kBlue[3] = { 0.0f, 0.35f, 1.0f };
            std::memcpy(extra.sabers[1], custom.side ? kBlue : kRed, sizeof kRed);
        }
        extra.meshes = nullptr; // a custom character's player 2 look: its pack's own <texture>_duel.stx files
        g_Extras.push_back(extra);
        LOG_INFO("Roster: %s on the Versus select screen (slot %d)", custom.className.c_str(),
            kFirstExtraSlot + int(g_Extras.size()) - 1);
    }
    g_ExtraCount = int(g_Extras.size());
    g_SlotCount = kFirstExtraSlot + g_ExtraCount;
    std::memset(g_TextIds, 0, sizeof(g_TextIds));
}

// --- Pictures -------------------------------------------------------------------------------------------------------
// An extra's own pictures are loaded from their files (the texture loader's direct path, as the concept art screen
// loads its pictures; characters.h LoadPictureFile) the first time they are drawn, and drawn in the place of their
// stand-ins: the strip of heads by the port (DrawSelectStrip); the busts and full bodies are menu Texture items (draw
// 0x2C7790, vtable 0x5D3F78 slot 3: +0x1C its picture record, whose +0xC is the texture drawn and +0x64/+0x68 its
// size; +0x20/+0x24 the item's y/x offsets), whose record, found in its set ([[item+8]+0x2C]: +8 count, +0xC records),
// names the slot. During that draw only, the record holds the extra's own texture. An extra without its own full body
// shows its bust in the arena screen (the stand-in entry), scaled into the full body's frame.
constexpr uint32_t kTextureItemVtable = 0x005D3F78;
constexpr uint32_t kTextureItemDraw = 0x002C7790;
void* g_Pictures[kMaxExtras][4];
bool g_PicturesTried[kMaxExtras][4];

void* ExtraPicture(int extra, int kind)
{
    if (extra < 0 || extra >= g_ExtraCount || !g_Extras[size_t(extra)].files[kind])
        return nullptr;
    if (!g_PicturesTried[extra][kind]) {
        g_PicturesTried[extra][kind] = true;
        MakeGeneratedResource(std::string(g_Extras[size_t(extra)].files[kind]) + ".stx"); // one the port makes
        g_Pictures[extra][kind] = LoadPictureFile(g_Extras[size_t(extra)].files[kind]);
        if (!g_Pictures[extra][kind])
            LOG_WARN("Roster: %s did not load", g_Extras[size_t(extra)].files[kind]);
    }
    return g_Pictures[extra][kind];
}

// Records the port draws with pictures of its own (DrawRecordWith): the file, then its texture once loaded.
struct RecordPicture {
    std::string file;
    void* texture = nullptr;
    bool tried = false;
};
std::vector<std::pair<uint8_t*, RecordPicture>> g_RecordPictures;

using TextureDrawFn = void(__fastcall*)(uint8_t*, void*, void*);

void __fastcall TextureItemDrawHook(uint8_t* item, void*, void* context)
{
    const auto draw = reinterpret_cast<TextureDrawFn>(uintptr_t(kTextureItemDraw));
    uint8_t* record = *reinterpret_cast<uint8_t**>(item + 0x1C);
    for (auto& [own, picture] : g_RecordPictures) {
        if (own != record || !record)
            continue;
        if (!picture.tried) {
            picture.tried = true;
            picture.texture = LoadPictureFile(picture.file.c_str());
            if (!picture.texture)
                LOG_WARN("Roster: %s did not load", picture.file.c_str());
        }
        if (!picture.texture)
            break;
        void*& texture = *reinterpret_cast<void**>(record + 0xC);
        void* const kept = texture;
        texture = picture.texture;
        draw(item, nullptr, context);
        texture = kept;
        return;
    }
    uint8_t* data = record ? *reinterpret_cast<uint8_t**>(item + 8) : nullptr;
    uint8_t* set = data ? *reinterpret_cast<uint8_t**>(data + 0x2C) : nullptr;
    const int count = set ? *reinterpret_cast<int*>(set + 8) : 0;
    uint8_t** records = set ? *reinterpret_cast<uint8_t***>(set + 0xC) : nullptr;
    int index = -1;
    for (int i = 0; records && i < count && i < 128; ++i)
        if (records[i] == record)
            index = i;
    // the set by its size: the busts (fighters, unused, extras, then three), the full bodies (two blocks and one)
    int extra = -1, kind = -1, player = 0;
    // (and by its first picture's name: other sets can have the same size, e.g. the arena screen's arenas)
    const auto named = [&](const char* part) {
        const char* name = records && count > 0 && records[0] ? *reinterpret_cast<const char**>(records[0] + 8) : nullptr;
        if (!name)
            return false;
        std::string lower = name;
        for (char& c : lower)
            c = char(tolower(static_cast<unsigned char>(c)));
        return lower.find(part) != std::string::npos;
    };
    if (index >= 0 && count == g_SlotCount + 3 && named("bust")) {
        extra = index - kFirstExtraSlot;
        kind = 1;
    } else if (index >= 0 && count == 2 * (g_SlotCount + 1) + 1 && named("_fb_")) {
        player = index / (g_SlotCount + 1);
        extra = index % (g_SlotCount + 1) - kFirstExtraSlot;
        kind = player < 2 ? 2 + player : -1;
    }
    if (kind < 0 || extra < 0 || extra >= g_ExtraCount) {
        draw(item, nullptr, context);
        return;
    }
    void* own = ExtraPicture(extra, kind);
    const bool bustInFrame = kind >= 2 && !g_Extras[size_t(extra)].files[kind]; // (Yoda's: the disc's bust)
    if (bustInFrame)
        own = ExtraPicture(extra, 1);
    if (!own && !bustInFrame) {
        draw(item, nullptr, context);
        return;
    }
    void*& texture = *reinterpret_cast<void**>(record + 0xC);
    float* size = reinterpret_cast<float*>(record + 0x64);
    float* offset = reinterpret_cast<float*>(item + 0x20);
    void* const keptTexture = texture;
    const float keptSize[2] = { size[0], size[1] }, keptOffset[2] = { offset[0], offset[1] };
    if (own)
        texture = own;
    if (bustInFrame) { // the 256x128 bust over the 128x256 full body's frame, centred on it, at its middle
        size[0] = 192.0f;
        size[1] = 96.0f;
        offset[0] += 96.0f;
        offset[1] = -32.0f;
    }
    draw(item, nullptr, context);
    texture = keptTexture;
    size[0] = keptSize[0];
    size[1] = keptSize[1];
    offset[0] = keptOffset[0];
    offset[1] = keptOffset[1];
}

// --- The select screen's strip of heads ----------------------------------------------------------------------------
// The fighters are one row of heads (0x2D56F0, the screen's slot 3: from the strip item's place ([screen+4] +0x58/+0x5A,
// its corner +0x5C/+0x5E, in 1/32 pixels), each head 51 pixels on, a locked fighter's the set's last picture; the
// cursors' items (player 1's [screen+0x58], player 2's [+0x5C], [+0x60], [+0x54]) moved to their slots by their x
// offsets +0x24). Twelve cells or more run off the screen's right edge: the port draws the strip as the game does,
// kStripCells at a time, scrolled with the cursor that moved last; the cursor of a fighter out of view is hidden.
constexpr int kStripCells = 11;
constexpr float kCellStep = 51.0f;  // the game's (0x5D6960)
constexpr uint32_t kTextureList = 0x0001F820; // thiscall (list {?, count, data}, set +4): the set's picture records
constexpr uint32_t kDrawPicture = 0x0025A9C0; // cdecl (context, picture record, const float* corner, const float* other)
constexpr uint32_t kEngineFree = 0x54;
int g_StripFirst = 0;
// The arrows at the strip's ends while fighters are out of view that way: the arena screen's rounds arrow
// (interfc\front_end\s_rounds_arrow, a glowing blue triangle pointing left; the shared PAK's), drawn with a borrowed
// picture record (the locked picture's: its texture swapped while drawn), mirrored for the right.
void* g_StripArrow = nullptr;
bool g_StripArrowTried = false;

void DrawStripArrow(uint32_t context, uint8_t* record, float x, float y, float size, bool right)
{
    if (!g_StripArrowTried) {
        g_StripArrowTried = true;
        g_StripArrow = LoadPicture("interfc\\front_end\\s_rounds_arrow");
    }
    if (!g_StripArrow || !record)
        return;
    void*& texture = *reinterpret_cast<void**>(record + 0xC);
    float* uv = reinterpret_cast<float*>(record + 0x20); // four corners' u, v
    void* const kept = texture;
    float keptUv[8];
    std::memcpy(keptUv, uv, sizeof keptUv);
    texture = g_StripArrow;
    if (right) // mirrored: the corners' u swapped left for right
        for (int i = 0; i < 4; ++i)
            uv[i * 2] = 1.0f - keptUv[i * 2];
    const float corner[2] = { x, y }, other[2] = { x + size, y + size };
    reinterpret_cast<void(__cdecl*)(uint32_t, void*, const float*, const float*)>(uintptr_t(0x0025A9C0))(
        context, record, corner, other);
    texture = kept;
    std::memcpy(uv, keptUv, sizeof keptUv);
}
int g_StripCursors[2] = { -1, -1 };

void FreeList(void* data) // the engine's free: its services table ([kEngineServices], functions) +0x54
{
    if (data)
        reinterpret_cast<void(__cdecl*)(void*)>(*reinterpret_cast<void* const*>(
            *reinterpret_cast<uint8_t* const*>(uintptr_t(kEngineServices)) + kEngineFree))(data);
}

float* ItemOffset(uint8_t* screen, uint32_t field)
{
    uint8_t* item = *reinterpret_cast<uint8_t**>(screen + field);
    return item ? reinterpret_cast<float*>(item + 0x24) : nullptr;
}

} // namespace

bool BothPlayersChose(const char* className)
{
    auto* launch = *reinterpret_cast<uint8_t**>(uintptr_t(kLaunchSettings));
    for (int player = 0; launch && className && player < 2; ++player) {
        const char* chosen = *reinterpret_cast<const char**>(launch + 4 + player * 4);
        if (!chosen || _stricmp(chosen, className) != 0)
            return false;
    }
    return launch != nullptr && className != nullptr;
}

bool DrawSelectStrip(uint8_t* screen, uint32_t context)
{
    uint8_t* strip = *reinterpret_cast<uint8_t**>(screen + 4);
    uint8_t* menu = *reinterpret_cast<uint8_t**>(screen + 8);
    uint8_t* set = menu ? *reinterpret_cast<uint8_t**>(menu + 0x2C) : nullptr;
    if (!strip || !set)
        return false;
    struct {
        uint32_t unknown;
        int count;
        void** data;
    } list = {};
    reinterpret_cast<void(__fastcall*)(void*, void*, uint8_t*)>(uintptr_t(kTextureList))(&list, nullptr, set + 4);
    const int cells = list.count - 1; // the last picture: the locked one
    bool ownHeads = false;
    for (int i = 0; i < g_ExtraCount; ++i)
        ownHeads = ownHeads || g_Extras[size_t(i)].files[0];
    if (cells <= kStripCells && !ownHeads) {
        FreeList(list.data);
        return false; // the game's own draw: everything fits
    }
    // the view follows the cursor that moved last
    const int cursors[2] = { *reinterpret_cast<int*>(screen + 0x2C), *reinterpret_cast<int*>(screen + 0x30) };
    int follow = -1;
    for (int p = 0; p < 2; ++p) {
        if (cursors[p] != g_StripCursors[p]) {
            follow = cursors[p];
            g_StripCursors[p] = cursors[p];
        }
    }
    // player 1 has picked (+0x28): from then on the second cursor picks (the opponent's, starting at the first
    // fighter), and stays in view
    if (*reinterpret_cast<uint8_t*>(screen + 0x28) && !*reinterpret_cast<uint8_t*>(screen + 0x2A))
        follow = cursors[1];
    if (follow >= 0) {
        if (follow < g_StripFirst)
            g_StripFirst = follow;
        else if (follow >= g_StripFirst + kStripCells)
            g_StripFirst = follow - kStripCells + 1;
    }
    g_StripFirst = std::clamp(g_StripFirst, 0, std::max(0, cells - kStripCells));
    const float k = 1.0f / 32;
    const auto words = reinterpret_cast<const int16_t*>(strip + 0x58);
    float corner[2] = { words[0] * k, words[1] * k }, other[2] = { words[2] * k, words[3] * k };
    const auto isLocked = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, int)>(uintptr_t(kIsLocked));
    for (int i = g_StripFirst; i < std::min(cells, g_StripFirst + kStripCells); ++i) {
        auto* record = static_cast<uint8_t*>(list.data[isLocked(screen, nullptr, i) ? list.count - 1 : i]);
        void* own = record == list.data[i] ? ExtraPicture(i - kFirstExtraSlot, 0) : nullptr;
        void*& texture = *reinterpret_cast<void**>(record + 0xC); // a picture record: its own head while drawn
        void* const kept = texture;
        if (own)
            texture = own;
        reinterpret_cast<void(__cdecl*)(uint32_t, void*, const float*, const float*)>(uintptr_t(kDrawPicture))(
            context, record, corner, other);
        texture = kept;
        corner[0] += kCellStep;
        other[0] += kCellStep;
    }
    // the arrows: fighters out of view to the left, to the right
    {
        const float firstX = words[0] * k, cellW = words[2] * k - firstX, top = words[1] * k;
        const float cellH = words[3] * k - top, size = std::min(24.0f, cellH * 0.55f), y = top + (cellH - size) / 2;
        uint8_t* record = static_cast<uint8_t*>(list.data[list.count - 1]);
        if (g_StripFirst > 0)
            DrawStripArrow(context, record, firstX - size - 3, y, size, false);
        if (g_StripFirst + kStripCells < cells)
            DrawStripArrow(context, record, firstX + (kStripCells - 1) * kCellStep + cellW + 3, y, size, true);
    }
    // the cursors: at their cells, or out of sight
    const float hidden = -4000.0f;
    auto place = [&](int slot) { return slot >= g_StripFirst && slot < g_StripFirst + kStripCells ?
        float(slot - g_StripFirst) * kCellStep : hidden; };
    if (float* x = ItemOffset(screen, 0x58))
        *x = place(cursors[0]);
    for (uint32_t field : { 0x5Cu, 0x60u, 0x54u })
        if (float* x = ItemOffset(screen, field))
            *x = place(cursors[1]);
    FreeList(list.data);
    return true;
}

void DrawRecordWith(uint8_t* record, const std::string& file)
{
    for (auto& entry : g_RecordPictures)
        if (entry.first == record) {
            if (entry.second.file != file)
                entry.second = { file };
            return;
        }
    g_RecordPictures.push_back({ record, { file } });
}

const char* SelectSlotClass(int slot)
{
    if (slot >= 0 && slot < kFighterCount)
        return reinterpret_cast<const char* const*>(uintptr_t(kDuelistClasses))[slot];
    if (const ExtraFighter* extra = Extra(slot))
        return extra->className;
    return nullptr;
}

void SyncRosterClasses()
{
    std::memcpy(g_Classes, reinterpret_cast<const void*>(uintptr_t(kDuelistClasses)), kFighterCount * sizeof(g_Classes[0]));
    g_Classes[kRandomSlot] = g_Classes[0]; // never read: Random becomes a fighter before the duel
    for (int i = 0; i < g_ExtraCount; ++i)
        g_Classes[kFirstExtraSlot + i] = g_Extras[size_t(i)].className;
    g_Classes[g_SlotCount] = nullptr;
}

const char* ExtraFighterCameras(int duelSlot)
{
    if (duelSlot < kFighterCount || duelSlot >= kDuelSlotCount || !g_DuelClasses[duelSlot])
        return nullptr;
    for (const ExtraFighter& extra : g_Extras) {
        if (_stricmp(extra.className, g_DuelClasses[duelSlot]) == 0)
            return extra.cameras;
    }
    return nullptr;
}

void InstallRoster()
{
    BuildExtras();
    std::memset(g_Pictures, 0, sizeof g_Pictures); // the textures of the last start are gone with it
    g_RecordPictures.clear();
    g_StripArrow = nullptr;
    g_StripArrowTried = false;
    std::memset(g_PicturesTried, 0, sizeof g_PicturesTried);
    const uint32_t drawSlot = kTextureItemVtable + 3 * 4;
    if (*reinterpret_cast<const uint32_t*>(uintptr_t(drawSlot)) == kTextureItemDraw) {
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&TextureItemDrawHook));
        PatchBytes(drawSlot, &hook, 4);
    }
    static_assert((kFirstExtraSlot + kMaxExtras + 1) * 4 <= 0x7F, "player 2's full-body offset is a signed 8-bit displacement");
    const uint8_t lockCheck[] = { 0x83, 0xFF, 0x09, 0x74 };
    const uint8_t gridLockCheck[] = { 0x83, 0xFE, 0x09, 0x74 };
    const uint8_t textLookup[] = { 0xA1, 0x7C, 0x5F, 0x64, 0x00 }; // mov eax, [kEngineServices]
    bool ok = Matches(kBodyOffsetSite, kBodyOffsetBytes, sizeof(kBodyOffsetBytes)) &&
        Matches(kArenaBodyOffsetSite, kArenaBodyOffsetBytes, sizeof(kArenaBodyOffsetBytes)) &&
        Matches(kIsLocked, kIsLockedPrologue, sizeof(kIsLockedPrologue)) &&
        Matches(kSetBodies, kSetBodiesPrologue, sizeof(kSetBodiesPrologue)) &&
        Matches(kDuelSetup, kDuelSetupPrologue, sizeof(kDuelSetupPrologue)) &&
        Matches(kSlotName, textLookup, sizeof(textLookup)) && Matches(kSlotTitle, textLookup, sizeof(textLookup)) &&
        Matches(kGridLockSite, gridLockCheck, sizeof(gridLockCheck)) &&
        Matches(kSelectClassSite.address, kSelectClassSite.bytes, kSelectClassSite.length);
    for (uint32_t site : kInlineLockSites)
        ok = ok && Matches(site, lockCheck, sizeof(lockCheck));
    for (uint32_t site : kRandomCalls) {
        int32_t target = int32_t(kRandomBelow) - int32_t(site + 5);
        ok = ok && *reinterpret_cast<const uint8_t*>(uintptr_t(site)) == 0xE8 &&
            std::memcmp(reinterpret_cast<const void*>(uintptr_t(site + 1)), &target, 4) == 0;
    }
    for (const TableSite& site : kDuelClassSites)
        ok = ok && Matches(site.address, site.bytes, site.length);
    for (const TableSite& site : kSaberSites)
        ok = ok && Matches(site.address, site.bytes, site.length);
    for (uint32_t site : kChosenCompareSites) {
        int32_t target = int32_t(kCompareNames) - int32_t(site + 5);
        ok = ok && *reinterpret_cast<const uint8_t*>(uintptr_t(site)) == 0xE8 &&
            std::memcmp(reinterpret_cast<const void*>(uintptr_t(site + 1)), &target, 4) == 0;
    }
    if (!ok) {
        LOG_WARN("Roster: the versus screens' code is not as expected; no extra fighters");
        return;
    }

    // The select and arena screens.
    const uint8_t offset = uint8_t((g_SlotCount + 1) * 4);
    PatchBytes(kBodyOffsetSite + 3, &offset, 1);
    PatchBytes(kArenaBodyOffsetSite + 3, &offset, 1);
    const uint8_t jae = 0x73;
    for (uint32_t site : kInlineLockSites)
        PatchBytes(site + 3, &jae, 1);
    PatchBytes(kGridLockSite + 3, &jae, 1);
    for (uint32_t site : kRandomCalls) {
        int32_t target = int32_t(reinterpret_cast<uintptr_t>(&RandomFighter)) - int32_t(site + 5);
        PatchBytes(site + 1, &target, 4);
    }
    g_OriginalSetBodies = reinterpret_cast<SetBodiesFn>(
        Detour(kSetBodies, kSetBodiesPrologue, sizeof(kSetBodiesPrologue), reinterpret_cast<const void*>(&BodiesHook)));
    g_OriginalIsLocked = reinterpret_cast<IsLockedFn>(
        Detour(kIsLocked, kIsLockedPrologue, sizeof(kIsLockedPrologue), reinterpret_cast<const void*>(&IsLockedHook)));
    PatchJump(kSlotName, reinterpret_cast<const void*>(&SlotNameHook));
    PatchJump(kSlotTitle, reinterpret_cast<const void*>(&SlotTitleHook));
    // Asked for by their source names; the PAK has the compiled .xbl_xml.
    RegisterResourcePatch("interfc\\front_end\\xml\\select_jedi.xml", &PatchSelectScreen);
    RegisterResourcePatch("interfc\\front_end\\xml\\select_arena.xml", &PatchArenaScreen);
    RegisterResourceGenerator(&DarkLookGenerator);
    SyncRosterClasses();
    PatchDisplacement(kSelectClassSite, g_Classes);

    // The duel.
    BuildDuelClasses();
    for (size_t i = 0; i < sizeof(kDuelClassSites) / sizeof(kDuelClassSites[0]); ++i)
        PatchDisplacement(kDuelClassSites[i], i + 1 < sizeof(kDuelClassSites) / sizeof(kDuelClassSites[0]) ?
            &g_DuelClasses[0] : &g_DuelClasses[1]);
    for (uint32_t site : kChosenCompareSites)
        PatchCall(site, reinterpret_cast<const void*>(&CompareChosenStub), 5);
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kFighterSaberStore)), kFighterSaberStoreBytes,
            sizeof kFighterSaberStoreBytes) == 0)
        PatchCall(kFighterSaberStore, reinterpret_cast<const void*>(&FighterSaberStoreStub), sizeof kFighterSaberStoreBytes);
    g_OriginalDuelSetup = reinterpret_cast<DuelSetupFn>(
        Detour(kDuelSetup, kDuelSetupPrologue, sizeof(kDuelSetupPrologue), reinterpret_cast<const void*>(&DuelSetupHook)));
    std::memcpy(g_SaberColours, reinterpret_cast<const void*>(uintptr_t(kSaberColours)), kFighterCount * 2 * 3 * sizeof(float));
    PatchDisplacement(kSaberSites[0], &g_SaberColours[2]);
    PatchDisplacement(kSaberSites[1], &g_SaberColours[1]);
    PatchDisplacement(kSaberSites[2], &g_SaberColours[0]);
    PatchDisplacement(kSaberSites[3], &g_SaberColours[0]);
}

} // namespace swrots::game
