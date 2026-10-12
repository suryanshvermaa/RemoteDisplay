#pragma once
// LAN discovery: finds the RemoteDisplay hosts on this network that are sharing right now.
//
// A small query goes to the broadcast address of every network adapter (and the limited broadcast). A host
// that is sharing answers with its computer name. Nothing secret is sent: no key, no session, and a host
// answers nothing when it is not sharing.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "protocol.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace rdapp {

struct FoundDevice {
    std::string ip;    // dotted address to connect to
    std::string name;  // the computer's name, as the host reports it
};

// Parses a dotted IPv4 address into network byte order; false when it is not one.
inline bool ParseIPv4(const char* text, uint32_t& out) {
    IN_ADDR a = {};
    if (inet_pton(AF_INET, text, &a) != 1) return false;
    out = a.S_un.S_addr;
    return true;
}

// Broadcast address of each IPv4 adapter that has one (network byte order).
inline std::vector<uint32_t> BroadcastAddresses() {
    std::vector<uint32_t> out;
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    if (size == 0) return out;
    std::vector<unsigned char> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    if (GetAdaptersInfo(list, &size) != NO_ERROR) return out;
    for (IP_ADAPTER_INFO* a = list; a; a = a->Next) {
        for (IP_ADDR_STRING* ip = &a->IpAddressList; ip; ip = ip->Next) {
            uint32_t addr = 0, mask = 0;
            if (!ParseIPv4(ip->IpAddress.String, addr) || !ParseIPv4(ip->IpMask.String, mask)) continue;
            if (addr == 0 || mask == 0) continue;  // no address, or no mask yet (adapter starting up)
            out.push_back(addr | ~mask);  // bitwise on network-order values gives the broadcast address
        }
    }
    return out;
}

// Keeps only printable characters of a name a host sent, so it can be shown safely.
inline std::string CleanName(const char* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n && out.size() < 64; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c >= 0x20 && c != 0x7F) out += char(c);
    }
    return out;
}

// Asks the network for sharing hosts and collects the answers for about waitMs milliseconds.
inline std::vector<FoundDevice> FindDevices(int waitMs = 1500) {
    std::vector<FoundDevice> found;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return found;
    BOOL broadcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
    DWORD timeout = 200;  // each receive returns after 200 ms at most, so the deadline is checked often
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::vector<uint32_t> targets = BroadcastAddresses();
    targets.push_back(htonl(INADDR_BROADCAST));
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(rd::kDefaultPort);
    for (int round = 0; round < 2; ++round) {  // twice: UDP can lose a packet
        for (uint32_t t : targets) {
            to.sin_addr.s_addr = t;
            sendto(s, rd::kDiscoverQuery, int(sizeof(rd::kDiscoverQuery)), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
    }

    std::set<std::string> seen;
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(waitMs);
    char buf[256];
    while (GetTickCount64() < deadline) {
        sockaddr_in from = {};
        int fl = sizeof(from);
        const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < int(sizeof(rd::kDiscoverReply)) || memcmp(buf, rd::kDiscoverReply, sizeof(rd::kDiscoverReply)) != 0) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        if (!seen.insert(ip).second) continue;
        std::string name = CleanName(buf + sizeof(rd::kDiscoverReply), size_t(n) - sizeof(rd::kDiscoverReply));
        found.push_back({ip, name.empty() ? std::string(ip) : name});
    }
    closesocket(s);
    return found;
}

}  // namespace rdapp
