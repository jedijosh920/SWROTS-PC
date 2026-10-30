// Netplay's network layer (see transport.h).

#include <winsock2.h>
#include <ws2tcpip.h>

#include "net/transport.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace swrots::net {

namespace {

SOCKET Sock(uintptr_t s) { return static_cast<SOCKET>(s); }

sockaddr_in ToSockaddr(const Endpoint& e)
{
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.ip);
    a.sin_port = htons(e.port);
    return a;
}

// Crockford's base 32: no I, L, O, U.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int AlphabetValue(char c)
{
    if (c >= 'a' && c <= 'z')
        c = char(c - 'a' + 'A');
    if (c == 'O')
        c = '0';
    if (c == 'I' || c == 'L')
        c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c)
            return i;
    return -1;
}

uint8_t Crc8(const uint8_t* data, size_t size)
{
    uint8_t crc = 0x5A;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = uint8_t(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

constexpr size_t kCodeBytes = 13; // public ip and port, local ip and port, check
constexpr size_t kCodeChars = 21; // 104 bits in 5-bit letters (105)

} // namespace

std::string Endpoint::ToString() const
{
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
    return text;
}

bool NetworkInit()
{
    static const bool ok = [] {
        WSADATA data;
        const int r = WSAStartup(MAKEWORD(2, 2), &data);
        if (r != 0)
            LOG_ERROR("Online: Winsock could not start (%d)", r);
        return r == 0;
    }();
    return ok;
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::IsOpen() const { return m_socket != ~uintptr_t(0); }

bool UdpSocket::Open(uint16_t port)
{
    Close();
    if (!NetworkInit())
        return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
    // A datagram that was refused (ICMP port unreachable) would otherwise make the next receive fail.
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
    int buffer = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        LOG_WARN("Online: UDP port %u is taken (%d)", port, WSAGetLastError());
        closesocket(s);
        return false;
    }
    int length = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    m_port = ntohs(a.sin_port);
    m_socket = uintptr_t(s);
    return true;
}

void UdpSocket::Close()
{
    if (IsOpen())
        closesocket(Sock(m_socket));
    m_socket = ~uintptr_t(0);
    m_port = 0;
}

bool UdpSocket::SendTo(const Endpoint& to, const void* data, size_t size)
{
    if (!IsOpen() || !to.Valid())
        return false;
    const sockaddr_in a = ToSockaddr(to);
    return sendto(Sock(m_socket), static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&a),
               sizeof(a)) == int(size);
}

size_t UdpSocket::Receive(void* buffer, size_t capacity, Endpoint& from, uint32_t timeoutMs)
{
    if (!IsOpen())
        return 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const long long left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        fd_set set;
        FD_ZERO(&set);
        FD_SET(Sock(m_socket), &set);
        timeval tv;
        tv.tv_sec = long(left > 0 ? left / 1000000 : 0);
        tv.tv_usec = long(left > 0 ? left % 1000000 : 0);
        if (select(0, &set, nullptr, nullptr, &tv) <= 0)
            return 0;
        sockaddr_in a = {};
        int length = sizeof(a);
        const int n = recvfrom(Sock(m_socket), static_cast<char*>(buffer), int(capacity), 0,
            reinterpret_cast<sockaddr*>(&a), &length);
        if (n > 0) {
            from.ip = ntohl(a.sin_addr.s_addr);
            from.port = ntohs(a.sin_port);
            return size_t(n);
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return 0;
    }
}

bool Resolve(const std::string& hostAndPort, uint16_t defaultPort, Endpoint& out)
{
    if (!NetworkInit())
        return false;
    std::string host = hostAndPort;
    uint16_t port = defaultPort;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        port = uint16_t(std::atoi(host.c_str() + colon + 1));
        host.resize(colon);
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
        return false;
    out.ip = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(result);
    return out.Valid();
}

uint32_t LocalAddress()
{
    if (!NetworkInit())
        return 0;
    // A UDP socket "connected" to a public address sends nothing; its local address is the interface's.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return 0;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        sockaddr_in local = {};
        int length = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0)
            ip = ntohl(local.sin_addr.s_addr);
    }
    closesocket(s);
    return ip;
}

bool StunPublicAddress(UdpSocket& socket, const Endpoint& server, Endpoint& out, uint32_t timeoutMs)
{
    constexpr uint32_t kCookie = 0x2112A442;
    uint8_t request[20] = {};
    request[1] = 0x01; // binding request
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xA4;
    request[7] = 0x42;
    const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int i = 0; i < 12; ++i)
        request[8 + i] = uint8_t(now >> ((i % 8) * 8)) ^ uint8_t(i * 37);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    uint8_t reply[600];
    for (int attempt = 0; std::chrono::steady_clock::now() < deadline; ++attempt) {
        socket.SendTo(server, request, sizeof(request));
        const auto resend = std::chrono::steady_clock::now() + std::chrono::milliseconds(250 << std::min(attempt, 3));
        while (std::chrono::steady_clock::now() < std::min(resend, deadline)) {
            Endpoint from;
            const size_t n = socket.Receive(reply, sizeof(reply), from, 50);
            if (n < 20 || from != server || reply[0] != 0x01 || reply[1] != 0x01 ||
                std::memcmp(reply + 8, request + 8, 12) != 0)
                continue;
            const size_t length = std::min<size_t>(n - 20, size_t(reply[2] << 8 | reply[3]));
            for (size_t at = 20; at + 4 <= 20 + length;) {
                const uint16_t type = uint16_t(reply[at] << 8 | reply[at + 1]);
                const uint16_t size = uint16_t(reply[at + 2] << 8 | reply[at + 3]);
                const uint8_t* v = reply + at + 4;
                if (at + 4 + size > n)
                    break;
                if ((type == 0x0020 || type == 0x0001) && size >= 8 && v[1] == 0x01) { // (XOR-)MAPPED-ADDRESS, IPv4
                    uint16_t port = uint16_t(v[2] << 8 | v[3]);
                    uint32_t ip = uint32_t(v[4]) << 24 | uint32_t(v[5]) << 16 | uint32_t(v[6]) << 8 | v[7];
                    if (type == 0x0020) {
                        port ^= uint16_t(kCookie >> 16);
                        ip ^= kCookie;
                    }
                    out.ip = ip;
                    out.port = port;
                    return true;
                }
                at += 4 + ((size + 3) & ~3u);
            }
        }
    }
    return false;
}

std::string MakeRoomCode(const Endpoint& publicAddress, const Endpoint& localAddress)
{
    uint8_t bytes[kCodeBytes] = {};
    for (int i = 0; i < 4; ++i)
        bytes[i] = uint8_t(publicAddress.ip >> (24 - 8 * i));
    bytes[4] = uint8_t(publicAddress.port >> 8);
    bytes[5] = uint8_t(publicAddress.port);
    for (int i = 0; i < 4; ++i)
        bytes[6 + i] = uint8_t(localAddress.ip >> (24 - 8 * i));
    bytes[10] = uint8_t(localAddress.port >> 8);
    bytes[11] = uint8_t(localAddress.port);
    bytes[12] = Crc8(bytes, 12);
    std::string code;
    uint32_t buffer = 0;
    int bits = 0;
    size_t byte = 0;
    for (size_t c = 0; c < kCodeChars; ++c) {
        while (bits < 5) {
            buffer = (buffer << 8) | (byte < kCodeBytes ? bytes[byte] : 0);
            ++byte;
            bits += 8;
        }
        code += kAlphabet[(buffer >> (bits - 5)) & 31];
        bits -= 5;
        if (c % 7 == 6 && c + 1 < kCodeChars)
            code += '-';
    }
    return code;
}

bool ReadRoomCode(const std::string& code, Endpoint& publicAddress, Endpoint& localAddress)
{
    uint8_t bytes[kCodeBytes + 1] = {};
    uint32_t buffer = 0;
    int bits = 0;
    size_t byte = 0, chars = 0;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        const int v = AlphabetValue(c);
        if (v < 0)
            return false;
        ++chars;
        buffer = (buffer << 5) | uint32_t(v);
        bits += 5;
        if (bits >= 8) {
            if (byte < sizeof(bytes))
                bytes[byte++] = uint8_t(buffer >> (bits - 8));
            bits -= 8;
        }
    }
    if (chars != kCodeChars || Crc8(bytes, 12) != bytes[12])
        return false;
    publicAddress.ip = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
    publicAddress.port = uint16_t(bytes[4] << 8 | bytes[5]);
    localAddress.ip = uint32_t(bytes[6]) << 24 | uint32_t(bytes[7]) << 16 | uint32_t(bytes[8]) << 8 | bytes[9];
    localAddress.port = uint16_t(bytes[10] << 8 | bytes[11]);
    return publicAddress.Valid() || localAddress.Valid();
}

} // namespace swrots::net
