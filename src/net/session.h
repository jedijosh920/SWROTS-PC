#pragma once
// Online play (lockstep): two PCs play the same game, each computing every frame from both players' inputs. The host
// opens a room (a UDP port; its public address found with STUN unless turned off) and gives its room code to a friend,
// who joins with it. Nothing is installed or signed up for, and there is no server of the project's: the guest sends
// to the host directly.
//
// Once joined, the host's profile and saves go to the guest, both PCs check that they run the same port and the same
// game files, and both start the game again in the same way (the deterministic scheduler, fixed addresses, the same
// profile, saves and gameplay settings, kernel/sched.h) at the main menu. From then on, every frame (every Swap) each
// PC sends its player's input for a few frames ahead (the input delay) and waits for the other's for the frame to
// come: player 1 is the host, player 2 the guest. Menus, the Versus select screen, levels and their restarts are
// played that way on both PCs alike. Every two seconds both compare a hash of their game's course: a difference (a
// desync) is reported. See docs/research/netplay.md.

#include <cstdint>
#include <string>

namespace swrots::net {

enum class OnlinePhase {
    Off,          // no session
    Opening,      // the host opens its room (its public address asked)
    Waiting,      // the host waits for a guest (the room code is shown)
    Connecting,   // the guest calls the host
    Preparing,    // the host's profile and saves go to the guest
    Starting,     // both start the game again for the match
    Playing,      // the match: frames in lockstep
    Ended,        // over (the message says why); back to the game's title
};

// The host's room: what the match will be (the guest sees it as the host sets it).
#pragma pack(push, 1)
struct OnlineRoomSettings {
    uint8_t mode = 0;     // 0 Versus (the only one for now)
    uint8_t rounds = 3;   // rounds to win: 1, 2, 3 or 5
    uint8_t arena = 0;    // the arena: 0 the host picks, 1 random, 2 the last match's loser picks, 3 its winner
    uint8_t rematch = 0;  // after a match: 0 back to the room, 1 the fighters' select again
    uint8_t picker = 0;   // who picks on the arena screen now (the host sets it): 0 the host, 1 the guest
    uint8_t open = 0;     // a public room: listed in every player's Public Rooms
    char name[24] = {};   // the room's name (Room Settings; never empty once the room is open)
    uint8_t time = 0;     // Versus: a round's time limit in seconds (0 none; 30, 60 or 99). At its end the fighter
                          // with less health loses the round; with the same health it is a draw (the round again)
    uint8_t reserved[3] = {};
};
#pragma pack(pop)

struct OnlineStatusInfo {
    OnlinePhase phase = OnlinePhase::Off;
    bool host = false;
    std::string roomCode;   // the host's code to give (Waiting on)
    std::string message;    // what is happening, or why it ended
    uint32_t frame = 0;     // frames played in lockstep
    uint32_t inputDelay = 0;
    int pingMs = -1;
    uint32_t stalls = 0;    // frames that waited for the other PC
    bool desync = false;    // the two games parted
    std::string localName, remoteName; // the players' names ([Online] Name, else the profile's, else NONAME)
    bool matchOn = false;   // both games started for the match (the room, the select screens, the duels)
    bool guestHere = false; // the host has its guest (from its call on)
    bool lost = false;      // it ended without this player leaving (the connection, the other player)
    OnlineRoomSettings room;
    bool entering = false;  // the match's start until its room shows (the game's restart, its main menu: covered)
    bool leaving = false;   // the match's end until the Online screen shows (the same)
    std::string arenaPlayed; // the host in a duel: its arena (from its map)
    bool localReady = false, remoteReady = false; // in the room: each player's Ready (both: the host starts the match)
    int clockTenths = -1;   // a Versus duel with a time limit: the round's time left (tenths of a second; -1 none)
    uint32_t draws = 0;     // the duels' draws so far (the time out with the same health: shown as it moves)
    uint8_t coopVotes[2] = {}; // Cooperative's Restart/Quit vote: player 1's (the host's) and player 2's (CoopVote)
    int coopVoteSeconds = -1;  // the game over's time left to vote (-1: none)
};

// Cooperative online: the mission's Restart and Quit (the game over's, the pause's) are the two players' vote. The
// mission again once both chose Restart; back to the room as soon as either chose Quit; at a game over, after 30
// seconds a missing vote counts as Quit. The host decides (the guest's choice comes as its pad on the host's game).
enum class CoopVote : uint8_t { None = 0, Retry = 1, Quit = 2 };
// The host's game: a player's choice (0 player 1, 1 player 2).
void OnlineCoopVote(int player, CoopVote choice);
// Each frame the vote's menu is up (`gameOver`: the game over's, with its time limit): the outcome to carry out now on
// this game (the host's once, as it decides; the guest's as the host's came), else None.
CoopVote OnlineCoopVoteFrame(bool gameOver);

// The protocol's version (two PCs play together only on the same one; Public Rooms lists those).
uint16_t OnlineProtocol();

// The player's name online, and its change (Online Settings: [Online] Name in settings.ini).
std::string OnlinePlayerName();
void SetOnlinePlayerName(const std::string& name);
// The host's room settings (sent to the guest).
void SetOnlineRoom(const OnlineRoomSettings& room);

// In the room: this player's Ready (both players ready: the host's game starts the match, OnlineTakeStart).
void SetOnlineReady(bool ready);
// The host: true once (both cleared for the next time) when both players are ready in the room.
bool OnlineTakeStart();
// True once after a match that three draws ended (its room says so).
bool OnlineTakeDrawEnd();
// The game is closing (its window closed, Alt+F4): the other PC is told at once that this player left.
void OnlineGoodbye();

// The host opens a room. False when it could not (the message says why).
bool OnlineHost();
// The guest joins with a room code, or an address "host[:port]" (a local network, a test on one PC).
bool OnlineJoin(const std::string& codeOrAddress);
// Leaves (the other PC is told); the game goes back to its title when a match was on.
void OnlineLeave();
OnlineStatusInfo OnlineStatus();
// A line about the session (the console).
std::string OnlineStatusLine();
// Whether a match's frames are played in lockstep (the inputs are the session's).
bool OnlineActive();

// At every Swap (d3d/device.cpp), on the game thread that swaps: the session's game-thread work (the host's profile),
// and in a match the next frame's inputs (waited for) and its pace.
void OnlineFrame();
// The game restarts (ResetRuntime): the match's first start arms the lockstep.
void OnlineReboot();

} // namespace swrots::net
