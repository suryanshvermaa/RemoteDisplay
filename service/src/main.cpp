// rd-host: capture -> hardware H.264 -> UDP.
//   rd-host [--listen <ip>] [--port <n>] [--fps <n>] [--mbps <n>]
// Default listens on loopback only: there is no encryption/auth yet (see docs/ARCHITECTURE.md).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "capture.h"
#include "encoder.h"
#include "protocol.h"

namespace {

uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Peer {
    sockaddr_in addr = {};
    bool valid = false;
    uint64_t lastSeenUs = 0;
};

void SendFrame(SOCKET s, const Peer& peer, const std::vector<uint8_t>& data, uint32_t frameId,
               bool key, uint64_t captureUs) {
    const size_t total = (data.size() + rd::kMaxPayload - 1) / rd::kMaxPayload;
    char pkt[sizeof(rd::PacketHeader) + rd::kMaxPayload];
    for (size_t i = 0; i < total; ++i) {
        const size_t off = i * rd::kMaxPayload;
        const size_t n = std::min(rd::kMaxPayload, data.size() - off);
        rd::PacketHeader h{rd::kMagic, uint8_t(rd::PacketType::Video),
                           uint8_t(key ? rd::kFlagKeyframe : 0), uint16_t(i), uint16_t(total),
                           uint16_t(n), frameId, captureUs};
        memcpy(pkt, &h, sizeof(h));
        memcpy(pkt + sizeof(h), data.data() + off, n);
        sendto(s, pkt, int(sizeof(h) + n), 0, reinterpret_cast<const sockaddr*>(&peer.addr),
               sizeof(peer.addr));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string listenIp = "127.0.0.1";
    int port = rd::kDefaultPort, fps = 60, mbps = 20;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--listen") listenIp = argv[i + 1];
        else if (a == "--port") port = atoi(argv[i + 1]);
        else if (a == "--fps") fps = atoi(argv[i + 1]);
        else if (a == "--mbps") mbps = atoi(argv[i + 1]);
    }
    if (listenIp != "127.0.0.1")
        fprintf(stderr, "WARNING: listening on %s with NO encryption or authentication.\n"
                        "Anyone who can reach this port can watch your screen.\n", listenIp.c_str());

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int buf = 4 << 20;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&buf), sizeof(buf));
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, listenIp.c_str(), &addr.sin_addr);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
        return 1;
    }

    Capture cap;
    Encoder enc;
    auto initPipeline = [&]() {
        return cap.Init() && enc.Init(cap.device(), cap.width(), cap.height(), fps, mbps * 1'000'000);
    };
    if (!initPipeline()) {
        fprintf(stderr, "capture/encoder init failed (need a GPU with a hardware H.264 encoder)\n");
        return 1;
    }
    printf("rd-host %ux%u @%dfps %dMbps, listening on %s:%d\n", cap.width(), cap.height(), fps, mbps,
           listenIp.c_str(), port);

    Peer peer;
    bool forceKey = true;
    uint32_t frameId = 0;
    std::vector<uint8_t> packet;
    uint64_t statStart = NowUs(), statBytes = 0, statFrames = 0;

    for (;;) {
        // Control packets from the client.
        char rbuf[64];
        sockaddr_in from;
        int fl = sizeof(from);
        while (true) {
            int n = recvfrom(sock, rbuf, sizeof(rbuf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < int(sizeof(rd::PacketHeader))) break;
            rd::PacketHeader h;
            memcpy(&h, rbuf, sizeof(h));
            if (h.magic != rd::kMagic) continue;
            if (h.type == uint8_t(rd::PacketType::Hello)) {
                if (!peer.valid || memcmp(&peer.addr, &from, sizeof(from)) != 0) forceKey = true;
                peer.addr = from;
                peer.valid = true;
                peer.lastSeenUs = NowUs();
            } else if (h.type == uint8_t(rd::PacketType::KeyReq)) {
                forceKey = true;
            }
        }
        if (peer.valid && NowUs() - peer.lastSeenUs > 5'000'000) {
            peer.valid = false;
            printf("client timed out\n");
        }

        bool lost = false;
        ID3D11Texture2D* tex = cap.Grab(peer.valid ? 8 : 100, lost);
        if (lost) {
            if (!initPipeline()) Sleep(500);
            forceKey = true;
            continue;
        }
        if (!tex && forceKey) tex = cap.Last();  // static screen but client needs an IDR
        if (!tex || !peer.valid) continue;

        const uint64_t capUs = NowUs();
        bool key = false;
        if (!enc.Encode(tex, forceKey, packet, key)) continue;
        if (forceKey && key) forceKey = false;

        SendFrame(sock, peer, packet, frameId++, key, capUs);
        statBytes += packet.size();
        ++statFrames;
        if (NowUs() - statStart >= 2'000'000) {
            double sec = (NowUs() - statStart) / 1e6;
            printf("%.0f fps, %.1f Mbps\n", statFrames / sec, statBytes * 8 / sec / 1e6);
            statStart = NowUs();
            statBytes = statFrames = 0;
        }
    }
}
