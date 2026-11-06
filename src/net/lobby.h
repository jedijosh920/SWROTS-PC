#pragma once
// Online play's lobby: the public rooms' list, on a small web service of the project's own (lobby/: a Cloudflare
// Worker). A host whose room is public ([Online] in the room: Public Room) announces it every 30 s while it is open
// and takes it away when it closes; Public Rooms lists them (the same protocol only). The service's address is
// [Online] Lobby in settings.ini (by default the project's).

#include <string>
#include <vector>

namespace swrots::net {

struct PublicRoom {
    std::string code;  // the room code (what joining takes)
    std::string name;  // the host's name
    std::string mode;  // VERSUS
    std::string arena; // the room's arena setting, or the arena played
    int players = 0, slots = 2;
};

// The Public Rooms screen is up: the list is kept fresh (every 5 s) while this is called.
void LobbyWantList();
// The last list and a word on it ("" when it is fine, else SEARCHING, NO LOBBY SERVER, LOBBY UNREACHABLE).
void LobbyRooms(std::vector<PublicRoom>& rooms, std::string& status);
// Starts the lobby's thread (announcing a public room, listing); safe to call again.
void StartLobby();

} // namespace swrots::net
