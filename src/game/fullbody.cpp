// Yoda's full-body select pictures, made from his own model, hilt and animation clips on the player's disc (see
// fullbody.h). The steps are tools/fullbody's (fb_render.py renders in Blender, fb_stx.py grades and encodes), in the
// space fb_render.py renders in: the game's model mirrored in x, its y up turned to z up, at 0.05 a game unit.

#include "game/fullbody.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace swrots::game::fullbody {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kScale = 0.05;

// --- vectors and matrices ---------------------------------------------------------------------------------------

using V3 = std::array<double, 3>;
V3 operator+(const V3& a, const V3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
V3 operator-(const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
V3 operator*(const V3& a, double k) { return { a[0] * k, a[1] * k, a[2] * k }; }
double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b)
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
double Length(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Unit(const V3& a)
{
    const double n = Length(a);
    return n > 0 ? a * (1 / n) : a;
}

struct M3 {
    double m[3][3];
};
M3 Mul(const M3& a, const M3& b)
{
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
V3 Mul(const M3& a, const V3& v)
{
    return { a.m[0][0] * v[0] + a.m[0][1] * v[1] + a.m[0][2] * v[2], a.m[1][0] * v[0] + a.m[1][1] * v[1] + a.m[1][2] * v[2],
        a.m[2][0] * v[0] + a.m[2][1] * v[1] + a.m[2][2] * v[2] };
}
M3 Inverse(const M3& a)
{
    const auto& m = a.m;
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    const double k = det != 0 ? 1 / det : 0;
    M3 r{};
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    return r;
}

// An affine matrix: m (3x3) then t; a point p is m p + t.
struct M4 {
    M3 r{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    V3 t{ 0, 0, 0 };
};
M4 Mul(const M4& a, const M4& b) { return { Mul(a.r, b.r), Mul(a.r, b.t) + a.t }; }
V3 Point(const M4& a, const V3& p) { return Mul(a.r, p) + a.t; }
M4 Inverse(const M4& a)
{
    M4 r;
    r.r = Inverse(a.r);
    r.t = Mul(r.r, a.t) * -1;
    return r;
}

// The rotation turning direction a into b (the shortest arc).
M3 RotBetween(V3 a, V3 b)
{
    a = Unit(a);
    b = Unit(b);
    const V3 v = Cross(a, b);
    const double c = Dot(a, b);
    M3 r{};
    if (c < -0.999999) {
        V3 axis = Cross(a, { 1, 0, 0 });
        if (Length(axis) < 1e-6)
            axis = Cross(a, { 0, 1, 0 });
        axis = Unit(axis);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = 2 * axis[i] * axis[j] - (i == j ? 1 : 0);
        return r;
    }
    const M3 vx{ { { 0, -v[2], v[1] }, { v[2], 0, -v[0] }, { -v[1], v[0], 0 } } };
    const M3 vv = Mul(vx, vx);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = (i == j ? 1 : 0) + vx.m[i][j] + vv.m[i][j] / (1 + c);
    return r;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// --- the model (.msh) ---------------------------------------------------------------------------------------------

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;
    template <typename T> T Get()
    {
        T v{};
        if (!ok || pos + sizeof(T) > n) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    const uint8_t* Take(size_t k)
    {
        if (!ok || k > n || pos + k > n) {
            ok = false;
            return nullptr;
        }
        const uint8_t* at = p + pos;
        pos += k;
        return at;
    }
    // A TArray's count (after its capacity), when sane.
    int32_t Count(int32_t most)
    {
        Get<int32_t>();
        const int32_t count = Get<int32_t>();
        if (count < 0 || count > most)
            ok = false;
        return ok ? count : 0;
    }
};

struct Part {
    int texture = -1; // in the model's textures
    std::vector<V3> pos, nrm;
    std::vector<std::array<double, 2>> uv;
    std::vector<std::array<int, 4>> bones;
    std::vector<std::array<double, 4>> weights;
    std::vector<std::array<int, 3>> tris;
};

struct Model {
    std::vector<std::string> names; // lower case
    std::vector<int> parents;
    std::vector<M4> bind; // each bone's world matrix at rest (the game's space)
    std::vector<Part> parts;
    std::vector<std::string> textures; // lower case
    int Index(const std::string& name) const
    {
        const std::string lower = Lower(name);
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == lower)
                return int(i);
        return -1;
    }
};

std::vector<std::string> ReadTable(Reader& r)
{
    r.Get<uint32_t>();
    r.Get<uint32_t>();
    const uint32_t used = r.Get<uint32_t>();
    const int32_t count = r.Count(4096);
    std::vector<uint32_t> offsets(static_cast<size_t>(count));
    for (auto& o : offsets)
        o = r.Get<uint32_t>();
    const uint8_t* text = r.Take(used);
    std::vector<std::string> names;
    if (!r.ok)
        return names;
    for (uint32_t o : offsets) {
        std::string s;
        for (uint32_t i = o; i < used && text[i]; ++i)
            s += char(text[i]);
        names.push_back(Lower(s));
    }
    return names;
}

int AddTexture(Model& model, const std::string& name)
{
    for (size_t i = 0; i < model.textures.size(); ++i)
        if (model.textures[i] == name)
            return int(i);
    model.textures.push_back(name);
    return int(model.textures.size() - 1);
}

// The game's mesh archive (tools/meshes/charmsh.py has the layout): its first model's bones and the triangles of
// its meshes (but those named with `skip`), each group with its first layer's texture. `skinned`: the groups carry
// bone weights and indices (palette slot x 3).
bool ReadModel(const std::vector<uint8_t>& data, bool skinned, const char* skip, Model& model)
{
    Reader r{ data.data(), data.size() };
    if (r.Get<uint32_t>() != 0xF8C203A1)
        return false;
    const uint32_t version = r.Get<uint32_t>();
    if (version < 0x90011)
        return false;
    for (int32_t h = r.Count(64); h > 0 && r.ok; --h) {
        r.Take(r.Get<uint32_t>());
        for (int32_t k = r.Count(4096); k > 0 && r.ok; --k)
            r.Take(r.Get<uint32_t>());
    }
    if (r.Count(16) < 1)
        return false;
    r.Take(r.Get<uint32_t>());
    r.Get<uint32_t>();
    for (int32_t mesh = r.Count(64); mesh > 0 && r.ok; --mesh) {
        const uint8_t* nameBytes = r.Take(32);
        if (!r.ok)
            return false;
        const std::string meshName = Lower(std::string(reinterpret_cast<const char*>(nameBytes),
            strnlen(reinterpret_cast<const char*>(nameBytes), 32)));
        const bool kept = !skip || meshName.find(skip) == std::string::npos;
        r.Take(12 + 4 + 16 + 4 + 4 + 4);
        if (!r.Get<uint32_t>() || r.Count(0) != 0) // the new format, no legacy appearances
            return false;
        struct Pending {
            Part part;
            int layer;
        };
        std::vector<Pending> groups;
        for (int32_t g = r.Count(256); g > 0 && r.ok; --g) {
            const uint8_t* head = r.Take(73);
            const uint32_t present = r.Get<uint32_t>();
            if (!r.ok)
                return false;
            if (!present)
                continue;
            uint32_t layer;
            std::memcpy(&layer, head + 30, 4); // shader at 17, then the first layer: u8, 4 x s16, its texture
            const uint8_t* format = r.Take(35);
            if (!r.ok)
                return false;
            const int stride = int8_t(format[0x22]);
            const int32_t count = r.Get<int32_t>();
            if (stride <= 0 || count < 0 || count > 65536)
                return false;
            const uint8_t* v = r.Take(size_t(count) * stride);
            auto slot = [&](int s) { return format[0x16 + s] == 0xFF ? -1 : int(format[0x16 + s]); };
            const int at[5] = { slot(0), slot(1), slot(3), slot(6), slot(7) };
            const bool usable = at[0] >= 0 && at[1] >= 0 && at[2] >= 0 && (!skinned || (at[3] >= 0 && at[4] >= 0));
            Part part;
            if (r.ok && usable) {
                for (int32_t i = 0; i < count; ++i) {
                    const uint8_t* x = v + size_t(i) * stride;
                    float f[3], uv[2];
                    std::memcpy(f, x + at[0], 12);
                    std::memcpy(uv, x + at[2], 8);
                    part.pos.push_back({ f[0], f[1], f[2] });
                    part.nrm.push_back({ (x[at[1]] - 127.5) / 127.5, (x[at[1] + 1] - 127.5) / 127.5,
                        (x[at[1] + 2] - 127.5) / 127.5 });
                    part.uv.push_back({ uv[0], uv[1] });
                    if (skinned) {
                        std::array<double, 4> w{};
                        double sum = 0;
                        for (int k = 0; k < 4; ++k)
                            sum += (w[k] = x[at[3] + k]);
                        if (sum > 0)
                            for (double& c : w)
                                c /= std::max(sum, 1.0);
                        else
                            w = { 1, 0, 0, 0 };
                        part.weights.push_back(w);
                    }
                }
                part.bones.assign(size_t(count), { 0, 0, 0, 0 });
            }
            int start = 0;
            for (int32_t prim = r.Count(256); prim > 0 && r.ok; --prim) {
                std::vector<uint16_t> palette(static_cast<size_t>(r.Count(256)));
                for (auto& b : palette)
                    b = r.Get<uint16_t>();
                std::vector<uint16_t> indices(static_cast<size_t>(r.Count(1000000)));
                for (auto& i : indices)
                    i = r.Get<uint16_t>();
                r.Get<uint32_t>();
                r.Get<uint32_t>();
                const int used = r.Get<uint16_t>();
                if (version >= 0x9001C)
                    r.Get<int16_t>();
                if (version > 0x90014)
                    r.Get<uint32_t>();
                if (!r.ok || !usable)
                    continue;
                if (skinned)
                    for (int k = start; k < start + used && k < count; ++k)
                        for (int j = 0; j < 4; ++j) {
                            const size_t slotIndex = (v + size_t(k) * stride)[at[4] + j] / 3;
                            part.bones[size_t(k)][size_t(j)] = slotIndex < palette.size() ? palette[slotIndex] : 0;
                        }
                for (size_t t = 0; t + 2 < indices.size(); ++t) {
                    const int a = indices[t], b = indices[t + 1], c = indices[t + 2];
                    if (a == b || b == c || a == c || a >= count || b >= count || c >= count)
                        continue;
                    part.tris.push_back(t % 2 ? std::array<int, 3>{ a, b, c } : std::array<int, 3>{ a, c, b });
                }
                start += used;
            }
            if (usable && kept)
                groups.push_back({ std::move(part), int(layer) });
        }
        const std::vector<std::string> textures = ReadTable(r);
        if (version >= 0x90008)
            ReadTable(r);
        if (r.Count(0) != 0) // no embedded images
            return false;
        r.Take(size_t(r.Count(256)) * 56);
        r.Take(24);
        if (r.Get<uint32_t>() && version >= 0x52049) {
            r.Take(size_t(r.Count(100000)) * 8);
            r.Take(24);
        }
        r.Get<uint32_t>();
        if (version >= 0x90007)
            r.Get<uint32_t>();
        if (!r.ok)
            return false;
        for (Pending& g : groups) {
            g.part.texture = g.layer > 0 && size_t(g.layer) <= textures.size() ?
                AddTexture(model, textures[size_t(g.layer) - 1]) : -1;
            model.parts.push_back(std::move(g.part));
        }
    }
    for (int32_t b = r.Count(512); b > 0 && r.ok; --b) {
        const uint8_t* name = r.Take(32);
        float f[12];
        const uint8_t* m = r.Take(48);
        const int16_t parent = r.Get<int16_t>();
        if (!r.ok)
            return false;
        std::memcpy(f, m, 48);
        model.names.push_back(Lower(std::string(reinterpret_cast<const char*>(name),
            strnlen(reinterpret_cast<const char*>(name), 32))));
        model.parents.push_back(parent);
        // W = [R | -(R t)]: R's rows, then t
        M4 w;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                w.r.m[i][j] = f[i * 3 + j];
        w.t = Mul(w.r, V3{ f[9], f[10], f[11] }) * -1;
        model.bind.push_back(w);
    }
    if (!r.ok)
        return false;
    for (const Part& p : model.parts)
        for (const auto& b : p.bones)
            for (int i : b)
                if (i >= int(model.names.size()))
                    return false;
    for (int p : model.parents)
        if (p >= int(model.names.size()))
            return false;
    return true;
}

// --- textures (.stx) ----------------------------------------------------------------------------------------------

struct Texture {
    int w = 0, h = 0;
    std::vector<std::array<double, 4>> px; // RGBA, sRGB, 0..1, row by row
};

// The first level of a DXT1 (format 0x40) or DXT5 (0x52) STX: its pixels follow the 0x184-byte header.
bool DecodeStx(const std::vector<uint8_t>& data, Texture& tex)
{
    if (data.size() < 0x184)
        return false;
    uint32_t w, h;
    std::memcpy(&w, data.data() + 0x0C, 4);
    std::memcpy(&h, data.data() + 0x10, 4);
    const uint8_t code = data[0x3C];
    const bool dxt5 = code == 0x52;
    if ((code != 0x40 && !dxt5) || w < 4 || h < 4 || w > 4096 || h > 4096 || w % 4 || h % 4)
        return false;
    const size_t bs = dxt5 ? 16 : 8;
    if (data.size() < 0x184 + size_t(w / 4) * (h / 4) * bs)
        return false;
    tex.w = int(w);
    tex.h = int(h);
    tex.px.assign(size_t(w) * h, {});
    const uint8_t* block = data.data() + 0x184;
    for (uint32_t by = 0; by < h / 4; ++by)
        for (uint32_t bx = 0; bx < w / 4; ++bx, block += bs) {
            const uint8_t* cb = dxt5 ? block + 8 : block;
            const int c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
            const uint32_t bits = uint32_t(cb[4]) | (uint32_t(cb[5]) << 8) | (uint32_t(cb[6]) << 16) | (uint32_t(cb[7]) << 24);
            auto rgb = [](int c) { return V3{ ((c >> 11) & 31) / 31.0, ((c >> 5) & 63) / 63.0, (c & 31) / 31.0 }; };
            const V3 a = rgb(c0), b = rgb(c1);
            const bool four = c0 > c1 || dxt5;
            V3 pal[4] = { a, b, four ? (a * 2 + b) * (1.0 / 3) : (a + b) * 0.5, four ? (a + b * 2) * (1.0 / 3) : V3{ 0, 0, 0 } };
            double alpha[16];
            if (dxt5) {
                const double a0 = block[0], a1 = block[1];
                uint64_t ab = 0;
                for (int k = 0; k < 6; ++k)
                    ab |= uint64_t(block[2 + k]) << (8 * k);
                double lv[8] = { a0, a1 };
                if (a0 > a1)
                    for (int j = 0; j < 6; ++j)
                        lv[2 + j] = ((6 - j) * a0 + (j + 1) * a1) / 7;
                else {
                    for (int j = 0; j < 4; ++j)
                        lv[2 + j] = ((4 - j) * a0 + (j + 1) * a1) / 5;
                    lv[6] = 0;
                    lv[7] = 255;
                }
                for (int i = 0; i < 16; ++i)
                    alpha[i] = lv[(ab >> (3 * i)) & 7] / 255;
            }
            for (int i = 0; i < 16; ++i) {
                const int idx = (bits >> (2 * i)) & 3;
                const V3& c = pal[idx];
                const double al = dxt5 ? alpha[i] : (!four && idx == 3 ? 0.0 : 1.0);
                tex.px[size_t(by * 4 + i / 4) * w + bx * 4 + i % 4] = { c[0], c[1], c[2], al };
            }
        }
    return true;
}

// Bilinear, wrapping (as the renderer's sampler: texel centres at +0.5).
V3 Sample(const Texture& tex, double u, double v)
{
    const double x = (u - std::floor(u)) * tex.w - 0.5, y = (v - std::floor(v)) * tex.h - 0.5;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    x0 = ((x0 % tex.w) + tex.w) % tex.w;
    y0 = ((y0 % tex.h) + tex.h) % tex.h;
    const int x1 = (x0 + 1) % tex.w, y1 = (y0 + 1) % tex.h;
    auto at = [&](int xx, int yy) { const auto& p = tex.px[size_t(yy) * tex.w + xx]; return V3{ p[0], p[1], p[2] }; };
    return at(x0, y0) * ((1 - fx) * (1 - fy)) + at(x1, y0) * (fx * (1 - fy)) + at(x0, y1) * ((1 - fx) * fy) +
        at(x1, y1) * (fx * fy);
}

// The dark side look (Sith Yoda): the texture nearly grey, darker and a little cold.
void Darken(Texture& tex, double level)
{
    for (auto& p : tex.px) {
        const double g = p[0] * 0.299 + p[1] * 0.587 + p[2] * 0.114;
        const double tint[3] = { 0.95, 0.92, 1.08 };
        for (int i = 0; i < 3; ++i)
            p[size_t(i)] = std::clamp((p[size_t(i)] + (g - p[size_t(i)]) * 0.95) * level * tint[i], 0.0, 1.0);
    }
}

// --- the animation (.bnm) -----------------------------------------------------------------------------------------

struct Key {
    int time;
    std::array<double, 4> v;
};
struct Track {
    int cid;
    std::vector<Key> rot, trans;
};
struct Clip {
    int frames = 0;
    std::vector<Track> tracks;
};

struct Bytes {
    const std::vector<uint8_t>& d;
    int operator[](size_t i) const { return i < d.size() ? d[i] : 0; }
    uint32_t U32(size_t i) const { return uint32_t((*this)[i]) | (uint32_t((*this)[i + 1]) << 8) | (uint32_t((*this)[i + 2]) << 16) | (uint32_t((*this)[i + 3]) << 24); }
    uint16_t U16(size_t i) const { return uint16_t((*this)[i] | ((*this)[i + 1] << 8)); }
    float F32(size_t i) const
    {
        const uint32_t u = U32(i);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    uint32_t Bits(size_t base, int off, int n) const
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const int b = off + i;
            v = (v << 1) | (((*this)[base + size_t(b >> 3)] >> (7 - (b & 7))) & 1);
        }
        return v;
    }
};

constexpr int kRotStride[18] = { 0, 0, 2, 3, 4, 0, 6, 7, 8, 0, 0x0A, 0x0B, 0, 9, 5, 0x12, 0, 0 };
constexpr int kTransStride[18] = { 0, 0, 2, 3, 0, 5, 0, 0, 8, 9, 0, 0, 0, 0, 0, 0x0E, 0, 0 };

int KeyTime(const Bytes& d, size_t sb, int fmt)
{
    if (fmt == 0x0F)
        return d[sb + 0x0C] | (d[sb + 0x0D] << 8);
    int hi = d[sb + size_t(fmt) - 2];
    const int lo = d[sb + size_t(fmt) - 1];
    if (fmt != 0x0B && fmt != 0x09)
        hi &= 3;
    return (hi << 8) | lo;
}

double Remap(double v, int qmin, int qspan) { return (qspan * 0.007843138) * (v + 1.0) * 0.5 + (qmin * 0.007843138 - 1.0); }

std::array<double, 4> Quat(const Bytes& d, size_t sb, int mode, const int bb[6])
{
    double x, y, z;
    auto b = [&](size_t i) { return d[sb + i]; };
    switch (mode) {
    case 2:
        x = (b(0) >> 6) * 0.6666667 - 1, y = ((b(0) >> 4) & 3) * 0.6666667 - 1, z = ((b(0) >> 2) & 3) * 0.6666667 - 1;
        break;
    case 3:
        x = (b(0) >> 4) * 0.13333334 - 1;
        y = (((b(0) & 0xF) << 1) | (b(1) >> 7)) * 0.06451613 - 1;
        z = ((b(1) >> 2) & 0x1F) * 0.06451613 - 1;
        break;
    case 4:
        x = (b(0) >> 1) * 0.015748031 - 1;
        y = (((b(0) & 1) * 0x40) + (b(1) >> 2)) * 0.015748031 - 1;
        z = (((b(1) & 3) * 0x40) + (b(2) >> 2)) * 0.007843138 - 1;
        break;
    case 6:
        x = ((b(0) << 4) + (b(1) >> 4)) * 0.0004884005 - 1;
        y = ((b(3) >> 7) + ((((b(1) & 0xF) << 8) + b(2)) * 2)) * 0.00024417043 - 1;
        z = (((b(3) & 0x7F) * 0x40) + (b(4) >> 2)) * 0.00024417043 - 1;
        break;
    case 7:
        x = ((b(0) * 0x80) + (b(1) >> 1)) * 6.103702e-05 - 1;
        y = ((((b(1) & 1) * 0x100) + b(2)) * 0x40 + (b(3) >> 2)) * 6.103702e-05 - 1;
        z = ((((b(3) & 3) * 0x100) + b(4)) * 0x40 + (b(5) >> 2)) * 3.0518044e-05 - 1;
        break;
    case 8:
        x = ((b(2) >> 6) + (((b(0) << 8) | b(1)) * 4)) * 7.629424e-06 - 1;
        y = (((b(2) & 0x3F) * 0x100 + b(3)) * 0x10 + (b(4) >> 4)) * 7.629424e-06 - 1;
        z = (((b(4) & 0x0F) * 0x100 + b(5)) * 0x40 + (b(6) >> 2)) * 7.629424e-06 - 1;
        break;
    case 0x0A: {
        const double xb = double((((b(0) << 8) | b(1)) << 7) | (b(2) >> 1));
        const double yb = double(((((((b(2) & 1) << 8) | b(3)) << 8) | b(4)) << 6) | (b(5) >> 2));
        const double zb = double(((((((b(5) & 3) << 8) | b(6)) << 8) | b(7)) << 6) | (b(8) >> 2));
        x = xb * 2.384186e-07 - 1, y = yb * 2.384186e-07 - 1, z = zb * 1.192093e-07 - 1;
        break;
    }
    case 0x0B:
        x = ((b(0) << 16) | (b(1) << 8) | b(2)) * 1.192093e-07 - 1;
        y = ((b(3) << 16) | (b(4) << 8) | b(5)) * 1.192093e-07 - 1;
        z = ((b(6) << 16) | (b(7) << 8) | b(8)) * 1.192093e-07 - 1;
        break;
    case 0x0D:
        x = d.Bits(sb, 0, 24) * 1.192093e-07 - 1, y = d.Bits(sb, 24, 24) * 1.192093e-07 - 1,
        z = d.Bits(sb, 48, 24) * 1.192093e-07 - 1;
        break;
    case 0x0E:
        x = d.Bits(sb, 0, 13) * 0.00024417043 - 1, y = d.Bits(sb, 13, 13) * 0.00024417043 - 1,
        z = d.Bits(sb, 26, 14) * 0.00012207776 - 1;
        break;
    case 0x0F: {
        x = d.F32(sb), y = d.F32(sb + 4), z = d.F32(sb + 8);
        const double w2 = 1 - x * x - y * y - z * z;
        return { x, y, z, w2 > 0 ? std::sqrt(w2) : 0.0 };
    }
    default:
        return { 0, 0, 0, 1 };
    }
    const double qx = Remap(x, bb[0], bb[1]), qy = Remap(y, bb[2], bb[3]), qz = Remap(z, bb[4], bb[5]);
    const double w2 = 1 - qx * qx - qy * qy - qz * qz;
    return { qx, qy, qz, w2 > 0 ? std::sqrt(w2) : 0.0 };
}

std::array<double, 4> Vec(const Bytes& d, size_t sb, int mode, const float mm[6])
{
    double t[3];
    auto b = [&](size_t i) { return d[sb + i]; };
    switch (mode) {
    case 2:
        t[0] = (b(0) >> 6) * 0.33333334, t[1] = ((b(0) >> 4) & 3) * 0.33333334, t[2] = ((b(0) >> 2) & 3) * 0.33333334;
        break;
    case 3:
        t[0] = (b(0) >> 4) * 0.06666667, t[1] = (((b(0) & 0xF) << 1) | (b(1) >> 7)) * 0.032258064,
        t[2] = ((b(1) >> 2) & 0x1F) * 0.032258064;
        break;
    case 5:
        t[0] = ((b(1) >> 6) + b(0) * 4) * 0.0009775171, t[1] = (((b(1) & 0x3F) * 0x10) + (b(2) >> 4)) * 0.0009775171,
        t[2] = (((b(2) & 0xF) * 0x40) + (b(3) >> 2)) * 0.0009775171;
        break;
    case 8:
        t[0] = ((b(2) >> 6) + ((b(0) << 8) | b(1)) * 4) * 3.814712e-06,
        t[1] = (((b(2) & 0x3F) * 0x100 + b(3)) * 0x10 + (b(4) >> 4)) * 3.814712e-06,
        t[2] = (((b(4) & 0x0F) * 0x100 + b(5)) * 0x40 + (b(6) >> 2)) * 3.814712e-06;
        break;
    case 9:
        t[0] = ((b(2) >> 5) + ((b(0) << 8) | b(1)) * 8) * 1.9073523e-06,
        t[1] = (((b(2) & 0x1F) * 0x100 + b(3)) * 0x40 + (b(4) >> 2)) * 1.9073523e-06,
        t[2] = (((b(4) & 3) * 0x100 + b(5)) * 0x100 + b(6)) * 3.814712e-06;
        break;
    case 0x0E:
        t[0] = d.Bits(sb, 0, 0x13) * 1.9073523e-06, t[1] = d.Bits(sb, 0x13, 0x13) * 1.9073523e-06,
        t[2] = d.Bits(sb, 0x26, 0x12) * 3.814712e-06;
        break;
    case 0x0F:
        return { d.F32(sb), d.F32(sb + 4), d.F32(sb + 8), 0 };
    default:
        return { 0, 0, 0, 0 };
    }
    return { (1 - t[0]) * mm[0] + t[0] * mm[3], (1 - t[1]) * mm[1] + t[1] * mm[4], (1 - t[2]) * mm[2] + t[2] * mm[5], 0 };
}

bool Letter(int c) // Python's str.isalpha for a Latin-1 character
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 0xAA || c == 0xB5 || c == 0xBA ||
        (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7);
}

bool ParseClip(const std::vector<uint8_t>& data, Clip& clip)
{
    if (data.size() < 0x80)
        return false;
    const Bytes d{ data };
    const int shift = Letter(data[0]) && Letter(data[1]) ? -4 : 0;
    auto at = [&](size_t o) { return size_t(int64_t(o) + shift); };
    clip.frames = int(d.U32(at(0x28)) & 0xFFFF);
    const uint32_t ntr = d.U32(at(0x2C));
    const size_t table = size_t(int64_t(d.U32(at(0x30))) + shift);
    if (ntr > 4096 || table + size_t(ntr) * 4 > data.size())
        return false;
    float mmA[6], mmB[6];
    for (int i = 0; i < 6; ++i) {
        mmA[i] = d.F32(at(0x4C) + size_t(i) * 4);
        mmB[i] = d.F32(at(0x64) + size_t(i) * 4);
    }
    struct Header {
        size_t th;
        int rk, tk;
        int64_t mmptr;
        int bb[6];
        int fmt;
        int32_t rrel, trel;
        int cid;
    };
    std::vector<Header> headers;
    std::vector<int64_t> ptrs;
    for (uint32_t i = 0; i < ntr; ++i) {
        Header h{};
        h.th = size_t(int64_t(d.U32(table + i * 4)) + shift);
        if (h.th + 0x20 > data.size())
            return false;
        h.rk = d.U16(h.th + 4);
        h.tk = d.U16(h.th + 6);
        h.mmptr = int64_t(d.U32(h.th + 8)) + shift;
        for (int k = 0; k < 6; ++k)
            h.bb[k] = d[h.th + 0x0C + size_t(k)];
        h.fmt = d[h.th + 0x12];
        h.rrel = int32_t(d.U32(h.th + 0x14));
        h.trel = int32_t(d.U32(h.th + 0x18));
        h.cid = d.U16(h.th + 0x1C);
        headers.push_back(h);
        if (std::find(ptrs.begin(), ptrs.end(), h.mmptr) == ptrs.end())
            ptrs.push_back(h.mmptr);
    }
    std::sort(ptrs.begin(), ptrs.end());
    for (const Header& h : headers) {
        const int rmode = h.fmt & 0x0F, tmode = (h.fmt >> 4) & 0x0F;
        const float* mm = ptrs.size() <= 1 || h.mmptr == ptrs[0] ? mmA : mmB;
        Track track{ h.cid, {}, {} };
        if (h.rk && kRotStride[rmode])
            for (int k = 0; k < h.rk; ++k) {
                const size_t sb = size_t(int64_t(h.th) + h.rrel + int64_t(k) * kRotStride[rmode]);
                Key key{ KeyTime(d, sb, rmode), Quat(d, sb, rmode, h.bb) };
                if (!track.rot.empty()) {
                    const auto& p = track.rot.back().v;
                    if (p[0] * key.v[0] + p[1] * key.v[1] + p[2] * key.v[2] + p[3] * key.v[3] < 0)
                        for (double& c : key.v)
                            c = -c;
                }
                track.rot.push_back(key);
            }
        if (h.tk && kTransStride[tmode])
            for (int k = 0; k < h.tk; ++k) {
                const size_t sb = size_t(int64_t(h.th) + h.trel + int64_t(k) * kTransStride[tmode]);
                track.trans.push_back({ KeyTime(d, sb, tmode), Vec(d, sb, tmode, mm) });
            }
        if (!track.rot.empty() || !track.trans.empty())
            clip.tracks.push_back(std::move(track));
    }
    return true;
}

std::array<double, 4> KeyAt(const std::vector<Key>& keys, int tm, bool quat)
{
    if (tm <= keys.front().time)
        return keys.front().v;
    if (tm >= keys.back().time)
        return keys.back().v;
    for (size_t i = 1; i < keys.size(); ++i)
        if (keys[i].time >= tm) {
            const Key &a = keys[i - 1], &b = keys[i];
            const double f = double(tm - a.time) / double(b.time - a.time);
            std::array<double, 4> v;
            for (int k = 0; k < 4; ++k)
                v[size_t(k)] = a.v[size_t(k)] * (1 - f) + b.v[size_t(k)] * f;
            if (quat) {
                const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
                for (double& c : v)
                    c /= n;
            }
            return v;
        }
    return keys.back().v;
}

M3 QuatMatrix(double x, double y, double z, double w)
{
    return { { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
        { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
        { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } } };
}

std::vector<std::string> RegistryNames(const std::string& text)
{
    std::vector<std::string> out;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos)
            end = text.size();
        std::string s;
        for (size_t i = begin; i < end; ++i)
            if (text[i] != '\r')
                s += text[i];
        const size_t a = s.find_first_not_of(" \t\v\f");
        const size_t b = s.find_last_not_of(" \t\v\f");
        s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        if (!s.empty())
            out.push_back(s);
        begin = end + 1;
    }
    return out;
}

// Each bone's world matrix (the game's space) at `frac` of the clip: a track's rotation (conjugated) replaces the
// bone's, its offset is added to the bone's bind translation; bones without a track keep their bind.
std::vector<M4> PoseWorlds(const Model& model, const Clip& clip, const std::vector<std::string>& registry, double frac)
{
    const size_t n = model.names.size();
    std::vector<M4> bindLocal(n), local(n), world(n);
    for (size_t i = 0; i < n; ++i)
        bindLocal[i] = model.parents[i] < 0 ? model.bind[i] : Mul(Inverse(model.bind[size_t(model.parents[i])]), model.bind[i]);
    local = bindLocal;
    const int frames = std::max(1, (clip.frames + 1) / 2);
    const int tm = int(std::nearbyint(frac * (frames - 1))) * 2;
    for (const Track& t : clip.tracks) {
        if (t.cid >= int(registry.size()))
            continue;
        const int bi = model.Index(registry[size_t(t.cid)]);
        if (bi < 0)
            continue;
        if (!t.rot.empty()) {
            const auto q = KeyAt(t.rot, tm, true);
            local[size_t(bi)].r = QuatMatrix(-q[0], -q[1], -q[2], q[3]);
        }
        if (!t.trans.empty()) {
            const auto v = KeyAt(t.trans, tm, false);
            local[size_t(bi)].t = bindLocal[size_t(bi)].t + V3{ v[0], v[1], v[2] };
        }
    }
    for (size_t i = 0; i < n; ++i) // parents come first
        world[i] = model.parents[i] < 0 ? local[i] : Mul(world[size_t(model.parents[i])], local[i]);
    return world;
}

// --- the scene ----------------------------------------------------------------------------------------------------

// The game's space to the render's: mirrored in x, scaled, then the armature's y up -> z up.
M4 ToScene(const M4& g)
{
    M4 m = g;
    m.r.m[0][1] = -m.r.m[0][1];
    m.r.m[0][2] = -m.r.m[0][2];
    m.r.m[1][0] = -m.r.m[1][0];
    m.r.m[2][0] = -m.r.m[2][0];
    m.t = V3{ -g.t[0], g.t[1], g.t[2] } * kScale;
    M4 out;
    for (int j = 0; j < 3; ++j) {
        out.r.m[0][j] = m.r.m[0][j];
        out.r.m[1][j] = -m.r.m[2][j];
        out.r.m[2][j] = m.r.m[1][j];
    }
    out.t = { m.t[0], -m.t[2], m.t[1] };
    return out;
}
V3 ToSceneDir(const V3& v) { return { v[0], -v[2], v[1] }; }

struct Skeleton {
    const Model& model;
    std::vector<M4> rest, now;
    std::vector<std::vector<int>> kids;

    Skeleton(const Model& m, const std::vector<M4>& worlds) : model(m)
    {
        for (size_t i = 0; i < m.names.size(); ++i) {
            rest.push_back(ToScene(m.bind[i]));
            now.push_back(ToScene(worlds[i]));
        }
        kids.resize(m.names.size());
        for (size_t i = 0; i < m.parents.size(); ++i)
            if (m.parents[i] >= 0)
                kids[size_t(m.parents[i])].push_back(int(i));
    }
    size_t I(const char* name) const { return size_t(model.Index(name)); }
    V3 Head(const char* name) const { return now[I(name)].t; }
    // The bone's axis as the importer builds it (toward its first child at rest, else its own y), turned as now.
    V3 TailDir(const char* name) const
    {
        const size_t i = I(name);
        V3 d{ 0, 0, 0 };
        bool have = false;
        if (!kids[i].empty()) {
            d = rest[size_t(kids[i][0])].t - rest[i].t;
            have = Length(d) >= 1e-9;
        }
        if (!have)
            d = { rest[i].r.m[0][1], rest[i].r.m[1][1], rest[i].r.m[2][1] };
        return Unit(Mul(Mul(now[i].r, Inverse(rest[i].r)), d));
    }
    // The bone and every bone under it turned about the bone's head, `from` -> `to`.
    void Turn(const char* name, const V3& from, const V3& to)
    {
        const size_t i = I(name);
        const V3 h = now[i].t;
        M4 g;
        g.r = RotBetween(from, to);
        g.t = h - Mul(g.r, h);
        std::vector<int> stack{ int(i) };
        while (!stack.empty()) {
            const int b = stack.back();
            stack.pop_back();
            now[size_t(b)] = Mul(g, now[size_t(b)]);
            stack.insert(stack.end(), kids[size_t(b)].begin(), kids[size_t(b)].end());
        }
    }
};

struct Mesh { // a part drawn: its points and normals in the scene
    std::vector<V3> p, n;
    const Part* part;
    const Texture* tex;
};

std::vector<Mesh> Skin(const Model& model, const Skeleton& sk)
{
    std::vector<M4> skin(sk.now.size());
    for (size_t i = 0; i < skin.size(); ++i)
        skin[i] = Mul(sk.now[i], Inverse(sk.rest[i]));
    std::vector<Mesh> out;
    for (const Part& part : model.parts) {
        Mesh m{ {}, {}, &part, nullptr };
        for (size_t v = 0; v < part.pos.size(); ++v) {
            const V3 rp = ToSceneDir(V3{ -part.pos[v][0], part.pos[v][1], part.pos[v][2] } * kScale);
            const V3 rn = ToSceneDir(V3{ -part.nrm[v][0], part.nrm[v][1], part.nrm[v][2] });
            V3 acc{ 0, 0, 0 }, nacc{ 0, 0, 0 };
            for (size_t k = 0; k < 4; ++k) {
                const M4& s = skin[size_t(part.bones[v][k])];
                const double w = part.weights[v][k];
                acc = acc + Point(s, rp) * w;
                nacc = nacc + Mul(s.r, rn) * w;
            }
            m.p.push_back(acc);
            m.n.push_back(nacc * (1 / std::max(Length(nacc), 1e-9)));
        }
        out.push_back(std::move(m));
    }
    return out;
}

// The hilt in the hand, in the wrist's frame (from the rest pose): its axis across the knuckles (index -> middle),
// in the palm.
M4 Grip(const Skeleton& sk, const char* wrist)
{
    const std::string side = std::string(wrist).substr(6, 1); // wrist_R_g
    const M4 inv = Inverse(sk.rest[sk.I(wrist)]);
    auto pos = [&](const char* b) { return Point(inv, sk.rest[sk.I((std::string(b) + "_" + side + "_g").c_str())].t); };
    const V3 index = pos("index1"), middle = pos("middle1"), thumb = pos("thumb1");
    const V3 axis = Unit(index - middle);
    const V3 knuckles = (index + middle) * 0.5;
    V3 fingers = Unit(knuckles);
    fingers = Unit(fingers - axis * Dot(fingers, axis));
    V3 palm = Unit(Cross(axis, fingers));
    if (Dot(palm, thumb - knuckles) < 0)
        palm = palm * -1;
    const V3 z = Cross(fingers, axis);
    M4 m;
    for (int i = 0; i < 3; ++i) {
        m.r.m[i][0] = fingers[size_t(i)];
        m.r.m[i][1] = axis[size_t(i)];
        m.r.m[i][2] = z[size_t(i)];
    }
    m.t = knuckles * 0.72 + palm * (1.5 * kScale);
    return m;
}

// The direction [right, up, toward the camera] in the view of a camera at `yaw` degrees.
V3 ViewDir(double yawDeg, const double v[3])
{
    const double yaw = yawDeg * kPi / 180;
    const V3 right{ std::cos(yaw), std::sin(yaw), 0 }, toward{ std::sin(yaw), -std::cos(yaw), 0 };
    return Unit(right * v[0] + V3{ 0, 0, 1 } * v[1] + toward * v[2]);
}

V3 Facing(const Skeleton& sk)
{
    V3 f{ 0, 0, 0 };
    const std::pair<const char*, double> parts[2] = { { "head_g", 0.6 }, { "spine3_g", 0.4 } };
    for (const auto& [name, weight] : parts) {
        const size_t i = sk.I(name);
        V3 d = Mul(Mul(sk.now[i].r, Inverse(sk.rest[i].r)), V3{ 0, -1, 0 });
        d[2] = 0;
        if (Length(d) > 1e-6)
            f = f + Unit(d) * weight;
    }
    return Length(f) > 1e-6 ? Unit(f) : V3{ 0, -1, 0 };
}

struct Camera {
    double yaw;
    V3 centre;
    double ortho;
};

struct Edit {
    enum Kind { Turn, Hang, Point, Aim, Grip } kind;
    std::vector<const char*> bones;
    const char* child;
    const char* from;
    double a[3];
    double b[3];
    double out, back, along;
};

struct Recipe {
    const char* clip;
    double frac, turn;
    const char* wrist;
    double bladeBase, bladeLength;
    uint8_t blade[3]; // the game's blade colour (measured on its full bodies)
    double dark, frameUnits, feet;
    std::vector<Edit> edits;
};

const Recipe& YodaRecipe(int player)
{
    static const Recipe recipes[2] = {
        // player 1: leaping toward the camera, nearly flat, his blade held out across the picture
        { "yoda_nav_stalkf", 0.8, 0, "wrist_R_g", 2.964, 22.0, { 48, 240, 80 }, 0, 125, 0.3,
            {
                { Edit::Turn, { "root_g" }, nullptr, nullptr, { 0, 1, 0 }, { 0, 0.766, 0.643 }, 0, 0, 0 },
                { Edit::Turn, { "neck_g" }, nullptr, nullptr, { 0, 0.766, 0.643 }, { 0, 1, 0 }, 0, 0, 0 },
                { Edit::Hang, { "ec_cape_FL_01_g", "ec_cape_FR_01_g", "ec_cape_BL_01_g", "ec_cape_BR_01_g",
                    "ec_cape_FL_02_g", "ec_cape_FR_02_g", "ec_cape_BL_02_g", "ec_cape_BR_02_g" },
                    nullptr, nullptr, {}, {}, 0.45, 0.6, 0 },
                { Edit::Point, { "shoulder_R_g" }, "wrist_R_g", "spine2_g", { -5, 4, 8 }, {}, 0, 0, 0 },
                { Edit::Aim, { "wrist_R_g" }, nullptr, nullptr, { 1, 0.05, 0.6 }, {}, 0, 0, 0 },
            } },
        // player 2 (Sith Yoda): on guard, the blade upright in both hands
        { "yoda_nav_run_out", 0.2, -15, "wrist_R_g", 2.964, 22.0, { 248, 40, 32 }, 0.45, 125, 0.19,
            {
                { Edit::Point, { "shoulder_R_g" }, "wrist_R_g", "spine2_g", { -3, -3, 6 }, {}, 0, 0, 0 },
                { Edit::Aim, { "wrist_R_g" }, nullptr, nullptr, { -0.2, 1, 0.1 }, {}, 0, 0, 0 },
                { Edit::Grip, { "shoulder_L_g" }, "middle1_L_g", nullptr, {}, {}, 0, 0, -2.0 },
            } },
    };
    return recipes[player ? 1 : 0];
}

struct Scene {
    const Model* model;
    const Model* hilt;
    std::vector<Mesh> meshes; // the body, then the hilt
    M4 hiltMatrix;
    V3 bladeA, bladeB;
    Camera cam;
};

// The camera: its yaw (the fighter turned `turn` degrees from it, unless kept), the picture `frameUnits` high, the
// feet line at `feet` of it, centred on the body across.
Camera Frame(const Skeleton& sk, const std::vector<Mesh>& body, const V3& bladeA, const V3& bladeB, const Recipe& rc,
    const double* yawKept)
{
    const char* feet[4] = { "ankle_L_g", "ankle_R_g", "toe_L_g", "toe_R_g" };
    double floor = 1e30, low = 1e30;
    for (const char* f : feet) {
        floor = std::min(floor, sk.rest[sk.I(f)].t[2]);
        low = std::min(low, sk.now[sk.I(f)].t[2]);
    }
    const double height = (sk.rest[sk.I("head_g")].t[2] - floor) * 1.13;
    low -= 0.04 * height;
    const double scale = rc.frameUnits * kScale;
    const V3 f = Facing(sk);
    const double turn = rc.turn * kPi / 180;
    double yaw = std::atan2(f[1], f[0]) - std::atan2(-std::cos(turn), std::sin(turn));
    if (yawKept)
        yaw = *yawKept * kPi / 180;
    const V3 right{ std::cos(yaw), std::sin(yaw), 0 };
    const V3 pelvis = sk.Head("root_pelvis_g");
    double bmin = 1e30, bmax = -1e30;
    for (const Mesh& m : body)
        for (const V3& p : m.p)
            if (Length(p - pelvis) < 0.75 * height) {
                bmin = std::min(bmin, Dot(p, right));
                bmax = std::max(bmax, Dot(p, right));
            }
    if (bmin > bmax) {
        bmin = std::min(Dot(bladeA, right), Dot(bladeB, right));
        bmax = std::max(Dot(bladeA, right), Dot(bladeB, right));
    }
    V3 centre{ 0, 0, 0 };
    const char* tracked[7] = { "head_g", "root_pelvis_g", "spine2_g", "ankle_L_g", "ankle_R_g", "toe_L_g", "toe_R_g" };
    for (const char* t : tracked)
        centre = centre + sk.Head(t);
    centre = centre * (1.0 / 7);
    const double mid = (bmax + bmin) / 2;
    centre = centre + right * (mid - Dot(centre, right));
    centre[2] = low + scale * (0.5 - rc.feet);
    return { yaw * 180 / kPi, centre, scale };
}

bool BuildScene(const Recipe& rc, const Model& model, const Model& hilt, const Clip& clip,
    const std::vector<std::string>& registry, Scene& scene, std::string& report)
{
    const char* needed[] = { "head_g", "spine3_g", "spine2_g", "root_pelvis_g", "ankle_L_g", "ankle_R_g", "toe_L_g",
        "toe_R_g", "index1_R_g", "middle1_R_g", "thumb1_R_g" };
    for (const char* n : needed)
        if (model.Index(n) < 0) {
            report = std::string("the model has no bone ") + n;
            return false;
        }
    for (const Edit& e : rc.edits) {
        for (const char* b : e.bones)
            if (model.Index(b) < 0) {
                report = std::string("the model has no bone ") + b;
                return false;
            }
        for (const char* b : { e.child, e.from })
            if (b && model.Index(b) < 0) {
                report = std::string("the model has no bone ") + b;
                return false;
            }
    }
    Skeleton sk(model, PoseWorlds(model, clip, registry, rc.frac));
    const M4 local = Grip(sk, rc.wrist);
    auto place = [&] {
        scene.hiltMatrix = Mul(sk.now[sk.I(rc.wrist)], local);
        scene.bladeA = Point(scene.hiltMatrix, { 0, rc.bladeBase * kScale, 0 });
        scene.bladeB = Point(scene.hiltMatrix, { 0, (rc.bladeBase + rc.bladeLength) * kScale, 0 });
    };
    place();
    const Camera first = Frame(sk, Skin(model, sk), scene.bladeA, scene.bladeB, rc, nullptr);
    const double yaw = first.yaw;
    const double yr = yaw * kPi / 180;
    const V3 right{ std::cos(yr), std::sin(yr), 0 }, toward{ std::sin(yr), -std::cos(yr), 0 };
    for (const Edit& e : rc.edits) {
        switch (e.kind) {
        case Edit::Turn:
            sk.Turn(e.bones[0], ViewDir(yaw, e.a), ViewDir(yaw, e.b));
            break;
        case Edit::Hang:
            for (const char* name : e.bones) {
                const V3 d = sk.TailDir(name);
                const double side = Dot(d, right) >= 0 ? 1 : -1;
                const double to[3] = { side * e.out, -1.0, -e.back };
                sk.Turn(name, d, ViewDir(yaw, to));
            }
            break;
        case Edit::Point: {
            const V3 target = sk.Head(e.from) + (right * e.a[0] + V3{ 0, 0, 1 } * e.a[1] + toward * e.a[2]) * kScale;
            const V3 root = sk.Head(e.bones[0]);
            sk.Turn(e.bones[0], sk.Head(e.child) - root, target - root);
            break;
        }
        case Edit::Aim: {
            place();
            const V3 axis{ scene.hiltMatrix.r.m[0][1], scene.hiltMatrix.r.m[1][1], scene.hiltMatrix.r.m[2][1] };
            sk.Turn(e.bones[0], axis, ViewDir(yaw, e.a));
            break;
        }
        case Edit::Grip: {
            place();
            const V3 target = Point(scene.hiltMatrix, { 0, e.along * kScale, 0 });
            const V3 root = sk.Head(e.bones[0]);
            sk.Turn(e.bones[0], sk.Head(e.child) - root, target - root);
            break;
        }
        }
        place();
    }
    scene.meshes = Skin(model, sk);
    scene.cam = Frame(sk, scene.meshes, scene.bladeA, scene.bladeB, rc, rc.edits.empty() ? nullptr : &yaw);
    for (const Part& part : hilt.parts) {
        Mesh m{ {}, {}, &part, nullptr };
        for (size_t v = 0; v < part.pos.size(); ++v) {
            m.p.push_back(Point(scene.hiltMatrix, V3{ -part.pos[v][0], part.pos[v][1], part.pos[v][2] } * kScale));
            m.n.push_back(Mul(scene.hiltMatrix.r, V3{ -part.nrm[v][0], part.nrm[v][1], part.nrm[v][2] }));
        }
        scene.meshes.push_back(std::move(m));
    }
    return true;
}

// --- the render (fb_render.py's lights, in software) --------------------------------------------------------------

constexpr int kW = 512, kH = 1024; // 4x the picture
constexpr int kShadowRes = 1024;

double ToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double ToSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

// A sun's direction toward it: its XYZ euler rotation (degrees) applied to (0, 0, -1), reversed.
V3 SunToward(double rx, double ry, double rz)
{
    rx *= kPi / 180, ry *= kPi / 180, rz *= kPi / 180;
    const M3 X{ { { 1, 0, 0 }, { 0, std::cos(rx), -std::sin(rx) }, { 0, std::sin(rx), std::cos(rx) } } };
    const M3 Y{ { { std::cos(ry), 0, std::sin(ry) }, { 0, 1, 0 }, { -std::sin(ry), 0, std::cos(ry) } } };
    const M3 Z{ { { std::cos(rz), -std::sin(rz), 0 }, { std::sin(rz), std::cos(rz), 0 }, { 0, 0, 1 } } };
    return Mul(Mul(Mul(Z, Y), X), V3{ 0, 0, -1 }) * -1;
}

struct Sun {
    double rot[3];
    double strength;
    double colour[3];
};
constexpr Sun kSuns[5] = { { { 55, 0, -35 }, 3.2, { 1.0, 0.97, 0.92 } }, { { 70, 0, 40 }, 1.2, { 0.85, 0.9, 1.0 } },
    { { 75, 0, -150 }, 4.0, { 0.75, 0.85, 1.0 } }, { { 75, 0, 150 }, 3.0, { 0.75, 0.85, 1.0 } },
    { { 0, 0, 0 }, 0.8, { 1, 1, 1 } } };
constexpr double kWorld[3] = { 0.05, 0.055, 0.07 };
const double kExposure = std::pow(2.0, 0.3);

// Rasterises a triangle (screen x, y) over pixel centres: calls f(x, y, w0, w1, w2) for each covered one.
template <typename F> void Raster(const double xs[3], const double ys[3], int w, int h, double tol, F f)
{
    const int x0 = std::max(int(std::floor(std::min({ xs[0], xs[1], xs[2] }))), 0);
    const int x1 = std::min(int(std::ceil(std::max({ xs[0], xs[1], xs[2] }))), w - 1);
    const int y0 = std::max(int(std::floor(std::min({ ys[0], ys[1], ys[2] }))), 0);
    const int y1 = std::min(int(std::ceil(std::max({ ys[0], ys[1], ys[2] }))), h - 1);
    if (x0 > x1 || y0 > y1)
        return;
    const double area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]);
    if (std::abs(area) < 1e-9)
        return;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double gx = x + 0.5, gy = y + 0.5;
            const double w0 = ((xs[1] - gx) * (ys[2] - gy) - (xs[2] - gx) * (ys[1] - gy)) / area;
            const double w1 = ((xs[2] - gx) * (ys[0] - gy) - (xs[0] - gx) * (ys[2] - gy)) / area;
            const double w2 = 1 - w0 - w1;
            if (w0 >= -tol && w1 >= -tol && w2 >= -tol)
                f(x, y, w0, w1, w2);
        }
}

// A sun's depth map: for each texel, the highest point toward the sun.
struct ShadowMap {
    V3 u, v, w;
    double lo[2], k;
    std::vector<float> top;

    ShadowMap(const std::vector<Mesh>& meshes, const V3& toward)
    {
        w = Unit(toward);
        const V3 up = std::abs(w[2]) < 0.95 ? V3{ 0, 0, 1 } : V3{ 0, 1, 0 };
        u = Unit(Cross(up, w));
        v = Cross(w, u);
        double hi[2] = { -1e30, -1e30 };
        lo[0] = lo[1] = 1e30;
        for (const Mesh& m : meshes)
            for (const V3& p : m.p) {
                lo[0] = std::min(lo[0], Dot(p, u));
                lo[1] = std::min(lo[1], Dot(p, v));
                hi[0] = std::max(hi[0], Dot(p, u));
                hi[1] = std::max(hi[1], Dot(p, v));
            }
        lo[0] -= 0.02, lo[1] -= 0.02, hi[0] += 0.02, hi[1] += 0.02;
        k = (kShadowRes - 1) / std::max(hi[0] - lo[0], hi[1] - lo[1]);
        top.assign(size_t(kShadowRes) * kShadowRes, -std::numeric_limits<float>::infinity());
        for (const Mesh& m : meshes) {
            std::vector<double> x(m.p.size()), y(m.p.size()), z(m.p.size());
            for (size_t i = 0; i < m.p.size(); ++i) {
                x[i] = (Dot(m.p[i], u) - lo[0]) * k;
                y[i] = (Dot(m.p[i], v) - lo[1]) * k;
                z[i] = Dot(m.p[i], w);
            }
            for (const auto& t : m.part->tris) {
                const double xs[3] = { x[size_t(t[0])], x[size_t(t[1])], x[size_t(t[2])] };
                const double ys[3] = { y[size_t(t[0])], y[size_t(t[1])], y[size_t(t[2])] };
                Raster(xs, ys, kShadowRes, kShadowRes, 0.01, [&](int px, int py, double w0, double w1, double w2) {
                    const double zz = w0 * z[size_t(t[0])] + w1 * z[size_t(t[1])] + w2 * z[size_t(t[2])];
                    float& at = top[size_t(py) * kShadowRes + px];
                    if (zz > at)
                        at = float(zz);
                });
            }
        }
    }

    // 1 where nothing stands between the point and the sun, 0 where something does (3x3 filtered).
    double Lit(const V3& p) const
    {
        const double x = (Dot(p, u) - lo[0]) * k, y = (Dot(p, v) - lo[1]) * k, z = Dot(p, w);
        int lit = 0;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const int xi = std::clamp(int(std::floor(x)) + dx, 0, kShadowRes - 1);
                const int yi = std::clamp(int(std::floor(y)) + dy, 0, kShadowRes - 1);
                lit += top[size_t(yi) * kShadowRes + xi] <= z + 0.01;
            }
        return lit / 9.0;
    }
};

struct Render {
    std::vector<V3> colour;      // sRGB
    std::vector<uint8_t> alpha;  // 0 / 1
    std::vector<double> depth;
    std::vector<uint8_t> blade;  // 0 / 1
};

Render Draw(const Scene& scene)
{
    const Camera& cam = scene.cam;
    const double yaw = cam.yaw * kPi / 180;
    const V3 right{ std::cos(yaw), std::sin(yaw), 0 }, fwd{ -std::sin(yaw), std::cos(yaw), 0 };
    const double k = kH / cam.ortho;
    const V3& c = cam.centre;
    const size_t n = size_t(kW) * kH;
    Render out;
    out.colour.assign(n, { 0, 0, 0 });
    out.alpha.assign(n, 0);
    out.depth.assign(n, std::numeric_limits<double>::infinity());
    // what each pixel shows (shaded afterwards): its mesh, triangle and weights
    struct Hit {
        int mesh = -1, tri = 0;
        float b[3] = {};
    };
    std::vector<Hit> hits(n);
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const Mesh& m = scene.meshes[mi];
        std::vector<double> sx(m.p.size()), sy(m.p.size()), dz(m.p.size());
        for (size_t i = 0; i < m.p.size(); ++i) {
            const V3 d = m.p[i] - c;
            sx[i] = kW / 2.0 + Dot(d, right) * k;
            sy[i] = kH / 2.0 - d[2] * k;
            dz[i] = Dot(d, fwd);
        }
        for (size_t ti = 0; ti < m.part->tris.size(); ++ti) {
            const auto& t = m.part->tris[ti];
            const double xs[3] = { sx[size_t(t[0])], sx[size_t(t[1])], sx[size_t(t[2])] };
            const double ys[3] = { sy[size_t(t[0])], sy[size_t(t[1])], sy[size_t(t[2])] };
            Raster(xs, ys, kW, kH, 0, [&](int x, int y, double w0, double w1, double w2) {
                const double z = w0 * dz[size_t(t[0])] + w1 * dz[size_t(t[1])] + w2 * dz[size_t(t[2])];
                const size_t at = size_t(y) * kW + x;
                if (z < out.depth[at]) {
                    out.depth[at] = z;
                    hits[at] = { int(mi), int(ti), { float(w0), float(w1), float(w2) } };
                }
            });
        }
    }
    // each drawn pixel's point, normal (the side facing the camera) and albedo
    struct Shade {
        size_t at;
        V3 p, n, albedo, light;
    };
    std::vector<Shade> shades;
    for (size_t at = 0; at < n; ++at) {
        const Hit& h = hits[at];
        if (h.mesh < 0)
            continue;
        const Mesh& m = scene.meshes[size_t(h.mesh)];
        const auto& t = m.part->tris[size_t(h.tri)];
        V3 p{ 0, 0, 0 }, nn{ 0, 0, 0 };
        double u = 0, v = 0;
        for (int j = 0; j < 3; ++j) {
            const size_t vi = size_t(t[size_t(j)]);
            p = p + m.p[vi] * h.b[j];
            nn = nn + m.n[vi] * h.b[j];
            u += m.part->uv[vi][0] * h.b[j];
            v += m.part->uv[vi][1] * h.b[j];
        }
        nn = nn * (1 / std::max(Length(nn), 1e-9));
        if (Dot(nn, fwd) > 0)
            nn = nn * -1;
        Shade s{ at, p, nn, { 0.5, 0.5, 0.5 }, V3{ kWorld[0], kWorld[1], kWorld[2] } * 0.8 };
        if (m.tex) {
            const V3 c3 = Sample(*m.tex, u, v);
            s.albedo = { ToLinear(c3[0]), ToLinear(c3[1]), ToLinear(c3[2]) };
        }
        shades.push_back(s);
        out.alpha[at] = 1;
    }
    std::vector<Hit>().swap(hits);
    for (const Sun& sun : kSuns) {
        const V3 L = SunToward(sun.rot[0], sun.rot[1], sun.rot[2]);
        const ShadowMap sm(scene.meshes, L);
        const V3 col = V3{ sun.colour[0], sun.colour[1], sun.colour[2] } * (sun.strength / kPi);
        for (Shade& s : shades) {
            const double facing = std::max(Dot(s.n, L), 0.0);
            if (facing > 0)
                s.light = s.light + col * (facing * sm.Lit(s.p + s.n * 0.004));
        }
    }
    for (const Shade& s : shades)
        for (int i = 0; i < 3; ++i)
            out.colour[s.at][size_t(i)] = ToSrgb(s.albedo[size_t(i)] * s.light[size_t(i)] * kExposure);
    // the blade: a cylinder of the glow's radius from the emitter, hidden where the body is in front
    const V3 a = scene.bladeA - c, b = scene.bladeB - c;
    const double pa[2] = { kW / 2.0 + Dot(a, right) * k, kH / 2.0 - a[2] * k };
    const double pb[2] = { kW / 2.0 + Dot(b, right) * k, kH / 2.0 - b[2] * k };
    const double za = Dot(a, fwd), zb = Dot(b, fwd);
    const double d[2] = { pb[0] - pa[0], pb[1] - pa[1] };
    const double l2 = std::max(d[0] * d[0] + d[1] * d[1], 1e-9);
    const double radius = 0.032 * k;
    out.blade.assign(n, 0);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const double gx = x + 0.5, gy = y + 0.5;
            const double t = std::clamp(((gx - pa[0]) * d[0] + (gy - pa[1]) * d[1]) / l2, 0.0, 1.0);
            const double dist = std::hypot(gx - (pa[0] + t * d[0]), gy - (pa[1] + t * d[1]));
            const size_t at = size_t(y) * kW + x;
            if (dist <= radius && !(out.depth[at] < za + t * (zb - za) - 1e-4))
                out.blade[at] = 1;
        }
    return out;
}

// --- the picture (fb_stx.py) --------------------------------------------------------------------------------------

constexpr int kPW = 128, kPH = 256;

struct Rgba {
    double c[4];
};
using Picture = std::vector<Rgba>; // 0..1, straight alpha

uint8_t Byte(double v) { return uint8_t(std::clamp(v * 255 + 0.5, 0.0, 255.0)); }

// For each pixel where `mask` is set, the distance to the nearest pixel where it is not (scipy's
// distance_transform_edt), looked for within `reach` pixels (farther: 1e9); 0 where it is not set.
std::vector<double> Distances(const std::vector<uint8_t>& mask, int reach)
{
    std::vector<std::pair<int, int>> offsets;
    for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx)
            if (dx * dx + dy * dy <= reach * reach)
                offsets.push_back({ dx, dy });
    std::sort(offsets.begin(), offsets.end(), [](const auto& p, const auto& q) {
        return p.first * p.first + p.second * p.second < q.first * q.first + q.second * q.second;
    });
    std::vector<double> d(mask.size(), 0);
    for (int y = 0; y < kPH; ++y)
        for (int x = 0; x < kPW; ++x) {
            const size_t at = size_t(y) * kPW + x;
            if (!mask[at])
                continue;
            d[at] = 1e9;
            for (const auto& [dx, dy] : offsets) {
                const int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < kPW && yy < kPH && !mask[size_t(yy) * kPW + xx]) {
                    d[at] = std::sqrt(double(dx * dx + dy * dy));
                    break;
                }
            }
        }
    return d;
}

// 4x4 box average of a byte image (as PIL's BOX resize).
std::vector<uint8_t> Shrink(const std::vector<uint8_t>& big)
{
    std::vector<uint8_t> small(size_t(kPW) * kPH);
    for (int y = 0; y < kPH; ++y)
        for (int x = 0; x < kPW; ++x) {
            int sum = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i)
                    sum += big[size_t(y * 4 + j) * kW + x * 4 + i];
            small[size_t(y) * kPW + x] = uint8_t((sum + 8) / 16);
        }
    return small;
}

// The body at the picture's size: premultiplied, averaged, then straight again (bytes).
std::vector<std::array<uint8_t, 4>> Compose(const Render& r)
{
    std::vector<uint8_t> ch[4];
    for (auto& c : ch)
        c.resize(size_t(kW) * kH);
    for (size_t at = 0; at < ch[0].size(); ++at) {
        for (int i = 0; i < 3; ++i)
            ch[i][at] = Byte(r.colour[at][size_t(i)] * r.alpha[at]);
        ch[3][at] = r.alpha[at] ? 255 : 0;
    }
    std::vector<uint8_t> small[4];
    for (int i = 0; i < 4; ++i)
        small[i] = Shrink(ch[i]);
    std::vector<std::array<uint8_t, 4>> out(size_t(kPW) * kPH);
    for (size_t at = 0; at < out.size(); ++at) {
        const double a = small[3][at] / 255.0;
        for (int i = 0; i < 3; ++i)
            out[at][size_t(i)] = a > 1e-3 ? Byte((small[i][at] / 255.0) / std::max(a, 1e-3)) : 0;
        out[at][3] = small[3][at];
    }
    return out;
}

// The game's own full bodies' grade (measured on them): deep blacks, a cool cast, richer colours, a blue rim light.
Picture Grade(const std::vector<std::array<uint8_t, 4>>& in)
{
    std::vector<uint8_t> inside(in.size());
    for (size_t at = 0; at < in.size(); ++at)
        inside[at] = in[at][3] / 255.0 > 0.5;
    const std::vector<double> d = Distances(inside, 10);
    Picture out(in.size());
    const double black = 0.08, white = 1.0, sat = 1.6, tint[3] = { 0.97, 1.0, 1.07 }, rimRgb[3] = { 0.55, 0.68, 1.0 };
    for (size_t at = 0; at < in.size(); ++at) {
        double rgb[3];
        for (int i = 0; i < 3; ++i)
            rgb[i] = std::clamp((in[at][size_t(i)] / 255.0 - black) / (white - black), 0.0, 1.0);
        const double lum = rgb[0] * 0.299 + rgb[1] * 0.587 + rgb[2] * 0.114;
        const double rim = inside[at] ? std::clamp(1 - (d[at] - 1) / 2.5, 0.0, 1.0) * 0.35 : 0.0;
        for (int i = 0; i < 3; ++i) {
            double v = std::clamp(lum + (rgb[i] - lum) * sat, 0.0, 1.0);
            v = std::clamp(v * tint[i], 0.0, 1.0);
            v = std::clamp(v + rimRgb[i] * rim, 0.0, 1.0);
            out[at].c[i] = Byte(v) / 255.0;
        }
        out[at].c[3] = Byte(in[at][3] / 255.0) / 255.0;
    }
    return out;
}

// The game's blades (measured on its full bodies): a white core, then a narrow band of pure colour and a short fade.
Picture BladeLayer(const Render& r, const uint8_t colour[3])
{
    std::vector<uint8_t> big(r.blade.size());
    for (size_t at = 0; at < big.size(); ++at)
        big[at] = r.blade[at] ? 255 : 0;
    const std::vector<uint8_t> small = Shrink(big);
    std::vector<uint8_t> outside(small.size());
    for (size_t at = 0; at < small.size(); ++at)
        outside[at] = !(small[at] / 255.0 > 0.45);
    const std::vector<double> d = Distances(outside, 10);
    const double profile[4][2] = { { 1.0, 0.95 }, { 2.0, 0.75 }, { 3.0, 0.35 }, { 4.0, 0.1 } };
    const double core[3] = { 248, 252, 248 };
    Picture out(small.size());
    for (size_t at = 0; at < small.size(); ++at) {
        double a = 0, prev = 0;
        for (const auto& p : profile) {
            if (d[at] > prev && d[at] <= p[0])
                a = p[1];
            prev = p[0];
        }
        if (!outside[at])
            a = 1;
        const double white = std::clamp(1.0 - d[at] / 0.9, 0.0, 1.0);
        for (int i = 0; i < 3; ++i)
            out[at].c[i] = white * core[i] / 255 + (1 - white) * colour[i] / 255.0;
        out[at].c[3] = a;
    }
    return out;
}

// The halo the game's own full bodies have around the figure: a blue-grey glow fading over ~7 px.
std::vector<std::array<uint8_t, 4>> AddHalo(const std::vector<std::array<uint8_t, 4>>& in)
{
    std::vector<uint8_t> clear(in.size());
    for (size_t at = 0; at < in.size(); ++at)
        clear[at] = in[at][3] / 255.0 < 0.9;
    const std::vector<double> d = Distances(clear, 10);
    const double rgb[3] = { 60 / 255.0, 80 / 255.0, 115 / 255.0 };
    std::vector<std::array<uint8_t, 4>> out(in.size());
    for (size_t at = 0; at < in.size(); ++at) {
        double halo = d[at] >= 1 ? 0.65 * std::exp(-0.47 * (d[at] - 1)) : 0.65;
        if (d[at] > 9)
            halo = 0;
        const double a = in[at][3] / 255.0;
        const double oa = a + halo * (1 - a);
        for (int i = 0; i < 3; ++i)
            out[at][size_t(i)] = Byte((in[at][size_t(i)] / 255.0 * a + rgb[i] * halo * (1 - a)) / std::max(oa, 1e-4));
        out[at][3] = Byte(oa);
    }
    return out;
}

// --- DXT5 ---------------------------------------------------------------------------------------------------------

int To565(const double c[3])
{
    int v[3];
    for (int i = 0; i < 3; ++i)
        v[i] = int(std::clamp(std::nearbyint(c[i]), 0.0, 255.0));
    return (v[0] * 31 / 255) << 11 | (v[1] * 63 / 255) << 5 | (v[2] * 31 / 255);
}
V3 From565(int v) { return { double((v >> 11 & 31) * 255 / 31), double((v >> 5 & 63) * 255 / 63), double((v & 31) * 255 / 31) }; }

// The largest eigenvalue's eigenvector of a symmetric 3x3 matrix (Jacobi).
V3 MainAxis(M3 a)
{
    M3 v{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a.m[0][1] * a.m[0][1] + a.m[0][2] * a.m[0][2] + a.m[1][2] * a.m[1][2];
        if (off < 1e-18)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a.m[p][q]) < 1e-30)
                    continue;
                const double theta = (a.m[q][q] - a.m[p][p]) / (2 * a.m[p][q]);
                const double t = (theta >= 0 ? 1 : -1) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double cs = 1 / std::sqrt(t * t + 1), sn = t * cs;
                for (int k = 0; k < 3; ++k) { // a = J^T a J
                    const double akp = a.m[k][p], akq = a.m[k][q];
                    a.m[k][p] = cs * akp - sn * akq;
                    a.m[k][q] = sn * akp + cs * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a.m[p][k], aqk = a.m[q][k];
                    a.m[p][k] = cs * apk - sn * aqk;
                    a.m[q][k] = sn * apk + cs * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v.m[k][p], vkq = v.m[k][q];
                    v.m[k][p] = cs * vkp - sn * vkq;
                    v.m[k][q] = sn * vkp + cs * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (a.m[i][i] > a.m[best][best])
            best = i;
    return { v.m[0][best], v.m[1][best], v.m[2][best] };
}

double Percentile(std::vector<double> t, double q)
{
    std::sort(t.begin(), t.end());
    const double pos = q / 100 * double(t.size() - 1);
    const size_t lo = size_t(std::floor(pos));
    const size_t hi = std::min(lo + 1, t.size() - 1);
    return t[lo] + (t[hi] - t[lo]) * (pos - double(lo));
}

void Put16(std::vector<uint8_t>& out, int v)
{
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}

// 8 bytes: endpoints along the block's main axis, four colours.
void ColourBlock(const V3 px[16], std::vector<uint8_t>& out)
{
    V3 mean{ 0, 0, 0 };
    for (int i = 0; i < 16; ++i)
        mean = mean + px[i];
    mean = mean * (1.0 / 16);
    M3 cov{};
    for (int i = 0; i < 16; ++i) {
        const V3 d = px[i] - mean;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cov.m[r][c] += d[size_t(r)] * d[size_t(c)];
    }
    const V3 axis = MainAxis(cov);
    std::vector<double> t(16);
    for (int i = 0; i < 16; ++i)
        t[size_t(i)] = Dot(px[i] - mean, axis);
    double bestErr = -1;
    int bestC0 = 0, bestC1 = 0, bestIdx[16] = {};
    const double quantiles[3][2] = { { 0, 100 }, { 3, 97 }, { 8, 92 } };
    for (const auto& q : quantiles) {
        const V3 e0 = mean + axis * Percentile(t, q[1]), e1 = mean + axis * Percentile(t, q[0]);
        int c0 = To565(e0.data()), c1 = To565(e1.data());
        if (c0 < c1)
            std::swap(c0, c1);
        V3 pal[4];
        if (c0 == c1) {
            pal[0] = pal[1] = pal[2] = pal[3] = From565(c0);
        } else {
            const V3 p0 = From565(c0), p1 = From565(c1);
            pal[0] = p0, pal[1] = p1, pal[2] = (p0 * 2 + p1) * (1.0 / 3), pal[3] = (p0 + p1 * 2) * (1.0 / 3);
        }
        double err = 0;
        int idx[16];
        for (int i = 0; i < 16; ++i) {
            double best = 1e30;
            for (int j = 0; j < 4; ++j) {
                const V3 e = px[i] - pal[j];
                const double dd = Dot(e, e);
                if (dd < best) {
                    best = dd;
                    idx[i] = j;
                }
            }
            err += best;
        }
        if (bestErr < 0 || err < bestErr) {
            bestErr = err;
            bestC0 = c0, bestC1 = c1;
            std::copy(idx, idx + 16, bestIdx);
        }
    }
    Put16(out, bestC0);
    Put16(out, bestC1);
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= uint32_t(bestIdx[i]) << (2 * i);
    for (int k = 0; k < 4; ++k)
        out.push_back(uint8_t(bits >> (8 * k)));
}

void AlphaBlock(const int a[16], std::vector<uint8_t>& out)
{
    const int a0 = *std::max_element(a, a + 16), a1 = *std::min_element(a, a + 16);
    out.push_back(uint8_t(a0));
    out.push_back(uint8_t(a1));
    uint64_t bits = 0;
    if (a0 != a1) {
        double pal[8] = { double(a0), double(a1) };
        for (int j = 0; j < 6; ++j)
            pal[2 + j] = ((6 - j) * a0 + (j + 1) * a1) / 7.0;
        for (int i = 0; i < 16; ++i) {
            int best = 0;
            for (int j = 1; j < 8; ++j)
                if (std::abs(a[i] - pal[j]) < std::abs(a[i] - pal[best]))
                    best = j;
            bits |= uint64_t(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k)
        out.push_back(uint8_t(bits >> (8 * k)));
}

// The header the game's own full-body pictures have (128x256, DXT5 0x52, one level).
std::vector<uint8_t> StxHeader(const char* name, uint32_t size)
{
    std::vector<uint8_t> h(0x184, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(h.data() + at, &v, 4); };
    std::memcpy(h.data(), "STX", 4);
    const uint32_t head[8] = { 4, 4, 128, 256, 128, 256, 127, 0 };
    for (int i = 0; i < 8; ++i)
        put(4 + size_t(i) * 4, head[i]);
    put(0x24, 1);
    put(0x28, 1);
    put(0x30, 3);
    put(0x34, size);
    put(0x38, 0x80);
    h[0x3C] = 0x52;
    std::memcpy(h.data() + 0x40, name, std::min<size_t>(std::strlen(name), 0x28));
    put(0x68, 2);
    put(0x6C, 10);
    put(0x84, 0x8080);
    put(0x88, 0x80);
    put(0x8C, 0x8080);
    put(0x90, 0x8080);
    std::memcpy(h.data() + 0x104, "BIR", 4);
    return h;
}

std::vector<uint8_t> Encode(const std::vector<std::array<uint8_t, 4>>& rgba, const char* name)
{
    std::vector<uint8_t> blocks;
    for (int by = 0; by < kPH; by += 4)
        for (int bx = 0; bx < kPW; bx += 4) {
            V3 px[16];
            int a[16];
            for (int i = 0; i < 16; ++i) {
                const auto& p = rgba[size_t(by + i / 4) * kPW + bx + i % 4];
                px[i] = { double(p[0]), double(p[1]), double(p[2]) };
                a[i] = p[3];
            }
            AlphaBlock(a, blocks);
            ColourBlock(px, blocks);
        }
    std::vector<uint8_t> out = StxHeader(name, uint32_t(0x80 + blocks.size()));
    out.insert(out.end(), blocks.begin(), blocks.end());
    return out;
}

} // namespace

std::vector<uint8_t> MakeYodaPicture(int player, const ReadResource& read, const char* textureName, std::string& report)
{
    const Recipe& rc = YodaRecipe(player);
    const std::string modelName = "meshes\\chars\\yoda\\yoda.msh", hiltName = "meshes\\weapons\\lsaberyoda\\lsaberyoda.msh";
    const std::string clipName = std::string("animation\\yoda\\") + rc.clip + ".bnm";
    std::vector<uint8_t> modelData, hiltData, clipData, boneNames;
    Model model, hilt;
    Clip clip;
    if (!read(modelName, modelData) || !ReadModel(modelData, true, "shadow", model)) {
        report = modelName + " could not be read";
        return {};
    }
    if (!read(hiltName, hiltData) || !ReadModel(hiltData, false, nullptr, hilt)) {
        report = hiltName + " could not be read";
        return {};
    }
    if (!read(clipName, clipData) || !ParseClip(clipData, clip)) {
        report = clipName + " could not be read";
        return {};
    }
    if (!read("gameinfo\\bonename.txt", boneNames)) {
        report = "gameinfo\\bonename.txt could not be read";
        return {};
    }
    const std::vector<std::string> registry = RegistryNames(std::string(boneNames.begin(), boneNames.end()));

    // the textures (one missing is drawn grey, as in the renders)
    std::vector<Texture> modelTex(model.textures.size()), hiltTex(hilt.textures.size());
    std::vector<bool> modelHas(model.textures.size()), hiltHas(hilt.textures.size());
    for (size_t i = 0; i < model.textures.size(); ++i) {
        std::vector<uint8_t> data;
        modelHas[i] = read("meshes\\chars\\yoda\\" + model.textures[i] + ".stx", data) && DecodeStx(data, modelTex[i]);
        if (modelHas[i] && rc.dark > 0)
            Darken(modelTex[i], rc.dark);
    }
    for (size_t i = 0; i < hilt.textures.size(); ++i) {
        std::vector<uint8_t> data;
        hiltHas[i] = read("meshes\\weapons\\lsaberyoda\\" + hilt.textures[i] + ".stx", data) && DecodeStx(data, hiltTex[i]);
    }

    Scene scene;
    if (!BuildScene(rc, model, hilt, clip, registry, scene, report))
        return {};
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        Mesh& m = scene.meshes[i];
        const bool ofHilt = i >= model.parts.size();
        const int t = m.part->texture;
        if (t >= 0 && (ofHilt ? hiltHas[size_t(t)] : modelHas[size_t(t)]))
            m.tex = ofHilt ? &hiltTex[size_t(t)] : &modelTex[size_t(t)];
    }
    const Render render = Draw(scene);

    // the body graded, the blade over it, the halo around them
    Picture body = Grade(Compose(render));
    const Picture blade = BladeLayer(render, rc.blade);
    std::vector<std::array<uint8_t, 4>> rgba(body.size());
    for (size_t at = 0; at < body.size(); ++at) {
        const double ta = blade[at].c[3], ua = body[at].c[3];
        const double oa = ta + ua * (1 - ta);
        for (int i = 0; i < 3; ++i)
            rgba[at][size_t(i)] = Byte((blade[at].c[i] * ta + body[at].c[i] * ua * (1 - ta)) / std::max(oa, 1e-4));
        rgba[at][3] = Byte(oa);
    }
    return Encode(AddHalo(rgba), textureName);
}

} // namespace swrots::game::fullbody
