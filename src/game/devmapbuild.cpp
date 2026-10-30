// The developer test map, made from Grievous Survival's level file (see devmapbuild.h and docs/research/levels.md).
//
// The level file is walked as the game reads it (TMeshWorld, 0x5D810), recording where the parts the dev map
// changes lie; most edits are values changed in place, the rest are spliced in (the floor's baked light, the new
// portals, the object section rewritten). A string is a u32 length and its characters, an array an i32 capacity,
// an i32 count and the elements, a bool 4 bytes.

#include "game/devmapbuild.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace swrots::game {

namespace {

// --- What the dev map is ----------------------------------------------------------------------------------------
// The cave's sector box, and the space it becomes (22,500 x 10,800 units): every coordinate equal to a limit of
// the old box moves to the new one's.
struct Range {
    double lo, hi;
};
constexpr Range kOldBox[3] = { { 3840.0, 8040.0 }, { -3115.924, 120.0 }, { 3881.164, 7560.0 } };
constexpr Range kSpace[3] = { { 1500.0, 24000.0 }, { -3115.924, 1100.0 }, { 2700.0, 13500.0 } };
// The floor brush covers the whole box (x, z); its top stays at the cave floor's height.
constexpr double kFloorY = -2557.5;
constexpr double kCaveCentreX = 6296.0, kCaveCentreZ = 6282.0;
constexpr double kLift = 100000.0; // the wall brushes are moved this far up, out of the way
// Under a map with brushes of its own the floor goes down this far, and the space's bottom with it (1442 below the
// cave floor): a map's lower parts (pits, stairs down) stand above the floor. Other maps keep the old box's bottom.
constexpr double kMapFloorDrop = 1150.0;
constexpr double kMapSpaceBottom = -4000.0;
double g_SpaceBottom = kSpace[1].lo; // this build's
// The layout's characters and start stand just above the floor: one placed higher falls when it wakes, and a fall
// stops what it plays (Heroes vs Villains' intros).
constexpr double kStandLift = 2.0;
// The objects a level needs (the managers, the lights, the sky, the sector's settings, the level's properties and
// the player's start), plus the sky's statics; the cave's props, its characters and its mission go.
constexpr const char* kKeep[] = { "IStaticManager", "ILightManager", "IBrushManager",
    "ITrivialCharacterPhysicsManager", "ILensFlareSlayerManager", "ISectorSkybox", "ISectorInfo", "ILevelProperties",
    "IMapIndexer", "IPointSpawnPlayer", "ILightStatic", "IInvisibleBrush" };
// The static manager's entries kept: the cave (its model becomes the floor) and the sky dome.
constexpr uint32_t kCaveMesh = 6, kSkyMesh = 7;
constexpr float kFloorAmbient = 0.8f;
constexpr float kCharacterAmbient = 0.55f;
constexpr uint32_t kLightRange = 12000;
constexpr uint32_t kLevelVersion = 0xA0042;

// The floor model: a grid of these cells, lit by its baked light (white, one entry per vertex).
constexpr int kCellsX = 44, kCellsZ = 21;
constexpr uint32_t kFloorVertices = (kCellsX + 1) * (kCellsZ + 1);

// --- Reading ------------------------------------------------------------------------------------------------------
struct Reader {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool ok = true;

    bool Take(size_t n)
    {
        if (!ok || n > d.size() - p) {
            ok = false;
            return false;
        }
        p += n;
        return true;
    }
    uint32_t U32()
    {
        uint32_t v = 0;
        if (Take(4))
            std::memcpy(&v, d.data() + p - 4, 4);
        return v;
    }
    uint16_t U16()
    {
        uint16_t v = 0;
        if (Take(2))
            std::memcpy(&v, d.data() + p - 2, 2);
        return v;
    }
    uint8_t U8() { return Take(1) ? d[p - 1] : 0; }
    std::string String()
    {
        const uint32_t n = U32();
        const size_t at = p;
        return Take(n) ? std::string(reinterpret_cast<const char*>(d.data() + at), n) : std::string();
    }
    // An array's count (its capacity skipped), refused when beyond what the file could hold.
    uint32_t Count(size_t elementSize = 1)
    {
        U32();
        const uint32_t n = U32();
        if (ok && uint64_t(n) * elementSize > d.size() - p)
            ok = false;
        return ok ? n : 0;
    }
    void Skip(uint64_t n) { Take(n > d.size() ? d.size() + 1 : size_t(n)); }
};

float GetF(const std::vector<uint8_t>& d, size_t at)
{
    float v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}
void PutF(std::vector<uint8_t>& d, size_t at, double v)
{
    const float f = float(v);
    std::memcpy(d.data() + at, &f, 4);
}
void PutU32(std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }

// A BSP tree (0x57840): convex volumes ("hedrons": u8 faces, u8 vertices, u8 edges, u8 planes, then the vertices,
// 3 x f32 each), nodes, planes (normal and d), an extra block and more blocks.
struct Tree {
    std::vector<size_t> hedrons; // where each hedron's data starts
    size_t planes = 0;
    uint32_t planeCount = 0;
    size_t start = 0, end = 0;   // the whole tree's bytes
};

void Blob(Reader& r, std::vector<size_t>* at)
{
    const uint32_t size = r.U32();
    if (size < 4) {
        r.ok = false;
        return;
    }
    if (at)
        at->push_back(r.p);
    r.Skip(size - 4);
}

bool ReadTree(Reader& r, Tree* tree)
{
    const size_t start = r.p;
    if (!r.U8())
        return false;
    Tree t;
    t.start = start;
    for (uint32_t i = 0, n = r.Count(4); i < n && r.ok; ++i)
        Blob(r, &t.hedrons);
    r.Skip(uint64_t(r.U32()) * 8); // nodes
    t.planeCount = r.Count(16);
    t.planes = r.p;
    r.Skip(uint64_t(t.planeCount) * 16);
    if (r.U32()) { // 0x56CF0
        const uint32_t a = r.U32(), b = r.U32();
        r.Skip(uint64_t(a) * 10 + uint64_t(b) * 4);
        r.Skip(uint64_t(r.U32()) * 0x20);
    }
    for (uint32_t i = 0, n = r.Count(4); i < n && r.ok; ++i)
        Blob(r, nullptr);
    t.end = r.p;
    if (tree)
        *tree = std::move(t);
    return true;
}

// A static instance (0x5AC80): its light handle, mesh, and, unless it refers elsewhere, its baked light.
struct Instance {
    uint32_t id = 0;
    bool lit = false;
    size_t count1 = 0;     // the vertex count
    size_t lightCount = 0; // the baked light's count, then the light (4 bytes per vertex)
    size_t lightEnd = 0;
};

void ReadInstance(Reader& r, Instance& s)
{
    s.id = r.U32();
    r.String();
    if (r.U16() != 0xFFFF)
        return;
    s.lit = true;
    r.Skip(12); // u32, bool, colour
    s.count1 = r.p;
    r.U32();
    s.lightCount = r.p;
    r.Skip(uint64_t(r.U32()) * 4);
    s.lightEnd = r.p;
    r.U32();
    r.Skip(uint64_t(r.U32()) * 2);
    r.Skip(8);
}

// A model (the mesh format, tools/meshes/charmsh.py): the parts of a sector's mesh the dev map changes.
struct Mesh {
    size_t centre = 0, radius = 0, bmin = 0, bmax = 0;
    struct Vertices {
        size_t at;
        uint32_t count, stride;
    };
    std::vector<Vertices> vertices;
};

void StringTable(Reader& r)
{
    r.U32();
    r.U32();
    const uint32_t used = r.U32();
    r.Skip(uint64_t(r.Count(4)) * 4);
    r.Skip(used);
}

void ReadMesh(Reader& r, Mesh& m)
{
    r.Skip(32); // name
    m.centre = r.p;
    r.Skip(12);
    m.radius = r.p;
    r.Skip(4 + 4 * 4 + 4 + 4 + 4); // radius, 4 bools, colour, triangle total, bool
    // the body
    if (!r.U32() || r.Count() != 0) { // the new format, no appearances
        r.ok = false;
        return;
    }
    for (uint32_t g = 0, groups = r.Count(); g < groups && r.ok; ++g) {
        r.Skip(16 + 1 + 4 + 4 * 13); // flags, colour, 2 floats, u8; material: shader, 4 layers
        if (!r.U32())
            continue;
        const size_t format = r.p;
        r.Skip(35);
        if (!r.ok)
            return;
        const uint32_t stride = uint32_t(int8_t(r.d[format + 0x22]));
        const uint32_t count = r.U32();
        m.vertices.push_back({ r.p, count, stride });
        if (stride < 12)
            r.ok = false;
        r.Skip(uint64_t(count) * stride);
        for (uint32_t i = 0, prims = r.Count(); i < prims && r.ok; ++i) {
            r.Skip(uint64_t(r.Count(2)) * 2); // bone palette
            r.Skip(uint64_t(r.Count(2)) * 2); // indices
            r.Skip(4 + 4 + 2 + 2 + 4);
        }
    }
    StringTable(r); // textures
    StringTable(r); // materials
    if (r.Count() != 0) { // embedded images
        r.ok = false;
        return;
    }
    r.Skip(uint64_t(r.Count(56)) * 56); // attachments
    m.bmin = r.p;
    m.bmax = r.p + 12;
    r.Skip(24);
    if (r.U32()) {
        r.Skip(uint64_t(r.Count(8)) * 8);
        r.Skip(24);
    }
    r.Skip(8);
}

// A portal: its points (an array of 3 x f32), u16, u16, a plane (normal, d), u32, u32.
struct Portal {
    size_t start = 0, points = 0, d = 0, end = 0;
    uint32_t pointCount = 0;
};

void ReadPortal(Reader& r, Portal& p)
{
    p.start = r.p;
    p.pointCount = r.Count(12);
    p.points = r.p;
    r.Skip(uint64_t(p.pointCount) * 12 + 4 + 12);
    p.d = r.p;
    r.Skip(12);
    p.end = r.p;
}

// The object section: a version, a u32, the object count, then each object: its class, its size (counting
// itself: to the object's end), its property count, its properties (a name, a size counting itself, the data) and
// what follows them (8 bytes).
struct Property {
    std::string name;
    std::vector<uint8_t> data;
};
struct Object {
    std::string cls;
    std::vector<Property> props;
    std::vector<uint8_t> tail;
};

bool ReadProperties(Reader& r, uint32_t n, std::vector<Property>& out)
{
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        Property p;
        p.name = r.String();
        const uint32_t size = r.U32();
        const size_t at = r.p;
        if (size < 4 || !r.Take(size - 4))
            return r.ok = false;
        p.data.assign(r.d.begin() + at, r.d.begin() + at + size - 4);
        out.push_back(std::move(p));
    }
    return r.ok;
}

struct Writer {
    std::vector<uint8_t> out;
    void Raw(const void* p, size_t n)
    {
        out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    }
    void U32(uint32_t v) { Raw(&v, 4); }
    void F32(float v) { Raw(&v, 4); }
    void String(const std::string& s)
    {
        U32(uint32_t(s.size()));
        Raw(s.data(), s.size());
    }
    void Properties(const std::vector<Property>& props)
    {
        U32(uint32_t(props.size()));
        for (const Property& p : props) {
            String(p.name);
            U32(uint32_t(p.data.size() + 4));
            Raw(p.data.data(), p.data.size());
        }
    }
};

Property* Find(std::vector<Property>& props, const char* name)
{
    for (Property& p : props)
        if (p.name == name)
            return &p;
    return nullptr;
}

// The objects of these classes in a level file, found by their class name as the object section writes it (its
// length, then the name; the object's size and properties after): for a level whose scene this builder does not
// read (a Versus arena's).
std::vector<Object> ObjectsOfClasses(const std::vector<uint8_t>& level, std::initializer_list<const char*> classes)
{
    std::vector<Object> out;
    for (const char* cls : classes) {
        const uint32_t length = uint32_t(strlen(cls));
        std::vector<uint8_t> pattern(4 + length);
        std::memcpy(pattern.data(), &length, 4);
        std::memcpy(pattern.data() + 4, cls, length);
        for (auto it = level.begin(); (it = std::search(it, level.end(), pattern.begin(), pattern.end())) != level.end(); ++it) {
            Reader r{ level };
            r.p = size_t(it - level.begin());
            Object o;
            o.cls = r.String();
            const size_t start = r.p;
            const uint32_t size = r.U32();
            const uint32_t count = r.U32();
            if (!r.ok || size < 8 || start + size > level.size() || count > 256)
                continue;
            ReadProperties(r, count, o.props);
            if (!r.ok || r.p > start + size)
                continue;
            o.tail.assign(level.begin() + r.p, level.begin() + start + size);
            out.push_back(std::move(o));
        }
    }
    return out;
}

Object* FindObject(std::vector<Object>& objects, const char* cls)
{
    for (Object& o : objects)
        if (o.cls == cls)
            return &o;
    return nullptr;
}

// A property holding an array of property groups (u32 count; each: u32 count, properties).
using Groups = std::vector<std::vector<Property>>;

bool ReadGroups(const std::vector<uint8_t>& data, Groups& groups)
{
    Reader r{ data };
    const uint32_t n = r.U32();
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        groups.emplace_back();
        ReadProperties(r, r.U32(), groups.back());
    }
    return r.ok && r.p == data.size();
}

std::vector<uint8_t> WriteGroups(const Groups& groups)
{
    Writer w;
    w.U32(uint32_t(groups.size()));
    for (const auto& g : groups)
        w.Properties(g);
    return w.out;
}

uint32_t U32Of(const Property* p)
{
    uint32_t v = 0;
    if (p && p->data.size() >= 4)
        std::memcpy(&v, p->data.data(), 4);
    return v;
}

template <size_t N>
void SetFloats(Property* p, const float (&v)[N], size_t at = 0)
{
    if (p && p->data.size() >= at + sizeof v)
        std::memcpy(p->data.data() + at, v, sizeof v);
}

double Remap(double v, int axis)
{
    if (std::fabs(v - kOldBox[axis].lo) < 0.01)
        return axis == 1 ? g_SpaceBottom : kSpace[axis].lo;
    if (std::fabs(v - kOldBox[axis].hi) < 0.01)
        return kSpace[axis].hi;
    return v;
}

void RemapPoint(std::vector<uint8_t>& d, size_t at)
{
    for (int k = 0; k < 3; ++k)
        PutF(d, at + 4 * k, Remap(GetF(d, at + 4 * k), k));
}

// A plane on an axis (normal +-1 there) moves with the box: n . p = d.
void RemapPlane(std::vector<uint8_t>& d, size_t normal, size_t dAt)
{
    double dist = GetF(d, dAt);
    for (int k = 0; k < 3; ++k) {
        const double n = GetF(d, normal + 4 * k);
        if (std::fabs(std::fabs(n) - 1.0) < 1e-4) {
            const double s = n > 0 ? 1.0 : -1.0;
            dist = s * Remap(s * dist, k);
        }
    }
    PutF(d, dAt, dist);
}

uint8_t HedronVertices(const std::vector<uint8_t>& d, size_t hedron) { return d[hedron + 1]; }

} // namespace

bool BuildDevMapLevel(std::vector<uint8_t>& level, std::string& report, const DevMapLayout* layout)
{
    const std::vector<uint8_t>& in = level;
    Reader r{ in };
    auto fail = [&](const char* why) {
        char text[160];
        std::snprintf(text, sizeof text, "%s (at 0x%zX)", why, r.p);
        report = text;
        return false;
    };
    g_SpaceBottom = layout && !layout->brushes.empty() ? kMapSpaceBottom : kSpace[1].lo;
    // the header: tag, version, the source path, two u32
    if (r.String() != "3DF!" || r.U32() != kLevelVersion)
        return fail("not a level file of the expected version");
    const std::string source = r.String();
    constexpr const char kSourceName[] = "U107_UTA_GrievousSurvival.SLS";
    constexpr size_t kSourceLength = sizeof kSourceName - 1;
    if (source.size() < kSourceLength || _stricmp(source.c_str() + source.size() - kSourceLength, kSourceName) != 0)
        return fail("not Grievous Survival's level file");
    r.Skip(8);
    if (r.U32() != 0 || r.U32() == 0) // no BSP mesh world; a scene
        return fail("unexpected level layout");

    // the scene (0x5C7E0)
    for (uint32_t i = 0, n = r.Count(); i < n && r.ok; ++i) // the sectors' volumes
        ReadTree(r, nullptr);
    r.Skip(uint64_t(r.Count(4)) * 4);
    std::vector<Tree> brushes; // the collision
    const size_t brushesCap = r.p, brushesCount = r.p + 4;
    for (uint32_t i = 0, n = r.Count(); i < n && r.ok; ++i) {
        Tree t;
        if (ReadTree(r, &t))
            brushes.push_back(std::move(t));
    }
    const size_t brushesEnd = r.p;
    std::vector<Instance> instances;
    for (uint32_t i = 0, n = r.Count(); i < n && r.ok; ++i) {
        instances.emplace_back();
        ReadInstance(r, instances.back());
    }
    Tree world;
    if (!ReadTree(r, &world) || !r.ok)
        return fail("the scene's world BSP");
    // the sectors: the first is the cave's
    Mesh mesh;
    std::vector<Portal> portals;
    size_t portalsCap = 0, portalsEnd = 0;
    for (uint32_t s = 0, n = r.Count(); s < n && r.ok; ++s) {
        r.Skip(20);
        r.String();
        Mesh m;
        ReadMesh(r, m);
        const size_t cap = r.p;
        std::vector<Portal> list(r.Count());
        for (Portal& p : list)
            ReadPortal(r, p);
        if (s == 0) {
            mesh = std::move(m);
            portals = std::move(list);
            portalsCap = cap;
            portalsEnd = r.p;
        }
        for (uint32_t i = 0, more = r.Count(); i < more && r.ok; ++i) {
            Portal p;
            ReadPortal(r, p);
        }
        r.Skip(4);
    }
    const size_t portalsCount = portalsCap + 4;
    if (!r.ok || portals.empty())
        return fail("the sectors");
    for (uint32_t i = 0, n = r.Count(); i < n && r.ok; ++i) { // groups
        r.Skip(20);
        r.String();
    }
    size_t bounds = 0;
    if (r.U32()) {
        bounds = r.p;
        r.Skip(36);
    }
    size_t octree = 0;
    for (uint32_t i = 0, n = r.U32(); i < n && r.ok; ++i) {
        const size_t head = r.p;
        r.Skip(28);
        if (!r.ok)
            break;
        uint32_t size;
        std::memcpy(&size, in.data() + head + 8, 4);
        if (i == 0)
            octree = r.p;
        r.Skip(size);
    }
    for (uint32_t i = 0, n = r.U32(); i < n && r.ok; ++i) { // index lists
        if (int16_t(r.U16()) >= 0)
            r.Skip(uint64_t(r.U32()) * 2);
    }
    r.Skip(12);
    if (!r.ok || !bounds || !octree)
        return fail("the scene's tail");

    // the objects
    const size_t objectsAt = r.p;
    if (r.U32() != kLevelVersion)
        return fail("the object section");
    const uint32_t nextId = r.U32();
    std::vector<Object> objects(r.U32());
    for (Object& o : objects) {
        o.cls = r.String();
        const size_t start = r.p;
        const uint32_t size = r.U32();
        ReadProperties(r, r.U32(), o.props);
        if (!r.ok || start + size < r.p || start + size > in.size())
            return fail("an object");
        o.tail.assign(in.begin() + r.p, in.begin() + start + size);
        r.p = start + size;
    }
    const size_t objectsEnd = r.p;
    if (!r.ok)
        return fail("the objects");

    std::vector<uint8_t> d = in;
    char text[512];
    int at = 0;
    auto say = [&](const char* format, auto... args) {
        if (at < int(sizeof text))
            at += std::snprintf(text + at, sizeof text - at, format, args...);
    };

    // statics: the static manager keeps the cave's entry (its model the floor, at the floor's height, lit by
    // ambient light) and the sky's; the floor's baked light (the static instance with its light handle) is white
    Object* manager = FindObject(objects, "IStaticManager");
    Property* list = manager ? Find(manager->props, "MGR_StrippedStaticsList") : nullptr;
    Groups statics, kept;
    if (!list || !ReadGroups(list->data, statics))
        return fail("the static manager");
    uint32_t floorHandle = 0;
    bool haveFloor = false;
    for (auto& g : statics) {
        const uint32_t model = U32Of(Find(g, "MGR_ManagedMeshID"));
        if (model == kCaveMesh) {
            float position[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, float(kFloorY), 0 };
            if (layout && !layout->modelName.empty()) { // a map's model, where it stands
                const float c = std::cos(layout->modelYaw), s = std::sin(layout->modelYaw);
                const float placed[12] = { c, 0, -s, 0, 1, 0, s, 0, c, layout->modelX, float(kFloorY + layout->modelY), layout->modelZ };
                std::copy(std::begin(placed), std::end(placed), position);
            }
            float ambient[3] = { kFloorAmbient, kFloorAmbient, kFloorAmbient };
            if (layout && layout->modelAmbient[0] >= 0)
                std::copy(std::begin(layout->modelAmbient), std::end(layout->modelAmbient), ambient);
            SetFloats(Find(g, "MGR_WorldPosition"), position);
            SetFloats(Find(g, "MGR_AmbientLight"), ambient);
            // lit by its ambient alone, unless a map brings its own baked light
            const bool baked = layout && !layout->bakedLight.empty() && layout->bakedLight.size() == layout->modelVertices;
            if (Property* p = Find(g, "MGR_Ambientlightingonly"); p && p->data.size() >= 4) {
                const uint32_t only = baked ? 0 : 1;
                std::memcpy(p->data.data(), &only, 4);
            }
            floorHandle = U32Of(Find(g, "MGR___lighthandle"));
            haveFloor = true;
            kept.push_back(g);
        } else if (model == kSkyMesh && !(layout && !layout->skyName.empty())) {
            kept.push_back(g);
        }
    }
    const Instance* floorLight = nullptr;
    for (const Instance& s : instances)
        if (haveFloor && s.lit && s.id == floorHandle)
            floorLight = &s;
    if (!floorLight)
        return fail("the cave's static entry");
    say("statics: %zu of %zu kept; ", kept.size(), statics.size());
    list->data = WriteGroups(kept);
    const bool mapModel = layout && !layout->modelName.empty() && layout->modelVertices > 0;
    const uint32_t lightVertices = mapModel ? layout->modelVertices : kFloorVertices;
    PutU32(d, floorLight->count1, lightVertices);
    // A map's model in the cave's entry of the managed meshes (u32 capacity, u32 count, then per mesh its name and
    // its folder under meshes\): the static entry's model then is the map's, read from its own folder.
    if (mapModel) {
        Property* meshes = Find(manager->props, "MGR_ManagedMeshData");
        if (!meshes)
            return fail("the managed meshes");
        Reader m{ meshes->data };
        const uint32_t cap = m.U32(), count = m.U32();
        std::vector<std::pair<std::string, std::string>> entries;
        for (uint32_t i = 0; i < count && m.ok; ++i) {
            std::string name = m.String();
            entries.push_back({ name, m.String() });
        }
        if (!m.ok || count <= kCaveMesh)
            return fail("the managed meshes");
        entries[kCaveMesh] = { layout->modelName, layout->modelFolder };
        Writer w;
        w.U32(cap);
        w.U32(count);
        for (const auto& e : entries) {
            w.String(e.first);
            w.String(e.second);
        }
        meshes->data = w.out;
        say("model %s%s; ", layout->modelFolder.c_str(), layout->modelName.c_str());
    }

    // spawn: the player's start in the middle of the floor, the sector's reference point (for the sky) too
    const double midX = (kSpace[0].lo + kSpace[0].hi) / 2, midZ = (kSpace[2].lo + kSpace[2].hi) / 2;
    Object* start = FindObject(objects, "IPointSpawnPlayer");
    Object* sectorInfo = FindObject(objects, "ISectorInfo");
    Property* startAt = start ? Find(start->props, "Transform") : nullptr;
    Property* sectorAt = sectorInfo ? Find(sectorInfo->props, "Transform") : nullptr;
    if (!startAt || startAt->data.size() < 48 || !sectorAt || sectorAt->data.size() < 48)
        return fail("the player's start");
    const float startPosition[3] = { float(midX), float(kFloorY + 60.0), float(midZ) };
    SetFloats(startAt, startPosition, 36);
    if (layout && layout->start) {
        const float c = std::cos(layout->startYaw), s = std::sin(layout->startYaw);
        const float transform[12] = { c, 0, -s, 0, 1, 0, s, 0, c, layout->startX, float(kFloorY + kStandLift), layout->startZ };
        SetFloats(startAt, transform);
    }
    const float sectorX[1] = { float(midX) }, sectorZ[1] = { float(midZ) };
    SetFloats(sectorAt, sectorX, 36);
    SetFloats(sectorAt, sectorZ, 44);

    // lighting: the characters' ambient raised; the point lights that light them moved from the cave to the
    // middle of the floor, their range made to reach all of it
    Object* properties = FindObject(objects, "ILevelProperties");
    Object* lights = FindObject(objects, "ILightManager");
    Property* lightList = lights ? Find(lights->props, "__lights") : nullptr;
    Groups lightGroups;
    if (!properties || !lightList || !ReadGroups(lightList->data, lightGroups))
        return fail("the lights");
    float characterAmbient[3] = { kCharacterAmbient, kCharacterAmbient, kCharacterAmbient };
    if (layout && layout->characterAmbient[0] >= 0)
        std::copy(std::begin(layout->characterAmbient), std::end(layout->characterAmbient), characterAmbient);
    SetFloats(Find(properties->props, "Character ambient color"), characterAmbient);
    SetFloats(Find(properties->props, "Dynamic-mesh ambient color"), characterAmbient);
    const double dx = midX - kCaveCentreX, dz = midZ - kCaveCentreZ;
    for (auto& g : lightGroups) {
        for (Property& p : g) {
            if (p.data.size() < 4)
                continue;
            float v;
            std::memcpy(&v, p.data.data(), 4);
            if (p.name == "__x")
                v = float(double(v) + dx);
            else if (p.name == "__z")
                v = float(double(v) + dz);
            else if (p.name == "__rng")
                std::memcpy(&v, &kLightRange, 4);
            else if (p.name == "__intn" && layout)
                v *= layout->lightScale;
            std::memcpy(p.data.data(), &v, 4);
        }
    }
    lightList->data = WriteGroups(lightGroups);
    say("%zu lights moved; ", lightGroups.size());

    // space: the sector's box stretched (the world BSP, the sector's mesh and bounds, its portals, the scene's
    // bounds and the octree), with portals on every side (the sky is drawn only through them)
    for (size_t h : world.hedrons)
        for (uint8_t i = 0; i < HedronVertices(d, h); ++i)
            RemapPoint(d, h + 4 + 12 * i);
    for (uint32_t i = 0; i < world.planeCount; ++i)
        RemapPlane(d, world.planes + 16 * i, world.planes + 16 * i + 12);
    RemapPoint(d, mesh.bmin);
    RemapPoint(d, mesh.bmax);
    double lo[3], hi[3], centre[3], radius2 = 0, extent = 0;
    for (int k = 0; k < 3; ++k) {
        lo[k] = GetF(d, mesh.bmin + 4 * k);
        hi[k] = GetF(d, mesh.bmax + 4 * k);
        centre[k] = (lo[k] + hi[k]) / 2;
        radius2 += ((hi[k] - lo[k]) / 2) * ((hi[k] - lo[k]) / 2);
        extent = std::max(extent, hi[k] - lo[k]);
    }
    const double radius = std::sqrt(radius2);
    for (int k = 0; k < 3; ++k)
        PutF(d, mesh.centre + 4 * k, centre[k]);
    PutF(d, mesh.radius, radius);
    // the sector's own faces made empty (every vertex at one corner): the open sky shows on every side
    for (const Mesh::Vertices& v : mesh.vertices)
        for (uint32_t i = 0; i < v.count; ++i)
            for (int k = 0; k < 3; ++k)
                PutF(d, v.at + size_t(i) * v.stride + 4 * k, lo[k]);
    for (const Portal& p : portals) {
        for (uint32_t i = 0; i < p.pointCount; ++i)
            RemapPoint(d, p.points + 12 * i);
        RemapPlane(d, p.d - 12, p.d);
    }
    // the cave had portals on its open sides (west, south, top): the other sides get theirs, made like the first
    Writer added;
    {
        const Portal& proto = portals[0];
        const size_t ab = proto.points + 12 * proto.pointCount, tail = proto.d + 4;
        auto add = [&](const double (&points)[3][3], int axis, double sign) {
            added.U32(3);
            added.U32(3);
            for (const auto& q : points)
                for (double c : q)
                    added.F32(float(c));
            added.Raw(d.data() + ab, 4);
            const float normal[3] = { axis == 0 ? float(sign) : 0.0f, 0.0f, axis == 2 ? float(sign) : 0.0f };
            added.Raw(normal, sizeof normal);
            added.F32(float(sign * (axis == 0 ? hi[0] : hi[2])));
            added.Raw(d.data() + tail, 8);
        };
        const double x0 = lo[0], y0 = lo[1], z0 = lo[2], x1 = hi[0], y1 = hi[1], z1 = hi[2];
        add({ { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 } }, 0, -1.0);
        add({ { x1, y0, z0 }, { x1, y1, z0 }, { x1, y0, z1 } }, 0, -1.0);
        add({ { x0, y1, z1 }, { x0, y0, z1 }, { x1, y0, z1 } }, 2, -1.0);
        add({ { x1, y1, z1 }, { x0, y1, z1 }, { x1, y0, z1 } }, 2, -1.0);
    }
    const uint32_t portalTotal = uint32_t(portals.size() + 4);
    uint32_t cap;
    std::memcpy(&cap, d.data() + portalsCap, 4);
    PutU32(d, portalsCap, std::max(cap, portalTotal));
    PutU32(d, portalsCount, portalTotal);
    RemapPoint(d, bounds + 12);
    RemapPoint(d, bounds + 24);
    RemapPoint(d, octree + 52);
    RemapPoint(d, octree + 64);
    for (int k = 0; k < 3; ++k)
        PutF(d, octree + 76 + 4 * k, centre[k]);
    PutF(d, octree + 88, radius);
    PutF(d, octree + 4, extent);
    say("space x %.0f..%.0f z %.0f..%.0f; ", lo[0], hi[0], lo[2], hi[2]);

    // floor: the floor brush (the one lowest at its top) covers the box; the wall brushes go far up
    if (brushes.empty())
        return fail("no brushes");
    auto top = [&](const Tree& t) {
        double y = -1e30;
        for (size_t h : t.hedrons)
            for (uint8_t i = 0; i < HedronVertices(d, h); ++i)
                y = std::max(y, double(GetF(d, h + 4 + 12 * i + 4)));
        return y;
    };
    const Tree* floor = &brushes[0];
    for (const Tree& t : brushes)
        if (top(t) < top(*floor))
            floor = &t;
    // a map with brushes of its own stands on them: the floor goes down near the space's bottom, under its lower parts
    const double floorDrop = layout && !layout->brushes.empty() ? kMapFloorDrop : 0.0;
    for (const Tree& t : brushes) {
        for (size_t h : t.hedrons) {
            for (uint8_t i = 0; i < HedronVertices(d, h); ++i) {
                const size_t v = h + 4 + 12 * i;
                if (&t == floor) {
                    PutF(d, v, GetF(d, v) > kCaveCentreX ? kSpace[0].hi : kSpace[0].lo);
                    PutF(d, v + 8, GetF(d, v + 8) > kCaveCentreZ ? kSpace[2].hi : kSpace[2].lo);
                    if (floorDrop > 0)
                        PutF(d, v + 4, GetF(d, v + 4) > kFloorY - 1.0 ? kFloorY - floorDrop : g_SpaceBottom);
                } else {
                    PutF(d, v + 4, GetF(d, v + 4) + kLift);
                }
            }
        }
        for (uint32_t i = 0; i < t.planeCount; ++i) {
            const size_t p = t.planes + 16 * i;
            const double nx = GetF(d, p), ny = GetF(d, p + 4), nz = GetF(d, p + 8);
            if (&t != floor)
                PutF(d, p + 12, GetF(d, p + 12) + ny * kLift);
            else if (std::fabs(ny) > 0.5) {
                if (floorDrop > 0)
                    PutF(d, p + 12, ny * (ny > 0 ? kFloorY - floorDrop : g_SpaceBottom));
            } else if (std::fabs(nx) > 0.5)
                PutF(d, p + 12, nx * (nx > 0 ? kSpace[0].hi : kSpace[0].lo));
            else if (std::fabs(nz) > 0.5)
                PutF(d, p + 12, nz * (nz > 0 ? kSpace[2].hi : kSpace[2].lo));
        }
    }

    // a map's collision boxes: copies of the cave's first wall brush (one box hedron: 8 vertices, 6 axis planes; its
    // edges, faces and nodes only index them), each vertex moved to its corner of the box, each plane turned with it
    Writer boxes;
    const size_t boxCount = layout ? layout->boxes.size() : 0;
    if (boxCount) {
        const Tree& box = brushes[0];
        if (box.hedrons.size() != 1 || HedronVertices(in, box.hedrons[0]) != 8 || box.planeCount != 6)
            return fail("the box brush");
        double boxCentre[3] = {};
        for (uint8_t i = 0; i < 8; ++i)
            for (int k = 0; k < 3; ++k)
                boxCentre[k] += GetF(in, box.hedrons[0] + 4 + 12 * i + 4 * k) / 8;
        for (const DevMapBox& b : layout->boxes) {
            std::vector<uint8_t> t(in.begin() + box.start, in.begin() + box.end);
            const size_t h = box.hedrons[0] - box.start, planes = box.planes - box.start;
            const double c = std::cos(b.yaw), s = std::sin(b.yaw), half[3] = { b.hx, b.hy, b.hz };
            const double place[3] = { b.cx, kFloorY + b.cy, b.cz };
            for (uint8_t i = 0; i < 8; ++i) {
                const size_t v = h + 4 + 12 * i;
                double local[3];
                for (int k = 0; k < 3; ++k)
                    local[k] = GetF(t, v + 4 * k) > boxCentre[k] ? half[k] : -half[k];
                PutF(t, v, place[0] + c * local[0] + s * local[2]);
                PutF(t, v + 4, place[1] + local[1]);
                PutF(t, v + 8, place[2] - s * local[0] + c * local[2]);
            }
            for (uint32_t i = 0; i < 6; ++i) {
                const size_t p = planes + 16 * i;
                const double nx = GetF(t, p), ny = GetF(t, p + 4), nz = GetF(t, p + 8);
                const double wx = c * nx + s * nz, wz = -s * nx + c * nz;
                PutF(t, p, wx);
                PutF(t, p + 8, wz);
                PutF(t, p + 12, wx * place[0] + ny * place[1] + wz * place[2] +
                    std::fabs(nx) * half[0] + std::fabs(ny) * half[1] + std::fabs(nz) * half[2]);
            }
            boxes.Raw(t.data(), t.size());
        }
    }
    // a map's own brushes (convex volumes in the model's coordinates, the level's tree format): placed as its model
    size_t mapBrushCount = 0;
    if (layout && !layout->brushes.empty()) {
        Reader b{ layout->brushes };
        const double c = std::cos(layout->modelYaw), s = std::sin(layout->modelYaw);
        const double place[3] = { layout->modelX, kFloorY + layout->modelY, layout->modelZ };
        while (b.p < layout->brushes.size() && b.ok) {
            Tree tree;
            if (!ReadTree(b, &tree) || !b.ok)
                return fail("the map's brushes");
            std::vector<uint8_t> t(layout->brushes.begin() + tree.start, layout->brushes.begin() + tree.end);
            for (size_t h : tree.hedrons) {
                for (uint8_t i = 0; i < HedronVertices(layout->brushes, h); ++i) {
                    const size_t v = h - tree.start + 4 + 12 * i;
                    const double x = GetF(t, v), y = GetF(t, v + 4), z = GetF(t, v + 8);
                    PutF(t, v, place[0] + c * x + s * z);
                    PutF(t, v + 4, place[1] + y);
                    PutF(t, v + 8, place[2] - s * x + c * z);
                }
            }
            for (uint32_t i = 0; i < tree.planeCount; ++i) {
                const size_t p = tree.planes - tree.start + 16 * i;
                const double nx = GetF(t, p), ny = GetF(t, p + 4), nz = GetF(t, p + 8);
                const double wx = c * nx + s * nz, wz = -s * nx + c * nz;
                PutF(t, p, wx);
                PutF(t, p + 8, wz);
                PutF(t, p + 12, GetF(t, p + 12) + wx * place[0] + ny * place[1] + wz * place[2]);
            }
            boxes.Raw(t.data(), t.size());
            ++mapBrushCount;
        }
    }
    if (boxCount + mapBrushCount) {
        uint32_t brushCap;
        std::memcpy(&brushCap, d.data() + brushesCap, 4);
        const uint32_t total = uint32_t(brushes.size() + boxCount + mapBrushCount);
        PutU32(d, brushesCap, std::max(brushCap, total));
        PutU32(d, brushesCount, total);
        // the brush manager: a handle per brush (its id, its class and what it stops), the boxes as the first wall
        Object* brushManager = FindObject(objects, "IBrushManager");
        Property* count = brushManager ? Find(brushManager->props, "__handlecount") : nullptr;
        Property* details = brushManager ? Find(brushManager->props, "__handledetails") : nullptr;
        Groups handles;
        if (!count || count->data.size() < 4 || !details || !ReadGroups(details->data, handles) || handles.empty())
            return fail("the brush manager");
        for (size_t i = 0; i < boxCount + mapBrushCount; ++i) {
            std::vector<Property> handle = handles[0];
            if (Property* id = Find(handle, "id"); id && id->data.size() >= 4)
                PutU32(id->data, 0, kDevMapFirstBoxId + uint32_t(i));
            handles.push_back(std::move(handle));
        }
        details->data = WriteGroups(handles);
        PutU32(count->data, 0, uint32_t(handles.size()));
        say("%zu collision boxes, %zu brushes; ", boxCount, mapBrushCount);
    }

    // objects: only what a level needs. With a map's music, the level's own music start is kept: the player's start
    // triggers a sequencer (its first trigger, "Single Random Event") over the cave's music objects (IAudio of Type
    // Music, event triggered). The sequencer keeps its first item alone, and that item's music object, naming the
    // map's track, is the only music object kept (two naming the same track streamed it both, out of step).
    const bool music = layout && !layout->music.empty();
    uint32_t sequencerId = 0, trackId = 0;
    if (music) {
        Object* spawn = FindObject(objects, "IPointSpawnPlayer");
        Property* triggers = spawn ? Find(spawn->props, "Initially trigger objects") : nullptr;
        if (triggers && triggers->data.size() >= 12)
            std::memcpy(&sequencerId, triggers->data.data() + 8, 4);
        for (Object& o : objects) {
            if (U32Of(Find(o.props, "ID")) != sequencerId)
                continue;
            Property* items = Find(o.props, "Items");
            Groups list;
            if (!items || !ReadGroups(items->data, list) || list.empty())
                break;
            Property* first = Find(list[0], "Objects"); // capacity, count, the ids
            if (!first || first->data.size() < 12)
                break;
            std::memcpy(&trackId, first->data.data() + 8, 4);
            list.resize(1);
            items->data = WriteGroups(list);
        }
        for (Object& o : objects) {
            Property* link = Find(o.props, "Sound Link");
            if (o.cls != "IAudio" || !link || U32Of(Find(o.props, "ID")) != trackId)
                continue;
            Writer name;
            name.String("ep3\\Ep3_Streaming\\music\\" + layout->music);
            link->data = name.out;
        }
        if (!sequencerId || !trackId)
            return fail("the level's music");
        say("music %s; ", layout->music.c_str());
    }
    // the goto marker the layout's markers copy: the level's plain one (no character of its own, no trigger)
    std::vector<Object> keptObjects;
    if (layout && !layout->markers.empty()) {
        const Object* marker = nullptr;
        for (Object& o : objects)
            if (o.cls == "ICharacterGoto" && !Find(o.props, "Character") && Find(o.props, "Transform"))
                marker = &o;
        if (!marker)
            return fail("the goto marker");
        for (size_t k = 0; k < layout->markers.size(); ++k) {
            Object copy = *marker;
            Writer id, name;
            id.U32(kDevMapFirstMarkerId + uint32_t(k));
            name.String("ICharacterGoto_dev" + std::to_string(k));
            const float lift = k < layout->markerLifts.size() ? layout->markerLifts[k] : 0.0f;
            const float m[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, layout->markers[k].first,
                float(kFloorY + kStandLift + lift), layout->markers[k].second };
            const float reach[1] = { layout->markerReach };
            for (Property& p : copy.props) {
                if (p.name == "ID")
                    p.data = id.out;
                else if (p.name == "Name")
                    p.data = name.out;
            }
            SetFloats(Find(copy.props, "Transform"), m);
            SetFloats(Find(copy.props, "Close Enough"), reach);
            // an attacked AI keeps its way (the port gives it the fight when an enemy is near)
            if (Property* p = Find(copy.props, "Cancel Goto If Attacked"); p && p->data.size() >= 4)
                std::memset(p->data.data(), 0, 4);
            keptObjects.push_back(std::move(copy));
        }
        say("%zu goto markers; ", layout->markers.size());
    }
    // A map's own sky: Utapau's backdrop (an IStatic drawn about the ISectorSkybox, "MeshName" its model's name and
    // folder) draws the map's sky model, turned as the map is; the cloud domes go.
    const bool ownSky = layout && !layout->skyName.empty();
    auto skyBackdrop = [&](Object& o) {
        Property* mesh = Find(o.props, "MeshName");
        if (!mesh)
            return false;
        Reader r{ mesh->data };
        if (r.String() != "backdrop")
            return false;
        Writer w;
        w.String(layout->skyName);
        w.String(layout->skyFolder);
        mesh->data = w.out;
        if (Property* t = Find(o.props, "Transform"); t && t->data.size() >= 48) {
            const float c = std::cos(layout->modelYaw), s = std::sin(layout->modelYaw);
            const float turn[9] = { c, 0, -s, 0, 1, 0, s, 0, c };
            SetFloats(t, turn);
        }
        return true;
    };
    bool skyPlaced = false;
    for (Object& o : objects) {
        const bool sky = o.cls == "IStatic" && Find(o.props, "Skybox Render Priority");
        const bool keep = std::any_of(std::begin(kKeep), std::end(kKeep), [&](const char* c) { return o.cls == c; }) ||
            // the sky (Utapau's): not around a map of its own (a closed room; outside it, nothing), unless it has one
            (sky && !(layout && !layout->modelName.empty())) || (sky && ownSky && skyBackdrop(o) && (skyPlaced = true)) ||
            (music && (U32Of(Find(o.props, "ID")) == sequencerId || U32Of(Find(o.props, "ID")) == trackId));
        if (keep)
            keptObjects.push_back(std::move(o));
    }
    if (ownSky)
        say("sky %s%s%s; ", layout->skyFolder.c_str(), layout->skyName.c_str(), skyPlaced ? "" : " (no backdrop)");
    say("%zu of %zu objects kept", keptObjects.size(), objects.size());
    // a duel: "Head to head arena?" (ILevelProperties +0xCC: the game's Versus start, rounds, HUD and cameras), not a
    // bonus mission, and the two fighters' spawn points (IVaderMultiSpawnPoint, an IPointSpawnPlayer by descent:
    // "Player ID" absent for player 1, 'Player 2' for player 2), on the floor (the game sets them down on it)
    if (layout && layout->versus) {
        Object* properties = FindObject(keptObjects, "ILevelProperties");
        Object* first = FindObject(keptObjects, "IPointSpawnPlayer");
        if (!properties || !first)
            return fail("the duel's objects");
        Writer one, zero;
        one.U32(1);
        zero.U32(0);
        if (Property* p = Find(properties->props, "Head to head arena?"))
            p->data = one.out;
        else
            properties->props.push_back({ "Head to head arena?", one.out });
        if (Property* p = Find(properties->props, "Single player Bonus Mission?"))
            p->data = zero.out;
        auto place = [&](Object& o, double z, float yaw) {
            const float c = std::cos(yaw), s = std::sin(yaw);
            const float m[12] = { c, 0, -s, 0, 1, 0, s, 0, c, float(midX), float(kFloorY + 2.0), float(z) };
            if (Property* t = Find(o.props, "Transform"); t && t->data.size() >= 48)
                SetFloats(t, m);
        };
        first->cls = "IVaderMultiSpawnPoint";
        place(*first, midZ - layout->duelGap / 2, 0.0f);
        const uint32_t firstId = U32Of(Find(first->props, "ID")); // (the list grows below: `first` goes stale)
        Object second;
        second.cls = "IVaderMultiSpawnPoint";
        Writer id, name, transform, player;
        id.U32(kDevMapDuelId);
        name.String("Player 2");
        const float blank[12] = {};
        transform.Raw(blank, sizeof blank);
        player.String("Player 2");
        second.props = { { "ID", id.out }, { "Name", name.out }, { "Transform", transform.out }, { "Player ID", player.out } };
        second.tail.assign(8, 0);
        place(second, midZ + layout->duelGap / 2, 3.14159265f);
        keptObjects.push_back(std::move(second));
        say(", a duel");
        // the duel's music: Versus starts the arena's own music object (an IAudio of Type Music with "Trigger Flags"
        // 0x20 and "3D?" off; no object triggers it), not Grievous Survival's (started by the player's start): the
        // map's track object takes the arena's music object's properties, keeping its own id, name and track
        if (music) {
            std::vector<Object> sounds = ObjectsOfClasses(layout->duelArena, { "IAudio" });
            Object* arenaMusic = nullptr;
            for (Object& o : sounds) {
                const Property* type = Find(o.props, "Type");
                if (type && type->data.size() >= 9 && std::memcmp(type->data.data() + 4, "Music", 5) == 0 &&
                    Find(o.props, "Trigger Flags")) {
                    arenaMusic = &o;
                    break;
                }
            }
            for (Object& o : keptObjects) {
                if (!arenaMusic || o.cls != "IAudio" || U32Of(Find(o.props, "ID")) != trackId)
                    continue;
                std::vector<Property> props = arenaMusic->props;
                for (Property& p : props)
                    if (p.name == "ID" || p.name == "Name" || p.name == "Sound Link" || p.name == "Transform")
                        if (const Property* own = Find(o.props, p.name.c_str()))
                            p.data = own->data;
                o.props = std::move(props);
                say(" (its music started by the duel)");
            }
        }
        // the arena's duel camera: its objects turned and moved from the arena's fighters (their spawn points'
        // middle, player 1 to player 2 along +z) onto these, their focus on these spawn points
        std::vector<Object> spawns = ObjectsOfClasses(layout->duelArena, { "IVaderMultiSpawnPoint" });
        std::vector<Object> camera = ObjectsOfClasses(layout->duelArena,
            { "ICameraControl", "ICameraControlHelper", "ICameraControlTriggerBound" });
        if (spawns.size() == 2 && !camera.empty()) {
            if (Find(spawns[0].props, "Player ID"))
                std::swap(spawns[0], spawns[1]); // player 1 first
            const uint32_t arenaIds[2] = { U32Of(Find(spawns[0].props, "ID")), U32Of(Find(spawns[1].props, "ID")) };
            const uint32_t ids[2] = { firstId, kDevMapDuelId };
            Property* t1 = Find(spawns[0].props, "Transform");
            Property* t2 = Find(spawns[1].props, "Transform");
            if (t1 && t2 && t1->data.size() >= 48 && t2->data.size() >= 48) {
                const double a[3] = { GetF(t1->data, 36), GetF(t1->data, 40), GetF(t1->data, 44) };
                const double b[3] = { GetF(t2->data, 36), GetF(t2->data, 40), GetF(t2->data, 44) };
                const double mid[3] = { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2 };
                const double dx = b[0] - a[0], dz = b[2] - a[2], d = std::max(1e-3, std::hypot(dx, dz));
                const double c = dz / d, s = -dx / d; // turns the arena's axis onto +z
                auto turn = [&](double x, double z, double& ox, double& oz) {
                    ox = c * x + s * z;
                    oz = -s * x + c * z;
                };
                for (Object& o : camera) {
                    if (Property* t = Find(o.props, "Transform"); t && t->data.size() >= 48) {
                        for (int row = 0; row < 3; ++row) {
                            double x, z;
                            turn(GetF(t->data, row * 12), GetF(t->data, row * 12 + 8), x, z);
                            PutF(t->data, row * 12, x);
                            PutF(t->data, row * 12 + 8, z);
                        }
                        double x, z;
                        turn(GetF(t->data, 36) - mid[0], GetF(t->data, 44) - mid[2], x, z);
                        PutF(t->data, 36, midX + x);
                        PutF(t->data, 40, GetF(t->data, 40) - mid[1] + kFloorY);
                        PutF(t->data, 44, midZ + z);
                    }
                    for (Property& p : o.props) // the focus on the arena's spawn points: on these
                        for (size_t at = 0; at + 4 <= p.data.size(); ++at)
                            for (int k = 0; k < 2; ++k)
                                if (p.name != "ID" && std::memcmp(p.data.data() + at, &arenaIds[k], 4) == 0)
                                    std::memcpy(p.data.data() + at, &ids[k], 4);
                    keptObjects.push_back(std::move(o));
                }
                say(" with the arena's camera (%zu objects)", camera.size());
            }
        }
    }
    // the layout's characters: an id, a name, a place (rows right, up, forward, position) and the 8 bytes after
    // the properties, as the level's own characters
    if (layout) {
        for (const DevMapCharacter& placed : layout->characters) {
            Object o;
            o.cls = placed.className;
            Writer id, name, transform;
            id.U32(placed.id);
            name.String(placed.className + "_dev" + std::to_string(placed.id - kDevMapFirstId));
            const float c = std::cos(placed.yaw), s = std::sin(placed.yaw);
            const float m[12] = { c, 0, -s, 0, 1, 0, s, 0, c, placed.x, float(kFloorY + kStandLift), placed.z };
            transform.Raw(m, sizeof m);
            o.props = { { "ID", id.out }, { "Name", name.out }, { "Transform", transform.out } };
            if (!placed.costume.empty()) {
                Writer costume;
                costume.String(placed.costume);
                o.props.push_back({ "Mesh Choice", costume.out });
            }
            o.tail.assign(8, 0);
            keptObjects.push_back(std::move(o));
        }
        say(", %zu characters placed", layout->characters.size());
    }
    Writer section;
    section.U32(kLevelVersion);
    section.U32(nextId);
    section.U32(uint32_t(keptObjects.size()));
    for (const Object& o : keptObjects) {
        section.String(o.cls);
        Writer body;
        body.Properties(o.props);
        body.Raw(o.tail.data(), o.tail.size());
        section.U32(uint32_t(body.out.size() + 4));
        section.Raw(body.out.data(), body.out.size());
    }

    // the result: the edited bytes with the floor's light, the new portals and the new object section in place
    std::vector<uint8_t> out;
    out.reserve(d.size() + added.out.size());
    out.insert(out.end(), d.begin(), d.begin() + brushesEnd);
    out.insert(out.end(), boxes.out.begin(), boxes.out.end());
    out.insert(out.end(), d.begin() + brushesEnd, d.begin() + floorLight->lightCount);
    const uint32_t vertices = lightVertices;
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&vertices), reinterpret_cast<const uint8_t*>(&vertices) + 4);
    if (mapModel && layout->bakedLight.size() == lightVertices)
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(layout->bakedLight.data()),
            reinterpret_cast<const uint8_t*>(layout->bakedLight.data() + lightVertices));
    else
        out.insert(out.end(), size_t(lightVertices) * 4, 0xFF);
    out.insert(out.end(), d.begin() + floorLight->lightEnd, d.begin() + portalsEnd);
    out.insert(out.end(), added.out.begin(), added.out.end());
    out.insert(out.end(), d.begin() + portalsEnd, d.begin() + objectsAt);
    out.insert(out.end(), section.out.begin(), section.out.end());
    out.insert(out.end(), d.begin() + objectsEnd, d.end());
    report = text;
    level = std::move(out);
    return true;
}

// --- The floor model: written from scratch (the mesh format: tools/meshes/charmsh.py) ------------------------------
// A flat grid at local y 0 (the level places it at the floor's height), one triangle strip, textured with the
// level's cave floor texture tiled every 512 units.
namespace {

constexpr float kFloorX0 = 1500.0f, kFloorX1 = 24000.0f, kFloorZ0 = 2700.0f, kFloorZ1 = 13500.0f;
constexpr float kTile = 512.0f;
constexpr const char* kFloorTexture = "M07_CaveFloorTile512_02";
// Vertex format: position at 0, normal (4 x u8) at 12, uv (2 x f32) at 16, colour (4 x u8) at 24; stride 28.
constexpr uint8_t kFormat[35] = { 0x00, 0x01, 0x03, 0x02, 0xcf, 0x02, 0x00, 0x00, 0xce, 0x02, 0x00, 0x02, 0x04,
    0x01, 0x04, 0x00, 0xcc, 0x02, 0x00, 0x00, 0xcb, 0x02, 0x00, 0x0c, 0x18, 0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x04, 0x1c };

struct MeshWriter : Writer {
    void U8(uint8_t v) { out.push_back(v); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void Zero(size_t n) { out.insert(out.end(), n, 0); }
    void Name32(const char* s)
    {
        char b[32] = {};
        strncpy_s(b, s, 31);
        Raw(b, 32);
    }
    void Identity()
    {
        const float m[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        Raw(m, sizeof m);
    }
    // a string table: u32 total, u32 0, u32 used, TArray<u32> offsets, the text
    void Table(const char* name)
    {
        const uint32_t n = uint32_t(strlen(name)) + 1;
        U32(n);
        U32(0);
        U32(n);
        U32(4);
        U32(1);
        U32(0);
        Raw(name, n);
    }
};

} // namespace

std::vector<uint8_t> DevMapFloorModel()
{
    const int vx = kCellsX + 1, vz = kCellsZ + 1, count = vx * vz, tris = 2 * kCellsX * kCellsZ;
    std::vector<uint16_t> strip;
    for (int j = 0; j < kCellsZ; ++j) {
        std::vector<uint16_t> row;
        for (int i = 0; i < vx; ++i) {
            row.push_back(uint16_t((j + 1) * vx + i));
            row.push_back(uint16_t(j * vx + i));
        }
        if (!strip.empty()) { // degenerate triangles join the rows, keeping the winding
            const uint16_t last = strip.back();
            strip.push_back(last);
            strip.push_back(row[0]);
            if (strip.size() % 2)
                strip.push_back(row[0]);
        }
        strip.insert(strip.end(), row.begin(), row.end());
    }
    MeshWriter w;
    w.U32(0xF8C203A1); // magic
    w.U32(kLevelVersion);
    w.U32(1); w.U32(1); w.String("Base"); w.U32(1); w.U32(1); w.String("Base"); // header: "Base" {"Base"}
    w.U32(1); w.U32(1);                  // models
    w.String("Base"); w.U32(1);          // model name, bool
    w.U32(1); w.U32(1);                  // meshes
    w.Name32("Base");
    w.F32((kFloorX0 + kFloorX1) / 2); w.F32(0.0f); w.F32((kFloorZ0 + kFloorZ1) / 2); // centre
    const float hx = (kFloorX1 - kFloorX0) / 2, hz = (kFloorZ1 - kFloorZ0) / 2;
    w.F32(float(std::sqrt(double(hx) * hx + double(hz) * hz))); // radius
    w.U32(0); w.U32(0); w.U32(0); w.U32(1); // bools
    w.U32(0xFF000000); w.U32(uint32_t(tris)); w.U32(1); // colour, triangle total, bool
    // body
    w.U32(1);           // new format
    w.U32(0); w.U32(0); // appearances
    w.U32(1); w.U32(1); // groups
    w.U32(0); w.U32(0xFF000000); w.F32(1.0f); w.F32(0.01f); w.U8(0); // flags, colour, 2 floats, u8
    w.U32(0);           // shader
    for (int l = 0; l < 4; ++l) { // layers: u8, 4 x s16 uv, u32 texture (1-based; 0 none)
        w.U8(l == 0 ? 255 : 0);
        w.Zero(8);
        w.U32(l == 0 ? 1 : 0);
    }
    w.U32(1);           // render data present
    w.Raw(kFormat, sizeof kFormat);
    w.U32(uint32_t(count));
    for (int j = 0; j < vz; ++j) {
        for (int i = 0; i < vx; ++i) {
            const float x = float(kFloorX0 + double(kFloorX1 - kFloorX0) * i / kCellsX);
            const float z = float(kFloorZ0 + double(kFloorZ1 - kFloorZ0) * j / kCellsZ);
            w.F32(x); w.F32(0.0f); w.F32(z);
            w.U8(128); w.U8(255); w.U8(128); w.U8(0); // normal: up
            w.F32(x / kTile); w.F32(z / kTile);
            w.U32(0xFFFFFFFF);                        // colour
        }
    }
    w.U32(1); w.U32(1); // prims
    w.U32(0); w.U32(0); // bone palette
    w.U32(uint32_t(strip.size())); w.U32(uint32_t(strip.size()));
    for (uint16_t i : strip)
        w.U16(i);
    w.U32(1); w.U32(0); w.U16(uint16_t(count)); w.U16(uint16_t(tris)); w.U32(uint32_t(tris * 3));
    w.Table(kFloorTexture);
    w.Table("DevFloorSG");
    w.U32(0); w.U32(0); // embedded images
    // attachments: one record (type 0, 1, 48 zero bytes); without it the static crashes the level's load (0x2078FD)
    w.U32(4); w.U32(1); w.U32(0); w.U32(1); w.Zero(48);
    w.F32(kFloorX0); w.F32(0.0f); w.F32(kFloorZ0); // bound box
    w.F32(kFloorX1); w.F32(0.0f); w.F32(kFloorZ1);
    w.U32(0); w.U32(0); w.U32(1); // bools
    // bones: the root
    w.U32(1); w.U32(1); w.Name32("root"); w.Identity(); w.U16(0xFFFF);
    w.Identity(); w.Identity(); // matrices
    w.U32(0); w.U32(0);         // flags
    w.U32(1);                   // mesh flags
    return w.out;
}

} // namespace swrots::game
