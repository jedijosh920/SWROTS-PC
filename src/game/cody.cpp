// Commander Cody's fighting kit (see cody.h).
//
// A clone under player control has one rule, the rifle butt on fast attack (the clone's rules 0x35AE70, player part
// 0x35B2A4); its shooting is its AI's (CloneTrooper.cpp: a layer over what it does, a bolt at its target). The cut
// playable Cody (ICommanderCody, whose script survives, rules 0x360D00) had his own: a shot on fast attack and a burst
// on heavy attack for 250 of his meter. Here his meter (Force +0xA40, which the clone has; refilled here) pays for all
// he does beyond the rifle butt, for the player's Cody and the computer's alike:
//   - heavy attack (Y) held: the shooting layer and a bolt every kShotIntervalMs (in the clone's
//     script's turn: its StateManager, called for every clone every frame, the input functions reading the script's
//     character's own controller);
//   - block (LT): a roll aside (left or right with the stick), the clone's roll out of cover (CTroop_Nav_RollOut_L/R)
//     as sequences of the port's (optional in the class, so not loaded in a duel: the port's are required on duel maps,
//     as the duelists' intros are), instead of a block, which a clone has not;
//   - push (RT): two clones of his rappelling down beside him (the clone's own CTroop_Rappel_Loop: rope, slide, landing)
//     for kHelperLifeMs, one squad at a time; he signals them in and calls for them ("Bring up the reinforcements!");
//   - critical (B): a bombardment: shells (the game's IExplosion) falling round his opponent, hurting none of his;
//     he signals the strike ("Concentrate fire on the target!").
// What the player's input asks for is made outside the scripts (CodyFrame).
//
// The same kit makes his Separatist counterpart, the built-in IDroidCommander (a B1 battle droid commander, the battle
// droid class on its captain's costume and yellow markings): the captain's rapid blaster, a duck under the saber (its
// kneel, quicker) for the roll, the droid's push-away for the rifle butt, two battle droids dropped in beside it (they
// fall in on their own fall and landing) and the same bombardment, each from its own moves and lines (Kind).

#include "game/cody.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/log.h"
#include "core/patch.h"
#include "input/controls.h"
#include "game/characters.h"
#include "game/classsetup.h"
#include "game/customchars.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/hvv.h"
#include "game/meshedit.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

constexpr uint32_t kIsComputer = 0x00176E30;             // cdecl bool (): the AI drives the script's character
constexpr uint32_t kScriptCharacterContext = 0x006943CC; // +0x68: the character whose script runs, +0x70 its state
constexpr uint32_t kHeldFrames = 0x00177FF0;             // cdecl int (button, 0), in a script's turn
constexpr uint32_t kPressed = 0x00178070;                // cdecl bool (button, 1, 0, 2)
constexpr uint32_t kButtonHeavy = 0x006964DC, kButtonCritical = 0x006964EC; // the script's button ids (Y, B)
constexpr uint32_t kButtonPush = 0x006964E8;     // (RT: F)
constexpr uint32_t kButtonDodge = 0x00696500;    // White (L1: Q; the Jedi's saber throw): LT is the lock-on's
constexpr uint32_t kSayEvent = 0x0017ACC0;               // cdecl (event, context, 1, 0, 0): the script character says
constexpr uint32_t kScriptHasTarget = 0x00176FF0;   // cdecl bool ()
constexpr uint32_t kScriptPlayLayer = 0x00173B40;   // cdecl (animation, ...): an animation layer
constexpr uint32_t kScriptStopLayer = 0x00173B80;   // cdecl (layer, blend)
constexpr uint32_t kScriptShotSpread = 0x00176AB0;  // cdecl bool (): no spread
constexpr uint32_t kCloneFireBolt = 0x003C0090;     // cdecl (instance, script, 0, spread, muzzle data, muzzle)
constexpr uint32_t kCloneNextMuzzle = 0x001760B0;   // cdecl (muzzle*, count)
constexpr uint32_t kScriptPlaySound = 0x0017AD20;   // cdecl (sound, near, 1, far, -1, 0)
constexpr uint32_t kCloneMuzzle = 0x3D8, kCloneMuzzleData = 0x3EC, kCloneFireSound = 0x3E4; // in the instance
constexpr char kCloneShootingLoop[] = "ctroop_atk_shooting_loop";
// the battle droid's blaster (BattleDroid script, InitSound 0x337500 / StartFX 0x3373B0): its bolt 0x3BF8D0 (instance,
// script, 0, spread, sound, muzzle), which plays the sound and the muzzle flash itself
constexpr uint32_t kDroidFireBolt = 0x003BF8D0;
constexpr uint32_t kDroidMuzzle = 0x268, kDroidFireSound = 0x24C; // in the instance
constexpr uint32_t kButtonFast = 0x006964D8;     // X (Mouse 1): the droid commander's push-away
constexpr uint32_t kCharacterHealth = 0x130, kCharacterHealthMost = 0x134, kCharacterMeter = 0xA40, kCharacterMeterMost = 0xA44;
constexpr uint32_t kCharacterTransform = 0x150, kCharacterOpponent = 0x3B8, kCharacterScript = 0x434;
constexpr uint32_t kCharacterAI = 0x9FC;
constexpr uint32_t kCharacterPort = 0x43C;     // the controller port driving it, -1 none
constexpr uint32_t kAISetTarget = 0x00193D90;  // thiscall on the AI controller (target)
constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id) -> object or null
constexpr uintptr_t kPlaySequence = 0x150E30;  // ICharacter, thiscall (sequence, 0, 0, 0, speed)
constexpr uintptr_t kFindSequence = 0x150D60;  // ICharacter, thiscall (name) -> its sequence record or null

// his meter
constexpr float kMeterPerSecond = 35;
constexpr float kSummonCost = 500, kBombardCost = 600, kShotCost = 20;
// his roll costs a little meter, one a kDodgeWaitMs at most; no hit lands on him in the middle of it (from kRollGuardFromMs
// to kRollGuardToMs after it starts: a roll in time beats a combo, a roll as the blow lands does not)
constexpr float kRollCost = 150;
constexpr ULONGLONG kDodgeWaitMs = 1000, kRollGuardFromMs = 120, kRollGuardToMs = 480;
// a round starts with his meter full, his specials no sooner than kSpecialsAfterMs, each with its own wait after use
constexpr ULONGLONG kSpecialsAfterMs = 8000, kSummonWaitMs = 30000, kBombardWaitMs = 20000;
constexpr ULONGLONG kPressKeepMs = 350;
constexpr ULONGLONG kShotIntervalMs = 450;
constexpr ULONGLONG kHelperLifeMs = 25000;
// the computer's Cody up close: a look this often, a roll away about half the time, else often his rifle butt
constexpr float kComputerCloseIn = 230, kComputerStrikeWithin = 130;
constexpr ULONGLONG kComputerLookMs = 700;
// a duel gives its fighters 1000 health (its bar a fixed scale): a blaster against a saber takes hits a third harder
constexpr float kDamageTaken = 1.6f;
constexpr float kHardestHit = 120; // before kDamageTaken
// A hit's reaction: ICharacter::Reaction_Call 0x159E20, thiscall on the one hit (bump, collision, &sequence) -> true
// when dealt with; its own early ways out (the script's "no reactions" bit, a cutscene) return true with none. It also
// counts a hit on a player holding the block button as blocked (no damage, no reaction): a clone has no block
constexpr float kHelperSide = 90;            // how far beside him they come down
constexpr float kRappelHeight = 320;         // and from how high (indoor arenas have ceilings)
constexpr ULONGLONG kHelperEngageMs = 2500;  // they turn on his opponent once down
constexpr float kComputerCallsWithin = 900;  // the computer's Cody calls his specials with his opponent this near
constexpr ULONGLONG kArmAfterMs = 7000;      // his specials only once the duel's intro is over (he appears in it)
// the bombardment
constexpr int kShells = 4;
constexpr ULONGLONG kFirstShellMs = 900, kShellGapMs = 350;
constexpr float kShellSpread = 110;

template <typename T> T& At(uint8_t* p, uint32_t offset)
{
    return *reinterpret_cast<T*>(p + offset);
}

bool Live(uint8_t* object, uint32_t id)
{
    return object && reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id) == object;
}

const float* Position(uint8_t* character)
{
    return reinterpret_cast<const float*>(character + kCharacterTransform + 48);
}

uint32_t Dice()
{
    static uint32_t state = 0x9E3779B9u ^ uint32_t(GetTickCount64());
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// A fighter of the kit: its class (built in, customchars.cpp), its script's class, its moves and its lines.
struct Kind {
    const char* className;
    const char* script;      // the script class its moves are added to
    const char* helper;      // its reinforcements: their class and costume
    const char* helperSkin;
    const char* helperLabel; // their mark
    bool rappel;             // they rappel down (else they drop in from the same height)
    bool droid;              // the droid's blaster and shooting layer
    const char* dodge;       // its dodge, and whether it goes the stick's way (a roll) or is made where it stands (a duck)
    bool dodgeTurns;
    const char* melee;       // its blow (the computer's; and the player's on fast attack when playerMelee)
    bool playerMelee;
    const char* call;        // its gesture calling the reinforcements, and the strike
    const char* strike;
    const char* event;       // the dialogue event its lines are said through
    const char* callLine;
    const char* strikeLine;
    bool trooper = false;    // a Battlefront trooper: no specials, its shots and dodges free (B calls its hero)
    float shotCost = kShotCost, rollCost = kRollCost;
};
constexpr Kind kKinds[] = {
    { "ICody", "CloneTrooper", "ICloneTrooper", "", "CODY'S CLONE", true, false, "Cody_RollRight", true,
      "ctroop_atk_riflebutt", false, "Cody_SignalLeft", "Cody_SignalStrike",
      "TKE", "CLC013", "CMC915" },  // "Bring up the reinforcements!", "Concentrate fire on the target!"
    { "IDroidCommander", "BattleDroid", "IBattleDroid", "hordeBattleDroid", "COMMANDER'S DROID", false, true,
      "Droid_Duck", false, "Droid_Pushaway", true, "Droid_Call", "Droid_Strike",
      "TNT", "BDD951", "BDD943" },  // "Move in, corporal, we'll cover you.", "Fire!"
};
// The player's trooper in Battlefront (hvv.h): a clone or a battle droid with Cody's or the droid commander's
// blaster, roll or duck and blow, nothing more.
constexpr Kind kCloneTrooper = { "ICloneTrooper", "CloneTrooper", nullptr, nullptr, nullptr, false, false,
    "Cody_RollRight", true, "ctroop_atk_riflebutt", false, nullptr, nullptr, nullptr, nullptr, nullptr, true, 0, 0 };
constexpr Kind kDroidTrooper = { "IBattleDroid", "BattleDroid", nullptr, nullptr, nullptr, false, true,
    "Droid_Duck", false, "Droid_Pushaway", true, nullptr, nullptr, nullptr, nullptr, nullptr, true, 0, 0 };

const Kind* KindOf(const uint8_t* character, bool droid = false)
{
    const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
    if (!custom || !custom->builtIn)
        return character && BattlefrontActive() && character == PlayerObject() ? (droid ? &kDroidTrooper : &kCloneTrooper)
                                                                               : nullptr;
    for (const Kind& kind : kKinds)
        if (custom->className == kind.className)
            return &kind;
    return nullptr;
}

struct Kit {
    uint32_t id = 0;
    const Kind* kind = nullptr;
    bool shooting = false;
    bool lastBlock = false;
    ULONGLONG armedAt = 0; // his specials from then on (not in the duel's intro)
    // a player's Cody: he had a controller (+0x43C, -1 for none) once since he came. The game's own test (0x1545A0)
    // also counts a player's character as the computer's while its AI drives it (the duel's intro), so it is not asked
    bool player = false;
    ULONGLONG lastShot = 0, lastFrame = 0, lastDodge = 0, rollAt = 0;
    ULONGLONG roundStart = 0, lastSummon = 0, lastBombard = 0; // his round's start, his specials' last use
    bool roundEnded = false;  // his round over (he or his opponent down), until the next one starts
    bool newRound = true;     // a round starts at his next turn: his specials' waits from then
    float rollDir[2] = {};         // the roll asked for: where the stick pointed (world x, z), 0 for none
    uint8_t* foe = nullptr;        // his opponent of the round
    uint32_t foeId = 0;
    bool summon = false;  // asked for, made in CodyFrame
    bool bombard = false; // likewise
    bool roll = false;    // likewise, toward rollDir
    bool melee = false;   // likewise (the computer's: his rifle butt)
    uint8_t* scripts = nullptr; // his script's context (CanAct again when CodyFrame starts his moves)
    ULONGLONG nextLook = 0; // the computer's: its next look for a dodge or a blow
    ULONGLONG summonAsked = 0, bombardAsked = 0, rollAsked = 0; // presses not done yet
};
std::unordered_map<uint8_t*, Kit> g_Kits;

struct Helper {
    uint8_t* object;
    uint32_t id;
    uint8_t* master;
    const char* label;
    bool enemy;          // against the player (its master the computer's)
    ULONGLONG engageAt;  // when it turns on its master's opponent (once down)
    ULONGLONG until;
};
std::vector<Helper> g_Helpers;

struct Shell {
    uint8_t* cody;
    uint32_t codyId;
    uint8_t* target;
    uint32_t targetId; // (a fighter taken away meanwhile: a match's respawn)
    ULONGLONG at;
};
std::vector<Shell> g_Shells;

// Whose shots are whose: a blaster bolt keeps its shooter's script instance (not the character), so each kit fighter's
// and each of their reinforcements' instance is noted in their turn, with the character credited (a reinforcement's
// shots count for its master).
struct Shooter {
    uint8_t* credited;
    uint32_t creditedId;
};
std::unordered_map<uint8_t*, Shooter> g_Shooters; // script instance -> credited character

uint32_t Button(uint32_t global)
{
    return *reinterpret_cast<const uint32_t*>(uintptr_t(global));
}

bool Pressed(uint32_t global)
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t, int, int, int)>(uintptr_t(kPressed))(Button(global), 1, 0, 2);
}

bool Held(uint32_t global)
{
    return reinterpret_cast<int(__cdecl*)(uint32_t, int)>(uintptr_t(kHeldFrames))(Button(global), 0) > 0;
}

bool Direction(uint32_t test, uint8_t* instance) // 0x1F5480 left, 0x1F54C0 right on [instance + 0xFC]
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t)>(uintptr_t(test))(At<uint32_t>(instance, 0xFC));
}

// A line of his, in his script's turn: the event asked as the game asks it, the line forced (customchars.h).
void Say(uint8_t* character, const char* event, const char* line)
{
    ForceNextLine(character, event, line);
    reinterpret_cast<int(__cdecl*)(const char*, const char*, int, int, int)>(uintptr_t(kSayEvent))(event, "", 1, 0, 0);
}

void Fire(const Kind& kind, uint8_t* script, uint8_t* instance)
{
    if (!reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kScriptHasTarget))())
        return;
    const float spread = reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kScriptShotSpread))() ? 0.0f : 10.0f;
    uint32_t spreadBits;
    std::memcpy(&spreadBits, &spread, 4);
    if (kind.droid) {
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t)>(uintptr_t(kDroidFireBolt))(
            instance, script, 0, spreadBits, At<uint32_t>(instance, kDroidFireSound), At<uint32_t>(instance, kDroidMuzzle));
        return;
    }
    reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t)>(uintptr_t(kCloneFireBolt))(
        instance, script, 0, spreadBits, At<uint32_t>(instance, kCloneMuzzleData), At<uint32_t>(instance, kCloneMuzzle));
    reinterpret_cast<void(__cdecl*)(uint8_t*, int)>(uintptr_t(kCloneNextMuzzle))(instance + kCloneMuzzle, 4);
    reinterpret_cast<void(__cdecl*)(uint32_t, float, int, float, int, int)>(uintptr_t(kScriptPlaySound))(
        At<uint32_t>(instance, kCloneFireSound), 100.0f, 1, 800.0f, -1, 0);
}

void Layer(Kit& kit, bool on)
{
    if (on == kit.shooting)
        return;
    if (on)
        reinterpret_cast<void(__cdecl*)(const char*, int, int, float, int, int, int, int, int)>(uintptr_t(kScriptPlayLayer))(
            kit.kind && kit.kind->droid ? "BDroid_Atk_Shooting_Loop" : kCloneShootingLoop, 0, 0, 1.0f, 5, 2, 0, 1, 2);
    else
        reinterpret_cast<void(__cdecl*)(int, int)>(uintptr_t(kScriptStopLayer))(2, 5);
    kit.shooting = on;
}

bool IsHelper(const uint8_t* c)
{
    return std::any_of(g_Helpers.begin(), g_Helpers.end(), [&](const Helper& h) { return h.object == c; });
}

// The opponent a character fights: its own (+0x3B8) when it is a standing character, else the nearest other one
// standing that is not one of the helpers.
// The game's "is that character my enemy?" (0x18F3F0, thiscall on the character's AI controller (other, flag)); true
// without a controller (a duel's).
bool IsEnemyOf(uint8_t* self, uint8_t* other)
{
    uint8_t* brain = self ? At<uint8_t*>(self, kCharacterAI) : nullptr;
    return !brain || reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, int)>(uintptr_t(0x0018F3F0))(brain, nullptr, other, 0);
}

uint8_t* OpponentOf(uint8_t* character)
{
    const auto standing = [](uint8_t* c) { return c && At<float>(c, kCharacterHealth) > 0; };
    const std::vector<uint8_t*> characters = LevelCharacters();
    uint8_t* own = At<uint8_t*>(character, kCharacterOpponent);
    if (own && own != character && standing(own) && !IsHelper(own) &&
        std::find(characters.begin(), characters.end(), own) != characters.end())
        return own;
    const float* p = Position(character);
    uint8_t* best = nullptr;
    float bestDistance = 1e30f;
    for (uint8_t* c : characters) {
        if (c == character || !standing(c) || IsHelper(c))
            continue;
        if (HvvActive() ? HvvSameSide(c, character) : !IsEnemyOf(character, c))
            continue;
        const float* q = Position(c);
        const float d = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

// Turns a character to face another where it stands (its placement, vfunc +0x1F4: rows right, up, forward, position).
void FaceToward(uint8_t* character, uint8_t* other)
{
    if (!other)
        return;
    const float* p = Position(character);
    const float* q = Position(other);
    float f[3] = { q[0] - p[0], 0, q[2] - p[2] };
    const float length = std::hypot(f[0], f[2]);
    if (length < 1)
        return;
    f[0] /= length;
    f[2] /= length;
    const float m[16] = { f[2], 0, -f[0], 0, 0, 1, 0, 0, f[0], 0, f[2], 0, p[0], p[1], p[2], 1 };
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(
        (*reinterpret_cast<void* const* const*>(character))[0x1F4 / 4])(character, nullptr, m);
}

// Plays one of a character's sequences by name, as the duel plays an intro; false without it.
bool PlaySequence(uint8_t* character, const char* name)
{
    uint8_t* record = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(kFindSequence)(character, nullptr, name);
    return record && reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
        character, nullptr, record, 0.0f, 0, 0, 1.0f);
}

// --- Their moves: sequences of the port's on their script classes (only in matches) ---
struct OwnMove {
    const char* script;
    const char* name;
    const char* animation;
    float speed;
    uint16_t handle;
};
OwnMove g_Moves[] = {
    { "CloneTrooper", "Cody_RollRight", "CTroop_Nav_RollOut_R", 1.7f, 0 },    // the roll out of cover, quicker: a dodge
    { "CloneTrooper", "Cody_SignalLeft", "ClnCom_Signal_Left", 1.6f, 0 },     // calling his clones in
    { "CloneTrooper", "Cody_SignalStrike", "ClnCom_Signal_Strike", 2.4f, 0 }, // calling the strike in (his intro's, quicker)
    { "BattleDroid", "Droid_Duck", "BDroid_Nav_KneelIn_V2", 2.2f, 0 },        // its kneel, quick: under the saber
    { "BattleDroid", "Droid_Pushaway", "BDroid_Atk_Pushaway", 1.2f, 0 },      // its shove (its attack row: AddDroidRows)
    { "BattleDroid", "Droid_Call", "Bdroid_Atk_PanicShoot", 1.4f, 0 },        // waving its arms for help
    { "BattleDroid", "Droid_Strike", "BDroid_Atk_Shooting_In", 1.0f, 0 },     // pointing its blaster: fire
};
constexpr int kMoveCount = int(std::size(g_Moves));
constexpr uint32_t kSetup = 0x006965D8;
const char kDuelMapTag[] = "DuelMap";

// As the clone's own roll (CTroop_Nav_RollOut_L, 0x3575B0): optional (and its flag 0x696537); at its first frame
// 0x38A140 (instance, class object, -1, -1), which hands the character back to its state machine when the move ends
// (without it the move played on and on).
template <int K> bool __fastcall MoveFn(uint8_t* self, void*, uint32_t, uint32_t)
{
    auto* setup = reinterpret_cast<uint8_t*>(uintptr_t(kSetup));
    OwnMove& move = g_Moves[K];
    const bool declared = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(0x001F5B20))(setup, nullptr, &move.handle, move.name, move.animation, move.speed, 0x0055FB60);
    if (declared) { // optional, required on duel maps (as Vader_Intro)
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696531)) = 1;
        *reinterpret_cast<uint8_t*>(uintptr_t(0x00696537)) = 1;
        *reinterpret_cast<const char**>(uintptr_t(0x006964B8)) = kDuelMapTag;
    }
    if (reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(0x001F62D0))(setup, nullptr)) { // first frame
        uint8_t* instance = *reinterpret_cast<uint8_t**>(uintptr_t(0x006964C0));
        reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int, int)>(uintptr_t(0x0038A140))(instance, self, -1, -1);
    }
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, const char*, uint32_t)>(uintptr_t(0x001F6C00))(
        setup, nullptr, reinterpret_cast<const char*>(uintptr_t(0x0059C07C)), 0x7D00 + K);
}

constexpr ClassSequenceFn kMoveFns[] = { &MoveFn<0>, &MoveFn<1>, &MoveFn<2>, &MoveFn<3>, &MoveFn<4>, &MoveFn<5>, &MoveFn<6> };
static_assert(std::size(kMoveFns) == kMoveCount, "one function per move");

int __cdecl MoveSequences(const char* shortName, ClassSequenceFn* fns, int max)
{
    const Kind* kind = nullptr;
    for (const Kind& k : kKinds)
        if (_stricmp(shortName, k.script) == 0 && FindCustomCharacter(k.className))
            kind = &k;
    if (!kind)
        return 0;
    int count = 0;
    for (int i = 0; i < kMoveCount && count < max; ++i)
        if (_stricmp(g_Moves[i].script, kind->script) == 0)
            fns[count++] = kMoveFns[i];
    LOG_INFO("Cody: %s's moves added to the %s class (%d)", kind->className, kind->script, count);
    return count;
}

// --- His clones rappel down ---
// The clone's own CTroop_Rappel_Loop (handle +0xFF6): at its first frame a rope from where it starts to its left hand
// (the rope manager, a singleton [0x7F2EAC], draws it: without one, or one not active ([+0xC] & 0x40000), the rope's
// add faults), no gravity, a steady slide down; on the floor its Rappel_Out lets go and gives it back to its AI.
bool RopesReady()
{
    const uint8_t* ropes = *reinterpret_cast<uint8_t* const*>(uintptr_t(0x007F2EAC));
    return ropes && (*reinterpret_cast<const uint32_t*>(ropes + 0xC) & 0x40000) != 0;
}

void CallSquad(uint8_t* cody, const Kind& kind, bool enemy, ULONGLONG now)
{
    const bool rappel = kind.rappel && RopesReady();
    int made = 0;
    for (int side = -1; side <= 1; side += 2) {
        float m[16];
        std::memcpy(m, cody + kCharacterTransform, sizeof m);
        for (int k = 0; k < 3; ++k) // beside him, a step back
            m[12 + k] += m[k] * kHelperSide * float(side) - m[8 + k] * 40.0f;
        if (rappel) // on the ground otherwise: a roof would hold them above it
            m[13] += kRappelHeight;
        else
            m[13] += 10.0f;
        std::string error;
        uint8_t* clone = SpawnHelper(kind.helper, std::string(), cody, enemy, m, error, kind.helperSkin);
        if (!clone) {
            LOG_WARN("Cody: a clone of his could not come (%s)", error.c_str());
            continue;
        }
        const bool roped = rappel && PlaySequence(clone, "CTroop_Rappel_Loop");
        if (rappel && !roped)
            LOG_WARN("Cody: his clone has no rappel here; it drops in");
        g_Helpers.push_back({ clone, At<uint32_t>(clone, 4), cody, kind.helperLabel, enemy,
            now + (roped || !kind.rappel ? kHelperEngageMs : 0), now + kHelperLifeMs });
        ++made;
    }
    LOG_INFO("Cody: %d of %s's reinforcements called in%s", made, kind.className, rappel ? ", rappelling" : "");
}

// --- The bombardment ---
// A shell is the game's IExplosion (class info 0x68E760, as a breakable or a rocket makes one): its properties at
// +0x1E0 (the effect key +0x1F0, the hit callback +0x200, the sound link +0x204, radii, lifetime, explode once), spawned
// (0xA2DE0) where it falls and set off (vfunc +0xA4): the effect, the sound, a damage sphere growing over its lifetime,
// which hands each object it reaches to the callback. The callback hurts his enemies only, modestly, through the
// game's own hit (0xA6720: a knock-down reaction from r_environment's EXPLOSION_* rows, the health change after it).
// The effects are each level's own: the first of these the level has; without one, the shell is heard, not seen.
constexpr uint32_t kExplosionClass = 0x0068E760;
constexpr const char* kShellEffects[] = { "IDS_FX_explosion_med", "IDS_FX_explosion_large06", "IDS_FX_explosion_barrel_small",
    "IDS_FX_explosion_large05", "IDS_FX_flak_explosion01", "IDS_FX_explosion_large", "IDS_FX_Homing_Rocket_Explosion",
    "IDS_FX_electric_explosion_small", "IDS_FX_flak_explosion03", "IDS_FX_flak_explosion02", "IDS_FX_dust_burst_explosion09",
    "IDS_FX_FURY_FORCESHOCK", "IDS_FX_FURY_SHOCKWAVE" }; // (the last two in every level)
struct ShellOf {
    uint8_t* owner;
    const char* damageType; // null: the shell's by distance (EXPLOSION_MEDIUM / _LIGHT)
    float damage;
};
std::unordered_map<uint8_t*, ShellOf> g_ShellOwners; // shell -> who called it, how it hurts

bool IsCharacter(uint8_t* object)
{
    return reinterpret_cast<bool(__cdecl*)(uint8_t*)>(uintptr_t(0x00089D10))(object);
}

void __cdecl ShellHit(uint8_t* shell, uint8_t* target, void*)
{
    __try {
        const auto owner = g_ShellOwners.find(shell);
        uint8_t* cody = owner == g_ShellOwners.end() ? nullptr : owner->second.owner;
        const char* type = owner == g_ShellOwners.end() ? nullptr : owner->second.damageType;
        if (!target || !IsCharacter(target) || target == cody || IsHelper(target))
            return;
        if (type && owner->second.damage <= 0) // only a look
            return;
        if (cody && (HvvActive() ? HvvSameSide(target, cody) : !IsEnemyOf(cody, target)))
            return;
        const float* centre = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(
            uintptr_t(0x000F2280))(shell, nullptr) + 0x30);
        const float* p = Position(target);
        const float d = std::hypot(p[0] - centre[0], p[2] - centre[2]);
        float damage = type ? owner->second.damage : d < 60 ? 60.0f : d < 110 ? 40.0f : 20.0f;
        const auto isA = reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint32_t)>(
            (*reinterpret_cast<void* const* const*>(target))[1]);
        if (isA(target, nullptr, 0x00249950) && At<uint8_t>(target, 0x1060)) // a Jedi blocking (as the canister)
            damage /= 2;
        const float from[4] = { centre[0], centre[1] + 720.0f, centre[2], 1.0f };
        reinterpret_cast<int(__cdecl*)(uint8_t*, uint8_t*, const char*, const float*, int, float, bool)>(uintptr_t(0x000A6720))(
            target, shell, type ? type : d < 60 ? "EXPLOSION_MEDIUM" : "EXPLOSION_LIGHT", from, 0, damage, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The two kinds of text in the properties, as the canister sets them (0x24F1C0): the effect key (+0x10) is an interned
// name (made by 0x222FA0 cdecl (out, text) into a holder of the game's that keeps it, and copied in as it is); the sound
// link (+0x24) a text the constructor made, set with its assign (0x225A10).
void AssignText(uint8_t* field, const char* text)
{
    reinterpret_cast<void(__fastcall*)(void*, void*, const char*)>(uintptr_t(0x00225A10))(field, nullptr, text);
}

bool SetEffect(uint8_t* field, const char* const* effects, size_t count)
{
    for (size_t i = 0; i < count; ++i) { // the effect manager's check (0x1B3B00): one this level has
        const char* key = effects[i];
        void* name = nullptr; // held, never released: the interned name lives on, as the canister's holder's does
        reinterpret_cast<void*(__cdecl*)(void*, const char*)>(uintptr_t(0x00222FA0))(&name, key);
        if (name && reinterpret_cast<bool(__cdecl*)(void*)>(uintptr_t(0x001B3B00))(&name)) {
            *reinterpret_cast<void**>(field) = name;
            return true;
        }
    }
    return false;
}

} // namespace

bool FireBlast(uint8_t* owner, const float at[3], const BlastLook& look)
{
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, at[0], at[1], at[2], 1 };
    uint8_t* shell = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(0x000A2DB0))(kExplosionClass);
    if (!shell)
        return false;
    uint8_t* props = shell + 0x1E0; // as its constructor left them (its texts made), the port's values set
    const bool seen = SetEffect(props + 0x10, look.effects, look.effectCount);
    *reinterpret_cast<void**>(props + 0x20) = reinterpret_cast<void*>(&ShellHit);
    AssignText(props + 0x24, look.sound);
    At<float>(props, 0x28) = 0;            // start radius
    At<float>(props, 0x2C) = look.radius;  // end radius
    At<float>(props, 0x30) = look.push;    // push
    At<float>(props, 0x34) = 60;           // damage (ours: the callback's)
    At<float>(props, 0x38) = 20;
    At<float>(props, 0x3C) = 0.4f;         // lifetime
    props[0x40] = 1;                       // explode once (removed at the end)
    g_ShellOwners[shell] = { owner, look.damageType, look.damage };
    if (look.made)
        *look.made = shell;
    if (!reinterpret_cast<bool(__cdecl*)(uint8_t*, void*, const float*, int)>(uintptr_t(0x000A2DE0))(shell, nullptr, m, 1))
        return false;
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>((*reinterpret_cast<void* const* const*>(shell))[0xA4 / 4])(shell, nullptr);
    return seen;
}

uint8_t* ShooterOf(const uint8_t* object)
{
    if (!object || g_Shooters.empty())
        return nullptr;
    __try {
        for (uint32_t at = 4; at < 0x300; at += 4) {
            const auto it = g_Shooters.find(*reinterpret_cast<uint8_t* const*>(object + at));
            if (it != g_Shooters.end() && Live(it->second.credited, it->second.creditedId))
                return it->second.credited;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

uint8_t* HelperMasterOf(const uint8_t* character)
{
    for (const Helper& h : g_Helpers)
        if (h.object == character)
            return h.master;
    return nullptr;
}

uint8_t* BlastOwner(const uint8_t* object)
{
    const auto it = g_ShellOwners.find(const_cast<uint8_t*>(object));
    return it == g_ShellOwners.end() ? nullptr : it->second.owner;
}

namespace {

void FireShell(uint8_t* cody, uint8_t* target)
{
    const float* p = Position(target);
    const ULONGLONG r = GameTime();
    const float angle = float(r % 628) / 100.0f, distance = float((r / 7) % 100) / 100.0f * kShellSpread;
    const float at[3] = { p[0] + std::cos(angle) * distance, p[1], p[2] + std::sin(angle) * distance };
    BlastLook look;
    look.effects = kShellEffects;
    look.effectCount = std::size(kShellEffects);
    const bool seen = FireBlast(cody, at, look);
    static bool noted = false;
    if (!noted) {
        noted = true;
        LOG_INFO("Cody: a shell falls (%s)", seen ? "with the level's explosion" : "the level has no explosion to show");
    }
}

// --- His helmet ---
// Cody's helmet (his select bust, his portrait) is the Phase II trooper's with a rangefinder: a stalk rising past the dome
// from a housing over his right ear. His body is the 212th's (meshes\chars\clonetrooper\hordetrooper.msh) with the two
// added to the helmet's group (headSG, skinned to the head), made at load under a name of his own, so the other clones
// keep theirs; the engine builds its animation binding as for any new body (the same skeleton). Sizes in model units,
// the texel each part takes its colour from in the 128-texel body texture (the visor's dark grey, the dome's white).
constexpr const char* kBodySource = "meshes\\chars\\clonetrooper\\hordetrooper.msh";
constexpr const char* kBody = "meshes\\chars\\clonetrooper\\hordetrooper_cody.msh";
const std::vector<MeshBox> kRangefinder = {
    // the helmet's front is at z 6 to 7.5 under the brow (y 70 to 71), its sides at x 4
    { { -3.2f, 70.5f, 4.6f }, { 3.2f, 71.1f, 7.3f }, { 6.0f, 92.0f } },     // the orange brim over the visor (the
                                                                          // painted brow's texel), out past the front
    { { 3.55f, 71.0f, 1.2f }, { 4.25f, 72.4f, 2.2f }, { 24.5f, 97.5f } },   // the rangefinder's hinge on his right
    { { 3.95f, 72.4f, 0.9f }, { 4.2f, 75.6f, 3.0f }, { 24.5f, 97.5f } },    // its viewplate, flipped up: a thin grey
                                                                          // plate standing over the dome's side
};

// His markings: the 212th's helmet (one 128x128 DXT1 texture for the whole body, the helmet unwrapped once and mirrored
// on both sides, u 0 the front's middle) gets an orange brow over the visor wrapping round the dome, and an orange
// panel by each end of the visor's top: in the 212th's own orange (its dome ring's), shaded by the plate's own
// brightness (its scratches kept), with a soft edge. Only the blocks they touch are decoded, painted and encoded
// again. Served as the texture set "_cody" of his costume (customchars.cpp), so the clones keep theirs.
constexpr const char* kHelmetSource = "meshes\\chars\\clonetrooper\\hordetrooper.stx";
constexpr const char* kHelmetTexture = "meshes\\chars\\clonetrooper\\hordetrooper_cody.stx";
constexpr const char kHelmetName[] = "hordeTrooper_cody";
constexpr size_t kStxPixels = 0x184, kStxNameAt = 0x40;
constexpr int kTextureSize = 128;

struct Point {
    float x, y;
};
const std::vector<std::vector<Point>> kMarkings = { // polygons in texels
    { { 0, 89 }, { 16, 89.5f }, { 16, 94 }, { 14, 99.5f }, { 0, 99.5f } },                                  // brow
    { { 16, 89.5f }, { 26, 90 }, { 33, 91 }, { 35, 92.5f }, { 32, 94 }, { 16, 94 } },                       // its wing
    { { 13, 99.5f }, { 16, 94 }, { 22, 94 }, { 22, 100 }, { 20, 106 }, { 17, 111 }, { 14, 110 }, { 13, 103 } }, // cheek
};

bool Inside(const std::vector<Point>& p, float x, float y)
{
    bool in = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        if ((p[i].y > y) != (p[j].y > y) && x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)
            in = !in;
    return in;
}

void Colour565(uint16_t c, float rgb[3])
{
    rgb[0] = float((c >> 11) & 31) * 255.0f / 31.0f;
    rgb[1] = float((c >> 5) & 63) * 255.0f / 63.0f;
    rgb[2] = float(c & 31) * 255.0f / 31.0f;
}

uint16_t To565(const float rgb[3])
{
    const auto q = [](float v, int bits) { return std::clamp(int(v * float((1 << bits) - 1) / 255.0f + 0.5f), 0, (1 << bits) - 1); };
    return uint16_t(q(rgb[0], 5) << 11 | q(rgb[1], 6) << 5 | q(rgb[2], 5));
}

void DecodeBlock(const uint8_t* block, float texels[16][3])
{
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    float palette[4][3];
    Colour565(c0, palette[0]);
    Colour565(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        palette[2][k] = c0 > c1 ? (2 * palette[0][k] + palette[1][k]) / 3 : (palette[0][k] + palette[1][k]) / 2;
        palette[3][k] = c0 > c1 ? (palette[0][k] + 2 * palette[1][k]) / 3 : 0;
    }
    for (int i = 0; i < 16; ++i)
        std::memcpy(texels[i], palette[(block[4 + i / 4] >> (2 * (i % 4))) & 3], sizeof texels[i]);
}

// A plain encoder: the block's darkest and brightest texels as the ends, each texel the nearest of the four.
void EncodeBlock(const float texels[16][3], uint8_t* block)
{
    const auto luma = [](const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
    int lo = 0, hi = 0;
    for (int i = 1; i < 16; ++i) {
        lo = luma(texels[i]) < luma(texels[lo]) ? i : lo;
        hi = luma(texels[i]) > luma(texels[hi]) ? i : hi;
    }
    uint16_t c0 = To565(texels[hi]), c1 = To565(texels[lo]);
    if (c0 < c1)
        std::swap(c0, c1);
    float palette[4][3];
    Colour565(c0, palette[0]);
    Colour565(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        palette[2][k] = (2 * palette[0][k] + palette[1][k]) / 3;
        palette[3][k] = (palette[0][k] + 2 * palette[1][k]) / 3;
    }
    block[0] = uint8_t(c0);
    block[1] = uint8_t(c0 >> 8);
    block[2] = uint8_t(c1);
    block[3] = uint8_t(c1 >> 8);
    for (int row = 0; row < 4; ++row) {
        uint8_t bits = 0;
        for (int col = 0; col < 4; ++col) {
            const float* t = texels[row * 4 + col];
            int best = 0;
            float bestDistance = 1e30f;
            for (int p = 0; p < (c0 == c1 ? 1 : 4); ++p) {
                float d = 0;
                for (int k = 0; k < 3; ++k)
                    d += (t[k] - palette[p][k]) * (t[k] - palette[p][k]);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = p;
                }
            }
            bits = uint8_t(bits | best << (2 * col));
        }
        block[4 + row] = bits;
    }
}

bool PaintHelmet(std::vector<uint8_t>& data)
{
    constexpr int blocks = kTextureSize / 4;
    if (data.size() < kStxPixels + size_t(blocks * blocks * 8) || std::memcmp(data.data(), "STX", 4) != 0 || data[8] != 3)
        return false;
    std::vector<float> image(size_t(kTextureSize * kTextureSize * 3));
    const auto at = [&](int x, int y) { return &image[size_t((y * kTextureSize + x) * 3)]; };
    for (int b = 0; b < blocks * blocks; ++b) {
        float texels[16][3];
        DecodeBlock(&data[kStxPixels + size_t(b) * 8], texels);
        for (int i = 0; i < 16; ++i)
            std::memcpy(at((b % blocks) * 4 + i % 4, (b / blocks) * 4 + i / 4), texels[i], sizeof texels[i]);
    }
    const auto luma = [](const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
    float orange[3] = {}, white = 0;
    int oranges = 0, whites = 0;
    for (int y = 79; y <= 84; ++y) // the dome's ring
        for (int x = 8; x <= 36; ++x) {
            const float* c = at(x, y);
            if (c[0] > 170 && c[1] > 95 && c[1] < 190 && c[2] < 110) {
                for (int k = 0; k < 3; ++k)
                    orange[k] += c[k];
                ++oranges;
            }
        }
    for (int y = 87; y <= 93; ++y) // the dome's plate
        for (int x = 16; x <= 36; ++x, ++whites)
            white += luma(at(x, y));
    if (!oranges || !whites)
        return false;
    for (float& o : orange)
        o /= float(oranges);
    white /= float(whites);
    std::vector<uint8_t> mark(size_t(kTextureSize * kTextureSize), 0); // 2 painted, 1 its edge
    for (int y = 0; y < kTextureSize; ++y)
        for (int x = 0; x < kTextureSize; ++x)
            for (const auto& p : kMarkings)
                if (Inside(p, float(x) + 0.5f, float(y) + 0.5f))
                    mark[size_t(y * kTextureSize + x)] = 2;
    const auto painted = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < kTextureSize && y < kTextureSize && mark[size_t(y * kTextureSize + x)] == 2;
    };
    for (int y = 0; y < kTextureSize; ++y)
        for (int x = 0; x < kTextureSize; ++x)
            if (!mark[size_t(y * kTextureSize + x)] && (painted(x - 1, y) || painted(x + 1, y) || painted(x, y - 1) || painted(x, y + 1)))
                mark[size_t(y * kTextureSize + x)] = 1;
    std::vector<bool> touched(size_t(blocks * blocks), false);
    for (int y = 0; y < kTextureSize; ++y)
        for (int x = 0; x < kTextureSize; ++x) {
            const uint8_t m = mark[size_t(y * kTextureSize + x)];
            if (!m)
                continue;
            float* c = at(x, y);
            const float shade = std::clamp(luma(c) / white, 0.0f, 1.15f);
            for (int k = 0; k < 3; ++k) {
                const float paint = std::min(255.0f, orange[k] * shade);
                c[k] = m == 2 ? paint : (c[k] + paint) / 2;
            }
            touched[size_t((y / 4) * blocks + x / 4)] = true;
        }
    for (int b = 0; b < blocks * blocks; ++b) {
        if (!touched[size_t(b)])
            continue;
        float texels[16][3];
        for (int i = 0; i < 16; ++i)
            std::memcpy(texels[i], at((b % blocks) * 4 + i % 4, (b / blocks) * 4 + i / 4), sizeof texels[i]);
        EncodeBlock(texels, &data[kStxPixels + size_t(b) * 8]);
    }
    std::memset(&data[kStxNameAt], 0, 32);
    std::memcpy(&data[kStxNameAt], kHelmetName, sizeof kHelmetName - 1);
    return true;
}

bool HelmetGenerator(const std::string& lowerName, std::vector<uint8_t>& data)
{
    return lowerName == kHelmetTexture && ReadDiscResource(kHelmetSource, data) && PaintHelmet(data);
}

bool BodyGenerator(const std::string& lowerName, std::vector<uint8_t>& data)
{
    if (lowerName != kBody || !ReadDiscResource(kBodySource, data))
        return false;
    std::string report;
    const bool made = AddMeshBoxes(data, "headSG", kRangefinder, 128, report);
    if (made)
        LOG_INFO("Cody: his helmet's rangefinder made (%s)", report.c_str());
    else
        LOG_WARN("Cody: his helmet could not be made (%s)", report.c_str());
    return made;
}

// Whether the script's character may start something now: not while its current sequence (its state's record, +0xC
// its name) is one it is caught in (a hit's reaction R1_.. to R4_.., a finisher's R_.., a fall, being gripped,
// stunned or shocked, dying, cut) or one of his own (a roll, a signal, his intro or outro). (The record's +0x30 bit 1,
// which the decision test 0x173D70 reads, is clear while running, strafing or idle and set in those: no help.)
uint8_t* ScriptContext()
{
    return *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacterContext));
}

// A character's current sequence by its script's context (kept from its turn, the context outlives it: +0x68 the
// character, +0x70 its state, vfunc 1 its current record, +0xC the name).
const char* CurrentSequence(uint8_t* scripts)
{
    uint8_t* state = scripts ? At<uint8_t*>(scripts, 0x70) : nullptr;
    if (!state)
        return nullptr;
    uint8_t* record = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*)>(
        (*reinterpret_cast<void* const* const*>(state))[1])(state, nullptr);
    return record ? At<const char*>(record, 0xC) : nullptr;
}

bool CanAct(uint8_t* scripts)
{
    const char* name = CurrentSequence(scripts);
    if (!name)
        return true;
    if (name[0] == 'R' && ((name[1] >= '1' && name[1] <= '9' && name[2] == '_') || name[1] == '_'))
        return false;
    static const char* const kCaught[] = { "Fall", "ForceGrasp", "Stun", "Shock", "Death", "Dismember", "KD_", "HitRe",
        "Rappel", "Cody_", "Droid_", "Grab", "_GR", "Kill_", "Launch", "Juggle", "WS_" };
    for (const char* part : kCaught)
        if (std::strstr(name, part))
            return false;
    return true;
}

bool Guarded(const Kit& kit, ULONGLONG now)
{
    return kit.rollAt && now >= kit.rollAt + kRollGuardFromMs && now < kit.rollAt + kRollGuardToMs;
}

// No reaction to a hit in his roll's guard (CodyDamage takes its damage away): dealt with, as the game's own ways out.
// Otherwise the hit as on anyone not holding the block button (a player's controller, +0x43C, out of it for the call).
bool CodyReaction(uint8_t* character, void* bump, void* collision, void* sequence, ReactionCallFn original)
{
    const auto kit = g_Kits.find(character);
    if (kit == g_Kits.end())
        return original(character, nullptr, bump, collision, sequence);
    if (Guarded(kit->second, GameTime()))
        return true;
    const int32_t port = At<int32_t>(character, kCharacterPort);
    At<int32_t>(character, kCharacterPort) = -1;
    const bool dealt = original(character, nullptr, bump, collision, sequence);
    At<int32_t>(character, kCharacterPort) = port;
    return dealt;
}

// The battle droid's script turn: its StateManager (0x3383B0, the clone's twin, same first bytes), the kit after it.
constexpr uint32_t kDroidStateManager = 0x003383B0;
constexpr uint8_t kStateManagerBytes[] = { 0x56, 0x57, 0x8B, 0x3D, 0xC0, 0x64, 0x69, 0x00 }; // push esi; push edi; mov edi, [0x6964C0]
using StateManagerFn = void(__fastcall*)(uint8_t*, void*, uint32_t, uint32_t);
StateManagerFn g_OriginalDroidStateManager = nullptr;

void __fastcall DroidStateManagerHook(uint8_t* script, void*, uint32_t a, uint32_t b)
{
    g_OriginalDroidStateManager(script, nullptr, a, b);
    const uint8_t* context = *reinterpret_cast<uint8_t* const*>(uintptr_t(kScriptCharacterContext));
    uint8_t* instance = *reinterpret_cast<uint8_t**>(uintptr_t(0x006964C0));
    if (context)
        CloneKitFrame(script, instance, *reinterpret_cast<uint8_t* const*>(context + 0x68), true);
}

uint8_t* Trampoline(uint32_t address, const uint8_t* bytes, uint32_t length)
{
    uint8_t* s = AllocStub(length + 5);
    std::memcpy(s, bytes, length);
    s[length] = 0xE9;
    const int32_t rel = int32_t(address + length) - int32_t(uintptr_t(s) + length + 5);
    std::memcpy(s + length + 1, &rel, 4);
    return s;
}

// The droid's shove as the droid commander's own move (Droid_Pushaway): the attack table's row for the droid's own
// (BDroid_Atk_Pushaway: damage, range, knockback) under its name too.
constexpr const char* kDroidTable = "meshes\\chars\\common\\a_battledroid.csv";

void AddDroidRows(std::vector<uint8_t>& data)
{
    const std::string text(data.begin(), data.end());
    const std::string key = "BDroid_Atk_Pushaway,";
    size_t at = text.find(key);
    if (at == std::string::npos || (at > 0 && text[at - 1] != '\n')) {
        LOG_WARN("Cody: the battle droid's attack table has no push-away row");
        return;
    }
    const size_t end = text.find('\n', at);
    std::string row = "Droid_Pushaway" + text.substr(at + key.size() - 1, (end == std::string::npos ? text.size() : end) - at - key.size() + 1);
    if (!row.empty() && row.back() == '\r')
        row.pop_back();
    if (!data.empty() && data.back() != '\n')
        data.insert(data.end(), { '\r', '\n' });
    row += "\r\n";
    data.insert(data.end(), row.begin(), row.end());
}

} // namespace

void InstallCody()
{
    g_Kits.clear();
    g_Helpers.clear();
    g_Shells.clear();
    g_ShellOwners.clear();
    g_Shooters.clear();
    for (OwnMove& move : g_Moves)
        move.handle = 0;
    if (std::none_of(std::begin(kKinds), std::end(kKinds), [](const Kind& k) { return FindCustomCharacter(k.className) != nullptr; }))
        return;
    AddClassSequences(&MoveSequences);
    if (FindCustomCharacter("IDroidCommander")) {
        if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kDroidStateManager)), kStateManagerBytes, sizeof kStateManagerBytes) == 0) {
            static uint8_t* stub = Trampoline(kDroidStateManager, kStateManagerBytes, sizeof kStateManagerBytes);
            g_OriginalDroidStateManager = reinterpret_cast<StateManagerFn>(stub);
            PatchJump(kDroidStateManager, reinterpret_cast<const void*>(&DroidStateManagerHook));
        } else {
            LOG_WARN("Cody: unexpected code at the battle droid's turn; the droid commander has no kit");
        }
        RegisterResourcePatch(kDroidTable, &AddDroidRows);
    }
    HookReactions(&CodyReaction);
    RegisterResourceGenerator(&BodyGenerator);
    RegisterResourceGenerator(&HelmetGenerator);
}

void CloneKitFrame(uint8_t* script, uint8_t* instance, uint8_t* character, bool droid)
{
    if (instance && character) {
        if (IsHelper(character)) {
            for (const Helper& h : g_Helpers)
                if (h.object == character)
                    g_Shooters[instance] = { h.master, At<uint32_t>(h.master, 4) };
        } else if (KindOf(character, droid)) {
            g_Shooters[instance] = { character, At<uint32_t>(character, 4) };
        }
    }
    const Kind* kind = KindOf(character, droid);
    if (!kind || !instance)
        return;
    Kit& kit = g_Kits[character];
    const uint32_t id = At<uint32_t>(character, 4);
    const ULONGLONG now = GameTime();
    if (kit.id != id) { // a new Cody where an old one was
        kit = Kit{};
        kit.id = id;
        kit.kind = kind;
        kit.armedAt = HvvActive() ? now : now + kArmAfterMs; // (the computer's: not in the duel's intro)
    }
    if (At<int32_t>(character, 0x43C) >= 0)
        kit.player = true;
    float& meter = At<float>(character, kCharacterMeter);
    if (kit.newRound) { // a round starts (CodyFrame): his specials wait from now, his meter full
        kit.newRound = false;
        kit.roundStart = now;
        kit.lastSummon = kit.lastBombard = 0;
        meter = At<float>(character, kCharacterMeterMost);
    }
    if (kit.lastFrame) // the meter refills
        meter = std::min(At<float>(character, kCharacterMeterMost), meter + kMeterPerSecond * float(now - kit.lastFrame) / 1000.0f);
    kit.lastFrame = now;
    if (At<float>(character, kCharacterHealth) <= 0 || (HvvActive() && GetHvvState().intro)) {
        Layer(kit, false);
        return;
    }
    // a player's Cody acts once the game stops driving him (the duel's intro); the computer's after its intro
    if (kit.player ? reinterpret_cast<bool(__cdecl*)()>(uintptr_t(kIsComputer))() : now < kit.armedAt) {
        Layer(kit, false);
        return;
    }
    // nothing new while he is gripped, down, hit, rolling or falling
    kit.scripts = ScriptContext();
    const bool free = CanAct(kit.scripts);
    const bool squadOut = std::any_of(g_Helpers.begin(), g_Helpers.end(), [&](const Helper& h) { return h.master == character; });
    const bool specials = now >= kit.roundStart + kSpecialsAfterMs;
    const bool canSummon = kind->call && specials && !squadOut && !kit.summon && meter >= kSummonCost &&
        (!kit.lastSummon || now >= kit.lastSummon + kSummonWaitMs);
    const bool canBombard = kind->strike && specials && !kit.bombard && meter >= kBombardCost &&
        (!kit.lastBombard || now >= kit.lastBombard + kBombardWaitMs);
    if (!kit.player) { // shoots as any clone; up close he rolls away or strikes; his specials when his opponent is near
        uint8_t* opponent = OpponentOf(character);
        if (!opponent || !free || kit.summon || kit.bombard || kit.roll || kit.melee)
            return;
        const float* p = Position(character);
        const float* q = Position(opponent);
        const float apart = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (apart < kComputerCloseIn && now >= kit.nextLook && now - kit.lastDodge >= kDodgeWaitMs && meter >= kRollCost) {
            kit.nextLook = now + kComputerLookMs;
            const uint32_t dice = Dice() % 100;
            if (dice < 45) { // away from the saber, a little aside
                const float side = dice % 2 ? 0.6f : -0.6f;
                kit.rollDir[0] = p[0] - q[0] + (q[2] - p[2]) * side;
                kit.rollDir[1] = p[2] - q[2] - (q[0] - p[0]) * side;
                kit.lastDodge = now;
                meter -= kRollCost;
                kit.roll = true;
            } else if (dice < 80 && apart < kComputerStrikeWithin) {
                kit.melee = true;
            }
            return;
        }
        if (apart < kComputerCallsWithin && canSummon) {
            meter -= kSummonCost;
            kit.lastSummon = now;
            kit.summon = true;
            Say(character, kind->event, kind->callLine);
        } else if (apart > 250 && apart < kComputerCallsWithin && canBombard && meter >= kBombardCost + 100) {
            meter -= kBombardCost;
            kit.lastBombard = now;
            kit.bombard = true;
            Say(character, kind->event, kind->strikeLine);
        }
        return;
    }
    // his presses, kept a moment (kPressKeepMs) when he cannot act yet: done as soon as he can, as the game's own
    // moves take a press made just before
    if (Pressed(kButtonPush))
        kit.summonAsked = now;
    if (Pressed(kButtonCritical)) {
        if (kind->trooper)
            BattlefrontHeroRequest(); // a trooper's B: its hero, when its points are enough
        else
            kit.bombardAsked = now;
    }
    const bool dodge = Held(kButtonDodge);
    if (dodge && !kit.lastBlock) { // the roll: where the stick points on the screen at the press
        kit.rollAsked = now;
        kit.rollDir[0] = kit.rollDir[1] = 0;
        float x = 0, y = 0, view[16], fov = 0;
        if (input::LeftStick(At<int32_t>(character, 0x43C), x, y) && GameRenderedView(view, fov) && std::hypot(x, y) > 0.3f) {
            kit.rollDir[0] = x * view[0] + y * view[2]; // the camera's right (the view's first column) and forward
            kit.rollDir[1] = x * view[8] + y * view[10];
        }
    }
    kit.lastBlock = dodge;
    const auto fresh = [&](ULONGLONG& asked) {
        const bool yes = asked && now - asked <= kPressKeepMs;
        if (yes)
            asked = 0;
        return yes;
    };
    if (!free) {
        Layer(kit, false);
        return;
    }
    // his clones (one squad at a time), the bombardment
    if (kit.summonAsked && canSummon && fresh(kit.summonAsked)) {
        meter -= kSummonCost;
        kit.lastSummon = now;
        kit.summon = true;
        Say(character, kind->event, kind->callLine);
    }
    if (kit.bombardAsked && canBombard && fresh(kit.bombardAsked)) {
        meter -= kBombardCost;
        kit.lastBombard = now;
        kit.bombard = true;
        Say(character, kind->event, kind->strikeLine);
    }
    if (kind->playerMelee && Pressed(kButtonFast) && !kit.melee) // the droid's shove (the clone's rifle butt is its own)
        kit.melee = true;
    // the roll, where the stick pointed (CodyFrame)
    if (kit.rollAsked && !kit.roll && now - kit.lastDodge >= kDodgeWaitMs && meter >= kind->rollCost && fresh(kit.rollAsked)) {
        kit.lastDodge = now;
        meter -= kind->rollCost;
        kit.roll = true;
    }
    // the blaster
    if (!Held(kButtonHeavy) || meter < kind->shotCost) {
        Layer(kit, false);
        return;
    }
    if (!kit.shooting) {
        Layer(kit, true);
        kit.lastShot = now - kShotIntervalMs + 120; // the first bolt a few frames into the layer
    }
    if (now - kit.lastShot >= kShotIntervalMs) {
        kit.lastShot = now;
        meter -= kind->shotCost;
        Fire(*kind, script, instance);
    }
}

namespace {

// A roll: the way the stick points on the screen, else away from his opponent. He is turned for the clone's roll out
// of cover to go there (his placement, vfunc +0x1F4: rows right, up, forward, position; measured: the roll goes along
// his forward row). While it lasts no hit lands on him (CodyReaction and CodyDamage).
void Roll(uint8_t* cody, Kit& kit, float dx, float dz, ULONGLONG now)
{
    if (!kit.kind->dodgeTurns) { // a duck where it stands
        if (PlaySequence(cody, kit.kind->dodge)) {
            kit.rollAt = now;
            LOG_INFO("Cody: %s ducks", kit.kind->className);
        }
        return;
    }
    if (std::hypot(dx, dz) < 0.01f)
        if (uint8_t* opponent = OpponentOf(cody)) {
            const float* p = Position(cody);
            const float* q = Position(opponent);
            dx = p[0] - q[0];
            dz = p[2] - q[2];
        }
    const float length = std::hypot(dx, dz);
    if (length > 0.01f) {
        dx /= length;
        dz /= length;
        const float* p = Position(cody);
        const float m[16] = { dz, 0, -dx, 0, 0, 1, 0, 0, dx, 0, dz, 0, p[0], p[1], p[2], 1 };
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const float*)>(
            (*reinterpret_cast<void* const* const*>(cody))[0x1F4 / 4])(cody, nullptr, m);
    }
    if (!PlaySequence(cody, kit.kind->dodge))
        return;
    kit.rollAt = now;
    LOG_INFO("Cody: rolls (%.2f, %.2f)", dx, dz);
}

// The round is over when his opponent of the round fell (a duel brings both back for the next).
bool RoundOver(Kit& kit)
{
    return kit.foe && (!Live(kit.foe, kit.foeId) || At<float>(kit.foe, kCharacterHealth) <= 0);
}

} // namespace

void CodyFrame()
{
    if (g_Kits.empty() && g_Helpers.empty() && g_Shells.empty())
        return;
    const ULONGLONG now = GameTime();
    // his clones: down, they go for his opponent; their time up, his or the round's, they fall as clones do (the
    // character's instant kill, vfunc +0xC8: its death, the game takes the body away; removing the object at once,
    // while the level's lists held it, left a hole in one: 0x16B3C7)
    for (auto it = g_Helpers.begin(); it != g_Helpers.end();) {
        const auto kit = g_Kits.find(it->master);
        const bool masterGone = kit == g_Kits.end() || !Live(it->master, kit->second.id) ||
            At<float>(it->master, kCharacterHealth) <= 0 || RoundOver(kit->second);
        if (!Live(it->object, it->id)) {
            it = g_Helpers.erase(it);
            continue;
        }
        if (now >= it->until || masterGone) {
            LOG_INFO("Cody: a clone of his falls (%s)", now >= it->until ? "his time" : "the round or Cody over");
            if (At<float>(it->object, kCharacterHealth) > 0)
                reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint32_t)>(
                    (*reinterpret_cast<void* const* const*>(it->object))[0xC8 / 4])(it->object, nullptr, 0);
            it = g_Helpers.erase(it);
            continue;
        }
        if (it->engageAt && now >= it->engageAt) {
            it->engageAt = 0;
            if (uint8_t* opponent = OpponentOf(it->master))
                if (uint8_t* brain = At<uint8_t*>(it->object, kCharacterAI))
                    reinterpret_cast<void(__fastcall*)(uint8_t*, void*, uint8_t*)>(uintptr_t(kAISetTarget))(brain, nullptr, opponent);
        }
        ++it;
    }
    // the shells falling
    for (auto it = g_Shells.begin(); it != g_Shells.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        if (Live(it->cody, it->codyId) && Live(it->target, it->targetId) && At<float>(it->target, kCharacterHealth) > 0)
            FireShell(it->cody, it->target);
        it = g_Shells.erase(it);
    }
    for (auto it = g_Kits.begin(); it != g_Kits.end();) {
        uint8_t* cody = it->first;
        Kit& kit = it->second;
        if (!Live(cody, kit.id)) {
            it = g_Kits.erase(it);
            continue;
        }
        const bool standing = At<float>(cody, kCharacterHealth) > 0;
        // a round ends with him or his opponent down; the next starts when both stand again
        if (!standing || RoundOver(kit)) {
            kit.roundEnded = true;
        } else if (kit.roundEnded) {
            kit.roundEnded = false;
            kit.newRound = true;
        }
        // his opponent of the round (to know its end), once one stands
        if (standing && (!kit.foe || RoundOver(kit)))
            if (uint8_t* foe = OpponentOf(cody); foe && foe != kit.foe) {
                kit.foe = foe;
                kit.foeId = At<uint32_t>(foe, 4);
            }
        // what he asked for in his script's turn, if he still can (a grab or a hit may have come in between)
        const bool canAct = standing && kit.scripts && At<uint8_t*>(kit.scripts, 0x68) == cody && CanAct(kit.scripts);
        if (kit.summon) {
            kit.summon = false;
            if (canAct) {
                PlaySequence(cody, kit.kind->call); // he waves them in
                CallSquad(cody, *kit.kind, !kit.player, now);
            }
        }
        if (kit.bombard) {
            kit.bombard = false;
            if (uint8_t* target = canAct ? OpponentOf(cody) : nullptr) {
                PlaySequence(cody, kit.kind->strike); // the strike signal
                for (int i = 0; i < kShells; ++i)
                    g_Shells.push_back({ cody, kit.id, target, At<uint32_t>(target, 4), now + kFirstShellMs + ULONGLONG(i) * kShellGapMs });
                LOG_INFO("Cody: bombardment on %p", target);
            }
        }
        if (kit.roll) {
            if (canAct)
                Roll(cody, kit, kit.rollDir[0], kit.rollDir[1], now);
            kit.roll = false;
        }
        if (kit.melee) { // the clone's own rifle butt, at his opponent
            kit.melee = false;
            if (canAct) {
                FaceToward(cody, OpponentOf(cody));
                PlaySequence(cody, kit.kind->melee);
            }
        }
        ++it;
    }
}

bool KeepsLimbs(const uint8_t* character)
{
    return KindOf(character) != nullptr;
}

std::vector<CodyHelper> CodyHelpers()
{
    std::vector<CodyHelper> out;
    for (const Helper& h : g_Helpers)
        if (Live(h.object, h.id))
            out.push_back({ h.object, h.master, h.enemy, h.label });
    return out;
}

float CodyDamage(uint8_t* character, float change)
{
    const auto kit = g_Kits.find(character);
    if (change >= 0 || kit == g_Kits.end())
        return change;
    if (Guarded(kit->second, GameTime()))
        return 0; // rolling: out of the way
    if (At<float>(character, kCharacterHealth) <= 0)
        return change; // already down (the game's own end of him)
    // a blow the game deals a trooper or a droid as a kill (its one-hit reactions: -1000) counts as a hard one
    if (kit->second.kind && kit->second.kind->trooper)
        return change; // a Battlefront trooper's health is the match's (hvv.cpp)
    return std::max(change, -kHardestHit) * kDamageTaken;
}

} // namespace swrots::game
