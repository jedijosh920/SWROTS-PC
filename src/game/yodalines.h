#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::game::yodalines {

// Yoda's lines in a duel, as the duelists have theirs: the generic dialogue database (dialogue\gen_dialogue.ddb, every
// level's) has a few for him (kill lines, taunts that need clones or droids or a Sith), his bonus mission's
// (u113_dialogue.ddb) more: its own taunts and kill lines (context U11). These are added to the generic one as
// generic entries of his, said against anyone (kLines), their voices taken from his mission's dialogue archive.
struct Line {
    const char* id;    // the line, e.g. "YDA060"
    const char* event; // its event: TNT (a taunt during the fight), KIL (an enemy down)
};
constexpr Line kLines[] = {
    { "YDA060", "TNT" }, { "YDA058", "TNT" },
    { "YDA066", "KIL" }, { "YDA067", "KIL" }, { "YDA068", "KIL" }, { "YDA072", "KIL" },
};

// The database with an entry added for each of `ids` (lines of kLines), or empty when it is not as expected (the
// reason in `report`). The database: a header (its events), then u32 count twice and the entries, sorted by name,
// then its speakers and categories. An entry: 16 bytes (u16 kind (2: said on its own), u16 2, u16 0, u16 its index,
// u16 a row number, u32 -1 (or the index of the entry answering it), u16 its event's code), its line and its name
// (u32 length, text: "TNTYDA___000GEN": event, speaker, ___, number, category), 12 bytes, its context (u32 length,
// text: "Versus" for a duel's intro and outro, else none), 4 bytes, its condition (u32 length, text, e.g.
// "EnemyIsA( Clone )"), 8 bytes. A new entry copies one of his of the same event (its kind and code), its number
// after theirs, no condition; the entries are numbered again and counted.
std::vector<uint8_t> AddEntries(const std::vector<uint8_t>& ddb, const std::vector<std::string>& ids, std::string& report);

} // namespace swrots::game::yodalines
