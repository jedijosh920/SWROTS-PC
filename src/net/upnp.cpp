// UPnP port mapping (see upnp.h).

#include <winsock2.h>
#include <ws2tcpip.h>

#include "net/upnp.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "net/transport.h"

namespace swrots::net {

namespace {

using Clock = std::chrono::steady_clock;

std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// A header's value in an HTTP-like reply (case-insensitive name), or "".
std::string Header(const std::string& reply, const std::string& name)
{
    const std::string lower = Lower(reply), key = "\n" + Lower(name) + ":";
    const size_t at = lower.find(key);
    if (at == std::string::npos)
        return {};
    size_t begin = at + key.size();
    while (begin < reply.size() && (reply[begin] == ' ' || reply[begin] == '\t'))
        ++begin;
    const size_t end = reply.find_first_of("\r\n", begin);
    return reply.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// The text of the first <tag>...</tag> at or after `from` (any namespace prefix), or "".
std::string Tag(const std::string& xml, const std::string& tag, size_t from = 0, size_t* endOut = nullptr)
{
    const std::string lower = Lower(xml), t = Lower(tag);
    for (size_t at = lower.find(t + ">", from); at != std::string::npos; at = lower.find(t + ">", at + 1)) {
        const size_t open = lower.rfind('<', at);
        if (open == std::string::npos || lower[open + 1] == '/')
            continue;
        const std::string name = lower.substr(open + 1, at + t.size() - open - 1);
        if (name != t && (name.size() <= t.size() || name[name.size() - t.size() - 1] != ':'))
            continue;
        const size_t begin = at + t.size() + 1;
        const size_t close = lower.find("</", begin);
        if (close == std::string::npos)
            return {};
        if (endOut)
            *endOut = close;
        return xml.substr(begin, close - begin);
    }
    return {};
}

struct Url {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

bool ParseUrl(const std::string& url, Url& out)
{
    const std::string prefix = "http://";
    if (Lower(url.substr(0, prefix.size())) != prefix)
        return false;
    const size_t hostStart = prefix.size();
    const size_t slash = url.find('/', hostStart);
    std::string hostPort = url.substr(hostStart, slash == std::string::npos ? std::string::npos : slash - hostStart);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hostPort.find(':');
    out.host = hostPort.substr(0, colon);
    out.port = colon == std::string::npos ? 80 : uint16_t(std::atoi(hostPort.c_str() + colon + 1));
    return !out.host.empty();
}

// One HTTP/1.0 request to the router (its body read until it closes, or the deadline).
bool Http(const Url& url, const std::string& method, const std::string& extraHeaders, const std::string& body,
    uint32_t timeoutMs, std::string& reply)
{
    Endpoint to;
    if (!Resolve(url.host + ":" + std::to_string(url.port), url.port, to))
        return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;
    DWORD ms = timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(to.ip);
    a.sin_port = htons(to.port);
    // a connect with a deadline: non-blocking, then wait for it
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    timeval tv = { long(timeoutMs / 1000), long(timeoutMs % 1000) * 1000 };
    if (select(0, nullptr, &w, nullptr, &tv) != 1) {
        closesocket(s);
        return false;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    std::string request = method + " " + url.path + " HTTP/1.0\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                          "\r\nConnection: close\r\n" + extraHeaders;
    if (!body.empty())
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    if (send(s, request.data(), int(request.size()), 0) != int(request.size())) {
        closesocket(s);
        return false;
    }
    reply.clear();
    char buffer[4096];
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const int n = recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0 || reply.size() > 256 * 1024 || Clock::now() > deadline)
            break;
        reply.append(buffer, size_t(n));
    }
    closesocket(s);
    return !reply.empty();
}

int Status(const std::string& reply)
{
    const size_t space = reply.find(' ');
    return space == std::string::npos ? 0 : std::atoi(reply.c_str() + space + 1);
}

std::string IpText(uint32_t ip)
{
    char t[16];
    std::snprintf(t, sizeof(t), "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    return t;
}

bool Soap(const UpnpMapping& m, const std::string& action, const std::string& arguments, uint32_t timeoutMs,
    std::string& reply)
{
    Url url;
    if (!ParseUrl(m.controlUrl, url))
        return false;
    const std::string body = "<?xml version=\"1.0\"?>\r\n"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                             "<u:" + action + " xmlns:u=\"" + m.serviceType + "\">" + arguments + "</u:" + action +
                             "></s:Body></s:Envelope>\r\n";
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + m.serviceType + "#" +
                                action + "\"\r\n";
    return Http(url, "POST", headers, body, timeoutMs, reply) && Status(reply) == 200;
}

// The router's description: its WANIPConnection (or WANPPPConnection) service's control URL.
bool FindService(const std::string& location, uint32_t timeoutMs, UpnpMapping& out)
{
    Url url;
    std::string reply;
    if (!ParseUrl(location, url) || !Http(url, "GET", "", "", timeoutMs, reply) || Status(reply) != 200)
        return false;
    std::string base = Tag(reply, "URLBase");
    if (base.empty())
        base = "http://" + url.host + ":" + std::to_string(url.port);
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    for (const char* type : { "urn:schemas-upnp-org:service:WANIPConnection:2",
             "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1" }) {
        const std::string lower = Lower(reply);
        const size_t at = lower.find(Lower(std::string(type)) + "<");
        if (at == std::string::npos)
            continue;
        const std::string control = Tag(reply, "controlURL", at);
        if (control.empty())
            continue;
        out.serviceType = type;
        out.controlUrl = Lower(control.substr(0, 7)) == "http://" ? control
                       : base + (control[0] == '/' ? "" : "/") + control;
        return true;
    }
    return false;
}

} // namespace

bool UpnpMapPort(uint16_t port, uint32_t localIp, uint32_t timeoutMs, UpnpMapping& out)
{
    out = {};
    if (!localIp)
        return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
    sockaddr_in bindTo = {};
    bindTo.sin_family = AF_INET;
    bindTo.sin_addr.s_addr = htonl(localIp); // out through the interface the room's address is on
    bind(s, reinterpret_cast<const sockaddr*>(&bindTo), sizeof(bindTo));
    sockaddr_in group = {};
    group.sin_family = AF_INET;
    group.sin_addr.s_addr = htonl(0xEFFFFFFA); // 239.255.255.250
    group.sin_port = htons(1900);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string location;
    for (int attempt = 0; attempt < 3 && location.empty() && Clock::now() < deadline; ++attempt) {
        for (const char* target : { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                 "urn:schemas-upnp-org:service:WANIPConnection:1" }) {
            const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                                   "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ") + target + "\r\n\r\n";
            sendto(s, search.data(), int(search.size()), 0, reinterpret_cast<const sockaddr*>(&group), sizeof(group));
        }
        const auto until = std::min(deadline, Clock::now() + std::chrono::milliseconds(800));
        while (location.empty() && Clock::now() < until) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            timeval tv = { 0, 100000 };
            if (select(0, &r, nullptr, nullptr, &tv) != 1)
                continue;
            char buffer[2048];
            const int n = recv(s, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0)
                continue;
            const std::string reply(buffer, size_t(n));
            const std::string st = Lower(Header(reply, "ST"));
            if (st.find("internetgatewaydevice") != std::string::npos || st.find("wanipconnection") != std::string::npos ||
                st.find("wanpppconnection") != std::string::npos)
                location = Header(reply, "LOCATION");
        }
    }
    closesocket(s);
    if (location.empty()) {
        LOG_INFO("Online: UPnP: no router answered");
        return false;
    }
    UpnpMapping m;
    if (!FindService(location, 2000, m)) {
        LOG_INFO("Online: UPnP: the router at %s has no port mapping service", location.c_str());
        return false;
    }
    const std::string p = std::to_string(port);
    const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p +
                             "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + p +
                             "</NewInternalPort><NewInternalClient>" + IpText(localIp) +
                             "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>SWROTS-PC online"
                             "</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>";
    std::string reply;
    if (!Soap(m, "AddPortMapping", args, 2000, reply)) {
        LOG_INFO("Online: UPnP: the router refused to map UDP port %u (%s)", port,
            Tag(reply, "errorDescription").c_str());
        return false;
    }
    m.port = port;
    if (Soap(m, "GetExternalIPAddress", "", 2000, reply)) {
        const std::string ip = Tag(reply, "NewExternalIPAddress");
        unsigned a, b, c, d;
        if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
            m.externalIp = a << 24 | b << 16 | c << 8 | d;
    }
    LOG_INFO("Online: UPnP: UDP port %u mapped on the router (its address %s)", port,
        m.externalIp ? IpText(m.externalIp).c_str() : "unknown");
    out = m;
    return true;
}

void UpnpUnmap(UpnpMapping& mapping)
{
    if (!mapping.Active())
        return;
    const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(mapping.port) +
                             "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
    std::string reply;
    if (Soap(mapping, "DeletePortMapping", args, 1500, reply))
        LOG_INFO("Online: UPnP: UDP port %u unmapped", mapping.port);
    mapping = {};
}

} // namespace swrots::net
