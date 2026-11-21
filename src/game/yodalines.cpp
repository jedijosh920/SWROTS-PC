// Yoda's duel lines added to the generic dialogue database (see yodalines.h).

#include "game/yodalines.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace swrots::game::yodalines {

namespace {

uint32_t U32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put16(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

struct Entry {
    std::vector<uint8_t> bytes;
    std::string name, line;
    int index = -1; // its index before (-1: added)
};

// a length-prefixed text at `at` (moved past it); false past the end
bool Text(const std::vector<uint8_t>& d, size_t& at, std::string* out)
{
    if (at + 4 > d.size())
        return false;
    const uint32_t length = U32(d.data() + at);
    if (length > 0x1000 || at + 4 + length > d.size())
        return false;
    if (out)
        out->assign(reinterpret_cast<const char*>(d.data() + at + 4), length);
    at += 4 + length;
    return true;
}

void PutText(std::vector<uint8_t>& out, const std::string& s)
{
    const size_t at = out.size();
    out.resize(at + 4);
    Put32(out.data() + at, uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

const Line* Find(const std::string& id)
{
    for (const Line& l : kLines)
        if (id == l.id)
            return &l;
    return nullptr;
}

} // namespace

std::vector<uint8_t> AddEntries(const std::vector<uint8_t>& ddb, const std::vector<std::string>& ids, std::string& report)
{
    // the entries: the first is the first "u32 6, a line id, u32 15" record, 16 bytes in, after the two counts
    size_t first = 0;
    for (size_t i = 24; i + 4 + 6 + 4 <= ddb.size(); ++i) {
        if (U32(ddb.data() + i) != 6 || U32(ddb.data() + i + 10) != 15)
            continue;
        bool id = true;
        for (size_t k = 0; k < 6; ++k)
            id = id && std::isalnum(ddb[i + 4 + k]) && !std::islower(ddb[i + 4 + k]);
        if (id) {
            first = i - 16;
            break;
        }
    }
    if (!first || U32(ddb.data() + first - 8) != U32(ddb.data() + first - 4)) {
        report = "the dialogue database is not as expected (its entries)";
        return {};
    }
    const uint32_t count = U32(ddb.data() + first - 8);
    std::vector<Entry> entries;
    size_t at = first;
    uint16_t row = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t start = at;
        Entry e;
        at += 16;
        std::string context, condition;
        bool ok = Text(ddb, at, &e.line) && Text(ddb, at, &e.name) && (at += 12, Text(ddb, at, &context)) &&
            (at += 4, Text(ddb, at, &condition)) && at + 8 <= ddb.size();
        at += 8;
        if (!ok || U16(ddb.data() + start + 6) != i) {
            report = "the dialogue database is not as expected (entry " + std::to_string(i) + ")";
            return {};
        }
        e.bytes.assign(ddb.begin() + start, ddb.begin() + at);
        e.index = int(i);
        row = std::max(row, U16(ddb.data() + start + 8));
        entries.push_back(std::move(e));
    }
    const size_t tail = at;

    int added = 0;
    for (const std::string& id : ids) {
        const Line* line = Find(id);
        if (!line)
            continue;
        bool there = false;
        const Entry* model = nullptr; // one of his entries of the event (said against anyone if any)
        int number = -1;
        const std::string prefix = std::string(line->event) + "YDA___";
        for (const Entry& e : entries) {
            if (e.line == id && e.name.compare(0, 3, line->event) == 0)
                there = true;
            if (e.name.size() == 15 && e.name.compare(0, 9, prefix) == 0 && e.name.compare(12, 3, "GEN") == 0) {
                if (!model)
                    model = &e;
                number = std::max(number, std::atoi(e.name.substr(9, 3).c_str()));
            }
        }
        if (there || !model)
            continue;
        char name[32];
        std::snprintf(name, sizeof name, "%sYDA___%03dGEN", line->event, std::min((number / 5 + 1) * 5, 995));
        Entry e;
        e.name = name;
        e.line = id;
        e.bytes.assign(model->bytes.begin(), model->bytes.begin() + 16);
        Put16(e.bytes.data() + 8, ++row);
        Put32(e.bytes.data() + 10, 0xFFFFFFFF); // no answer
        e.index = -1;
        PutText(e.bytes, e.line);
        PutText(e.bytes, e.name);
        e.bytes.resize(e.bytes.size() + 12, 0);
        PutText(e.bytes, ""); // no context: in the fight
        e.bytes.resize(e.bytes.size() + 4, 0);
        PutText(e.bytes, ""); // no condition: against anyone
        e.bytes.resize(e.bytes.size() + 8, 0);
        const auto where = std::upper_bound(entries.begin(), entries.end(), e.name,
            [](const std::string& n, const Entry& x) { return n < x.name; });
        entries.insert(where, std::move(e));
        ++added;
    }
    if (!added) {
        report = "no line to add";
        return {};
    }

    std::vector<uint8_t> out(ddb.begin(), ddb.begin() + first);
    Put32(out.data() + first - 8, uint32_t(entries.size()));
    Put32(out.data() + first - 4, uint32_t(entries.size()));
    std::vector<int> moved(entries.size() + 1, -1); // old index -> new (an entry's answer names one)
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].index >= 0 && size_t(entries[i].index) < moved.size())
            moved[size_t(entries[i].index)] = int(i);
    for (size_t i = 0; i < entries.size(); ++i) {
        Put16(entries[i].bytes.data() + 6, uint32_t(i));
        const uint16_t answer = U16(entries[i].bytes.data() + 10);
        if (answer != 0xFFFF && answer < moved.size() && moved[answer] >= 0)
            Put16(entries[i].bytes.data() + 10, uint32_t(moved[answer]));
        out.insert(out.end(), entries[i].bytes.begin(), entries[i].bytes.end());
    }
    out.insert(out.end(), ddb.begin() + tail, ddb.end());
    report = std::to_string(added) + " entries added";
    return out;
}

} // namespace swrots::game::yodalines
