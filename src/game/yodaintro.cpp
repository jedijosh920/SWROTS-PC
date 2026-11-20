// Yoda's Versus intro clip, cut from his bonus mission's opening cutscene (see yodaintro.h).
//
// A clip's memory image (the cutscene's actors' are the same): u32 its size, its name (0x20), a float, u16 its length
// (time units of 1/60 s), two bytes, u32 its track count, u32 the offset of its track table (0x80), 0x4C and 0x64 two
// sets of translation bounds (min xyz, max xyz), then the table and the tracks. A track: u32 its size (header and
// keys), u16 rotation keys, u16 translation keys, u32 the bounds it uses (one of two values, the lower the first set),
// six bytes of rotation bounds (min and span per axis, in 1/127.5 from -1), its key formats (rotation in the low
// nibble), a byte, s32 rotation keys' and translation keys' offsets from the track, u16 the bone (an index into
// gameinfo\bonename.txt), two bytes. The cutscene's tracks are all of format 0x9B: rotation keys of 11 bytes (x, y, z
// in 24 bits each, the time in 16), translation keys of 9 (19, 19 and 18 bits between the bounds, the time in 16).

#include "game/yodaintro.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

namespace swrots::game::yodaintro {

namespace {

constexpr const char* kActor = "R1_Yoda_baseShape";
constexpr const char* kTemplate = "animation\\yoda\\yoda_nav_stalkidle.bnm"; // one of his own clips
constexpr int kFormat = 0x9B;
constexpr int kRotStride = 11, kTransStride = 9;
constexpr double kRotUnit = 1.192093e-07, kTransUnit = 1.9073523e-06, kTransUnitZ = 3.814712e-06;

uint32_t U32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void Put16(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

using Quat = std::array<double, 4>; // x, y, z, w
using Vec = std::array<double, 3>;

double Low(int qmin) { return qmin * 0.007843138 - 1.0; }
double Span(int qspan) { return qspan * 0.007843138; }

Quat DecodeRot(const uint8_t* k, const uint8_t bb[6])
{
    Quat q{};
    double sum = 0;
    for (int i = 0; i < 3; ++i) {
        const uint32_t v = (uint32_t(k[i * 3]) << 16) | (uint32_t(k[i * 3 + 1]) << 8) | k[i * 3 + 2];
        const double x = v * kRotUnit - 1;
        q[size_t(i)] = Span(bb[i * 2 + 1]) * (x + 1.0) * 0.5 + Low(bb[i * 2]);
        sum += q[size_t(i)] * q[size_t(i)];
    }
    q[3] = sum < 1 ? std::sqrt(1 - sum) : 0.0;
    return q;
}

void EncodeRot(Quat q, const uint8_t bb[6], uint8_t* k)
{
    if (q[3] < 0)
        for (double& c : q)
            c = -c;
    for (int i = 0; i < 3; ++i) {
        const double span = Span(bb[i * 2 + 1]);
        const double x = span > 0 ? (q[size_t(i)] - Low(bb[i * 2])) / (span * 0.5) - 1 : -1;
        const uint32_t v = uint32_t(std::clamp(std::nearbyint((x + 1) / kRotUnit), 0.0, double(0xFFFFFF)));
        k[i * 3] = uint8_t(v >> 16);
        k[i * 3 + 1] = uint8_t(v >> 8);
        k[i * 3 + 2] = uint8_t(v);
    }
}

Vec DecodeTrans(const uint8_t* k, const float mm[6])
{
    const uint32_t b0 = (uint32_t((k[0] << 8) | k[1]) << 3) | (k[2] >> 5);
    const uint32_t b1 = (uint32_t(k[2] & 0x1F) << 14) | (uint32_t(k[3]) << 6) | (k[4] >> 2);
    const uint32_t b2 = (uint32_t(k[4] & 3) << 16) | (uint32_t(k[5]) << 8) | k[6];
    const double t[3] = { b0 * kTransUnit, b1 * kTransUnit, b2 * kTransUnitZ };
    return { (1 - t[0]) * mm[0] + t[0] * mm[3], (1 - t[1]) * mm[1] + t[1] * mm[4], (1 - t[2]) * mm[2] + t[2] * mm[5] };
}

void EncodeTrans(const Vec& v, const float mm[6], uint8_t* k)
{
    uint32_t b[3];
    for (int i = 0; i < 3; ++i) {
        const double range = double(mm[3 + i]) - mm[i];
        const double t = range != 0 ? std::clamp((v[size_t(i)] - mm[i]) / range, 0.0, 1.0) : 0.0;
        const double unit = i < 2 ? kTransUnit : kTransUnitZ;
        const double most = i < 2 ? double((1 << 19) - 1) : double((1 << 18) - 1);
        b[i] = uint32_t(std::clamp(std::nearbyint(t / unit), 0.0, most));
    }
    k[0] = uint8_t(b[0] >> 11);
    k[1] = uint8_t(b[0] >> 3);
    k[2] = uint8_t(((b[0] & 7) << 5) | (b[1] >> 14));
    k[3] = uint8_t(b[1] >> 6);
    k[4] = uint8_t(((b[1] & 0x3F) << 2) | (b[2] >> 16));
    k[5] = uint8_t(b[2] >> 8);
    k[6] = uint8_t(b[2]);
}

int KeyTime(const uint8_t* k, int stride) { return (k[stride - 2] << 8) | k[stride - 1]; }
void SetKeyTime(uint8_t* k, int stride, int time)
{
    k[stride - 2] = uint8_t(time >> 8);
    k[stride - 1] = uint8_t(time);
}

// The keys from `start` on, moved to start at 0: a key made at `start` from the two around it (unless one is there),
// then the later ones as they are. `decode` / `encode` read and write a key's value (without its time).
template <typename T, typename Decode, typename Encode, typename Mix>
std::vector<uint8_t> CutKeys(const uint8_t* keys, int count, int stride, int start, Decode decode, Encode encode, Mix mix)
{
    std::vector<uint8_t> out;
    int first = 0;
    while (first < count && KeyTime(keys + first * stride, stride) < start)
        ++first;
    const bool exact = first < count && KeyTime(keys + first * stride, stride) == start;
    if (!exact) {
        T value;
        if (first == 0)
            value = decode(keys);
        else if (first == count)
            value = decode(keys + (count - 1) * stride);
        else {
            const uint8_t* a = keys + (first - 1) * stride;
            const uint8_t* b = keys + first * stride;
            const double f = double(start - KeyTime(a, stride)) / double(KeyTime(b, stride) - KeyTime(a, stride));
            value = mix(decode(a), decode(b), f);
        }
        std::vector<uint8_t> key(size_t(stride), 0);
        encode(value, key.data());
        SetKeyTime(key.data(), stride, 0);
        out.insert(out.end(), key.begin(), key.end());
    }
    for (int i = first; i < count; ++i) {
        const uint8_t* k = keys + i * stride;
        const size_t at = out.size();
        out.insert(out.end(), k, k + stride);
        SetKeyTime(out.data() + at, stride, KeyTime(k, stride) - start);
    }
    return out;
}

Quat MixQuat(const Quat& a, Quat b, double f)
{
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0)
        for (double& c : b)
            c = -c;
    Quat q;
    double n = 0;
    for (size_t i = 0; i < 4; ++i) {
        q[i] = a[i] * (1 - f) + b[i] * f;
        n += q[i] * q[i];
    }
    n = std::sqrt(n);
    for (double& c : q)
        c /= n > 0 ? n : 1;
    return q;
}

Vec MixVec(const Vec& a, const Vec& b, double f)
{
    return { a[0] * (1 - f) + b[0] * f, a[1] * (1 - f) + b[1] * f, a[2] * (1 - f) + b[2] * f };
}


std::vector<std::string> BoneNames(const std::vector<uint8_t>& text)
{
    std::vector<std::string> out;
    std::string line;
    auto flush = [&] {
        const size_t a = line.find_first_not_of(" \t\r");
        const size_t b = line.find_last_not_of(" \t\r");
        std::string s = a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        if (!s.empty())
            out.push_back(s);
        line.clear();
    };
    for (uint8_t c : text) {
        if (c == '\n')
            flush();
        else
            line += char(c);
    }
    flush();
    return out;
}

int BoneIndex(const std::vector<std::string>& names, const char* name)
{
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i].size() == std::strlen(name) &&
            std::equal(names[i].begin(), names[i].end(), name, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            return int(i);
    return -1;
}

} // namespace

std::vector<uint8_t> MakeIntroClip(const fullbody::ReadResource& read, std::string& report)
{
    std::vector<uint8_t> cin, boneText;
    if (!read(kCutscene, cin)) {
        report = std::string(kCutscene) + " could not be read";
        return {};
    }
    if (!read("gameinfo\\bonename.txt", boneText)) {
        report = "gameinfo\\bonename.txt could not be read";
        return {};
    }
    // the clip's header and its last section (0x38) are those of one of his own clips: the game's clips all have
    // that section (an extra track of its own), the cutscene's not, and the loader is the gameplay clips' one
    std::vector<uint8_t> model;
    if (!read(kTemplate, model) || model.size() < 0x80 || U32(model.data()) > model.size() ||
        U32(model.data() + 0x38) < 0x80 || U32(model.data() + 0x38) >= U32(model.data()) ||
        U32(model.data() + 0x38) + 4 > model.size() ||
        U32(model.data() + U32(model.data() + 0x38)) > U32(model.data()) - U32(model.data() + 0x38)) {
        report = std::string(kTemplate) + " could not be read";
        return {};
    }
    const std::vector<uint8_t> extra(model.begin() + U32(model.data() + 0x38),
        model.begin() + U32(model.data() + 0x38) + U32(model.data() + U32(model.data() + 0x38)));
    uint32_t modelLowest = 0xFFFFFFFF;
    for (uint32_t i = 0; i < U32(model.data() + 0x2C) && U32(model.data() + 0x30) + i * 4 + 4 <= model.size(); ++i) {
        const uint32_t th = U32(model.data() + U32(model.data() + 0x30) + i * 4);
        if (th + 12 <= model.size())
            modelLowest = std::min(modelLowest, U32(model.data() + th + 8));
    }
    const std::vector<std::string> bones = BoneNames(boneText);
    const int origin = BoneIndex(bones, "origin"), root = BoneIndex(bones, "root_g");
    // the actor's clip: its size, then its name
    const size_t nameLength = std::strlen(kActor);
    size_t at = 0;
    for (size_t i = 4; i + nameLength + 1 <= cin.size(); ++i)
        if (std::memcmp(cin.data() + i, kActor, nameLength + 1) == 0) {
            at = i - 4;
            break;
        }
    if (!at || at + 0x80 > cin.size() || U32(cin.data() + at) > cin.size() - at || U32(cin.data() + at) < 0x80) {
        report = std::string(kActor) + "'s animation is not in the cutscene";
        return {};
    }
    const uint8_t* clip = cin.data() + at;
    const uint32_t size = U32(clip);
    const int length = U16(clip + 0x28);
    const uint32_t tracks = U32(clip + 0x2C), table = U32(clip + 0x30);
    if (length <= kStartTime || tracks > 256 || table + tracks * 4 > size) {
        report = "the cutscene's animation is not as expected";
        return {};
    }
    float mmA[6], mmB[6];
    std::memcpy(mmA, clip + 0x4C, sizeof mmA);
    std::memcpy(mmB, clip + 0x64, sizeof mmB);
    uint32_t lowest = 0xFFFFFFFF;
    for (uint32_t i = 0; i < tracks; ++i) {
        const uint32_t th = U32(clip + table + i * 4);
        if (th + 0x20 > size) {
            report = "the cutscene's animation is not as expected";
            return {};
        }
        lowest = std::min(lowest, U32(clip + th + 8));
    }

    // the new clip: the header, the table, the tracks (each from a 4-byte boundary)
    std::vector<uint8_t> out(model.begin(), model.begin() + 0x80);
    std::memset(out.data() + 4, 0, 0x20);
    std::memcpy(out.data() + 4, kClipName, std::strlen(kClipName));
    Put16(out.data() + 0x28, uint32_t(length - kStartTime));
    Put32(out.data() + 0x30, 0x80);
    std::memcpy(out.data() + 0x4C, mmA, sizeof mmA); // the cutscene's bounds (its keys are kept)
    // the second bounds, widened to hold the root's lift and the origin at 0
    float liftedB[6];
    std::memcpy(liftedB, mmB, sizeof mmB);
    for (int i = 0; i < 3; ++i) {
        liftedB[i] = std::min(liftedB[i], 0.0f);
        liftedB[3 + i] = std::max(liftedB[3 + i], 0.0f);
    }
    liftedB[4] = std::max(liftedB[4], float(kLift) + 1);
    std::memcpy(out.data() + 0x64, liftedB, sizeof liftedB);
    // and as the first set too: when every track names the same set (the cutscene's world bounds served only its
    // origin), the loader takes the first (as tools/meshes' reader does)
    std::memcpy(out.data() + 0x4C, liftedB, sizeof liftedB);
    std::vector<std::vector<uint8_t>> made;
    for (uint32_t i = 0; i < tracks; ++i) {
        const uint8_t* th = clip + U32(clip + table + i * 4);
        const int rk = U16(th + 4), tk = U16(th + 6);
        const int cid = U16(th + 0x1C);
        if (cid == origin)
            continue; // the cutscene's position in its level
        if (th[0x12] != kFormat) {
            report = "a track of the cutscene's animation has another key format";
            return {};
        }
        const int32_t rrel = int32_t(U32(th + 0x14)), trel = int32_t(U32(th + 0x18));
        const uint8_t* rot = th + rrel;
        const uint8_t* trans = th + trel;
        if (rrel < 0x20 || trel < 0x20 || size_t(rot - clip) + size_t(rk) * kRotStride > size || size_t(trans - clip) + size_t(tk) * kTransStride > size) {
            report = "a track of the cutscene's animation is not as expected";
            return {};
        }
        uint8_t bb[6];
        std::memcpy(bb, th + 0x0C, 6);
        const bool second = U32(th + 8) != lowest;
        const float* mm = second ? mmB : mmA;
        std::vector<uint8_t> rotKeys, transKeys;
        if (rk > 0) {
            rotKeys = CutKeys<Quat>(rot, rk, kRotStride, kStartTime, [&](const uint8_t* k) { return DecodeRot(k, bb); },
                [&](const Quat& q, uint8_t* k) { EncodeRot(q, bb, k); }, &MixQuat);
        }
        if (tk > 0)
            transKeys = CutKeys<Vec>(trans, tk, kTransStride, kStartTime,
                [&](const uint8_t* k) { return DecodeTrans(k, mm); }, [&](const Vec& v, uint8_t* k) { EncodeTrans(v, mm, k); },
                &MixVec);
        // the second bounds widened (liftedB): its keys written again in them
        if (second)
            for (size_t k = 0; k + kTransStride <= transKeys.size(); k += kTransStride)
                EncodeTrans(DecodeTrans(transKeys.data() + k, mmB), liftedB, transKeys.data() + k);
        // the root raised to stand as in his own clips (kLift), in the second bounds
        const bool lifted = cid == root && tk == 0;
        if (lifted) {
            transKeys.assign(kTransStride, 0);
            EncodeTrans({ 0, kLift, 0 }, liftedB, transKeys.data());
            SetKeyTime(transKeys.data(), kTransStride, 0);
        }
        std::vector<uint8_t> track(th, th + 0x20);
        track.insert(track.end(), rotKeys.begin(), rotKeys.end());
        track.insert(track.end(), transKeys.begin(), transKeys.end());
        Put32(track.data(), uint32_t(track.size()));
        Put16(track.data() + 4, uint32_t(rotKeys.size() / kRotStride));
        Put16(track.data() + 6, uint32_t(transKeys.size() / kTransStride));
        std::memcpy(track.data() + 0x0C, bb, 6);
        // which bounds: as the model clip names its first set (the second 0x18 bytes after)
        Put32(track.data() + 8, modelLowest + (second || lifted ? 0x18 : 0));
        Put32(track.data() + 0x14, 0x20);
        Put32(track.data() + 0x18, uint32_t(0x20 + rotKeys.size()));
        made.push_back(std::move(track));
    }
    // his own "origin" at 0 (the duel places a fighter by his intro's origin: without one, he stays where the round
    // put him, away from his intro camera): one key, in the cutscene's second bounds (they hold 0)
    if (origin >= 0) {
        std::vector<uint8_t> track(0x20 + kTransStride, 0);
        Put32(track.data(), uint32_t(track.size()));
        Put16(track.data() + 6, 1);
        Put32(track.data() + 8, modelLowest + 0x18);
        track[0x12] = uint8_t(kFormat);
        Put32(track.data() + 0x14, 0x20);
        Put32(track.data() + 0x18, 0x20);
        Put16(track.data() + 0x1C, uint32_t(origin));
        EncodeTrans({ 0, 0, 0 }, liftedB, track.data() + 0x20);
        SetKeyTime(track.data() + 0x20, kTransStride, 0);
        made.insert(made.begin(), std::move(track));
    }
    Put32(out.data() + 0x2C, uint32_t(made.size()));
    out.resize(0x80 + made.size() * 4, 0);
    for (size_t i = 0; i < made.size(); ++i) {
        out.resize((out.size() + 3) & ~size_t(3), 0);
        Put32(out.data() + 0x80 + i * 4, uint32_t(out.size()));
        out.insert(out.end(), made[i].begin(), made[i].end());
    }
    out.resize((out.size() + 3) & ~size_t(3), 0);
    Put32(out.data() + 0x38, uint32_t(out.size()));
    out.insert(out.end(), extra.begin(), extra.end());
    Put32(out.data(), uint32_t(out.size()));
    out.resize((out.size() + 15) & ~size_t(15), 0); // as the game's images
    return out;
}

namespace {

// A duel camera of the game's, brought down to his size: its positions (relative to the fighter, who stands at the
// origin) scaled about him, so that it frames him as it frames the duelist it was made for (each shot looks along the
// same line, at the point scaled as well).
std::vector<uint8_t> ScaledCamera(const fullbody::ReadResource& read, const char* name, std::string& report)
{
    std::vector<uint8_t> data;
    if (!read(name, data) || data.size() < 0x120) {
        report = std::string(name) + " could not be read";
        return {};
    }
    const uint32_t count = U32(data.data() + 0xEC), positions = U32(data.data() + 0xF4);
    if (count > 10000 || positions < 0x120 || positions + uint64_t(count) * 16 > data.size()) {
        report = std::string(name) + " is not as expected";
        return {};
    }
    for (uint32_t k = 0; k < count; ++k) {
        float p[3];
        std::memcpy(p, data.data() + positions + k * 16, 12);
        for (float& c : p)
            c *= float(kCameraScale);
        std::memcpy(data.data() + positions + k * 16, p, 12);
    }
    return data;
}

} // namespace

// The intro camera: a fixed shot of him from the front, low, as the bonus mission's cutscene films this moment
// (kShot*), as long as his intro clip. It takes the layout of Anakin's (the rotations from 0x120, the positions
// after, the field of view, the table of the pointers the loader relocates). A camera's rotation q gives R (as a
// clip's bones do) whose rows are its right, up and forward directions (right = up x forward); positions are
// relative to the fighter (his clip's origin, facing +z).
std::vector<uint8_t> MakeIntroCamera(const fullbody::ReadResource& read, std::string& report)
{
    std::vector<uint8_t> model;
    if (!read(kCameraTemplate, model) || model.size() < 0x120) {
        report = std::string(kCameraTemplate) + " could not be read";
        return {};
    }
    const uint32_t modelTable = U32(model.data() + 0x30);
    if (modelTable + 32 > model.size()) {
        report = std::string(kCameraTemplate) + " is not as expected";
        return {};
    }
    const uint32_t count = kShotKeys;
    const uint32_t posAt = 0x120 + count * 16, fovAt = posAt + count * 16, tableAt = fovAt + 16;
    std::vector<uint8_t> out(model.begin(), model.begin() + 0x120);
    Put32(out.data(), count * 28 + 100); // as the game's cameras have (their data's size)
    Put32(out.data() + 0x0C, count * 2);
    Put32(out.data() + 0x14, 2);
    for (uint32_t o : { 0xE0u, 0xE4u, 0xECu, 0xF0u })
        Put32(out.data() + o, count);
    Put32(out.data() + 0xE8, 0x120);
    Put32(out.data() + 0xF4, posAt);
    Put32(out.data() + 0xF8, 1);
    Put32(out.data() + 0xFC, 1);
    Put32(out.data() + 0x100, fovAt);
    for (uint32_t o : { 0x30u, 0x34u, 0x58u, 0x110u })
        Put32(out.data() + o, tableAt);
    out.resize(tableAt, 0);
    const double p[3] = { kShotPosition[0], kShotPosition[1], kShotPosition[2] };
    double f[3] = { kShotTarget[0] - p[0], kShotTarget[1] - p[1], kShotTarget[2] - p[2] };
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& c : f)
        c /= fl;
    // right = (0, 1, 0) x f, up = f x right
    double r[3] = { f[2], 0, -f[0] };
    const double rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    r[0] /= rl, r[2] /= rl;
    const double u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    const double m[3][3] = { { r[0], r[1], r[2] }, { u[0], u[1], u[2] }, { f[0], f[1], f[2] } };
    double q[4];
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const double s4 = std::sqrt(trace + 1) * 2;
        q[3] = s4 / 4, q[0] = (m[2][1] - m[1][2]) / s4, q[1] = (m[0][2] - m[2][0]) / s4, q[2] = (m[1][0] - m[0][1]) / s4;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s4 = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s4, q[0] = s4 / 4, q[1] = (m[0][1] + m[1][0]) / s4, q[2] = (m[0][2] + m[2][0]) / s4;
    } else if (m[1][1] > m[2][2]) {
        const double s4 = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s4, q[0] = (m[0][1] + m[1][0]) / s4, q[1] = s4 / 4, q[2] = (m[1][2] + m[2][1]) / s4;
    } else {
        const double s4 = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s4, q[0] = (m[0][2] + m[2][0]) / s4, q[1] = (m[1][2] + m[2][1]) / s4, q[2] = s4 / 4;
    }
    const float rot[4] = { float(q[0]), float(q[1]), float(q[2]), float(q[3]) };
    const float pos[3] = { float(p[0]), float(p[1]), float(p[2]) };
    for (uint32_t k = 0; k < count; ++k) {
        std::memcpy(out.data() + 0x120 + k * 16, rot, 16);
        std::memcpy(out.data() + posAt + k * 16, pos, 12);
        Put32(out.data() + posAt + k * 16 + 12, 0xBAADF00D);
    }
    const float fov = float(kShotFov);
    std::memcpy(out.data() + fovAt, &fov, 4);
    out.insert(out.end(), model.begin() + modelTable, model.begin() + modelTable + 32);
    return out;
}

std::vector<uint8_t> MakeOutroCamera(const fullbody::ReadResource& read, std::string& report)
{
    return ScaledCamera(read, kOutroCameraTemplate, report);
}

} // namespace swrots::game::yodaintro
