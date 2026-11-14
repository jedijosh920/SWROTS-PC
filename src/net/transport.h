#pragma once
// Netplay's network layer: one UDP socket per session, the public address a STUN server sees for it (on by default,
// [Online] Stun=0 turns it off), the PC's own address on its network, and room codes (a host's addresses written as a
// short code to give a friend). No server of the project's: the guest sends to the host's address directly. See
// docs/research/netplay.md.

#include <cstdint>
#include <string>
#include <vector>

namespace swrots::net {

// An IPv4 address and port (host byte order).
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool Valid() const { return ip != 0 && port != 0; }
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
    std::string ToString() const;
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Bound to `port` on every interface (0: any free port). False when the port is taken.
    bool Open(uint16_t port);
    void Close();
    bool IsOpen() const;
    uint16_t LocalPort() const { return m_port; }

    bool SendTo(const Endpoint& to, const void* data, size_t size);
    // Waits up to `timeoutMs` for a datagram; its size (0 when none came). Other errors (a port unreachable message
    // from an earlier send) are skipped.
    size_t Receive(void* buffer, size_t capacity, Endpoint& from, uint32_t timeoutMs);

private:
    uintptr_t m_socket = ~uintptr_t(0);
    uint16_t m_port = 0;
};

// Winsock, started once (false when it could not be).
bool NetworkInit();

// "host:port" or "a.b.c.d:port" resolved (IPv4); false when it cannot be.
bool Resolve(const std::string& hostAndPort, uint16_t defaultPort, Endpoint& out);
// The PC's address on its network (the interface that would reach the Internet); 0 when none.
uint32_t LocalAddress();

// Asks a STUN server (RFC 5389 binding request, through `socket`) the address it sees: the socket's public address
// behind the router. Datagrams that are not the answer are dropped while it waits. False after `timeoutMs`.
bool StunPublicAddress(UdpSocket& socket, const Endpoint& server, Endpoint& out, uint32_t timeoutMs);

// Room codes: the host's public address and (the low half of) its address on its network, as 15 letters and digits
// in groups of five (e.g. "K3QF7-ZMA0W-XRD2P"; the game's code keyboard holds 15), with a check: mistyped codes are
// refused. Lower case, the letters I/L for 1 and O for 0, spaces and dashes are accepted when read. The local
// address read takes its high half from this PC's (the same home network); its port is the host's public one or
// `defaultPort`.
std::string MakeRoomCode(const Endpoint& publicAddress, const Endpoint& localAddress);
bool ReadRoomCode(const std::string& code, uint16_t defaultPort, Endpoint& publicAddress, Endpoint& localAddress);

} // namespace swrots::net
