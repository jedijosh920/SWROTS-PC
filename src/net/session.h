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
};

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
