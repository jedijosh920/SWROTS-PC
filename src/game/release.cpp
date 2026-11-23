#include "game/release.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/settings.h"

namespace swrots::game {

const wchar_t* const kSupportedReleases =
    L"the North American (USA) release, or the PAL release (France, or Europe in English, Spanish and Italian)";

namespace {

struct Range {
    uint32_t first, end; // North American addresses [first, end)
    int32_t delta;       // PAL address - North American address
};

constexpr Range kPalRanges[] = {
#define PAL_RANGE(first, end, delta) { first, end, delta },
#include "game/pal_map.inc"
#undef PAL_RANGE
};

// The game's image: [0x10000, 0x970000). Values outside it are not game addresses.
constexpr uint32_t kImageFirst = 0x00010000, kImageLimit = 0x00970000;

bool g_Selected = false;
Release g_Release = Release::NorthAmerica;
std::vector<Range> g_PalToNorthAmerica; // the same ranges by PAL address, delta negated

struct Registered {
    uint32_t* first;
    size_t count;
    bool end; // one past the last byte of a range
};

std::vector<Registered>& Registry()
{
    static std::vector<Registered> registry;
    return registry;
}

const Range* Find(const Range* begin, const Range* end, uint32_t address)
{
    const Range* it = std::upper_bound(begin, end, address, [](uint32_t a, const Range& r) { return a < r.first; });
    if (it == begin)
        return nullptr;
    --it;
    return address < it->end ? it : nullptr;
}

// The PAL address, or 0 if there is none.
uint32_t TryPal(uint32_t northAmerica)
{
    const Range* r = Find(std::begin(kPalRanges), std::end(kPalRanges), northAmerica);
    return r ? uint32_t(int32_t(northAmerica) + r->delta) : 0;
}

} // namespace

bool FindRelease(const std::string& md5, Release* release)
{
    struct Known {
        const char* md5;
        Release release;
    };
    static const Known kKnown[] = {
        { "6460ef37862de3af364a0563a0a18c4e", Release::NorthAmerica },
        { "84ae8a86018f3ab4a1b5756b238cefd2", Release::France },
        { "a48f65aa84a9848030b8644e1fd35b89", Release::Europe },
    };
    for (const Known& k : kKnown)
        if (md5 == k.md5) {
            *release = k.release;
            return true;
        }
    return false;
}

const char* ReleaseName(Release release)
{
    switch (release) {
    case Release::NorthAmerica: return "North America (USA)";
    case Release::France: return "France (PAL)";
    case Release::Europe: return "Europe (PAL: English, Spanish, Italian)";
    }
    return "?";
}

Release CurrentRelease() { return g_Release; }

bool ReleaseKnown() { return g_Selected; }

bool IsPal() { return g_Release != Release::NorthAmerica; }

void SelectRelease(Release release)
{
    if (g_Selected) {
        if (release != g_Release)
            Fatal("The game's executable changed release while running");
        return;
    }
    g_Release = release;
    g_Selected = true;
    LOG_INFO("Release: %s", ReleaseName(release));
    if (!IsPal())
        return;
    for (const Range& r : kPalRanges)
        g_PalToNorthAmerica.push_back({ uint32_t(int32_t(r.first) + r.delta), uint32_t(int32_t(r.end) + r.delta), -r.delta });
    std::sort(g_PalToNorthAmerica.begin(), g_PalToNorthAmerica.end(),
        [](const Range& a, const Range& b) { return a.first < b.first; });
    size_t count = 0;
    for (const Registered& r : Registry())
        for (size_t i = 0; i < r.count; ++i, ++count)
            r.first[i] = r.end ? Va(r.first[i] - 1) + 1 : Va(r.first[i]);
    LOG_INFO("Release: %zu addresses moved to the PAL executable's", count);
}

uint32_t ConsoleLanguage()
{
    static const struct {
        const char* name;
        uint32_t code;
    } kLanguages[] = { { "English", 1 }, { "German", 3 }, { "French", 4 }, { "Spanish", 5 }, { "Italian", 6 } };
    for (const auto& l : kLanguages)
        if (GetSettings().language == l.name)
            return l.code;
    return g_Release == Release::France ? 4 : 1;
}

bool PortInFrench() { return ConsoleLanguage() == 4; }

const wchar_t* PortText(const wchar_t* english, const wchar_t* french) { return PortInFrench() ? french : english; }

const char* PortText(const char* english, const char* french) { return PortInFrench() ? french : english; }

uint32_t Va(uint32_t northAmerica)
{
    if (!g_Selected)
        Fatal("The game address %08X was used before the game's release was known", northAmerica);
    if (!IsPal() || northAmerica < kImageFirst || northAmerica >= kImageLimit)
        return northAmerica;
    const uint32_t pal = TryPal(northAmerica);
    if (!pal)
        Fatal("The game address %08X has no equivalent in the PAL executable", northAmerica);
    return pal;
}

uint32_t NorthAmericaVa(uint32_t running)
{
    if (!IsPal())
        return running;
    const Range* r = Find(g_PalToNorthAmerica.data(), g_PalToNorthAmerica.data() + g_PalToNorthAmerica.size(), running);
    return r ? uint32_t(int32_t(running) + r->delta) : 0;
}

uint32_t RegisterVa(uint32_t* variable, uint32_t northAmerica)
{
    // Function-local statics may be initialised after the release is chosen: translate them at once.
    if (g_Selected)
        return Va(northAmerica);
    Registry().push_back({ variable, 1, false });
    return northAmerica;
}

uint32_t RegisterVaEnd(uint32_t* variable, uint32_t northAmericaEnd)
{
    if (g_Selected)
        return Va(northAmericaEnd - 1) + 1;
    Registry().push_back({ variable, 1, true });
    return northAmericaEnd;
}

bool RegisterVas(uint32_t* first, size_t count)
{
    if (g_Selected) {
        for (size_t i = 0; i < count; ++i)
            first[i] = Va(first[i]);
    } else {
        Registry().push_back({ first, count, false });
    }
    return true;
}

bool MatchesCode(uint32_t address, const void* expected, size_t length)
{
    const auto* have = reinterpret_cast<const uint8_t*>(uintptr_t(address));
    const auto* want = static_cast<const uint8_t*>(expected);
    if (!IsPal())
        return std::memcmp(have, want, length) == 0;
    const uint32_t northAmerica = NorthAmericaVa(address);
    size_t i = 0;
    while (i < length) {
        if (have[i] == want[i]) {
            ++i;
            continue;
        }
        // A 4-byte operand covering byte i that holds the PAL executable's own address.
        bool relocated = false;
        for (size_t s = i >= 3 ? i - 3 : 0; s <= i && s + 4 <= length && !relocated; ++s) {
            uint32_t w, h;
            std::memcpy(&w, want + s, 4);
            std::memcpy(&h, have + s, 4);
            if (w >= kImageFirst && w < kImageLimit && TryPal(w) == h)
                relocated = true; // absolute address
            else if (northAmerica) {
                const uint32_t wantTarget = northAmerica + uint32_t(s) + 4 + w;
                const uint32_t haveTarget = address + uint32_t(s) + 4 + h;
                relocated = wantTarget >= kImageFirst && wantTarget < kImageLimit && TryPal(wantTarget) == haveTarget;
            }
            if (relocated)
                i = s + 4;
        }
        if (!relocated)
            return false;
    }
    return true;
}

} // namespace swrots::game
