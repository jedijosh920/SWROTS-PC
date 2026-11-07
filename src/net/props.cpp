// The arena's props in client/server netplay (see props.h).

#include "net/props.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "net/duelstate.h"

namespace swrots::net {

namespace {

constexpr uint32_t kObjectById = 0x000A31F0;   // cdecl (instance id): the object manager's object, or null
constexpr uint32_t kDestroyObject = 0x000A2FE0; // cdecl (object): its tear-down (vfunc +0x1CC), then the manager's Delete
constexpr uint32_t kMatrix = 0x150;             // an object's place (IObject: 0xF2280 gives it)
constexpr uint32_t kVtablesFrom = 0x0055F660, kVtablesTo = 0x00612BC8; // .rdata
constexpr uint32_t kCodeFrom = 0x00011000, kCodeTo = 0x004FEB00;

uint8_t* ObjectById(uint32_t id)
{
    return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(uintptr_t(kObjectById))(id);
}

// An object's class name (vtable slot 3: a function returning it, as every object class has).
bool ClassNameRaw(uint8_t* object, char (&out)[48])
{
    out[0] = 0;
    __try {
        const uint32_t vtable = *reinterpret_cast<const uint32_t*>(object);
        if (vtable < kVtablesFrom || vtable >= kVtablesTo || (vtable & 3))
            return false;
        const uint32_t fn = reinterpret_cast<const uint32_t*>(uintptr_t(vtable))[3];
        if (fn < kCodeFrom || fn >= kCodeTo)
            return false;
        const char* name = reinterpret_cast<const char*(__fastcall*)(uint8_t*, void*)>(uintptr_t(fn))(object, nullptr);
        if (!name || uintptr_t(name) < kVtablesFrom || uintptr_t(name) >= 0x00962BF8)
            return false;
        size_t i = 0;
        for (; i + 1 < sizeof(out) && name[i]; ++i)
            out[i] = name[i];
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadMatrix(const uint8_t* object, float (&m)[16])
{
    __try {
        std::memcpy(m, object + kMatrix, sizeof(m));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMatrix(uint8_t* object, const float (&m)[16])
{
    __try {
        std::memcpy(object + kMatrix, m, sizeof(m));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DestroyRaw(uint8_t* object)
{
    __try {
        reinterpret_cast<void(__cdecl*)(uint8_t*)>(uintptr_t(kDestroyObject))(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The candidates of a memory range: aligned dwords that look like an object's vtable, whose object (there) the
// object manager knows by the id it holds (+4).
void ScanRange(uintptr_t from, size_t size, std::vector<uint8_t*>& out)
{
    __try {
        const uintptr_t to = from + size;
        for (uintptr_t a = (from + 3) & ~uintptr_t(3); a + 8 <= to; a += 4) {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(a);
            if (v < kVtablesFrom || v >= kVtablesTo || (v & 3))
                continue;
            const uint32_t first = *reinterpret_cast<const uint32_t*>(uintptr_t(v));
            if (first < kCodeFrom || first >= kCodeTo)
                continue;
            const uint32_t id = *reinterpret_cast<const uint32_t*>(a + 4);
            if (!id || id > 0x00FFFFFF)
                continue;
            if (ObjectById(id) == reinterpret_cast<uint8_t*>(a))
                out.push_back(reinterpret_cast<uint8_t*>(a));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::vector<uint8_t*> ScanObjects()
{
    std::vector<uint8_t*> objects;
    for (const auto& [base, size] : kernel::GameMemoryRegions())
        ScanRange(base, size, objects);
    std::sort(objects.begin(), objects.end());
    objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
    return objects;
}

// The classes synced: what the Force lifts, what breaks, what lies loose on the ground or moves on its own.
bool IsProp(const char* name)
{
    static const char* const kWords[] = { "Graspable", "Breakable", "GroundObject", "MovingProp", "Crate", "Barrel" };
    for (const char* w : kWords)
        if (std::strstr(name, w))
            return true;
    return false;
}

uint32_t IdOf(const uint8_t* object)
{
    uint32_t id = 0;
    __try {
        id = *reinterpret_cast<const uint32_t*>(object + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return id;
}

// The host's props.
struct Prop {
    uint8_t* object = nullptr;
    uint32_t id = 0;
    char cls[48] = {};
    float last[16] = {};
    uint32_t changedAt = 0; // the frame it last moved (+1; 0 never)
    uint32_t goneAt = 0;    // the frame it was found gone (+1; 0 there)
};
std::vector<Prop> g_Props;
bool g_Scanned = false;
size_t g_Next = 0; // the round of moved props sent
uint32_t g_Sent = 0, g_GoneSent = 0;

void ScanPropsLocked()
{
    g_Props.clear();
    g_Next = 0;
    const ULONGLONG start = GetTickCount64();
    const std::vector<uint8_t*> objects = ScanObjects();
    std::map<std::string, int> classes;
    for (uint8_t* o : objects) {
        Prop p;
        if (!ClassNameRaw(o, p.cls) || !IsProp(p.cls))
            continue;
        p.object = o;
        p.id = IdOf(o);
        if (!ReadMatrix(o, p.last))
            continue;
        ++classes[p.cls];
        g_Props.push_back(p);
    }
    std::string list;
    for (const auto& [name, n] : classes)
        list += " " + name + " x" + std::to_string(n);
    LOG_INFO("Online: %zu prop(s) among %zu objects (%llu ms):%s", g_Props.size(), objects.size(),
        GetTickCount64() - start, list.c_str());
    g_Scanned = true;
}

bool Moved(const float (&a)[16], const float (&b)[16])
{
    for (int i = 0; i < 16; ++i)
        if (std::fabs(a[i] - b[i]) > (i >= 12 ? 0.05f : 0.002f))
            return true;
    return false;
}

// The guest's: the host's latest place of each prop, and those taken away.
struct Remote {
    float matrix[16];
    uint32_t at; // the frame it came
};
std::unordered_map<uint32_t, Remote> g_Remote;
std::unordered_map<uint32_t, uint32_t> g_Gone; // id -> the frame it came (taken away here once)
uint32_t g_Applied = 0, g_Destroyed = 0;

} // namespace

void PropsReset()
{
    g_Props.clear();
    g_Scanned = false;
    g_Remote.clear();
    g_Gone.clear();
}

bool PropsCapture(uint32_t frame, PropState* props, int& count, uint32_t* gone, int& goneCount)
{
    count = goneCount = 0;
    if (!g_Scanned)
        ScanPropsLocked();
    const uint32_t now = frame + 1;
    for (Prop& p : g_Props) {
        if (p.goneAt)
            continue;
        float m[16];
        if (ObjectById(p.id) != p.object || !ReadMatrix(p.object, m)) {
            p.goneAt = now;
            LOG_INFO("Online: prop %u (%s) gone", p.id, p.cls);
            continue;
        }
        if (Moved(m, p.last)) {
            std::memcpy(p.last, m, sizeof(m));
            p.changedAt = now;
        }
    }
    // moved within the last second (sent again while it moves and a little after: a lost message is made good)
    const size_t n = g_Props.size();
    for (size_t k = 0; k < n && count < kPropsPerMsg; ++k) {
        Prop& p = g_Props[(g_Next + k) % n];
        if (p.goneAt || !p.changedAt || now - p.changedAt > 60)
            continue;
        props[count].id = p.id;
        std::memcpy(props[count].matrix, p.last, sizeof(p.last));
        ++count;
        if (count == kPropsPerMsg)
            g_Next = (g_Next + k + 1) % n;
    }
    for (const Prop& p : g_Props)
        if (p.goneAt && now - p.goneAt < 180 && goneCount < kGonePerMsg)
            gone[goneCount++] = p.id;
    g_Sent += uint32_t(count);
    g_GoneSent += uint32_t(goneCount);
    return count || goneCount;
}

void PropsReceive(uint32_t frame, const PropState* props, int count, const uint32_t* gone, int goneCount)
{
    for (int i = 0; i < count; ++i) {
        Remote& r = g_Remote[props[i].id];
        std::memcpy(r.matrix, props[i].matrix, sizeof(r.matrix));
        r.at = frame;
    }
    for (int i = 0; i < goneCount; ++i)
        g_Gone.emplace(gone[i], frame);
}

void PropsApply(uint32_t frame)
{
    for (auto it = g_Remote.begin(); it != g_Remote.end();) {
        if (frame - it->second.at > 90) { // still where the host's last was: the game's own now
            it = g_Remote.erase(it);
            continue;
        }
        if (uint8_t* o = ObjectById(it->first)) {
            if (WriteMatrix(o, it->second.matrix))
                ++g_Applied;
        }
        ++it;
    }
    for (auto& [id, at] : g_Gone) {
        if (at == ~0u)
            continue; // taken away already
        if (uint8_t* o = ObjectById(id)) {
            char cls[48];
            ClassNameRaw(o, cls);
            const bool ok = DestroyRaw(o);
            ++g_Destroyed;
            LOG_INFO("Online: prop %u (%s) gone on the host's game: %s", id, cls, ok ? "taken away" : "could not be");
        }
        at = ~0u;
        g_Remote.erase(id);
    }
}

std::string PropsStats()
{
    char line[160];
    std::snprintf(line, sizeof(line), "props: %zu found, %u places and %u gone sent; %u places applied, %u taken away",
        g_Props.size(), g_Sent, g_GoneSent, g_Applied, g_Destroyed);
    return line;
}

namespace {

struct Character {
    uint8_t* object = nullptr;
    uint32_t id = 0;
};
std::vector<Character> g_Chars;
bool g_CharsScanned = false;
uint32_t g_ProbeFrom = 0, g_ProbeCalls = 0;
size_t g_CharNext = 0;

// The object manager ([[0x68D944]+0x58]; 0xB4CC0 finds an object by its id): besides its id map (+0x340), three
// lists it looks through in turn: +0x3F0 count, +0x3F4 objects; +0x3B4 count, +0x3B8 (id, object) pairs; +0x35C count,
// +0x360 objects. Characters spawned during a level (a mission's enemies, made from its templates) are found there.
void ListedObjectsRaw(std::vector<uint8_t*>& out, int (&counts)[3])
{
    counts[0] = counts[1] = counts[2] = 0;
    __try {
        const uint8_t* root = *reinterpret_cast<uint8_t* const*>(uintptr_t(0x0068D944));
        const uint8_t* m = root ? *reinterpret_cast<uint8_t* const*>(root + 0x58) : nullptr;
        if (!m)
            return;
        const int n0 = *reinterpret_cast<const int*>(m + 0x3F0);
        uint8_t* const* a0 = *reinterpret_cast<uint8_t* const* const*>(m + 0x3F4);
        for (int i = 0; a0 && i < n0 && i < 8192; ++i)
            out.push_back(a0[i]);
        const int n1 = *reinterpret_cast<const int*>(m + 0x3B4);
        const uint32_t* a1 = *reinterpret_cast<const uint32_t* const*>(m + 0x3B8);
        for (int i = 0; a1 && i < n1 && i < 8192; ++i)
            out.push_back(reinterpret_cast<uint8_t*>(uintptr_t(a1[2 * i + 1])));
        const int n2 = *reinterpret_cast<const int*>(m + 0x35C);
        uint8_t* const* a2 = *reinterpret_cast<uint8_t* const* const*>(m + 0x360);
        for (int i = 0; a2 && i < n2 && i < 8192; ++i)
            out.push_back(a2[i]);
        counts[0] = n0;
        counts[1] = n1;
        counts[2] = n2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AddCharacter(uint8_t* o, uint32_t id, const uint8_t* p1, const uint8_t* p2)
{
    if (!o || !id)
        return;
    if (o == p1 || o == p2 || !IsCharacter(o))
        return;
    for (const Character& c : g_Chars)
        if (c.id == id)
            return;
    char cls[48];
    ClassNameRaw(o, cls);
    if (g_Chars.size() < 400 || g_Chars.size() % 50 == 0)
        LOG_INFO("Online: character %u (%s) followed", id, cls);
    g_Chars.push_back({ o, id });
}

} // namespace

void CharactersReset()
{
    g_Chars.clear();
    g_CharsScanned = false;
    g_ProbeFrom = 0;
    g_ProbeCalls = 0;
    g_CharNext = 0;
}

size_t CharactersKnown()
{
    return g_Chars.size();
}

std::string DescribeCharacters(size_t max)
{
    std::vector<Character> list = g_Chars;
    std::sort(list.begin(), list.end(), [](const Character& a, const Character& b) { return a.id < b.id; });
    std::string line;
    size_t n = 0;
    for (size_t i = 0; i < list.size() && n < max; ++i)
        if (CharacterActive(list[i].object)) {
            line += DescribeCharacter(list[i].id, list[i].object) + "; ";
            ++n;
        }
    return line;
}

uint8_t* ObjectOfId(uint32_t id)
{
    return id ? ObjectById(id) : nullptr;
}

int CharactersCapture(const uint8_t* player1, const uint8_t* player2, uint32_t* ids, uint8_t** objects, int max)
{
    if (!g_CharsScanned) {
        g_CharsScanned = true;
        const ULONGLONG start = GetTickCount64();
        const std::vector<uint8_t*> all = ScanObjects();
        for (uint8_t* o : all) {
            const uint32_t id = IdOf(o);
            g_ProbeFrom = std::max(g_ProbeFrom, id + 1);
            AddCharacter(o, id, player1, player2);
        }
        LOG_INFO("Online: %zu character(s) among %zu objects (%llu ms); new ones from id %u", g_Chars.size(), all.size(),
            GetTickCount64() - start, g_ProbeFrom);
    }
    if (g_ProbeCalls % 30 == 0) { // the manager's lists: the characters spawned
        std::vector<uint8_t*> listed;
        int counts[3];
        ListedObjectsRaw(listed, counts);
        const size_t before = g_Chars.size();
        for (uint8_t* o : listed)
            if (o && ObjectById(IdOf(o)) == o)
                AddCharacter(o, IdOf(o), player1, player2);
        static int logged = 0;
        if ((g_Chars.size() != before || logged == 0) && logged++ < 40)
            LOG_INFO("Online: the object manager's lists: %d, %d, %d object(s); %zu character(s) now", counts[0],
                counts[1], counts[2], g_Chars.size());
    }
    if (++g_ProbeCalls % 30 == 0 && g_ProbeFrom) { // the new objects: ids after the last one known
        const uint32_t from = g_ProbeFrom;
        for (uint32_t id = from; id < from + 512; ++id) {
            uint8_t* o = ObjectById(id);
            if (!o)
                continue;
            g_ProbeFrom = id + 1;
            AddCharacter(o, id, player1, player2);
        }
    }
    g_Chars.erase(std::remove_if(g_Chars.begin(), g_Chars.end(),
                      [](const Character& c) { return ObjectById(c.id) != c.object; }),
        g_Chars.end());
    int count = 0;
    const size_t n = g_Chars.size();
    for (size_t k = 0; k < n && count < max; ++k) {
        const Character& c = g_Chars[(g_CharNext + k) % n];
        if (c.object == player1 || c.object == player2)
            continue;
        ids[count] = c.id;
        objects[count] = c.object;
        ++count;
    }
    if (n)
        g_CharNext = (g_CharNext + size_t(count)) % n;
    return count;
}

std::string ObjectsCommand(const std::string& verb, const std::string& cls, float amount)
{
    const std::vector<uint8_t*> objects = ScanObjects();
    if (verb.empty() || verb == "list") {
        std::map<std::string, int> classes;
        for (uint8_t* o : objects) {
            char name[48];
            if (!ClassNameRaw(o, name))
                continue;
            ++classes[name];
            float m[16];
            if ((cls.empty() || std::strstr(name, cls.c_str())) && ReadMatrix(o, m))
                LOG_INFO("Objects: %08X id %u %s at %.0f %.0f %.0f", uint32_t(uintptr_t(o)), IdOf(o), name, m[12], m[13],
                    m[14]);
        }
        std::string out = std::to_string(objects.size()) + " objects (the log has each):";
        for (const auto& [name, n] : classes)
            out += " " + name + " x" + std::to_string(n);
        LOG_INFO("Objects: %s", out.c_str());
        return out;
    }
    for (uint8_t* o : objects) {
        char name[48];
        if (!ClassNameRaw(o, name) || !std::strstr(name, cls.c_str()))
            continue;
        float m[16];
        if (!ReadMatrix(o, m))
            continue;
        if (verb == "move") {
            m[13] += amount;
            WriteMatrix(o, m);
            LOG_INFO("Objects: %s %u moved up %.0f, now at %.0f %.0f %.0f", name, IdOf(o), amount, m[12], m[13], m[14]);
            return "moved " + std::string(name) + " " + std::to_string(IdOf(o));
        }
        if (verb == "destroy") {
            const uint32_t id = IdOf(o);
            DestroyRaw(o);
            LOG_INFO("Objects: %s %u taken away", name, id);
            return "took away " + std::string(name) + " " + std::to_string(id);
        }
    }
    return "no object of class " + cls;
}

} // namespace swrots::net
