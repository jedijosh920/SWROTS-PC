#pragma once
// UPnP port mapping on the host's router (Internet Gateway Device: SSDP discovery, then the WANIPConnection or
// WANPPPConnection service's AddPortMapping over HTTP), so a guest reaches the host even behind a router that STUN
// alone would not get through. Most home routers have it on; when one does not answer, nothing changes (the room code
// holds the address STUN found). On by default; [Online] Upnp=0 in settings.ini turns it off. See
// docs/research/netplay.md.

#include <cstdint>
#include <string>

namespace swrots::net {

struct UpnpMapping {
    std::string controlUrl;   // the router's service to talk to
    std::string serviceType;
    uint32_t externalIp = 0;  // the router's public address (0: it did not say)
    uint16_t port = 0;        // mapped (the same port outside and in)
    bool Active() const { return port != 0; }
};

// Finds the router (up to `timeoutMs`) and maps UDP `port` outside to `localIp`:`port`. False when there is no such
// router or it refused (the reason logged).
bool UpnpMapPort(uint16_t port, uint32_t localIp, uint32_t timeoutMs, UpnpMapping& out);
// Takes the mapping back (best effort).
void UpnpUnmap(UpnpMapping& mapping);

} // namespace swrots::net
