#pragma once
// Client/server netplay: the arena's props (a crate lifted with the Force, a breakable) are the host's. The host
// finds them at a duel's start (objects of the prop classes, by their vtables in memory, each still known to the
// object manager by its instance id), sends those that move (their place, the matrix at +0x150) and those that are
// gone (broken, taken away); the guest puts its own (the same level: the same instance ids) where the host's are and
// takes away those gone on the host.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace swrots::net {

#pragma pack(push, 1)
struct PropState {
    uint32_t id;       // the object's instance id (+4)
    float matrix[16];  // its place (+0x150: three rows and the position)
};
#pragma pack(pop)
constexpr int kPropsPerMsg = 16;
constexpr int kGonePerMsg = 32;

// A new level or duel: the props are found again (both games).
void PropsReset();
// The host, at every frame of a duel: what to send now (moved props, those gone). False when there is nothing.
bool PropsCapture(uint32_t frame, PropState* props, int& count, uint32_t* gone, int& goneCount);
// The guest: the host's props (kept until applied), then at every frame of a duel written into its own.
void PropsReceive(uint32_t frame, const PropState* props, int count, const uint32_t* gone, int goneCount);
void PropsApply(uint32_t frame);
std::string PropsStats();

// Client/server netplay in a Cooperative mission: its other characters (enemies, allies) are the host's. Both games
// keep them known (a full scan at the mission's start, then the new ones every half second): each by a key the same on
// both games (a level's own: its instance id; one spawned during the mission: its class and its rank among that
// class's spawned ones, as ids from 0x40000000 go to every object made and differ). The host gives up to `max` of them
// a frame, in turn (net/duelstate.h's ApplyCharacter on the guest, by key).
void CharactersReset();
void CharactersUpdate(const uint8_t* player1, const uint8_t* player2);
int CharactersCapture(uint32_t* keys, uint8_t** objects, int max);
uint8_t* CharacterByKey(uint32_t key);
// The spawned characters of a key's class (its upper half: 0x80000000 | the class's hash), with their keys.
std::vector<std::pair<uint32_t, uint8_t*>> CharactersOfClass(uint32_t keyClass);
// This game's active spawned characters, with their keys.
std::vector<std::pair<uint32_t, uint8_t*>> SpawnedCharacters();
// The active characters known (diagnostics).
std::vector<uint8_t*> CharacterObjects();
size_t CharactersKnown();
// The characters (diagnostics: the lowest `max` keys of the active ones).
std::string DescribeCharacters(size_t max);

// The console's `objects` (research and tests): the level's objects by class (the log has each), and a prop of a
// class moved up (`move`) or taken away (`destroy`) on this game.
std::string ObjectsCommand(const std::string& verb, const std::string& cls, float amount);

} // namespace swrots::net
