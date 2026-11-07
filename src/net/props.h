#pragma once
// Client/server netplay: the arena's props (a crate lifted with the Force, a breakable) are the host's. The host
// finds them at a duel's start (objects of the prop classes, by their vtables in memory, each still known to the
// object manager by its instance id), sends those that move (their place, the matrix at +0x150) and those that are
// gone (broken, taken away); the guest puts its own (the same level: the same instance ids) where the host's are and
// takes away those gone on the host.

#include <cstdint>
#include <string>

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

// Client/server netplay in a Cooperative mission: its other characters (enemies, allies) are the host's. The host
// finds them at the mission's start (a full scan) and the new ones as they come (the ids after the last one known,
// a few hundred every half second), and gives up to `max` of them a frame, in turn (net/duelstate.h's ApplyCharacter
// on the guest).
void CharactersReset();
int CharactersCapture(const uint8_t* player1, const uint8_t* player2, uint32_t* ids, uint8_t** objects, int max);
size_t CharactersKnown();
// The host's characters followed, by id (diagnostics: the lowest `max`).
std::string DescribeCharacters(size_t max);
// The object of this instance id on this game, or null.
uint8_t* ObjectOfId(uint32_t id);

// The console's `objects` (research and tests): the level's objects by class (the log has each), and a prop of a
// class moved up (`move`) or taken away (`destroy`) on this game.
std::string ObjectsCommand(const std::string& verb, const std::string& cls, float amount);

} // namespace swrots::net
