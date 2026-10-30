// Characters of mods' own (see customchars.h).
//
// A class info (what the class registry, 0xE9B60 cdecl (name), returns) is an object whose vtable slot 0x30 creates a
// character of the class (thiscall (), from 0xE2350); the class's constructor sets the character's costume list
// (+0x1E0, a static list per class: a 0x2C-byte header, then 20-byte records {name, mesh "folder\file", ?, code, ?}
// up to a null name; 0x29C000.. the constructors). A custom class's info is a copy of its base's with a vtable of its
// own whose create calls the base's, then gives the character the custom list (its costume first) and its saber
// colour (+0xF20, a pointer to an RGB the saber takes when the character equips it in ICharacter::Init).

#include "game/customchars.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <memory>
#include <sstream>
#include <utility>

#include "core/log.h"
#include "core/patch.h"
#include "game/classsetup.h"
#include "game/devmap.h"
#include "game/eventtree.h"
#include "game/game.h"
#include "game/menus.h"
#include "kernel/kernel.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

constexpr uint32_t kClassLookup = 0x000E9B60;
constexpr uint8_t kClassLookupBytes[] = { 0xA1, 0xE8, 0xEB, 0x68, 0x00, 0x83, 0xEC, 0x0C }; // mov eax, [0x68EBE8]; sub esp, 0xC
constexpr uint32_t kCreateSlot = 0x30 / 4;
constexpr size_t kInfoCopy = 0x200;     // bytes of the base's class info copied (it is smaller; reads only)
constexpr size_t kVtableCopy = 96;      // slots of its vtable copied
constexpr uint32_t kCharacterSaberColor = 0xF20;
constexpr uint32_t kCharacterTextureSet = 0x1EC; // its texture set, 1-based (0: the plain one)
constexpr uint32_t kCharacterTextureSuffix = 0x1F4; // a suffix its textures are asked with (Versus player 2's "_duel")

// A custom character's saber colour now: the one its pointer (+0xF20) gives when something chose one for it (a Versus
// mirror's player 2: Saber2=, the duel's colour table), else its own.
const float* SaberColourOf(const uint8_t* character, const CustomCharacter& custom)
{
    const float* chosen = *reinterpret_cast<const float* const*>(character + kCharacterSaberColor);
    if (chosen && chosen != custom.saber && (chosen[0] != 0 || chosen[1] != 0 || chosen[2] != 0))
        return chosen;
    return custom.saber;
}

// The lines a custom character says: player 2's in a Versus mirror ([Voice2]) when it has them, else its own.
const std::vector<std::pair<std::string, std::vector<std::string>>>& VoiceOf(const uint8_t* character,
    const CustomCharacter& custom)
{
    return !custom.voice2.empty() && IsMirrorPlayer2(character) ? custom.voice2 : custom.voice;
}

// A Jedi's sabers: the class's Init fills up to 4 weapon records (+0x1064, 0x20 bytes each: +0 double-bladed flag, +4
// the hilt mesh (a TString), +8 its colour) before the Jedi's weapon setup (0x2814C0) makes a saber of each record
// with a mesh (0x280480: the double-bladed class for a flagged one). The Jedi Brute's is double-bladed (Lsaberjdbrute,
// flag 1); Serra Keto's two records make her two sabers. At 0x2814D2 (lea ebx, [esi + 0x1068]; 6 bytes, replaced by a
// call) a custom character's Blades= changes the records first.
constexpr uint32_t kSaberSetup = 0x002814D2;
constexpr uint8_t kSaberSetupBytes[] = { 0x8D, 0x9E, 0x68, 0x10, 0x00, 0x00 };
constexpr uint32_t kWeaponRecords = 0x1064, kWeaponRecordSize = 0x20;
constexpr uint32_t kAssignString = 0x00225A10; // TString, thiscall (const char*)
constexpr char kSingleHilt[] = "Meshes\\Weapons\\LsaberKnight\\LsaberKnight";
constexpr char kDoubleHilt[] = "Meshes\\Weapons\\Lsaberjdbrute\\Lsaberjdbrute";

std::vector<CustomCharacter> g_Customs;
std::wstring g_Mods;

// A custom class's info, for this start's base info.
struct Proxy {
    const CustomCharacter* custom = nullptr;
    uint8_t* baseInfo = nullptr;
    std::unique_ptr<uint8_t[]> info;
    std::unique_ptr<uint32_t[]> vtable;
    std::vector<uint8_t> list;           // the costume list: the custom costume, then the base's
    std::string costumeName, costumeMesh, costumeCode;
    const uint8_t* baseList = nullptr;   // the base's list the copy was made from
    int textureSet = 0;                  // its own texture set's number (1-based, character +0x1EC), 0 none
};
std::vector<std::unique_ptr<Proxy>> g_Proxies;

using LookupFn = uint8_t*(__cdecl*)(const char*);
LookupFn g_OriginalLookup = nullptr;

Proxy* ProxyOf(const uint8_t* info)
{
    for (auto& p : g_Proxies)
        if (p->info.get() == info)
            return p.get();
    return nullptr;
}

// The custom list from the base's: header, the custom costume, the base's costumes, a null record.
void BuildList(Proxy& p, const uint8_t* baseList)
{
    p.baseList = baseList;
    int count = 0;
    while (baseList && count < 32 &&
        *reinterpret_cast<const char* const*>(baseList + kVariantRecords + count * kVariantRecordSize))
        ++count;
    p.list.assign(kVariantRecords + (count + 2) * kVariantRecordSize, 0);
    if (baseList) {
        std::memcpy(p.list.data(), baseList, kVariantRecords);
        std::memcpy(p.list.data() + kVariantRecords + kVariantRecordSize, baseList + kVariantRecords,
            size_t(count) * kVariantRecordSize);
    }
    // its own texture set: the header's first free suffix (+0x14, up to six; the set's number is its place + 1)
    p.textureSet = 0;
    for (int i = 0; i < 6 && !p.custom->textureSet.empty() && baseList; ++i) {
        const char*& suffix = *reinterpret_cast<const char**>(p.list.data() + 0x14 + i * 4);
        if (suffix && _stricmp(suffix, p.custom->textureSet.c_str()) == 0) { // one of the base's own
            p.textureSet = i + 1;
            break;
        }
        if (!suffix) {
            suffix = p.custom->textureSet.c_str();
            p.textureSet = i + 1;
            break;
        }
    }
    uint8_t* first = p.list.data() + kVariantRecords;
    *reinterpret_cast<const char**>(first) = p.costumeName.c_str();
    *reinterpret_cast<const char**>(first + 4) = p.costumeMesh.c_str();
    // the record's code is its dialogue speaker (RegisterSpeaker 0x8D800 takes it: the lines the dialogue engine
    // picks for it, as the game's own codes AKN, OBW...): its own (Code=) or the base's first costume's
    if (p.custom->code.empty() && count > 0)
        p.costumeCode = *reinterpret_cast<const char* const*>(baseList + kVariantRecords + 12);
    *reinterpret_cast<const char**>(first + 12) = p.costumeCode.c_str();
}

// A subtitle's speaker name: the character's (+0xA04, the text of IDS_SPEAKER_<its costume list's name>), set by
// ICharacter::Init (0x15BC56: mov [esi+0xA04], edx, just after its string table lookup). A custom character's list
// keeps the base's name (the duel also finds the intro camera by it), so the store takes its own name instead: the
// text of IDS_SPEAKER_<its key>, which the port's string hook supplies. (Answering it from the getter, 0x151B80, at
// subtitle time, looked it up from there: the game hung.)
constexpr uint32_t kSpeakerNameStore = 0x0015BC56;
constexpr uint8_t kSpeakerNameStoreBytes[] = { 0x89, 0x96, 0x04, 0x0A, 0x00, 0x00 };
constexpr uint32_t kStringCrc = 0x00222BA0;  // cdecl (const char*): a string id's key
constexpr uint32_t kGameCore = 0x00645F7C;   // [+0x38] +0x94: the string tables; vfunc +4 thiscall (out, &key, 0)

// The game's text handles, once a start (cleared at every start: the game's string pool is made anew).
std::vector<std::pair<const CustomCharacter*, uint32_t>> g_SpeakerTexts;

uint32_t SpeakerText(const CustomCharacter& custom)
{
    auto& cache = g_SpeakerTexts;
    for (const auto& c : cache)
        if (c.first == &custom)
            return c.second;
    const std::string id = "IDS_SPEAKER_" + custom.speakerKey;
    uint32_t key = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(uintptr_t(kStringCrc))(id.c_str());
    uint32_t text = 0;
    uint8_t* core = *reinterpret_cast<uint8_t**>(uintptr_t(kGameCore));
    uint8_t* tables = core ? *reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(core + 0x38) + 0x94) : nullptr;
    if (tables) {
        using Find = uint32_t*(__fastcall*)(uint8_t*, void*, uint32_t*, uint32_t*, int);
        reinterpret_cast<Find>((*reinterpret_cast<void* const* const*>(tables))[1])(tables, nullptr, &text, &key, 0);
        cache.push_back({ &custom, text });
    }
    return text;
}

// The dialogue engine's start of a line (StartEntry, thiscall (speaker, entry, 0, 0, 0), ret 0x14): its first six
// bytes (push ebp / mov ebp, esp / and esp, -16) moved to a stub.
constexpr uint32_t kStartEntry = 0x00090330;
constexpr uint8_t kStartEntryBytes[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
using StartEntryFn = int(__fastcall*)(uint8_t*, void*, uint8_t*, int, int, int, int);
StartEntryFn g_StartEntry = nullptr;

// A custom character's own lines: the dialogue engine picks an entry as for its base (its speaker code); for an
// event the character has lines for, the entry's line id (+0x10, a pooled string, e.g. "AKN045") is one of its own
// for the call: the subtitle's text, the voice (the dialogue archive, served with the lines added: see
// BuildVoiceArchive) and the line's length all follow it. The entries: engine +0x118, 0x3C bytes each, +0x14 the
// entry's name, its first three letters the event ("BATAKN___005GEN").
constexpr uint32_t kDialogueEntries = 0x118;
constexpr uint32_t kDialogueEntrySize = 0x3C;
bool g_VoiceServed = false; // this start serves the archive with the custom lines

// The custom line ids as the game's pooled strings, and each event's next line: per start (a reboot makes the game's
// string pool anew).
std::vector<std::pair<std::string, const char*>> g_PooledLines;
std::vector<std::pair<const void*, size_t>> g_LineTurns;

const char* PooledString(const std::string& text)
{
    for (const auto& p : g_PooledLines)
        if (p.first == text)
            return p.second;
    uint8_t* core = *reinterpret_cast<uint8_t**>(uintptr_t(kGameCore));
    if (!core)
        return nullptr;
    const auto acquire = *reinterpret_cast<const char*(__cdecl**)(const char*)>(core + 0xE0);
    const char* pooled = acquire(text.c_str());
    if (pooled)
        g_PooledLines.push_back({ text, pooled });
    return pooled;
}

size_t& LineTurn(const void* lines)
{
    for (auto& t : g_LineTurns)
        if (t.first == lines)
            return t.second;
    g_LineTurns.push_back({ lines, 0 });
    return g_LineTurns.back().second;
}

// StartEntry returns 0 when it refuses the entry (another line is on) and the engine asks again every frame (a saber
// lock's HIT for seconds), else the started or queued line's id: the character's next line is taken only then.
// A duel intro's or outro's line (SayEvent "TNT" / "KIL" in the "Versus" context, see IntroSequences), waiting for
// the dialogue engine to start it (it may refuse and ask again on later frames): the character's INT / WIN lines
// are said for it when it has them.
struct DuelLine {
    const uint8_t* speaker = nullptr;
    const char* event = nullptr; // "TNT" or "KIL", as asked
    const char* own = nullptr;   // "INT" or "WIN"
    ULONGLONG until = 0;
};
DuelLine g_DuelLine;

// A line asked for by the port (ForceNextLine): the speaker's next entry of the event says it.
struct ForcedLine {
    const uint8_t* speaker = nullptr;
    char event[4] = {};
    const char* id = nullptr; // pooled
    ULONGLONG until = 0;
};
ForcedLine g_Forced;

// The lines logged this start, by character and line (or event, for its base's): each once, or a long fight's grunts
// and pains would flood the log (cleared at every start).
std::vector<std::pair<const CustomCharacter*, std::string>> g_LoggedLines;

bool FirstLog(const CustomCharacter* custom, const char* key)
{
    const std::string k = key ? key : "?";
    for (const auto& l : g_LoggedLines)
        if (l.first == custom && _stricmp(l.second.c_str(), k.c_str()) == 0)
            return false;
    g_LoggedLines.push_back({ custom, k });
    return true;
}

// For an event a custom character has no lines for, the lines of related events it may have (else its base's line).
// Its base's efforts (grunts, pain: line numbers 900 and up) take its EFF or HIT lines.
constexpr const char* kVoiceFallbacks[][3] = {
    { "KIL", "WIN", "TNT" }, { "WIN", "KIL", "TNT" }, { "INT", "TNT", "BAT" }, { "FSE", "BAT", "TNT" },
    { "TNT", "BAT", "KIL" }, { "BAT", "TNT", "FSE" },
};

int __fastcall StartEntryHook(uint8_t* engine, void*, uint8_t* speaker, int entry, int a, int b, int c)
{
    if (speaker && speaker == g_Forced.speaker && entry >= 0 && g_Forced.id && GetTickCount64() < g_Forced.until) {
        uint8_t* e = *reinterpret_cast<uint8_t**>(engine + kDialogueEntries) + size_t(entry) * kDialogueEntrySize;
        const char* name = *reinterpret_cast<const char**>(e + 0x14);
        if (name && _strnicmp(name, g_Forced.event, 3) == 0) {
            const char*& id = *reinterpret_cast<const char**>(e + 0x10);
            const char* own = id;
            id = g_Forced.id;
            const int result = g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
            id = own;
            if (result) {
                LOG_INFO("Custom characters: says %s (%s)", g_Forced.id, name);
                g_Forced = {};
            }
            return result;
        }
    }
    const CustomCharacter* custom = speaker && g_VoiceServed ? CustomCharacterOf(speaker) : nullptr;
    const auto* voice = custom ? &VoiceOf(speaker, *custom) : nullptr;
    if (!custom || voice->empty() || entry < 0)
        return g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
    uint8_t* e = *reinterpret_cast<uint8_t**>(engine + kDialogueEntries) + size_t(entry) * kDialogueEntrySize;
    const char* name = *reinterpret_cast<const char**>(e + 0x14);
    const char* duel = nullptr; // this entry answers the duel's line: its INT / WIN lines first
    if (g_DuelLine.speaker == speaker && name && GetTickCount64() < g_DuelLine.until &&
        _strnicmp(name, g_DuelLine.event, 3) == 0)
        duel = g_DuelLine.own;
    // the events whose lines it says: the duel's (INT / WIN), the entry's own, else a related one it has lines for (a
    // custom character without kills says its victory or taunt lines rather than its base's kill lines)
    const char* wanted[6] = { duel, name, nullptr, nullptr, nullptr, nullptr };
    for (const auto& f : kVoiceFallbacks)
        if (name && _strnicmp(name, f[0], 3) == 0) {
            wanted[2] = f[1];
            wanted[3] = f[2];
        }
    // its base's efforts (line numbers 900 and up: PHR, PHH pain, PGH, EHG...): its own efforts (EFF: jumps, falls)
    // or pain (HIT), the pains first for a pain
    const char* baseLine = *reinterpret_cast<const char* const*>(e + 0x10);
    if (baseLine && strlen(baseLine) >= 6 && atoi(baseLine + 3) >= 900) {
        const bool pain = name && (name[0] == 'P' || name[0] == 'p');
        wanted[4] = pain ? "HIT" : "EFF";
        wanted[5] = pain ? "EFF" : "HIT";
    }
    for (const char* key : wanted)
    for (const auto& event : *voice) {
        if (!key || event.second.empty() || _strnicmp(key, event.first.c_str(), 3) != 0)
            continue;
        size_t& turn = LineTurn(&event.second);
        const char* line = PooledString(event.second[turn % event.second.size()]);
        if (!line)
            break;
        const char*& id = *reinterpret_cast<const char**>(e + 0x10);
        const char* own = id;
        id = line;
        const int result = g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
        id = own;
        if (result) {
            ++turn;
            if (duel)
                g_DuelLine = {};
            if (FirstLog(custom, line))
                LOG_INFO("Custom characters: %s says %s (%s)", custom->className.c_str(), line, name);
        }
        return result;
    }
    const int result = g_StartEntry(engine, nullptr, speaker, entry, a, b, c);
    if (result && FirstLog(custom, (std::string("base:") + (name ? name : "?")).c_str())) {
        const char* id = *reinterpret_cast<const char* const*>(e + 0x10);
        LOG_INFO("Custom characters: %s says its base's %s (%s): no line of its own for it", custom->className.c_str(),
            id ? id : "?", name ? name : "?");
    }
    return result;
}

// The duel's say (0x17ACC0, cdecl (event, context, ...): the current script's character says a line of the event;
// first bytes mov eax, [0x6943CC]). A custom character playing another duelist's intro (Intro=) has a base whose
// speaker has no lines for the "Versus" context: its line is asked without it (the speaker's taunts and kills).
constexpr uint32_t kSayEvent = 0x0017ACC0;
constexpr uint8_t kSayEventBytes[] = { 0xA1, 0xCC, 0x43, 0x69, 0x00 };
using SayEventFn = int(__cdecl*)(const char*, const char*, int, int, int);
SayEventFn g_SayEvent = nullptr;

int __cdecl SayEventHook(const char* event, const char* context, int a, int b, int c)
{
    const CustomCharacter* custom = nullptr;
    uint8_t* character = nullptr;
    if (context && event && strcmp(context, "Versus") == 0) {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(0x006943CC));
        character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        custom = character ? CustomCharacterOf(character) : nullptr;
    }
    if (!custom)
        return g_SayEvent(event, context, a, b, c);
    const bool intro = _strnicmp(event, "TNT", 3) == 0;
    g_DuelLine = { character, intro ? "TNT" : "KIL", intro ? "INT" : "WIN", GetTickCount64() + 4000 };
    // a borrowed or own intro (Intro=, IntroAnimation=): its speaker has no Versus lines of its own (the base never
    // dueled), so the line is asked without the context
    const bool ownIntro = custom->intro >= 0 || !custom->introAnimation.empty() || !custom->outroAnimation.empty();
    return g_SayEvent(event, ownIntro ? "" : context, a, b, c);
}

// The custom lines' files and texts, all characters'.
struct VoiceLine {
    std::string id;       // upper case, e.g. "LUK003"
    std::wstring file;    // headerless Xbox ADPCM, mono, 22050 Hz (the game's dialogue lines' format)
    std::wstring text;    // its subtitle
};
std::vector<VoiceLine> g_VoiceLines;

uint32_t __cdecl SpeakerNameFor(uint8_t* character, uint32_t text)
{
    const CustomCharacter* custom = CustomCharacterOf(character);
    const uint32_t own = custom ? SpeakerText(*custom) : 0;
    return own ? own : text;
}

void AssignString(uint8_t* string, const char* text)
{
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const char*)>(uintptr_t(kAssignString))(string, nullptr, text);
}

void __cdecl ApplyBlades(uint8_t* character)
{
    const CustomCharacter* custom = CustomCharacterOf(character);
    if (!custom)
        return;
    uint8_t* first = character + kWeaponRecords;
    uint8_t* second = first + kWeaponRecordSize;
    // its colour as the records' own too: some moves (a special's power-up) put a saber back to its record's colour,
    // which the +0xF20 override does not cover (the Jedi Brute's is the green key colour)
    if (custom->hasSaber)
        for (int i = 0; i < 2; ++i)
            std::memcpy(first + i * kWeaponRecordSize + 8, i == 1 && custom->hasSaberLeft ? custom->saberLeft
                : SaberColourOf(character, *custom), sizeof custom->saber);
    // its own hilt (a single blade's: a double-bladed one keeps the double hilt the blades need)
    if (!custom->hilt.empty() && custom->blades != 2 && !first[0])
        AssignString(first + 4, custom->hilt.c_str());
    if (!custom->blades)
        return;
    const char* hilt = *reinterpret_cast<const char**>(first + 4);
    const bool hadDouble = first[0] != 0;
    if (custom->blades == 2) {
        first[0] = 1;
        if (!hadDouble)
            AssignString(first + 4, kDoubleHilt);
        AssignString(second + 4, "");
        return;
    }
    first[0] = 0;
    if (hadDouble || !hilt || !*hilt)
        AssignString(first + 4, kSingleHilt);
    if (custom->blades == 1) {
        AssignString(second + 4, "");
        return;
    }
    second[0] = 0; // dual: a second saber like the first, in the other hand
    AssignString(second + 4, *reinterpret_cast<const char**>(first + 4));
    std::memcpy(second + 8, first + 8, 12);
}

// The Jedi set-up (0x2814C0, thiscall () -> bool: the sabers made, then the Force set to a constant, +0xA40 and its
// most +0xA44; a class whose Init sets no Force of its own after it, as the Jedi Brute, has none): a custom
// character's Force and size after it. Its first instructions: push esi; mov esi, ecx; call 0x2670F0 (relative,
// re-made in the stub).
constexpr uint32_t kJediSetup = 0x002814C0;
constexpr uint8_t kJediSetupBytes[] = { 0x56, 0x8B, 0xF1, 0xE8 };
constexpr uint32_t kJediSetupCallee = 0x002670F0;
constexpr uint32_t kCharacterForce = 0xA40, kCharacterMaxForce = 0xA44, kObjectScale = 0x194;
using JediSetupFn = bool(__fastcall*)(uint8_t*, void*);
JediSetupFn g_OriginalJediSetup = nullptr;

bool __fastcall JediSetupHook(uint8_t* character, void*)
{
    const bool result = g_OriginalJediSetup(character, nullptr);
    if (const CustomCharacter* custom = CustomCharacterOf(character)) {
        if (custom->force >= 0) {
            *reinterpret_cast<float*>(character + kCharacterForce) = custom->force;
            *reinterpret_cast<float*>(character + kCharacterMaxForce) = custom->force;
        }
        if (custom->scale != 1.0f)
            *reinterpret_cast<float*>(character + kObjectScale) = custom->scale;
    }
    return result;
}

// A special's charge-up effect (the streaks around the saber, IDS_FX_Saber_Chargeup_<colour>: saber_powerup_red, _grn,
// _blue, _purple) is picked by the script instance's saber colour (+0x374, an RGB; 0x4106D0, thiscall (instance, ...4),
// ret 0x14: exactly red, green, blue or purple, else none), which a custom character's base set (the Jedi Brute's
// green). For a custom character with its own colour, the pick sees the nearest of the four.
constexpr uint32_t kChargeupPick = 0x004106D0;
constexpr uint8_t kChargeupPickBytes[] = { 0x8B, 0x54, 0x24, 0x04, 0xF3, 0x0F, 0x10, 0x8A, 0x74, 0x03, 0x00, 0x00 };
using ChargeupPickFn = void(__fastcall*)(void*, void*, uint8_t*, void*, void*, void*, void*);
ChargeupPickFn g_ChargeupPick = nullptr;

void __fastcall ChargeupPickHook(void* self, void*, uint8_t* instance, void* a, void* b, void* c, void* d)
{
    uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(0x006943CC));
    uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
    const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
    if (!custom || !custom->hasSaber || !instance)
        return g_ChargeupPick(self, nullptr, instance, a, b, c, d);
    static const float kColours[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 } };
    const float* saber = SaberColourOf(character, *custom);
    int best = 0;
    float bestDistance = 1e9f;
    for (int i = 0; i < 4; ++i) {
        float distance = 0;
        for (int k = 0; k < 3; ++k)
            distance += (saber[k] - kColours[i][k]) * (saber[k] - kColours[i][k]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    float* colour = reinterpret_cast<float*>(instance + 0x374);
    float own[3];
    std::memcpy(own, colour, sizeof own);
    std::memcpy(colour, kColours[best], sizeof own);
    static const char* const kNames[4] = { "red", "green", "blue", "purple" };
    LOG_INFO("Custom characters: %s's charge-up effect is the %s one", custom->className.c_str(), kNames[best]);
    g_ChargeupPick(self, nullptr, instance, a, b, c, d);
    std::memcpy(colour, own, sizeof own);
}

// A script's size change of its character (0x178FD0, cdecl (scale, frames): at once (+0x194) or eased over the frames
// (+0x2C0 the target, +0x2C4 the time)): Vader's duel intro grows him to 1.1 and back to 1. A custom character's
// size (Scale=) is its 1: the change is relative to it, so another duelist's intro keeps its size.
constexpr uint32_t kScriptSetScale = 0x00178FD0;
constexpr uint8_t kScriptSetScaleBytes[] = { 0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0 };

void __cdecl ScriptSetScale(float scale, int frames)
{
    uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(0x006943CC)); // [..] +0x68: the script's character
    uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
    if (!character)
        return;
    if (const CustomCharacter* custom = CustomCharacterOf(character))
        scale *= custom->scale;
    if (!frames) {
        *reinterpret_cast<float*>(character + kObjectScale) = scale;
        return;
    }
    *reinterpret_cast<float*>(character + 0x2C0) = scale;
    *reinterpret_cast<float*>(character + 0x2C4) = float(frames) * *reinterpret_cast<const float*>(uintptr_t(0x00645EA4));
}

// Force controls for a base without them. Every Jedi script class has the Force sequences (the shared Jedi set-up:
// their handles at the same places in every class object, e.g. Force push's at +0x119E), but a class's player rules
// decide what the buttons do: each class's StateManager calls its own (cdecl (script instance, class object)); the
// Jedi Brute's (0x3D9D50, called at 0x3D9D38) has no Force rules. The port's run after it, as Vader's (0x48E5B0:
// inputs 0x48E5C1, gate 0x48EA70, which powers 0x48EB6E, rules 0x48ECB4-0x48F50D): a branch is offered with
// 0x175D60 (cdecl (condition, sequence, 3, 3, 1, 1.0f); the sequence from its handle by 0x1F53E0 (handle, file,
// line)); the second stance (state 0x32, 0x1741A0) has its own set (each handle +2).
constexpr uint32_t kBruteRulesCall = 0x003D9D38;
constexpr uint32_t kBruteRules = 0x003D9D50;
constexpr uint32_t kScriptCharacter = 0x006943CC; // [..] +0x68: the character whose script runs
using RulesFn = void(__cdecl*)(uint8_t*, uint8_t*);

bool Pressed(uint32_t button) // the button id's global; 0x178070 (id, 5, 0, 2)
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t, int, int, int)>(uintptr_t(0x00178070))(
        *reinterpret_cast<const uint32_t*>(uintptr_t(button)), 5, 0, 2);
}

bool Direction(uint32_t test, uint8_t* inst) // 0x1F5440 back, 0x1F5480 left, 0x1F54C0 right on [inst+0xFC]
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t)>(uintptr_t(test))(*reinterpret_cast<uint32_t*>(inst + 0xFC));
}

void Offer(bool condition, uint8_t* classObject, uint32_t handleAt)
{
    const uint16_t handle = *reinterpret_cast<uint16_t*>(classObject + handleAt);
    void* sequence = reinterpret_cast<void*(__cdecl*)(uint32_t, const char*, int)>(uintptr_t(0x001F53E0))(handle,
        "customchars.cpp", __LINE__);
    reinterpret_cast<void(__cdecl*)(bool, void*, int, int, int, float)>(uintptr_t(0x00175D60))(condition, sequence,
        3, 3, 1, 1.0f);
}

void ForceRules(uint8_t* inst, uint8_t* classObject, const CustomCharacter& custom)
{
    const auto call = [](uint32_t f) { return reinterpret_cast<bool(__cdecl*)()>(uintptr_t(f))(); };
    // what the Jedi set-up 0x400EE0 does and the Jedi Brute's Init leaves out: the powers' costs (lightning, throw,
    // push, grip), once; which powers (the Jedi constructor's defaults, or the character's)
    float* costs = reinterpret_cast<float*>(inst + 0x330);
    if (costs[0] == 0 && costs[3] == 0) {
        costs[0] = costs[1] = 100;
        costs[2] = 150;
        costs[3] = 250;
        costs[4] = 100;
        if (custom.forcePowers >= 0) {
            inst[0x2DC] = custom.forcePowers & 1 ? 1 : 0;
            inst[0x2DB] = custom.forcePowers & 2 ? 1 : 0;
            inst[0x2DF] = custom.forcePowers & 4 ? 1 : 0;
            inst[0x2DD] = custom.forcePowers & 8 ? 1 : 0;
            inst[0x2D9] = custom.forcePowers & 16 ? 1 : 0;
        }
    }
    // the computer's character (0x176E30) offers every power it can pay for; its attack table's rows (the Force
    // class's, added to the base's: ForceRows) choose among them by range and facing, its conditions are not asked
    const bool ai = call(0x00176E30);
    // the gate (nothing else chosen, ...), as Vader's
    if (inst[0x1DA] || call(0x002BD150) || !call(0x002BC160) || inst[0x2F2])
        return;
    const int taps = reinterpret_cast<int(__cdecl*)(uint32_t, int, int, int, int, int)>(uintptr_t(0x001780F0))(
        *reinterpret_cast<const uint32_t*>(uintptr_t(0x006964E8)), 0, 0, 10, 1, 0);
    const bool push = taps >= 1, lightning = Pressed(0x006964F4), saberThrow = Pressed(0x00696500);
    const bool heal = Pressed(0x00696514) && Pressed(0x00696518);
    const float force = ai ? reinterpret_cast<float(__cdecl*)()>(uintptr_t(0x00177B10))() : 0.0f;
    const auto affords = [&](uint32_t costAt) { return !ai || force >= *reinterpret_cast<float*>(inst + costAt); };
    const bool canPush = inst[0x2DC] && affords(0x33C), canGrip = inst[0x2DB] && affords(0x340);
    const bool canLightning = inst[0x2DD] && affords(0x330), canThrow = inst[0x2DF] && !inst[0x2F9] && affords(0x338);
    // heal: the computer's only below its health threshold (0x2BCF60)
    const bool canHeal = inst[0x2D9] && (!ai || reinterpret_cast<float(__cdecl*)()>(uintptr_t(0x00177B40))() <
        reinterpret_cast<float(__cdecl*)()>(uintptr_t(0x002BCF60))());
    if (canGrip) // the grip's target (jedi_functions: 0x426190 (inst, class object, 0x1E0, 24, 350))
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int, float, float)>(uintptr_t(0x00426190))(inst,
            classObject, 0x1E0, 24.0f, 350.0f);
    if (ai && !call(0x002BD030)) // the character's "Allow Force Powers"
        return;
    const uint32_t alt = reinterpret_cast<bool(__cdecl*)(int)>(uintptr_t(0x001741A0))(0x32) ? 2 : 0;
    const bool back = Direction(0x001F5440, inst), left = Direction(0x001F5480, inst), right = Direction(0x001F54C0, inst);
    // each power: back, left, right, then any way
    const auto power = [&](bool trigger, uint32_t backAt, uint32_t leftAt, uint32_t rightAt, uint32_t plainAt) {
        Offer(trigger && back, classObject, backAt + alt);
        Offer(trigger && left, classObject, leftAt + alt);
        Offer(trigger && right, classObject, rightAt + alt);
        Offer(trigger, classObject, plainAt + alt);
    };
    if (canPush)
        power(push, 0x11AA, 0x11A2, 0x11A6, 0x119E);
    if (canHeal)
        Offer(heal, classObject, 0x1248 + alt);
    if (canLightning)
        power(lightning, 0x11FC, 0x11F4, 0x11F8, 0x11F0);
    if (canThrow)
        power(saberThrow, 0x122A, 0x1222, 0x1226, 0x121E);
    if (canGrip) // offered only: the grip comes from holding the push (the push sequence's own logic)
        Offer(false, classObject, 0x11B6 + alt);
}

// The Jedi Brute's script, starting (its Init 0x3DA3E0), sets its character's Force to none (0x177AE0 (0, 0), cdecl
// (force, most) on the script's character [[0x6943CC]+0x68], called at 0x3DA43C): a custom character's Force= instead.
constexpr uint32_t kBruteSetForceCall = 0x003DA43C;
constexpr uint32_t kSetForce = 0x00177AE0;

// Its health, the same way: the Jedi Brute's script sets 220 (0x177AC0 (health): the script's character's vfunc
// +0x380), called at 0x3DA433.
constexpr uint32_t kBruteSetHealthCall = 0x003DA433;
constexpr uint32_t kSetHealth = 0x00177AC0;

void __cdecl BruteSetHealthHook(float health)
{
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->health > 0)
            health = custom->health;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    reinterpret_cast<void(__cdecl*)(float)>(uintptr_t(kSetHealth))(health);
}

void __cdecl CloneSetHealthHook(float health)
{
    BruteSetHealthHook(health);
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->force >= 0)
            reinterpret_cast<void(__cdecl*)(float, float)>(uintptr_t(kSetForce))(custom->force, custom->force);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __cdecl BruteSetForceHook(float force, float most)
{
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->force >= 0)
            force = most = custom->force;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    reinterpret_cast<void(__cdecl*)(float, float)>(uintptr_t(kSetForce))(force, most);
}

// The computer's choice among the moves its rules offer is its attack table's (a_<class>.csv, one per script
// class: TAttackTable::Cache 0x1ADFF0 from ICharacter::Init, bound to the class's sequences by name, 0x1ABEB0): a
// row gives a move its range, facing, bias and damage; an offered move without one is taken at any distance. The
// Jedi Brute's has no Force rows: the Force class's (ForceFrom=) are added to it, those it lacks. The Jedi Brute's
// own rules never offer a Force move, so the game's brutes choose as before.
constexpr const char* kForceBaseTable = "meshes\\chars\\common\\a_jdbrute.csv";
std::string g_ForceRowsFrom; // the Force class's table, e.g. "meshes\chars\common\a_vader.csv"

void AddForceRows(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> donor;
    if (g_ForceRowsFrom.empty() || !ReadDiscResource(g_ForceRowsFrom, donor))
        return;
    const auto rowName = [](const std::string& line) { return line.substr(0, line.find(',')); };
    const auto lines = [](const std::vector<uint8_t>& bytes) {
        std::vector<std::string> out;
        std::string line;
        for (uint8_t b : bytes) {
            if (b == '\n') {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                out.push_back(line);
                line.clear();
            } else {
                line += char(b);
            }
        }
        if (!line.empty())
            out.push_back(line);
        return out;
    };
    std::vector<std::string> own = lines(data);
    std::string added;
    int count = 0;
    for (const std::string& line : lines(donor)) {
        const std::string name = rowName(line);
        static const char* const kForceMoves[] = { "ForcePush", "ForceGrasp", "ForceLightning", "ForceHeal", "ForceSaberThrow" };
        if (std::none_of(std::begin(kForceMoves), std::end(kForceMoves),
                [&](const char* p) { return _strnicmp(name.c_str(), p, strlen(p)) == 0; }))
            continue;
        if (std::any_of(own.begin(), own.end(), [&](const std::string& o) { return _stricmp(rowName(o).c_str(), name.c_str()) == 0; }))
            continue;
        added += line + "\r\n";
        ++count;
    }
    if (!data.empty() && data.back() != '\n')
        data.insert(data.end(), { '\r', '\n' });
    data.insert(data.end(), added.begin(), added.end());
    LOG_INFO("Custom characters: %s gets %d Force rows from %s", kForceBaseTable, count, g_ForceRowsFrom.c_str());
}

// Its Force sounds: a character's sounds are its event tree's (eventtree.h), the Jedi Brute's (s_jdbrute) without
// Force sounds. Vader's Force entries are added to it as it loads (his Force sequences and animations are the Jedi
// Brute's too); the game's brutes never play those, so they sound as before.
constexpr const char* kForceBaseTree = "meshes\\chars\\common\\s_jdbrute.xml";
constexpr const char* kForceDonorTree = "meshes\\chars\\common\\s_vader.xbl_xml";
constexpr const char* kForceDonorTreeSource = "meshes\\chars\\common\\s_vader.xml";

void AddForceSounds(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> donor;
    const size_t before = data.size();
    if (!ReadDiscResource(kForceDonorTree, donor) && !ReadDiscResource(kForceDonorTreeSource, donor)) {
        LOG_WARN("Custom characters: Vader's event tree is not on the disc; Maul's Force stays silent");
        return;
    }
    if (!AppendEventEntries(data, donor, &VaderForceEntry)) {
        LOG_WARN("Custom characters: Vader's Force sounds could not be added to the Jedi Brute's event tree (%zu, %zu bytes)",
            data.size(), donor.size());
        return;
    }
    LOG_INFO("Custom characters: %s gets Vader's Force sounds (%zu -> %zu bytes)", kForceBaseTree, before, data.size());
}

void __cdecl BruteRulesHook(uint8_t* inst, uint8_t* classObject)
{
    reinterpret_cast<RulesFn>(uintptr_t(kBruteRules))(inst, classObject);
    __try {
        uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
        uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (custom && custom->forceControls && inst && classObject)
            ForceRules(inst, classObject, *custom);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A duelist's Versus intro and outro for a custom character (Intro=). Each duelist class has two sequence functions
// (<prefix>_Intro / _Outro: the animation of the same name, the line said at an animation event, SayEvent "TNT" /
// "KIL" in the "Versus" context), which the duel finds by name in the character's class's records (0x27B550:
// "%s_%s" of [char +0x324] and "Intro"/"Outro", looked up by 0x150D60). The donor's functions are added to the
// base class's setup (classsetup.h) as they are, through wrappers that hand them a scratch class object (their
// only use of it is the handle slot they write, which would be one of the base's own); the duel's name for the
// character is the donor's prefix (at 0x27B5A2: push eax ("Intro"/"Outro"); push edi (the name); lea eax,
// [esp+0x1C]).
struct Duelist {
    const char* className;
    const char* prefix;
    uint32_t intro, outro;
    const char* cameras; // cinematics\introcamera\<cameras>_Intro_Cam.cin
};
constexpr Duelist kDuelists[] = {
    { "IVader", "Vader", 0x0048A990, 0x0048AAA0, "Vader" },
    { "IAnakin", "Anakin", 0x00314A10, 0x00314AF0, "Anakin" },
    { "ICinDrallig", "Cin", 0x0034CAB0, 0x0034CBA0, "CinDrallig" },
    { "IDooku", "Dooku", 0x00372FF0, 0x003730D0, "Dooku" },
    { "IMace", "Mace", 0x00421AC0, 0x00421BA0, "Mace" },
    { "IObiwan", "Obi", 0x00447090, 0x00447170, "Obiwan" },
    { "IOldObiwan", "OldObiwan", 0x00461170, 0x00461250, "obiold" },
    { "ISerra", "Serra", 0x00474450, 0x00474530, "Serra" },
};
constexpr int kMaxIntroFns = 16;
uint32_t g_IntroTargets[kMaxIntroFns] = {};
alignas(16) uint8_t g_ScratchClass[0x2000] = {};

template <int K> bool __fastcall IntroFn(uint8_t*, void*, uint32_t a, uint32_t b)
{
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t, uint32_t)>(uintptr_t(g_IntroTargets[K]))(
        g_ScratchClass, nullptr, a, b);
}

template <int... K> constexpr std::array<ClassSequenceFn, sizeof...(K)> IntroFns(std::integer_sequence<int, K...>)
{
    return { &IntroFn<K>... };
}
constexpr auto kIntroFns = IntroFns(std::make_integer_sequence<int, kMaxIntroFns>());

// A custom character's own intro and outro (IntroAnimation=, OutroAnimation=): a sequence "<costume>_Intro" or
// "<costume>_Outro" playing one of its base's animations the way a duelist's does (Vader_Intro, 0x48A990: optional,
// required on duel maps ("DuelMap"); at its first frame 0x38A140 (instance, class object, 2, 2)), its line said as it
// starts (a taunt, a kill line: its own IntroLine= / OutroLine= when it has one). Nothing else: an attack's animation
// played so is no attack (no attack-table row, no hit).
struct OwnIntro {
    std::string name, animation, line;
    float speed = 1.0f;
    bool outro = false;
    uint16_t handle = 0;
};
constexpr int kMaxOwnIntros = 8;
OwnIntro g_OwnIntros[kMaxOwnIntros];
constexpr uint32_t kSetup = 0x006965D8;
const char kDuelMapTag[] = "DuelMap";

template <int K> bool __fastcall OwnIntroFn(uint8_t* self, void*, uint32_t, uint32_t)
{
    auto* setup = reinterpret_cast<uint8_t*>(uintptr_t(kSetup));
    OwnIntro& own = g_OwnIntros[K];
    const bool declared = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(0x001F5B20))(setup, nullptr, &own.handle, own.name.c_str(), own.animation.c_str(), own.speed, 0x0055FB60);
    if (declared) {
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696531)) = 1;
        *reinterpret_cast<const char**>(uintptr_t(0x006964B8)) = kDuelMapTag;
    }
    if (reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(0x001F62D0))(setup, nullptr)) { // as Vader's
        uint8_t* instance = *reinterpret_cast<uint8_t**>(uintptr_t(0x006964C0));
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int, int)>(uintptr_t(0x0038A140))(instance, self, 2, 2);
    }
    // its line as it starts (0x1F6310: the event the Jedi Brute's twirl turns its spinning saber on at; 0x1F62D0's
    // came as the intro ended)
    if (reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(0x001F6310))(setup, nullptr)) {
        const char* event = own.outro ? "KIL" : "TNT";
        LOG_INFO("Custom characters: %s plays", own.name.c_str());
        if (!own.line.empty()) {
            uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(0x006943CC));
            if (uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr)
                ForceNextLine(character, event, own.line.c_str());
        }
        reinterpret_cast<int(__cdecl*)(const char*, const char*, int, int, int)>(uintptr_t(kSayEvent))(event, "Versus", 1, 0, 0);
    }
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, const char*, uint32_t)>(uintptr_t(0x001F6C00))(
        setup, nullptr, reinterpret_cast<const char*>(uintptr_t(0x0059C07C)), 0x7E00 + K);
}

template <int... K> constexpr std::array<ClassSequenceFn, sizeof...(K)> OwnIntroFns(std::integer_sequence<int, K...>)
{
    return { &OwnIntroFn<K>... };
}
constexpr auto kOwnIntroFns = OwnIntroFns(std::make_integer_sequence<int, kMaxOwnIntros>());

std::string IntroPrefix(const CustomCharacter& c)
{
    return c.costume.empty() ? c.className.substr(1) : c.costume;
}

int __cdecl IntroSequences(const char* shortName, ClassSequenceFn* fns, int max)
{
    // only where intros play (a Versus duel, Heroes vs Villains): the added sequences' animations become the class's
    // required ones, which another level need not have
    if (!BootIsMatch())
        return 0;
    int count = 0, owns = 0;
    bool added[std::size(kDuelists)][2] = {};
    for (const CustomCharacter& c : g_Customs) {
        if ((c.intro < 0 && c.introAnimation.empty() && c.outroAnimation.empty()) || c.base.size() < 2 ||
            _stricmp(c.base.c_str() + 1, shortName) != 0)
            continue;
        for (int outro = 0; outro < 2; ++outro) {
            const std::string& animation = outro ? c.outroAnimation : c.introAnimation;
            if (animation.empty() || owns >= kMaxOwnIntros || count >= max)
                continue;
            OwnIntro& own = g_OwnIntros[owns];
            own.name = IntroPrefix(c) + (outro ? "_Outro" : "_Intro");
            own.animation = animation;
            own.speed = outro ? c.outroSpeed : c.introSpeed;
            own.line = outro ? c.outroLine : c.introLine;
            own.outro = outro != 0;
            own.handle = 0;
            fns[count++] = kOwnIntroFns[size_t(owns++)];
            LOG_INFO("Custom characters: %s's duel %s is %s", c.className.c_str(), outro ? "outro" : "intro", animation.c_str());
        }
        if (c.intro < 0)
            continue;
        const Duelist& d = kDuelists[c.intro];
        for (int outro = 0; outro < 2; ++outro) {
            if ((outro ? !c.outroAnimation.empty() : !c.introAnimation.empty()) || added[c.intro][outro] || count >= max ||
                count >= kMaxIntroFns)
                continue;
            added[c.intro][outro] = true;
            g_IntroTargets[count] = outro ? d.outro : d.intro;
            fns[count] = kIntroFns[size_t(count)];
            ++count;
            LOG_INFO("Custom characters: %s plays %s's duel %s", c.className.c_str(), d.className, outro ? "outro" : "intro");
        }
    }
    return count;
}

constexpr uint32_t kIntroName = 0x0027B5A2;
constexpr uint8_t kIntroNameBytes[] = { 0x50, 0x57, 0x8D, 0x44, 0x24, 0x1C };
const char* g_IntroName = nullptr;

const char* __stdcall IntroName(const uint8_t* character, const char* name, const char* kind)
{
    const CustomCharacter* custom = CustomCharacterOf(character);
    if (!custom)
        return name;
    const bool outro = kind && _stricmp(kind, "Outro") == 0;
    if (!(outro ? custom->outroAnimation : custom->introAnimation).empty()) // its own
        if (const char* own = PooledString(IntroPrefix(*custom)))
            return own;
    return custom->intro >= 0 ? kDuelists[custom->intro].prefix : name;
}

__declspec(naked) void IntroNameEntry()
{
    __asm {
        pushad
        push eax
        push edi
        push esi
        call IntroName
        mov g_IntroName, eax
        popad
        push eax
        push g_IntroName
        lea eax, [esp + 0x1C]
        push 0x0027B5A8
        ret
    }
}

__declspec(naked) void SaberSetupHook()
{
    __asm {
        pushad
        push esi
        call ApplyBlades
        add esp, 4
        popad
        lea ebx, [esi + 0x1068]
        ret
    }
}

// At 0x15BC56 (6 bytes, replaced by a call): esi the character, edx its name's text.
__declspec(naked) void SpeakerNameStore()
{
    __asm {
        pushad
        push edx
        push esi
        call SpeakerNameFor
        add esp, 8
        mov [esp + 20], eax // the saved edx
        popad
        mov [esi + 0xA04], edx
        ret
    }
}

// The custom characters made this start (to find a saber's owner; a removed one no longer has a custom list).
std::vector<uint8_t*> g_Made;

bool IsLiveObject(uint8_t* object);

} // namespace

// A second saber of its own colour (SaberLeft=): the equip gives both sabers the character's colour (+0xF20), and a
// Versus intro shows them before its script first runs, so each frame its left one (the second weapon, +0x10A0) is
// given its own colour when it has another (SetColor, vfunc +0x604, stores it at +0x814).
void PruneMade()
{
    g_Made.erase(std::remove_if(g_Made.begin(), g_Made.end(), [](uint8_t* c) { return !IsLiveObject(c); }), g_Made.end());
}

void CustomCharactersFrame()
{
    PruneMade(); // those taken away (a match's fighters after a respawn) out
    __try {
        for (uint8_t* c : g_Made) {
            if (!IsLiveObject(c))
                continue;
            const CustomCharacter* custom = CustomCharacterOf(c);
            if (!custom || !custom->hasSaberLeft)
                continue;
            uint8_t* left = *reinterpret_cast<uint8_t**>(c + 0x1080 + 0x20);
            if (left && std::memcmp(left + 0x814, custom->saberLeft, sizeof custom->saberLeft) != 0)
                reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(
                    (*reinterpret_cast<void* const* const*>(left))[0x604 / 4])(left, nullptr, custom->saberLeft);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

namespace {

bool IsLiveObject(uint8_t* object)
{
    // an object still in the level: the instance id at +4 finds it again (0xA31F0); a removed one's memory may be gone
    __try {
        return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(0x000A31F0))(*reinterpret_cast<uint32_t*>(object + 4)) == object;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A saber's colour is set through its class's SetColor (vtable +0x604: 0x286180 for the single-bladed classes,
// 0x2864F0 for the double-bladed one), by the equip, the power-up switch and some moves (the Jedi Brute's specials
// light its saber in its own green): a custom character's sabers keep its colour.
constexpr uint32_t kSaberVtables[] = { 0x005B1FF0, 0x005B2620, 0x005B2C90, 0x005B32F8, 0x005B3958 };
constexpr uint32_t kSaberSetColorSlot = 0x604 / 4;
constexpr uint32_t kCharacterWeapons = 0x1080; // 4 slots, 0x20 apart
using SetColorFn = void(__fastcall*)(uint8_t*, void*, const float*);
SetColorFn g_SetColorOriginals[std::size(kSaberVtables)] = {};

const float* OwnColour(uint8_t* weapon)
{
    __try {
        for (uint8_t* c : g_Made) {
            if (!IsLiveObject(c)) // one taken away (a Heroes vs Villains fighter's old body)
                continue;
            const CustomCharacter* custom = CustomCharacterOf(c);
            if (!custom || !custom->hasSaber)
                continue;
            for (int i = 0; i < 4; ++i)
                if (*reinterpret_cast<uint8_t**>(c + kCharacterWeapons + i * 0x20) == weapon)
                    return i == 1 && custom->hasSaberLeft ? custom->saberLeft : SaberColourOf(c, *custom);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void __fastcall SaberSetColorHook(uint8_t* weapon, void*, const float* rgb)
{
    const uint32_t vtable = *reinterpret_cast<uint32_t*>(weapon);
    SetColorFn original = nullptr;
    for (size_t i = 0; i < std::size(kSaberVtables); ++i)
        if (kSaberVtables[i] == vtable)
            original = g_SetColorOriginals[i];
    if (!original)
        return;
    if (const float* own = OwnColour(weapon))
        rgb = own;
    original(weapon, nullptr, rgb);
}

uint8_t* __fastcall CreateCustom(uint8_t* info, void*)
{
    Proxy* p = ProxyOf(info);
    if (!p)
        return nullptr;
    using CreateFn = uint8_t*(__fastcall*)(uint8_t*, void*);
    const auto create = reinterpret_cast<CreateFn>((*reinterpret_cast<void* const* const*>(p->baseInfo))[kCreateSlot]);
    uint8_t* character = create(p->baseInfo, nullptr);
    if (!character)
        return nullptr;
    const uint8_t* baseList = *reinterpret_cast<const uint8_t* const*>(character + kCharacterVariants);
    if (p->list.empty() || baseList != p->baseList)
        BuildList(*p, baseList);
    *reinterpret_cast<uint8_t**>(character + kCharacterVariants) = p->list.data();
    // only a Jedi has the colour pointer (IsA, vfunc +4, the IJedi type 0x249950): +0xF20 is past a clone's end
    const auto isA = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>(
        (*reinterpret_cast<void* const* const*>(character))[1]);
    if (p->custom->hasSaber && isA(character, nullptr, 0x00249950))
        *reinterpret_cast<const float**>(character + kCharacterSaberColor) = p->custom->saber;
    if (!p->textureSet) // the plain one (not a set the base's list names: a respawn could pick a _duel look)
        *reinterpret_cast<int*>(character + kCharacterTextureSet) = 0;
    if (p->textureSet) { // its own texture set
        *reinterpret_cast<int*>(character + kCharacterTextureSet) = p->textureSet;
        if (!p->custom->textureSetFile.empty() && !DeclareDiscResource(p->custom->textureSetFile))
            DeclareGeneratedResource(p->custom->textureSetFile, 0);
    }
    g_Made.push_back(character);
    return character;
}

bool CopyInfo(uint8_t* info, uint32_t* vtable, const uint8_t* baseInfo)
{
    __try {
        std::memcpy(info, baseInfo, kInfoCopy);
        std::memcpy(vtable, *reinterpret_cast<void* const*>(baseInfo), kVtableCopy * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t* __cdecl LookupHook(const char* name)
{
    const CustomCharacter* custom = name ? FindCustomCharacter(name) : nullptr;
    if (!custom)
        return g_OriginalLookup(name);
    uint8_t* baseInfo = g_OriginalLookup(custom->base.c_str());
    if (!baseInfo)
        return nullptr;
    for (auto& p : g_Proxies)
        if (p->custom == custom && p->baseInfo == baseInfo)
            return p->info.get();
    // A copy of the base's info and vtable; the create is the custom one.
    auto p = std::make_unique<Proxy>();
    p->custom = custom;
    p->baseInfo = baseInfo;
    p->info.reset(new uint8_t[kInfoCopy]);
    p->vtable.reset(new uint32_t[kVtableCopy]);
    if (!CopyInfo(p->info.get(), p->vtable.get(), baseInfo))
        return baseInfo;
    p->vtable[kCreateSlot] = uint32_t(reinterpret_cast<uintptr_t>(&CreateCustom));
    *reinterpret_cast<uint32_t**>(p->info.get()) = p->vtable.get();
    p->costumeName = custom->costume;
    p->costumeMesh = custom->mesh;
    p->costumeCode = custom->code;
    LOG_INFO("Custom characters: %s is a %s with %s's body", custom->className.c_str(), custom->base.c_str(),
        custom->mesh.c_str());
    g_Proxies.push_back(std::move(p));
    return g_Proxies.back()->info.get();
}

std::string ProfileIn(const std::wstring& file, const wchar_t* section, const wchar_t* key)
{
    wchar_t value[1024] = {};
    GetPrivateProfileStringW(section, key, L"", value, 1024, file.c_str());
    std::string out;
    for (const wchar_t* c = value; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

std::string Profile(const std::wstring& file, const wchar_t* key, const wchar_t* fallback = L"")
{
    wchar_t value[256] = {};
    GetPrivateProfileStringW(L"Character", key, fallback, value, 256, file.c_str());
    std::string out;
    for (const wchar_t* c = value; *c; ++c)
        out += char(*c < 128 ? *c : '?');
    return out;
}

std::string Upper(std::string text)
{
    for (char& c : text)
        c = char(toupper(static_cast<unsigned char>(c)));
    return text;
}

void ReadCharacters(const std::wstring& mods)
{
    g_Customs.clear();
    g_VoiceLines.clear();
    WIN32_FIND_DATAW found;
    const std::wstring folder = mods + L"\\characters\\";
    HANDLE find = FindFirstFileW((folder + L"*.ini").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring file = folder + found.cFileName;
        CustomCharacter c;
        c.className = Profile(file, L"Class");
        c.base = Profile(file, L"Base");
        c.mesh = Profile(file, L"Mesh");
        c.mesh2 = Profile(file, L"Mesh2");
        if (c.className.empty() || c.base.empty() || c.mesh.empty() || _stricmp(c.className.c_str(), c.base.c_str()) == 0) {
            LOG_WARN("Custom characters: %ls needs Class, Base and Mesh", found.cFileName);
            continue;
        }
        c.name = Upper(Profile(file, L"Name", L""));
        if (c.name.empty())
            c.name = Upper(c.className.size() > 1 && c.className[0] == 'I' ? c.className.substr(1) : c.className);
        c.title = Upper(Profile(file, L"Title"));
        c.costume = Profile(file, L"Costume");
        if (c.costume.empty())
            c.costume = c.className.size() > 1 && c.className[0] == 'I' ? c.className.substr(1) : c.className;
        c.code = Profile(file, L"Code");
        c.speaker = Profile(file, L"Speaker");
        if (c.speaker.empty()) { // the name's first word, as the game's own ("Anakin", "Obi-Wan")
            const std::string name = Profile(file, L"Name");
            c.speaker = name.substr(0, name.find(' '));
            if (c.speaker.empty())
                c.speaker = c.className.size() > 1 && c.className[0] == 'I' ? c.className.substr(1) : c.className;
        }
        c.speakerKey = "PORT_" + Upper(c.className);
        SetMenuText(("IDS_SPEAKER_" + c.speakerKey).c_str(), std::wstring(c.speaker.begin(), c.speaker.end()));
        const std::string saber = Profile(file, L"Saber");
        int r, g, b;
        if (std::sscanf(saber.c_str(), "%d %d %d", &r, &g, &b) == 3) {
            c.hasSaber = true;
            c.saber[0] = r / 255.0f;
            c.saber[1] = g / 255.0f;
            c.saber[2] = b / 255.0f;
        }
        if (std::sscanf(Profile(file, L"Saber2").c_str(), "%d %d %d", &r, &g, &b) == 3) {
            c.hasSaber2 = true;
            c.saber2[0] = r / 255.0f;
            c.saber2[1] = g / 255.0f;
            c.saber2[2] = b / 255.0f;
        }
        if (std::sscanf(Profile(file, L"SaberLeft").c_str(), "%d %d %d", &r, &g, &b) == 3) {
            c.hasSaberLeft = true;
            c.saberLeft[0] = r / 255.0f;
            c.saberLeft[1] = g / 255.0f;
            c.saberLeft[2] = b / 255.0f;
        }
        c.lightning = std::atoi(Profile(file, L"Lightning").c_str()) != 0;
        c.grapples = Profile(file, L"Grapples");
        c.forceStorm = std::atoi(Profile(file, L"ForceStorm").c_str()) != 0;
        c.side = _stricmp(Profile(file, L"Side", L"Hero").c_str(), "villain") == 0 ? 1 : 0;
        c.scale = std::clamp(float(atof(Profile(file, L"Scale", L"1").c_str())), 0.5f, 2.0f);
        c.breathing = std::atoi(Profile(file, L"Breathing", L"1").c_str()) != 0;
        const std::string force = Profile(file, L"Force");
        c.force = force.empty() ? -1.0f : std::clamp(float(atof(force.c_str())), 0.0f, 10000.0f);
        const std::string health = Profile(file, L"Health");
        c.health = health.empty() ? -1.0f : std::clamp(float(atof(health.c_str())), 1.0f, 100000.0f);
        c.forceFrom = Profile(file, L"ForceFrom");
        c.forceControls = !c.forceFrom.empty();
        {
            const std::string powers = Upper(Profile(file, L"ForcePowers"));
            if (!powers.empty()) {
                c.forcePowers = 0;
                static const struct { const char* name; int bit; } kPowers[] = { { "PUSH", 1 }, { "GRIP", 2 },
                    { "GRASP", 2 }, { "SABERTHROW", 4 }, { "LIGHTNING", 8 }, { "HEAL", 16 } };
                for (const auto& p : kPowers)
                    if (powers.find(p.name) != std::string::npos)
                        c.forcePowers |= p.bit;
            }
        }
        c.introAnimation = Profile(file, L"IntroAnimation");
        if (const std::string speed = Profile(file, L"IntroSpeed"); !speed.empty() && std::atof(speed.c_str()) > 0)
            c.introSpeed = float(std::atof(speed.c_str()));
        c.outroAnimation = Profile(file, L"OutroAnimation");
        if (const std::string speed = Profile(file, L"OutroSpeed"); !speed.empty() && std::atof(speed.c_str()) > 0)
            c.outroSpeed = float(std::atof(speed.c_str()));
        c.introLine = Upper(Profile(file, L"IntroLine"));
        c.outroLine = Upper(Profile(file, L"OutroLine"));
        if (const std::string intro = Profile(file, L"Intro"); !intro.empty()) {
            for (size_t i = 0; i < std::size(kDuelists); ++i)
                if (_stricmp(intro.c_str(), kDuelists[i].className) == 0 && _stricmp(intro.c_str(), c.base.c_str()) != 0)
                    c.intro = int(i);
            if (c.intro < 0 && _stricmp(intro.c_str(), c.base.c_str()) != 0)
                LOG_WARN("Custom characters: %ls: Intro=%s is not a duelist with an intro of its own", found.cFileName,
                    intro.c_str());
        }
        c.hilt = Profile(file, L"Hilt");
        if (!c.hilt.empty() && _strnicmp(c.hilt.c_str(), "Meshes", 6) != 0) // "lsaberluke\lsaberluke" alone too
            c.hilt = "Meshes\\Weapons\\" + c.hilt;
        const std::string blades = Profile(file, L"Blades");
        c.blades = _stricmp(blades.c_str(), "single") == 0 ? 1 : _stricmp(blades.c_str(), "double") == 0 ? 2
            : _stricmp(blades.c_str(), "dual") == 0 ? 3 : 0;
        if (!blades.empty() && !c.blades)
            LOG_WARN("Custom characters: %ls: Blades=%s is none of single, double, dual", found.cFileName, blades.c_str());
        std::string costume = c.costume;
        for (char& ch : costume)
            ch = char(tolower(static_cast<unsigned char>(ch)));
        const std::string face = "interfc\\hud_b\\hud_face_" + costume + "01";
        g_Mods = mods; // (ModFileExists)
        if (ModFileExists(face + ".stx")) { // loaded from its file by the HUD (characters.cpp)
            c.face = face;
            c.hudFace = face;
            if (ModFileExists(face + "_duel.stx"))
                c.duelFace = face + "_duel";
        }
        std::string source;
        for (const wchar_t* ch = found.cFileName; *ch; ++ch)
            source += char(*ch < 128 ? *ch : '?');
        c.source = source;
        // its voice: [Voice] EVENT=line line ..., the lines in characters\<name>\voice\<line>.xadpcm, their
        // texts in [Subtitles] LINE=text; player 2's in a Versus mirror likewise in [Voice2], if it has one
        {
            const std::wstring stem = file.substr(0, file.size() - 4);
            auto readVoice = [&](const wchar_t* section, std::vector<std::pair<std::string, std::vector<std::string>>>& out) {
                wchar_t keys[4096] = {};
                GetPrivateProfileStringW(section, nullptr, L"", keys, 4096, file.c_str());
                for (const wchar_t* k = keys; *k; k += wcslen(k) + 1) {
                    std::string event;
                    for (const wchar_t* ch = k; *ch; ++ch)
                        event += char(toupper(*ch < 128 ? *ch : '?'));
                    std::vector<std::string> ids;
                    std::istringstream words(ProfileIn(file, section, k));
                    std::string id;
                    while (words >> id) {
                        for (char& ch : id)
                            ch = char(toupper(static_cast<unsigned char>(ch)));
                        std::string lower = id;
                        for (char& ch : lower)
                            ch = char(tolower(static_cast<unsigned char>(ch)));
                        const std::wstring path = stem + L"\\voice\\" + std::wstring(lower.begin(), lower.end()) + L".xadpcm";
                        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                            LOG_WARN("Custom characters: %s's line %s has no file", c.className.c_str(), id.c_str());
                            continue;
                        }
                        ids.push_back(id);
                        bool known = false;
                        for (const VoiceLine& v : g_VoiceLines)
                            known |= v.id == id;
                        if (!known) {
                            const std::string text = ProfileIn(file, L"Subtitles", std::wstring(id.begin(), id.end()).c_str());
                            g_VoiceLines.push_back({ id, path, std::wstring(text.begin(), text.end()) });
                        }
                    }
                    if (!ids.empty())
                        out.push_back({ event, ids });
                }
            };
            readVoice(L"Voice", c.voice);
            readVoice(L"Voice2", c.voice2);
        }
        LOG_INFO("Custom characters: %s (%s, from %s), from characters\\%s", c.className.c_str(), c.name.c_str(),
            c.base.c_str(), source.c_str());
        g_Customs.push_back(std::move(c));
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

// Pictures of the disc's served under another name (generated, written to cache\disc): their texture name (0x40, 32
// bytes) is the new one. Built-in fighters' select head and HUD face.
std::vector<std::pair<std::string, std::string>> g_PictureCopies; // new name, the disc's (lower case, with .stx)

bool CopyPicture(const std::string& lowerName, std::vector<uint8_t>& data)
{
    for (const auto& [name, source] : g_PictureCopies) {
        if (name != lowerName)
            continue;
        if (!ReadDiscResource(source, data) || data.size() < 0x104)
            return false;
        std::string texture = name.substr(name.rfind('\\') + 1);
        texture = texture.substr(0, texture.size() - 4);
        std::memset(data.data() + 0x40, 0, 32);
        std::memcpy(data.data() + 0x40, texture.data(), std::min<size_t>(texture.size(), 31));
        return true;
    }
    return false;
}

// Cody's Separatist counterpart: a B1 battle droid commander, the battle droid class on its captain's costume
// (hordeBattleDroidCaptain, list index 5: the rapid blaster and the captain's pulse bolt) with the yellow markings
// (texture set _var01), its speaker code BDD (the captain's BDC has no lines), 1000 health and a 1000 meter as Cody's.
// Its kit is Cody's (cody.cpp). Pictures: none of a B1 on the disc; the random bust and head stand in
// unless a mod gives its own (by its costume's name: interfc\front_end\s_selduel_hordebattledroidcaptainhead.stx /
// ...bust.stx, s_selduel_fb_hordebattledroidcaptain_v1/_v2.stx; interfc\hud_b\hud_face_droidport01.stx).
void AddDroidCommander()
{
    if (FindCustomCharacter("IDroidCommander"))
        return;
    CustomCharacter c;
    c.builtIn = true;
    c.className = "IDroidCommander";
    c.name = "DROID COMMANDER";
    c.title = "SEPARATIST DROID";
    c.base = "IBattleDroid";
    c.costume = "hordeBattleDroidCaptain";
    c.mesh = "battledroid\\hordeBattleDroid";
    c.code = "BDD";
    c.speaker = "Battle droid";
    c.speakerKey = "Battledroid";
    c.health = 1000;
    c.force = 1000;
    c.side = 1;
    c.introAnimation = "BDroid_Atk_Shooting_In";
    c.introLine = "BDD945";              // "Ready weapons."
    c.outroAnimation = "BDroid_Atk_Shooting_Out";
    c.outroLine = "BDD963";              // "Jedi down. Situation normal."
    c.textureSet = "_var01";             // the yellow markings
    c.textureSetFile = "meshes\\chars\\battledroid\\hordebattledroid_var01.stx";
    // its bust: a mod's own (drawn in the stand-in's place, as a custom character's), else the disc's random one
    if (!ModFileExists("interfc\\front_end\\s_selduel_hordebattledroidcaptainbust.stx"))
        c.selectBust = "s_selduel_randombust";
    c.face = c.hudFace = "interfc\\hud_b\\hud_face_droidport01";
    c.source = "(built in)";
    if (!ModFileExists("interfc\\front_end\\s_selduel_hordebattledroidcaptainhead.stx"))
        g_PictureCopies.push_back({ "interfc\\front_end\\s_selduel_hordebattledroidcaptainhead.stx",
            "interfc\\front_end\\s_selduel_randomhead.stx" });
    if (!ModFileExists("interfc\\hud_b\\hud_face_droidport01.stx"))
        g_PictureCopies.push_back({ "interfc\\hud_b\\hud_face_droidport01.stx", "interfc\\front_end\\s_selduel_randomhead.stx" });
    g_Customs.push_back(c);
    LOG_INFO("Custom characters: IDroidCommander (DROID COMMANDER, from IBattleDroid), built in");
}

// Commander Cody: the cut playable Cody (ICommanderCody: its script survives, but no body, definition or tables) was a
// clone trooper with his own rules, so he is one here: the clone class on the 212th's body (the clone's costume with its
// plain texture set), his speaker code (CMC: his lines in the generic dialogue every duel level has), his name
// (IDS_SPEAKER_CommanderCody), 1000 health and a 1000 meter for his specials as the cut Cody's Init gave him, the
// signal to attack as his intro. Pictures: his select bust (the menus' PAK has it), his HUD face as his strip head.
void AddBuiltInCharacters()
{
    AddDroidCommander();
    if (FindCustomCharacter("ICody"))
        return; // a mod's own Cody
    CustomCharacter c;
    c.builtIn = true;
    c.className = "ICody";
    c.name = "COMMANDER CODY";
    c.title = "CLONE COMMANDER";
    c.base = "ICloneTrooper";
    c.costume = "Cody";
    c.mesh = "clonetrooper\\hordeTrooper_cody"; // the 212th's body with his helmet's rangefinder (cody.cpp)
    c.code = "CMC";
    c.speaker = "Commander Cody";
    c.speakerKey = "CommanderCody"; // the game's own text
    c.health = 1000; // as a duelist's (the HUD's bar is drawn on that scale); hits on him count more (cody.cpp)
    c.force = 1000;
    c.side = 0;
    c.introAnimation = "ClnCom_Signal_Strike";
    c.introSpeed = 2.0f;                 // about 3 s
    c.introLine = "CMC951";              // "Ready weapons."
    c.outroAnimation = "ClnCom_Signal_Right";
    c.outroLine = "CMC967";              // "Target eliminated."
    c.textureSet = "_cody";              // his helmet's markings (cody.cpp)
    c.textureSetFile = "meshes\\chars\\clonetrooper\\hordetrooper_cody.stx";
    c.selectBust = "s_selduel_codybust";
    c.face = c.hudFace = "interfc\\hud_b\\hud_face_codyport01";
    c.source = "(built in)";
    // his helmet (the game's Cody unlock portrait; hud_face_cody01 is the clones' blaster icon)
    g_PictureCopies.push_back({ "interfc\\front_end\\s_selduel_codyhead.stx", "interfc\\hud_b\\hud_face_unlock_cody01.stx" });
    g_PictureCopies.push_back({ "interfc\\hud_b\\hud_face_codyport01.stx", "interfc\\hud_b\\hud_face_unlock_cody01.stx" });
    g_Customs.push_back(c);
    LOG_INFO("Custom characters: ICody (COMMANDER CODY, from ICloneTrooper), built in");
}

// Vader's script (its VaderBreath state, 0x48D4B0) breathes in and out through the script's sound call (0x17AD20,
// cdecl (sound, near, 1, far, -1, 0)) at these four places; a custom character on his class with Breathing=0 does not.
constexpr uint32_t kScriptPlaySound = 0x0017AD20;
constexpr uint32_t kBreathCalls[] = { 0x0048D547, 0x0048D577, 0x0048D5BE, 0x0048D5EE };

int __cdecl BreathSoundHook(int sound, int nearDistance, int one, int farDistance, int loop, int zero)
{
    uint8_t* scripts = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacter));
    const uint8_t* character = scripts ? *reinterpret_cast<uint8_t**>(scripts + 0x68) : nullptr;
    const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
    if (custom && !custom->breathing)
        return 0;
    return reinterpret_cast<int(__cdecl*)(int, int, int, int, int, int)>(uintptr_t(kScriptPlaySound))(
        sound, nearDistance, one, farDistance, loop, zero);
}

// A clone's Init sets its health (0x35B36D: 0x177AC0 (50, 1)); a custom character on it gets its own, and its meter
// (Force +0xA40/+0xA44, which the clone has but leaves 0). The battle droid's the same (0x338B06: 30).
constexpr uint32_t kCloneSetHealthCall = 0x0035B36D;
constexpr uint32_t kDroidSetHealthCall = 0x00338B06;

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (256 << 20);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(file);
    return ok;
}

// The game's generic dialogue archive (Dialogue\gen_dialogue.rpx, mounted in every level) with the custom lines
// added, served in its place for this start. The archive: u32 1, u32 count, 0x18 bytes, then `count` 0x2C-byte
// records {name[0x20] (lower case, sorted), u32 offset, u32 size, u32 (the same in all)}, free room up to the first
// line's data, the lines (2048-aligned). The new records go in their sorted places, their lines at the end; when the
// room before the first line is too small for the records (the game's archive has room for some 20 more), every line
// moves down by whole sectors (the offsets are absolute). Made from a mod's own archive when there is one
// (mods\Dialogue\gen_dialogue.rpx: its replaced lines stay), and rebuilt in cache\voice\ when its inputs change.
void BuildVoiceArchive(const std::wstring& mods, const std::wstring& gameData, const std::wstring& cache)
{
    static const std::string kArchive = "\\dialogue\\gen_dialogue.rpx";
    kernel::SetDiscFileOverride(kArchive, L"");
    g_VoiceServed = false;
    if (g_VoiceLines.empty())
        return;
    std::wstring source = mods + L"\\Dialogue\\gen_dialogue.rpx";
    const bool modsArchive = GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!modsArchive)
        source = gameData + L"\\Dialogue\\gen_dialogue.rpx";
    const std::wstring folder = cache + L"\\voice", target = folder + L"\\gen_dialogue.rpx";
    // its inputs: the archive's size and every line's file and size
    std::string stamp = modsArchive ? "mods;" : "game;";
    WIN32_FILE_ATTRIBUTE_DATA info;
    auto describe = [&](const std::wstring& path) {
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
            stamp += std::to_string(info.nFileSizeLow) + ":" + std::to_string(info.ftLastWriteTime.dwLowDateTime) + ";";
    };
    describe(source);
    for (const VoiceLine& v : g_VoiceLines) {
        stamp += v.id + "=";
        describe(v.file);
    }
    std::vector<uint8_t> old;
    if (!(ReadWhole(folder + L"\\stamp.txt", old) && std::string(old.begin(), old.end()) == stamp &&
            GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        std::vector<uint8_t> archive;
        if (!ReadWhole(source, archive) || archive.size() < 0x20) {
            LOG_WARN("Custom characters: the dialogue archive is missing; custom voices are not heard");
            return;
        }
        uint32_t count;
        std::memcpy(&count, archive.data() + 4, 4);
        struct Record {
            std::string name;
            uint32_t offset, size, tag;
        };
        std::vector<Record> records;
        uint32_t firstData = uint32_t(archive.size());
        for (uint32_t i = 0; i < count && 0x20 + (i + 1) * 0x2C <= archive.size(); ++i) {
            const uint8_t* r = archive.data() + 0x20 + i * 0x2C;
            Record record{ std::string(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 0x20)) };
            std::memcpy(&record.offset, r + 0x20, 4);
            std::memcpy(&record.size, r + 0x24, 4);
            std::memcpy(&record.tag, r + 0x28, 4);
            firstData = std::min(firstData, record.offset);
            records.push_back(record);
        }
        size_t added = 0;
        for (const VoiceLine& v : g_VoiceLines) {
            std::string name = v.id;
            for (char& ch : name)
                ch = char(tolower(static_cast<unsigned char>(ch)));
            std::vector<uint8_t> line;
            if (name.size() >= 0x20 || !ReadWhole(v.file, line) || line.empty())
                continue;
            if (std::any_of(records.begin(), records.end(), [&](const Record& r) { return _stricmp(r.name.c_str(), name.c_str()) == 0; }))
                continue;
            archive.resize((archive.size() + 2047) & ~size_t(2047), 0);
            records.push_back({ name, uint32_t(archive.size()), uint32_t(line.size()), records.empty() ? 0 : records[0].tag });
            archive.insert(archive.end(), line.begin(), line.end());
            ++added;
        }
        const size_t table = 0x20 + records.size() * 0x2C;
        if (table > firstData) {
            const uint32_t shift = uint32_t((table - firstData + 2047) & ~size_t(2047));
            archive.insert(archive.begin() + firstData, shift, 0);
            for (Record& r : records)
                r.offset += shift;
        }
        archive.resize((archive.size() + 2047) & ~size_t(2047), 0);
        std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
        const uint32_t total = uint32_t(records.size());
        std::memcpy(archive.data() + 4, &total, 4);
        for (size_t i = 0; i < records.size(); ++i) {
            uint8_t* r = archive.data() + 0x20 + i * 0x2C;
            std::memset(r, 0, 0x20);
            std::memcpy(r, records[i].name.data(), records[i].name.size());
            std::memcpy(r + 0x20, &records[i].offset, 4);
            std::memcpy(r + 0x24, &records[i].size, 4);
            std::memcpy(r + 0x28, &records[i].tag, 4);
        }
        CreateDirectoryW(folder.c_str(), nullptr);
        auto write = [](const std::wstring& path, const void* data, size_t size) {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                return false;
            DWORD written = 0;
            const bool ok = WriteFile(f, data, DWORD(size), &written, nullptr) && written == size;
            CloseHandle(f);
            return ok;
        };
        if (!write(target, archive.data(), archive.size()) || !write(folder + L"\\stamp.txt", stamp.data(), stamp.size())) {
            LOG_WARN("Custom characters: the dialogue archive could not be written to the cache");
            return;
        }
        LOG_INFO("Custom characters: the dialogue archive with %zu custom lines built", added);
    }
    kernel::SetDiscFileOverride(kArchive, target);
    g_VoiceServed = true;
    for (const VoiceLine& v : g_VoiceLines) // the subtitles' texts (the dialogue looks a line's id up)
        SetMenuText(v.id.c_str(), v.text);
}

} // namespace

void InstallCustomCharacters(const std::wstring& mods, const std::wstring& gameData, const std::wstring& cache)
{
    g_Proxies.clear(); // class infos are made anew at every start
    g_PooledLines.clear();
    g_LineTurns.clear();
    g_SpeakerTexts.clear();
    g_LoggedLines.clear();
    g_Mods = mods;
    ReadCharacters(mods);
    g_PictureCopies.clear();
    AddBuiltInCharacters();
    RegisterResourceGenerator(&CopyPicture);
    BuildVoiceArchive(mods, gameData, cache);
    if (g_Customs.empty())
        return;
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kClassLookup)), kClassLookupBytes, sizeof(kClassLookupBytes)) != 0) {
        LOG_WARN("Custom characters: the class lookup is not as expected; no custom characters");
        g_Customs.clear();
        return;
    }
    // The stub lives in the port's memory and survives reboots; the jump is patched into every new image.
    static uint8_t* stub = [] {
        uint8_t* s = AllocStub(sizeof(kClassLookupBytes) + 5);
        std::memcpy(s, kClassLookupBytes, sizeof(kClassLookupBytes));
        s[sizeof(kClassLookupBytes)] = 0xE9;
        const int32_t rel = int32_t(kClassLookup + sizeof(kClassLookupBytes)) -
            int32_t(uintptr_t(s) + sizeof(kClassLookupBytes) + 5);
        std::memcpy(s + sizeof(kClassLookupBytes) + 1, &rel, 4);
        return s;
    }();
    g_OriginalLookup = reinterpret_cast<LookupFn>(stub);
    PatchJump(kClassLookup, reinterpret_cast<const void*>(&LookupHook));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kStartEntry)), kStartEntryBytes, sizeof kStartEntryBytes) == 0) {
        static uint8_t* startEntryStub = [] { // once: the port's memory outlives the game's reboots
            uint8_t* stub = AllocStub(sizeof kStartEntryBytes + 5);
            std::memcpy(stub, kStartEntryBytes, sizeof kStartEntryBytes);
            stub[sizeof kStartEntryBytes] = 0xE9;
            const int32_t rel = int32_t(kStartEntry + sizeof kStartEntryBytes) -
                int32_t(uintptr_t(stub) + sizeof kStartEntryBytes + 5);
            std::memcpy(stub + sizeof kStartEntryBytes + 1, &rel, 4);
            return stub;
        }();
        g_StartEntry = reinterpret_cast<StartEntryFn>(startEntryStub);
        PatchJump(kStartEntry, reinterpret_cast<const void*>(&StartEntryHook));
    }
    g_DuelLine = {};
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kChargeupPick)), kChargeupPickBytes, sizeof kChargeupPickBytes) == 0) {
        static uint8_t* pickStub = [] { // once: the port's memory outlives the game's reboots
            uint8_t* stub = AllocStub(sizeof kChargeupPickBytes + 5);
            std::memcpy(stub, kChargeupPickBytes, sizeof kChargeupPickBytes);
            stub[sizeof kChargeupPickBytes] = 0xE9;
            const int32_t rel = int32_t(kChargeupPick + sizeof kChargeupPickBytes) -
                int32_t(uintptr_t(stub) + sizeof kChargeupPickBytes + 5);
            std::memcpy(stub + sizeof kChargeupPickBytes + 1, &rel, 4);
            return stub;
        }();
        g_ChargeupPick = reinterpret_cast<ChargeupPickFn>(pickStub);
        PatchJump(kChargeupPick, reinterpret_cast<const void*>(&ChargeupPickHook));
    }
    if (std::any_of(g_Customs.begin(), g_Customs.end(), [](const CustomCharacter& c) { return c.scale != 1.0f; }) &&
        std::memcmp(reinterpret_cast<const void*>(uintptr_t(kScriptSetScale)), kScriptSetScaleBytes, sizeof kScriptSetScaleBytes) == 0)
        PatchJump(kScriptSetScale, reinterpret_cast<const void*>(&ScriptSetScale));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kSayEvent)), kSayEventBytes, sizeof kSayEventBytes) == 0) {
        static uint8_t* sayStub = [] { // once: the port's memory outlives the game's reboots
            uint8_t* stub = AllocStub(sizeof kSayEventBytes + 5);
            std::memcpy(stub, kSayEventBytes, sizeof kSayEventBytes);
            stub[sizeof kSayEventBytes] = 0xE9;
            const int32_t rel = int32_t(kSayEvent + sizeof kSayEventBytes) - int32_t(uintptr_t(stub) + sizeof kSayEventBytes + 5);
            std::memcpy(stub + sizeof kSayEventBytes + 1, &rel, 4);
            return stub;
        }();
        g_SayEvent = reinterpret_cast<SayEventFn>(sayStub);
        PatchJump(kSayEvent, reinterpret_cast<const void*>(&SayEventHook));
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kSaberSetup)), kSaberSetupBytes, sizeof kSaberSetupBytes) == 0)
        PatchCall(kSaberSetup, reinterpret_cast<const void*>(&SaberSetupHook), sizeof kSaberSetupBytes);
    g_Made.clear();
    for (size_t i = 0; i < std::size(kSaberVtables); ++i) {
        uint32_t* slot = reinterpret_cast<uint32_t*>(uintptr_t(kSaberVtables[i])) + kSaberSetColorSlot;
        const uint32_t hook = uint32_t(reinterpret_cast<uintptr_t>(&SaberSetColorHook));
        if (*slot == hook)
            continue;
        g_SetColorOriginals[i] = reinterpret_cast<SetColorFn>(uintptr_t(*slot));
        PatchBytes(uintptr_t(slot), &hook, 4);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kSetHealth) - int32_t(kBruteSetHealthCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBruteSetHealthCall)), call, 5) == 0)
            PatchCall(kBruteSetHealthCall, reinterpret_cast<const void*>(&BruteSetHealthHook), 5);
    }
    if (std::any_of(g_Customs.begin(), g_Customs.end(), [](const CustomCharacter& c) { return !c.breathing; })) {
        for (uint32_t site : kBreathCalls) {
            uint8_t call[5] = { 0xE8 };
            const int32_t rel = int32_t(kScriptPlaySound) - int32_t(site + 5);
            std::memcpy(call + 1, &rel, 4);
            if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(site)), call, 5) == 0)
                PatchCall(site, reinterpret_cast<const void*>(&BreathSoundHook), 5);
        }
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kSetHealth) - int32_t(kCloneSetHealthCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kCloneSetHealthCall)), call, 5) == 0)
            PatchCall(kCloneSetHealthCall, reinterpret_cast<const void*>(&CloneSetHealthHook), 5);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kSetHealth) - int32_t(kDroidSetHealthCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kDroidSetHealthCall)), call, 5) == 0)
            PatchCall(kDroidSetHealthCall, reinterpret_cast<const void*>(&CloneSetHealthHook), 5);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kSetForce) - int32_t(kBruteSetForceCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBruteSetForceCall)), call, 5) == 0)
            PatchCall(kBruteSetForceCall, reinterpret_cast<const void*>(&BruteSetForceHook), 5);
    }
    {
        uint8_t call[5] = { 0xE8 };
        const int32_t rel = int32_t(kBruteRules) - int32_t(kBruteRulesCall + 5);
        std::memcpy(call + 1, &rel, 4);
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kBruteRulesCall)), call, 5) == 0)
            PatchCall(kBruteRulesCall, reinterpret_cast<const void*>(&BruteRulesHook), 5);
    }
    g_ForceRowsFrom.clear();
    for (const CustomCharacter& c : g_Customs) {
        if (!c.forceControls || _stricmp(c.base.c_str(), "IJDBrute") != 0 || c.forceFrom.size() < 2)
            continue;
        std::string table = "meshes\\chars\\common\\a_" + c.forceFrom.substr(1) + ".csv";
        for (char& ch : table)
            ch = char(tolower(static_cast<unsigned char>(ch)));
        g_ForceRowsFrom = table; // one table per base: the first character's Force class
        RegisterResourcePatch(kForceBaseTable, &AddForceRows);
        if (_stricmp(c.forceFrom.c_str(), "IVader") == 0) // the sounds chosen for that pair
            RegisterResourcePatch(kForceBaseTree, &AddForceSounds);
        break;
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kJediSetup)), kJediSetupBytes, sizeof kJediSetupBytes) == 0) {
        static uint8_t* jediStub = [] { // once: the port's memory outlives the game's restarts
            uint8_t* s = AllocStub(13);
            s[0] = 0x56; // push esi
            s[1] = 0x8B; // mov esi, ecx
            s[2] = 0xF1;
            s[3] = 0xE8; // call 0x2670F0
            const int32_t call = int32_t(kJediSetupCallee) - int32_t(uintptr_t(s) + 8);
            std::memcpy(s + 4, &call, 4);
            s[8] = 0xE9; // jmp back after the call
            const int32_t back = int32_t(kJediSetup + 8) - int32_t(uintptr_t(s) + 13);
            std::memcpy(s + 9, &back, 4);
            return s;
        }();
        g_OriginalJediSetup = reinterpret_cast<JediSetupFn>(jediStub);
        PatchJump(kJediSetup, reinterpret_cast<const void*>(&JediSetupHook));
    }
    if (std::any_of(g_Customs.begin(), g_Customs.end(),
            [](const CustomCharacter& c) { return c.intro >= 0 || !c.introAnimation.empty() || !c.outroAnimation.empty(); })) {
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kIntroName)), kIntroNameBytes, sizeof kIntroNameBytes) == 0) {
            AddClassSequences(&IntroSequences);
            PatchJump(kIntroName, reinterpret_cast<const void*>(&IntroNameEntry));
            PatchNop(kIntroName + 5, sizeof kIntroNameBytes - 5);
        } else {
            LOG_WARN("Custom characters: the duel intro's name is not as expected; custom characters keep the base's intro");
        }
    }
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kSpeakerNameStore)), kSpeakerNameStoreBytes,
            sizeof kSpeakerNameStoreBytes) == 0)
        PatchCall(kSpeakerNameStore, reinterpret_cast<const void*>(&SpeakerNameStore), sizeof kSpeakerNameStoreBytes);
    else
        LOG_WARN("Custom characters: the subtitles' speaker name is not as expected; custom characters keep the base's");
}

void ForceNextLine(const uint8_t* speaker, const char* event, const char* lineId)
{
    const char* id = PooledString(lineId);
    if (!speaker || !event || !id)
        return;
    g_Forced.speaker = speaker;
    strncpy_s(g_Forced.event, event, 3);
    g_Forced.id = id;
    g_Forced.until = GetTickCount64() + 3000;
}

const char* IntroCameras(const CustomCharacter& custom)
{
    return custom.intro >= 0 ? kDuelists[custom.intro].cameras : nullptr;
}

const std::vector<CustomCharacter>& CustomCharacters()
{
    return g_Customs;
}

const CustomCharacter* CustomCharacterOf(const uint8_t* character)
{
    if (!character)
        return nullptr;
    const uint8_t* list = *reinterpret_cast<const uint8_t* const*>(character + kCharacterVariants);
    if (!list)
        return nullptr;
    for (auto& p : g_Proxies)
        if (!p->list.empty() && p->list.data() == list)
            return p->custom;
    // a character in another body (`player <class> mesh`, `spawn ... mesh`) wears its own copy of its class's list
    // (characters.cpp, Dress): the copy keeps the records' names, so its first one is the custom costume's own string
    __try {
        const char* first = *reinterpret_cast<const char* const*>(list + kVariantRecords);
        for (auto& p : g_Proxies)
            if (!p->list.empty() && first == p->costumeName.c_str())
                return p->custom;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool IsMirrorPlayer2(const uint8_t* character)
{
    __try {
        // player 2 in Versus: the duel gives it the texture suffix "_duel" (character +0x1F4, 0x27B88D)
        const char* suffix = *reinterpret_cast<const char* const*>(character + kCharacterTextureSuffix);
        return suffix && _stricmp(suffix, "_duel") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

const std::string& CustomFaceOf(const uint8_t* character, const CustomCharacter& custom)
{
    if (!custom.duelFace.empty() && IsMirrorPlayer2(character))
        return custom.duelFace;
    return custom.hudFace;
}

bool ModFileExists(const std::string& relative)
{
    if (g_Mods.empty())
        return false;
    const std::wstring path = g_Mods + L"\\" + std::wstring(relative.begin(), relative.end());
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ReadModFile(const std::string& relative, std::vector<uint8_t>& data)
{
    if (g_Mods.empty())
        return false;
    const std::wstring path = g_Mods + L"\\" + std::wstring(relative.begin(), relative.end());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        data.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(file);
    return ok;
}

const CustomCharacter* FindCustomCharacter(const char* className)
{
    for (const CustomCharacter& c : g_Customs)
        if (className && _stricmp(c.className.c_str(), className) == 0)
            return &c;
    return nullptr;
}

} // namespace swrots::game
