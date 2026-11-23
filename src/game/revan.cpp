// Other duelists' moves for custom characters on Serra Keto's class (see revan.h).
//
// The game's own per-character sequence substitution (0x173AB0 cdecl (A, B), in a character's script turn: when the
// character's script asks for A, B plays; each class's Init makes its own, e.g. Count Dooku's grapple throws instead of
// the generic ones) gives such a character other moves while Serra keeps hers:
//   - Lightning=1: ForceStun (+0x120C), ForceStun_Alt (+0x120E) and ForceGrasp_Stun (+0x11D2), which Serra's rules
//     offer on the stun button, become ForceLightning (+0x11F0), ForceLightning_Alt (+0x11F2) and ForceGrasp_Lightning
//     (+0x11D0), every Jedi class's own (the lightning network is the IJedi's); its lightning enabled (inst +0x2DD).
//   - Grapples=IDooku: her grab (Serra_Grab_Attempt, +0x17C6) becomes the generic one (Grab_Attempt +0x1120, its hold
//     Grab_GB_Human), whose throws (Grab_KD_GB_Human +0x112A, Grab_Launch_GB_Human +0x112C) become Dooku's knock-down
//     and launch, made here as sequences of the port's (Dooku's own, 0x371840 / 0x371A00, write his script instance's
//     fields past the end of Serra's, so they are written out again: their animations, events and frame codes, his
//     palm blast from the port's own effect slots). Their victims play Dooku_Grab_KD_GR / _Launch_GR, which every class
//     has.
//   - ForceStorm=1: block held + stun/lightning (LT + Black; Shift + R): it raises a hand and lightning strikes the
//     enemies near it (the game's lightning impact and Force shock effects, "Electrified" damage: every class's hit
//     reaction), for kStormCost of its Force; the computer's uses it too.
// Its event tree gets Dooku's lightning and grapple sounds (s_serra: entries she has not, keyed by sequences she never
// plays and his grapple animations), its attack table his grapple rows for the computer.

#include "game/revan.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "core/patch.h"
#include "game/characters.h"
#include "game/classsetup.h"
#include "game/cody.h"
#include "game/customchars.h"
#include "game/devmap.h"
#include "game/eventtree.h"
#include "game/hvv.h"
#include "game/release.h"
#include "game/resources.h"

namespace swrots::game {

namespace {

static GAME_VA(kScriptCharacterContext, 0x006943CC); // +0x68: the character whose script runs
static GAME_VA(kScriptInstance, 0x006964C0);
static GAME_VA(kSetup, 0x006965D8);
static GAME_VA(kSerraRulesCall, 0x00479228);          // call 0x479240: cdecl (instance, class object)
static GAME_VA(kSerraRules, 0x00479240);
static GAME_VA(kSubstitute, 0x00173AB0);              // cdecl (record A, record B)
static GAME_VA(kRecordOf, 0x001F53E0);                // cdecl (handle) -> the class's record
static GAME_VA(kHeldFrames, 0x00177FF0);
static GAME_VA(kPressed, 0x00178070);
static GAME_VA(kButtonBlock, 0x006964E4);
static GAME_VA(kButtonStun, 0x006964F4);
constexpr uint32_t kCharacterHealth = 0x130, kCharacterTransform = 0x150, kCharacterForce = 0xA40;
static GAME_VA(kObjectById, 0x000A31F0);
static GAME_VA(kPlaySequence, 0x150E30);
static GAME_VA(kFindSequence, 0x150D60);

// Serra's class object's handles (the shared Jedi header and her own)
constexpr uint32_t kForceStun = 0x120C, kForceStunAlt = 0x120E, kForceGraspStun = 0x11D2;
constexpr uint32_t kForceLightning = 0x11F0, kForceLightningAlt = 0x11F2, kForceGraspLightning = 0x11D0;
constexpr uint32_t kGrabAttempt = 0x1120, kSerraGrabAttempt = 0x17C6, kGrabKD = 0x112A, kGrabLaunch = 0x112C;
constexpr uint32_t kInstanceLightningOn = 0x2DD; // byte: its lightning allowed

// the storm
constexpr float kStormCost = 500, kStormRange = 600;
constexpr ULONGLONG kStormWaitMs = 15000, kFirstStrikeMs = 550, kStrikeGapMs = 260;
constexpr int kStrikes = 5;
constexpr float kStrikeDamage = 55;
// one-shot effects only: the lightning's own impact (IDS_FX_Force_lightning_impact) and static shock loop as long as
// the lightning lasts, and a blast that ends does not stop them (they stayed in the air)
constexpr const char* kStrikeEffects[] = { "IDS_FX_Valor_Palm_Blast", "IDS_FX_electric_explosion_small",
    "IDS_FX_Force_Stunned" };
// the strike's column: small electric explosions (the lightning's own impact loops on after its object is gone), the palm blast where a level lacks them
constexpr const char* kColumnEffects[] = { "IDS_FX_electric_explosion_small", "IDS_FX_Valor_Palm_Blast" };
constexpr ULONGLONG kSparkMs = 450; // a column's piece is taken away after this long
constexpr const char* kStrikeGroundEffects[] = { "IDS_FX_FURY_FORCESHOCK", "IDS_FX_Valor_FORCESHOCK", "IDS_FX_FURY_SHOCKWAVE" };

template <typename T> T& At(uint8_t* p, uint32_t offset)
{
    return *reinterpret_cast<T*>(p + offset);
}

uint8_t* ScriptCharacter()
{
    uint8_t* context = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptCharacterContext));
    return context ? At<uint8_t*>(context, 0x68) : nullptr;
}

bool Live(uint8_t* object, uint32_t id)
{
    return object && reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id) == object;
}

const float* Position(uint8_t* character)
{
    return reinterpret_cast<const float*>(character + kCharacterTransform + 48);
}

uint32_t Button(uint32_t global)
{
    return *reinterpret_cast<const uint32_t*>(uintptr_t(global));
}

bool Held(uint32_t global)
{
    return reinterpret_cast<int(__cdecl*)(uint32_t, int)>(uintptr_t(kHeldFrames))(Button(global), 0) > 0;
}

bool Pressed(uint32_t global)
{
    return reinterpret_cast<bool(__cdecl*)(uint32_t, int, int, int)>(uintptr_t(kPressed))(Button(global), 1, 0, 2);
}

uint8_t* RecordOf(uint16_t handle)
{
    return handle ? reinterpret_cast<uint8_t*(__cdecl*)(int)>(uintptr_t(kRecordOf))(handle) : nullptr;
}

bool PlaySequence(uint8_t* character, const char* name)
{
    uint8_t* record = reinterpret_cast<uint8_t*(__fastcall*)(uint8_t*, void*, const char*)>(kFindSequence)(character, nullptr, name);
    return record && reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, float, int, int, float)>(kPlaySequence)(
        character, nullptr, record, 0.0f, 0, 0, 1.0f);
}

bool Wants(const CustomCharacter& c)
{
    return _stricmp(c.base.c_str(), "ISerra") == 0 && (c.lightning || !c.grapples.empty() || c.forceStorm);
}

bool DookuGrapples(const CustomCharacter& c)
{
    return _stricmp(c.base.c_str(), "ISerra") == 0 && _stricmp(c.grapples.c_str(), "IDooku") == 0;
}

// --- Per character ---
struct Fighter {
    uint32_t id = 0;
    bool set = false;
    uint32_t palmBlast = 0, launchBlast = 0; // the palm blast effect slots (as Dooku's instance +0x434 / +0x438)
    bool player = false;
    ULONGLONG lastStorm = 0, nextLook = 0;
    bool storm = false;                      // asked for in its turn, made in RevanFrame
    ULONGLONG armedAt = 0;                   // the storm from then on (not in a duel's intro or a round's start)
    bool roundEnded = false;
};
constexpr ULONGLONG kArmAfterMs = 8000;
std::unordered_map<uint8_t*, Fighter> g_Fighters; // node-based: the slots keep their addresses

// A strike comes down as a column of lightning (the impact effect from high above to the ground, a step every
// kBoltStepMs), then hits where it lands.
struct Strike {
    uint8_t* owner;
    uint32_t ownerId;
    uint8_t* target;
    uint32_t targetId;
    ULONGLONG at;
    int step = 0;
    float where[3] = {};
};
constexpr int kBoltSteps = 6;
constexpr float kBoltHeight = 420;
constexpr ULONGLONG kBoltStepMs = 30;
std::vector<Strike> g_Strikes;
struct Spark {
    uint8_t* object;
    uint32_t id;
    ULONGLONG until;
};
std::vector<Spark> g_Sparks;

// --- Dooku's throws, written out again ---
uint16_t g_KDHandle = 0, g_LaunchHandle = 0, g_StormHandle = 0;
const char kDuelMapTag[] = "DuelMap";

template <typename R = void, typename... A> R Call(uint32_t address, A... args)
{
    return reinterpret_cast<R(__cdecl*)(A...)>(uintptr_t(address))(args...);
}

bool SetupEvent(uint32_t test)
{
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*)>(uintptr_t(test))(reinterpret_cast<uint8_t*>(uintptr_t(kSetup)), nullptr);
}

bool Declare(uint16_t* handle, const char* name, const char* animation, float speed)
{
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint16_t*, const char*, const char*, float, uint32_t)>(
        uintptr_t(Va(0x001F5B20)))(reinterpret_cast<uint8_t*>(uintptr_t(kSetup)), nullptr, handle, name, animation, speed, Va(0x0055FB60));
}

bool Finish(uint32_t line)
{
    return reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, const char*, uint32_t)>(uintptr_t(Va(0x001F6C00)))(
        reinterpret_cast<uint8_t*>(uintptr_t(kSetup)), nullptr, reinterpret_cast<const char*>(uintptr_t(Va(0x0059C07C))), line);
}

void GrappleFlags()
{
    for (uint32_t flag : { 0x696536u, 0x696531u, 0x696541u, 0x696565u })
        *reinterpret_cast<uint8_t*>(uintptr_t(flag)) = 1;
}

uint32_t* PalmSlot(bool launch)
{
    const auto it = g_Fighters.find(ScriptCharacter());
    if (it == g_Fighters.end() || !it->second.set)
        return nullptr;
    return launch ? &it->second.launchBlast : &it->second.palmBlast;
}

void Blast(bool launch, int kind)
{
    if (uint32_t* slot = PalmSlot(launch))
        Call(Va(0x001760B0), slot, kind);
}

// Dooku_Grab_KD_GB (0x371840): the knock-down throw
bool __fastcall DookuGrabKD(uint8_t* self, void*, uint32_t, uint32_t)
{
    uint8_t* inst = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptInstance));
    if (Declare(&g_KDHandle, "Dooku_Grab_KD_GB", "Dooku_Grapple_KD_GB", 1.0f))
        GrappleFlags();
    if (SetupEvent(Va(0x001F6310))) { // start
        Call(Va(0x002BC210), 1, 0, 0);
        Call(Va(0x00172E00), 0, 0, 0);
        Call(Va(0x00172A40), Call<uint32_t>(Va(0x00172D20), 0x9C4, 0));
        Call(Va(0x00174B40), "Dooku_Grab_KD_GR", 0, 0, 0, 0);
        Call(Va(0x0017A7D0), 0);
    }
    if (SetupEvent(Va(0x001F6350))) { // end
        Call(Va(0x002BC1B0), 0, 0);
        Call(Va(0x00172E20));
        Call(Va(0x0017A7D0), 1);
    }
    if (SetupEvent(Va(0x001F62D0))) // back to its state machine after
        Call(Va(0x0038A140), inst, self, 3, 3);
    if (SetupEvent(Va(0x001F6390)) && inst) { // its frame codes
        switch (At<int32_t>(inst, 0x120)) {
        case 20: Call(Va(0x002BC1B0), 1, 0); break;
        case 69: Blast(false, 4); break;
        case 71: Call(Va(0x002BBEE0), 20.0f); break;
        case 73: Call(Va(0x00172E20)); break;
        default: break;
        }
    }
    return Finish(0x7E00);
}

// Dooku_Grab_Launch_GB (0x371A00): the launch
bool __fastcall DookuGrabLaunch(uint8_t* self, void*, uint32_t, uint32_t)
{
    uint8_t* inst = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptInstance));
    if (Declare(&g_LaunchHandle, "Dooku_Grab_Launch_GB", "Dooku_Grapple_Lnch_GB", 1.0f))
        GrappleFlags();
    if (SetupEvent(Va(0x001F6310))) {
        Call(Va(0x00172E00), 0, 0, 0);
        Call(Va(0x00172A40), Call<uint32_t>(Va(0x00172D20), 0x9C4, 0));
        Call(Va(0x00174B40), "Dooku_Grab_Launch_GR", 0, 0, 0, 0);
        Call(Va(0x0017A7D0), 0);
    }
    if (SetupEvent(Va(0x001F6350))) {
        Call(Va(0x00172E20));
        Blast(true, 5);
        Call(Va(0x0017A7D0), 1);
    }
    if (SetupEvent(Va(0x001F62D0)))
        Call(Va(0x0038A140), inst, self, 3, 3);
    if (SetupEvent(Va(0x001F6390)) && inst) {
        switch (At<int32_t>(inst, 0x120)) {
        case 0x64: Blast(true, 4); break;
        case 0x6E: Call(Va(0x002BBEE0), 20.0f); break;
        case 0x73: Call(Va(0x00172E20)); break;
        default: break;
        }
    }
    return Finish(0x7E01);
}

// The storm's cast: a hand raised (the lightning's own start, Serra's ForceStun animation), handed back after.
bool __fastcall StormCast(uint8_t* self, void*, uint32_t, uint32_t)
{
    if (Declare(&g_StormHandle, "Revan_ForceStorm", "Dooku_Frc_Light_In", 0.8f)) {
        *reinterpret_cast<uint8_t*>(uintptr_t(Va(0x00696531))) = 1;
        *reinterpret_cast<uint8_t*>(uintptr_t(Va(0x00696537))) = 1;
        *reinterpret_cast<const char**>(uintptr_t(Va(0x006964B8))) = kDuelMapTag;
    }
    if (SetupEvent(Va(0x001F62D0))) {
        uint8_t* inst = *reinterpret_cast<uint8_t**>(uintptr_t(kScriptInstance));
        Call(Va(0x0038A140), inst, self, -1, -1);
    }
    return Finish(0x7E02);
}

int __cdecl RevanSequences(const char* shortName, ClassSequenceFn* fns, int max)
{
    if (_stricmp(shortName, "Serra") != 0 || max < 3)
        return 0;
    int count = 0;
    const auto& customs = CustomCharacters();
    if (std::any_of(customs.begin(), customs.end(), [](const CustomCharacter& c) { return DookuGrapples(c); })) {
        fns[count++] = &DookuGrabKD;
        fns[count++] = &DookuGrabLaunch;
    }
    if (std::any_of(customs.begin(), customs.end(), [](const CustomCharacter& c) { return Wants(c) && c.forceStorm; }))
        fns[count++] = &StormCast;
    if (count)
        LOG_INFO("Revan: %d moves added to Serra's class", count);
    return count;
}

// --- Its turn ---
void Substitute(uint8_t* classObject, uint32_t from, uint16_t toHandle, const char* what)
{
    uint8_t* a = RecordOf(At<uint16_t>(classObject, from));
    uint8_t* b = RecordOf(toHandle);
    if (!a || !b) {
        LOG_WARN("Revan: %s not made (a sequence missing)", what);
        return;
    }
    Call(kSubstitute, a, b);
}

// Its effect slots for the palm blasts, as Dooku's StartFX (0x378A60) makes his: the effect, the joint, the offset.
void MakePalmBlasts(Fighter& f)
{
    const auto make = [](uint32_t* slot, uint32_t effectGlobal, uintptr_t joint, float x, float y, float z, float pitch, float yaw, float roll) {
        Call(Va(0x00176000), slot, effectGlobal); // the effect's interned name (its holder's address)
        void* name = nullptr;
        void* interned = Call<void*>(Va(0x00222FA0), &name, reinterpret_cast<const char*>(joint));
        Call(Va(0x00176100), slot, interned, 0);
        Call(Va(0x001761D0), slot, x, y, z, pitch, yaw, roll, 1);
    };
    make(&f.palmBlast, Va(0x00807644), Va(0x005E1938), 0, 0, 18.0f, 90.0f, 0, 0);       // elbow_L_g
    make(&f.launchBlast, Va(0x008093C4), Va(0x005AFBC4), 0, 12.0f, 6.0f, 90.0f, 0, -45.0f); // wrist_L_g
}

// The game's "is that character my enemy?" (0x18F3F0, thiscall on the character's AI controller (other, flag)).
bool IsEnemyOf(uint8_t* self, uint8_t* other)
{
    uint8_t* brain = At<uint8_t*>(self, 0x9FC);
    return !brain || reinterpret_cast<bool(__fastcall*)(uint8_t*, void*, uint8_t*, int)>(uintptr_t(Va(0x0018F3F0)))(brain, nullptr, other, 0);
}

uint8_t* NearestEnemy(uint8_t* self, float range, std::vector<uint8_t*>* all)
{
    const float* p = Position(self);
    uint8_t* best = nullptr;
    float bestDistance = range;
    for (uint8_t* c : LevelCharacters()) {
        if (c == self || At<float>(c, kCharacterHealth) <= 0)
            continue;
        if (HvvActive() ? HvvSameSide(c, self) : !IsEnemyOf(self, c))
            continue;
        const float* q = Position(c);
        const float d = std::hypot(q[0] - p[0], q[2] - p[2]);
        if (d >= range)
            continue;
        if (all)
            all->push_back(c);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

void RevanTurn(uint8_t* inst, uint8_t* classObject)
{
    {
        uint8_t* character = ScriptCharacter();
        const CustomCharacter* custom = character ? CustomCharacterOf(character) : nullptr;
        if (!custom || !Wants(*custom) || !inst || !classObject)
            return;
        Fighter& f = g_Fighters[character];
        const uint32_t id = At<uint32_t>(character, 4);
        const ULONGLONG now = GameTime();
        if (f.id != id) {
            f = Fighter{};
            f.id = id;
            f.armedAt = now + kArmAfterMs;
        }
        if (!f.set) { // once: its own moves instead of Serra's
            f.set = true;
            if (custom->lightning) {
                Substitute(classObject, kForceStun, At<uint16_t>(classObject, kForceLightning), "ForceStun -> ForceLightning");
                Substitute(classObject, kForceStunAlt, At<uint16_t>(classObject, kForceLightningAlt), "ForceStun_Alt -> ForceLightning_Alt");
                Substitute(classObject, kForceGraspStun, At<uint16_t>(classObject, kForceGraspLightning), "ForceGrasp_Stun -> ForceGrasp_Lightning");
                inst[kInstanceLightningOn] = 1;
            }
            if (DookuGrapples(*custom) && g_KDHandle && g_LaunchHandle) {
                Substitute(classObject, kSerraGrabAttempt, At<uint16_t>(classObject, kGrabAttempt), "Serra_Grab_Attempt -> Grab_Attempt");
                Substitute(classObject, kGrabAttempt, At<uint16_t>(classObject, kGrabAttempt), "Grab_Attempt (Serra's own undone)");
                Substitute(classObject, kGrabKD, g_KDHandle, "Grab_KD_GB_Human -> Dooku_Grab_KD_GB");
                Substitute(classObject, kGrabLaunch, g_LaunchHandle, "Grab_Launch_GB_Human -> Dooku_Grab_Launch_GB");
                MakePalmBlasts(f);
            }
            LOG_INFO("Revan: %s fights with%s%s%s", custom->className.c_str(), custom->lightning ? " lightning" : "",
                DookuGrapples(*custom) ? " Dooku's grapples" : "", custom->forceStorm ? " Force Storm" : "");
        }
        if (custom->lightning)
            inst[kInstanceLightningOn] = 1;
        if (At<int32_t>(character, 0x43C) >= 0)
            f.player = true;
        // Force Storm: the player's on block + stun; the computer's when enemies are near; not before a round is on
        if (!custom->forceStorm || f.storm || !g_StormHandle || now < f.armedAt || (HvvActive() && GetHvvState().intro))
            return;
        if (f.lastStorm && now < f.lastStorm + kStormWaitMs)
            return;
        float& force = At<float>(character, kCharacterForce);
        if (force < kStormCost)
            return;
        bool cast = false;
        if (f.player) {
            cast = Held(kButtonBlock) && Pressed(kButtonStun) && NearestEnemy(character, kStormRange * 1.5f, nullptr);
        } else if (now >= f.nextLook) {
            f.nextLook = now + 1500;
            std::vector<uint8_t*> nearby;
            NearestEnemy(character, kStormRange, &nearby);
            cast = !nearby.empty() && (GameTime() / 7) % 3 == 0;
        }
        if (cast) {
            force -= kStormCost;
            f.lastStorm = now;
            f.storm = true;
        }
    }
}

void __cdecl SerraRulesHook(uint8_t* inst, uint8_t* classObject)
{
    reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*)>(uintptr_t(kSerraRules))(inst, classObject);
    __try {
        RevanTurn(inst, classObject);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Dooku's lightning and grapple sounds for Serra's event tree: what she never plays.
bool DookuSoundEntry(const std::string& kind, const std::string& key)
{
    std::string k = key;
    for (char& ch : k)
        ch = char(tolower(static_cast<unsigned char>(ch)));
    static const char* const kLoads[] = { "for_lightning", "force_lightning_lp", "for_force_shockwave" };
    static const char* const kSequences[] = { "seq:forcelightning_loop", "seq:forcelightningb", "seq:forcelightningl",
        "seq:forcelightningr", "seq:forcelightningb_alt", "seq:forcelightningl_alt", "seq:forcelightningr_alt",
        "seq:forcegrasp_lightning", "anim:dooku_grapple_kd_gb", "anim:dooku_grapple_lnch_gb" };
    if (kind == "CharEvent_LoadSound")
        return std::any_of(std::begin(kLoads), std::end(kLoads), [&](const char* s) { return k == s; });
    if (kind == "CharEvent_Sequence")
        return std::any_of(std::begin(kSequences), std::end(kSequences), [&](const char* s) { return k == s; });
    return false;
}

void AddDookuSounds(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> donor;
    if (!ReadDiscResource("meshes\\chars\\common\\s_dooku.xbl_xml", donor) &&
        !ReadDiscResource("meshes\\chars\\common\\s_dooku.xml", donor)) {
        LOG_WARN("Revan: Dooku's event tree is not on the disc; his lightning stays quieter");
        return;
    }
    const size_t before = data.size();
    if (AppendEventEntries(data, donor, &DookuSoundEntry))
        LOG_INFO("Revan: Serra's event tree gets Dooku's lightning and grapple sounds (%zu -> %zu bytes)", before, data.size());
}

// Dooku's grapple rows (the computer offers moves by their rows) in Serra's attack table: the names she lacks.
void AddDookuRows(std::vector<uint8_t>& data)
{
    std::vector<uint8_t> donor;
    if (!ReadDiscResource("meshes\\chars\\common\\a_dooku.csv", donor))
        return;
    const std::string own(data.begin(), data.end()), theirs(donor.begin(), donor.end());
    static const char* const kRows[] = { "Grab_Attempt,", "Grab_KD_GB_Human,", "Grab_Launch_GB_Human,", "ForceLightning",
        "ForceGrasp_Lightning," };
    std::string added;
    size_t at = 0;
    while (at < theirs.size()) {
        size_t end = theirs.find('\n', at);
        if (end == std::string::npos)
            end = theirs.size();
        std::string line = theirs.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string name = line.substr(0, line.find(','));
        if (name.empty() || std::none_of(std::begin(kRows), std::end(kRows),
                [&](const char* r) { return _strnicmp(line.c_str(), r, strlen(r)) == 0; }))
            continue;
        if (own.find("\n" + name + ",") != std::string::npos || own.rfind(name + ",", 0) == 0)
            continue;
        added += line + "\r\n";
    }
    if (!data.empty() && data.back() != '\n')
        data.insert(data.end(), { '\r', '\n' });
    data.insert(data.end(), added.begin(), added.end());
}

} // namespace

void InstallRevan()
{
    g_Fighters.clear();
    g_Strikes.clear();
    g_KDHandle = g_LaunchHandle = g_StormHandle = 0;
    const auto& customs = CustomCharacters();
    if (std::none_of(customs.begin(), customs.end(), [](const CustomCharacter& c) { return Wants(c); }))
        return;
    uint8_t call[5] = { 0xE8 };
    const int32_t rel = int32_t(kSerraRules) - int32_t(kSerraRulesCall + 5);
    std::memcpy(call + 1, &rel, 4);
    if (!MatchesCode(kSerraRulesCall, call, 5)) {
        LOG_WARN("Revan: unexpected code at Serra's rules; no other moves for her class's custom characters");
        return;
    }
    PatchCall(kSerraRulesCall, reinterpret_cast<const void*>(&SerraRulesHook), 5);
    AddClassSequences(&RevanSequences);
    RegisterResourcePatch("meshes\\chars\\common\\s_serra.xml", &AddDookuSounds);
    if (std::any_of(customs.begin(), customs.end(), [](const CustomCharacter& c) { return DookuGrapples(c); }))
        RegisterResourcePatch("meshes\\chars\\common\\a_serra.csv", &AddDookuRows);
}

void RevanFrame()
{
    if (g_Fighters.empty() && g_Strikes.empty() && g_Sparks.empty())
        return;
    const ULONGLONG now = GameTime();
    for (auto it = g_Fighters.begin(); it != g_Fighters.end();) {
        uint8_t* character = it->first;
        Fighter& f = it->second;
        if (!Live(character, f.id)) {
            it = g_Fighters.erase(it);
            continue;
        }
        // a round ends with it or every enemy down; the next starts when it and an enemy stand again
        const bool standing = At<float>(character, kCharacterHealth) > 0;
        if (!standing || !NearestEnemy(character, 1e9f, nullptr)) {
            f.roundEnded = true;
        } else if (f.roundEnded) {
            f.roundEnded = false;
            f.armedAt = now + kArmAfterMs;
        }
        if (f.storm) {
            f.storm = false;
            std::vector<uint8_t*> nearby;
            NearestEnemy(character, kStormRange * 1.5f, &nearby);
            if (At<float>(character, kCharacterHealth) > 0 && !nearby.empty() && PlaySequence(character, "Revan_ForceStorm")) {
                for (int i = 0; i < kStrikes; ++i)
                {
                    uint8_t* target = nearby[size_t(i) % nearby.size()];
                    g_Strikes.push_back({ character, f.id, target, At<uint32_t>(target, 4), now + kFirstStrikeMs + ULONGLONG(i) * kStrikeGapMs });
                }
                LOG_INFO("Revan: Force Storm on %zu enemies", nearby.size());
            }
        }
        ++it;
    }
    for (auto it = g_Sparks.begin(); it != g_Sparks.end();) { // the columns' pieces: away
        if (now < it->until) {
            ++it;
            continue;
        }
        if (Live(it->object, it->id))
            reinterpret_cast<void(__cdecl*)(uint8_t*)>(uintptr_t(Va(0x000A2FE0)))(it->object);
        it = g_Sparks.erase(it);
    }
    for (auto it = g_Strikes.begin(); it != g_Strikes.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        if (!Live(it->owner, it->ownerId) || At<float>(it->owner, kCharacterHealth) <= 0 || !Live(it->target, it->targetId) ||
            At<float>(it->target, kCharacterHealth) <= 0) {
            it = g_Strikes.erase(it);
            continue;
        }
        if (it->step == 0) { // where it comes down: by the target as it stands now
            const float* p = Position(it->target);
            const ULONGLONG r = now ^ (uintptr_t(it->target) >> 4);
            const float angle = float(r % 628) / 100.0f, off = float((r / 7) % 30);
            it->where[0] = p[0] + std::cos(angle) * off;
            it->where[1] = p[1];
            it->where[2] = p[2] + std::sin(angle) * off;
        }
        if (it->step < kBoltSteps) { // the column, from the top
            const float h = kBoltHeight * float(kBoltSteps - it->step) / float(kBoltSteps);
            const float spark[3] = { it->where[0], it->where[1] + h, it->where[2] };
            BlastLook column;
            column.effects = kColumnEffects;
            column.effectCount = std::size(kColumnEffects);
            uint8_t* made = nullptr;
            column.made = &made;
            column.sound = it->step == 0 ? "for_lightning" : "";
            column.radius = 1;
            column.push = 0;
            column.damageType = "Electrified";
            column.damage = 0;
            FireBlast(it->owner, spark, column);
            if (made)
                g_Sparks.push_back({ made, *reinterpret_cast<uint32_t*>(made + 4), now + kSparkMs });
            ++it->step;
            it->at = now + kBoltStepMs;
            ++it;
            continue;
        }
        {
            const float* at = it->where;
            BlastLook bolt;
            bolt.effects = kStrikeEffects;
            bolt.effectCount = std::size(kStrikeEffects);
            bolt.sound = "for_lightning";
            bolt.radius = 70;
            bolt.push = 60;
            bolt.damageType = "Electrified";
            bolt.damage = kStrikeDamage;
            FireBlast(it->owner, at, bolt);
            BlastLook ground; // the strike's flash on the ground, harmless
            ground.effects = kStrikeGroundEffects;
            ground.effectCount = std::size(kStrikeGroundEffects);
            ground.sound = "";
            ground.radius = 1;
            ground.push = 0;
            ground.damageType = "Electrified";
            ground.damage = 0;
            FireBlast(it->owner, at, ground);
        }
        it = g_Strikes.erase(it);
    }
}

} // namespace swrots::game
