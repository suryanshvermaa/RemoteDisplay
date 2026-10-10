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
#include "session.h"

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

void SendFrame(SOCKET s, const Peer& peer, rd::Session& sess, const std::vector<uint8_t>& data,
               uint32_t frameId, bool key, uint64_t captureUs) {
    const size_t total = (data.size() + rd::kMaxPayload - 1) / rd::kMaxPayload;
    uint8_t plain[sizeof(rd::PacketHeader) + rd::kMaxPayload];
    uint8_t wire[sizeof(rd::WireHeader) + sizeof(plain) + rd::kTagLen];
    for (size_t i = 0; i < total; ++i) {
        const size_t off = i * rd::kMaxPayload;
        const size_t n = std::min(rd::kMaxPayload, data.size() - off);
        rd::PacketHeader h{rd::kMagic, uint8_t(rd::PacketType::Video),
                           uint8_t(key ? rd::kFlagKeyframe : 0), uint16_t(i), uint16_t(total),
                           uint16_t(n), frameId, captureUs};
        memcpy(plain, &h, sizeof(h));
        memcpy(plain + sizeof(h), data.data() + off, n);
        size_t wn = sess.Seal(plain, sizeof(h) + n, wire);
        if (!wn) return;
        sendto(s, reinterpret_cast<char*>(wire), int(wn), 0,
               reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string listenIp = "127.0.0.1";
    int port = rd::kDefaultPort, fps = 60, mbps = 20;
    std::string keyText;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--listen") listenIp = argv[i + 1];
        else if (a == "--port") port = atoi(argv[i + 1]);
        else if (a == "--fps") fps = atoi(argv[i + 1]);
        else if (a == "--mbps") mbps = atoi(argv[i + 1]);
        else if (a == "--key") keyText = argv[i + 1];
    }
    uint8_t psk[rd::kKeyLen];
    if (keyText.empty()) {
        rd::crypto::Random(psk, sizeof(psk));
    } else if (!rd::ParseKey(keyText, psk)) {
        fprintf(stderr, "invalid --key (expected 26 base32 characters)\n");
        return 1;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
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
        if (!cap.Init()) { fprintf(stderr, "capture init failed\n"); return false; }
        return enc.Init(cap.device(), cap.width(), cap.height(), fps, mbps * 1'000'000);
    };
    if (!initPipeline()) {
        fprintf(stderr, "capture/encoder init failed (need a GPU with a hardware H.264 encoder)\n");
        return 1;
    }
    printf("rd-host %ux%u @%dfps %dMbps, listening on %s:%d\n", cap.width(), cap.height(), fps, mbps,
           listenIp.c_str(), port);
    printf("Pairing key: %s\n(enter this in the client with --key; all traffic is encrypted)\n",
           rd::FormatKey(psk).c_str());

    Peer peer;
    rd::Session active, pending;
    uint8_t cachedHs1[rd::kHs1Len] = {};
    uint8_t cachedHs2[sizeof(rd::WireHeader) + rd::kHs2Len] = {};
    bool forceKey = true;
    uint32_t frameId = 0;
    std::vector<uint8_t> packet;
    uint64_t statStart = NowUs(), statBytes = 0, statFrames = 0, lastHs = 0;

    for (;;) {
        // Datagrams from clients: handshake or encrypted control packets.
        uint8_t rbuf[512], plain[512];
        sockaddr_in from;
        while (true) {
            int fl = sizeof(from);
            int n = recvfrom(sock, reinterpret_cast<char*>(rbuf), sizeof(rbuf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < int(sizeof(rd::WireHeader))) break;
            rd::WireHeader wh;
            memcpy(&wh, rbuf, sizeof(wh));
            if (wh.magic != rd::kWireMagic) continue;

            if (wh.type == uint8_t(rd::WireType::Hs1)) {
                if (n != int(sizeof(wh) + rd::kHs1Len)) continue;
                // Client retransmits the same Hs1 until it sees our Hs2; answer duplicates with the
                // same cached Hs2 so the pending session stays consistent with what the client holds.
                if (pending.valid() && memcmp(rbuf + sizeof(wh), cachedHs1, rd::kHs1Len) == 0) {
                    sendto(sock, reinterpret_cast<char*>(cachedHs2), sizeof(cachedHs2), 0,
                           reinterpret_cast<sockaddr*>(&from), sizeof(from));
                    continue;
                }
                if (NowUs() - lastHs < 100'000) continue;  // rate-limit new handshakes
                lastHs = NowUs();
                rd::WireHeader oh = {rd::kWireMagic, uint8_t(rd::WireType::Hs2), {}, 0};
                memcpy(cachedHs2, &oh, sizeof(oh));
                if (!rd::HostRespond(psk, rbuf + sizeof(wh), cachedHs2 + sizeof(oh), pending)) continue;
                memcpy(cachedHs1, rbuf + sizeof(wh), rd::kHs1Len);
                sendto(sock, reinterpret_cast<char*>(cachedHs2), sizeof(cachedHs2), 0,
                       reinterpret_cast<sockaddr*>(&from), sizeof(from));
                continue;
            }
            if (wh.type != uint8_t(rd::WireType::Data)) continue;

            size_t pl = 0;
            bool ok = active.valid() && active.Open(rbuf, n, plain, pl);
            if (!ok && pending.valid() && pending.Open(rbuf, n, plain, pl)) {
                // First authenticated packet of a new handshake: the client knows the pairing key.
                active = std::move(pending);
                pending = rd::Session();
                ok = true;
                forceKey = true;
                printf("client connected from %d.%d.%d.%d:%u\n", from.sin_addr.S_un.S_un_b.s_b1,
                       from.sin_addr.S_un.S_un_b.s_b2, from.sin_addr.S_un.S_un_b.s_b3,
                       from.sin_addr.S_un.S_un_b.s_b4, ntohs(from.sin_port));
            }
            if (!ok || pl < sizeof(rd::PacketHeader)) continue;
            rd::PacketHeader h;
            memcpy(&h, plain, sizeof(h));
            if (h.magic != rd::kMagic) continue;
            peer.addr = from;  // authenticated, so following the client across NAT rebinding is safe
            peer.valid = true;
            peer.lastSeenUs = NowUs();
            if (h.type == uint8_t(rd::PacketType::KeyReq)) forceKey = true;
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

        SendFrame(sock, peer, active, packet, frameId++, key, capUs);
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
