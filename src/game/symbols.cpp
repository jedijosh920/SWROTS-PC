#include "game/game.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"

namespace swrots::game {

// North American addresses.
static const SdkSymbol kSymbols[] = {
#define SDK_SYMBOL(addr, name, kind) { addr, name, kind },
#include "game/sdk_symbols.inc"
#undef SDK_SYMBOL
};

// The table in the running release: once the release is known, a translated copy sorted by address (the PAL
// executable's functions are not all in the same order).
static const std::vector<SdkSymbol>& Symbols()
{
    static std::vector<SdkSymbol> symbols(std::begin(kSymbols), std::end(kSymbols));
    static bool translated = false;
    if (!translated && ReleaseKnown()) {
        for (SdkSymbol& s : symbols)
            s.address = Va(s.address);
        std::stable_sort(symbols.begin(), symbols.end(),
            [](const SdkSymbol& a, const SdkSymbol& b) { return a.address < b.address; });
        translated = true;
    }
    return symbols;
}

const SdkSymbol* SdkSymbols(uint32_t* count)
{
    *count = uint32_t(Symbols().size());
    return Symbols().data();
}

const SdkSymbol* FindSdkSymbol(const char* name)
{
    for (const SdkSymbol& s : Symbols())
        if (std::strcmp(s.name, name) == 0)
            return &s;
    return nullptr;
}

const SdkSymbol* NearestSdkSymbol(uint32_t address)
{
    // Table is sorted by address.
    const SdkSymbol* best = nullptr;
    for (const SdkSymbol& s : Symbols()) {
        if (s.address > address)
            break;
        if (s.kind != SymbolKind::Data)
            best = &s;
    }
    return best;
}

namespace {

struct GameSymbol {
    uint32_t address, size;
    int level;
    std::string name;
};

std::vector<GameSymbol> g_GameSymbols;

void LoadGameSymbols()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    path = path.substr(0, path.find_last_of(L"\\/")) + L"\\symbols\\swrots.map";
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        // address, kind, level, name, size, ...
        char kind[16] = {}, name[512] = {};
        unsigned address = 0, size = 0;
        int level = 0;
        if (sscanf_s(line.c_str(), "%x\t%15[^\t]\t%d\t%511[^\t]\t%u", &address, kind, unsigned(sizeof(kind)), &level,
                name, unsigned(sizeof(name)), &size) == 5 && std::strcmp(kind, "function") == 0 && size)
            g_GameSymbols.push_back({ address, size, level, name });
    }
    std::sort(g_GameSymbols.begin(), g_GameSymbols.end(),
        [](const GameSymbol& a, const GameSymbol& b) { return a.address < b.address; });
}

} // namespace

bool DescribeGameAddress(uint32_t address, char* out, size_t size)
{
    static std::once_flag once;
    std::call_once(once, LoadGameSymbols);
    address = NorthAmericaVa(address); // the map is the North American executable's
    auto it = std::upper_bound(g_GameSymbols.begin(), g_GameSymbols.end(), address,
        [](uint32_t a, const GameSymbol& s) { return a < s.address; });
    if (it == g_GameSymbols.begin())
        return false;
    --it;
    if (address - it->address >= it->size)
        return false;
    snprintf(out, size, "%s+0x%X", it->name.c_str(), address - it->address);
    return true;
}

void LogGameCallers(const char* tag, const void* stackTop, int max)
{
    auto* stack = static_cast<const uint32_t*>(stackTop);
    int found = 0;
    for (int i = 0; i < 2048 && found < max; ++i) {
        uint32_t v = stack[i];
        if (v < Va(0x00011006) || v >= Va(0x004A0000))
            continue;
        auto* code = reinterpret_cast<const uint8_t*>(uintptr_t(v));
        bool afterCall = code[-5] == 0xE8 || (code[-2] == 0xFF && (code[-1] & 0x38) == 0x10)
            || (code[-3] == 0xFF && (code[-2] & 0x38) == 0x10) || (code[-6] == 0xFF && (code[-5] & 0x38) == 0x10);
        if (!afterCall)
            continue;
        char name[160] = "";
        DescribeGameAddress(v, name, sizeof(name));
        LOG_INFO("  %s: [esp+%03X] %08X %s", tag, i * 4, v, name);
        ++found;
    }
}

} // namespace swrots::game
