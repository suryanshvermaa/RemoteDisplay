#pragma once
// The host: capture -> hardware H.264 -> encrypted UDP. Runs on its own thread, stoppable, and
// reports state through HostStatus so both the CLI (rd-host) and the GUI app can drive it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "capture.h"
#include "encoder.h"
#include "protocol.h"
#include "session.h"

namespace rdh {

struct HostConfig {
    std::string listenIp = "0.0.0.0";
    int port = rd::kDefaultPort;
    int fps = 60;
    int mbps = 20;
    std::array<uint8_t, rd::kKeyLen> psk{};
    bool requireApproval = true;  // hold video until the local user clicks Allow
};

struct HostStatus {
    enum State { Stopped, Starting, Waiting, AwaitingApproval, Streaming, Failed };
    std::atomic<int> state{Stopped};
    std::atomic<float> fps{0}, mbps{0};
    std::atomic<bool> approve{false}, deny{false}, kick{false};

    void SetText(std::string& dst, const std::string& v) { std::lock_guard<std::mutex> l(m_); dst = v; }
    std::string Peer() { std::lock_guard<std::mutex> l(m_); return peer_; }
    std::string Error() { std::lock_guard<std::mutex> l(m_); return error_; }
    void SetPeer(const std::string& v) { SetText(peer_, v); }
    void SetError(const std::string& v) { SetText(error_, v); }

private:
    std::mutex m_;
    std::string peer_, error_;
};

namespace detail {
inline uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Peer {
    sockaddr_in addr = {};
    bool valid = false;
    uint64_t lastSeenUs = 0;
};

inline void SendFrame(SOCKET s, const Peer& peer, rd::Session& sess, const std::vector<uint8_t>& data,
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

inline std::string IpOnly(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return ip;
}

inline std::string AddrToString(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

struct ComScope {
    ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};
struct SocketGuard {
    SOCKET s;
    ~SocketGuard() { if (s != INVALID_SOCKET) closesocket(s); }
};
}  // namespace detail

// Blocks until `stop` is set. Safe to call again after it returns.
inline void RunHost(const HostConfig& cfg, std::atomic<bool>& stop, HostStatus& st) {
    using detail::NowUs;
    st.state = HostStatus::Starting;
    st.SetError("");
    detail::ComScope com;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    detail::SocketGuard sock{socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    int buf = 4 << 20;
    setsockopt(sock.s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&buf), sizeof(buf));
    u_long nb = 1;
    ioctlsocket(sock.s, FIONBIO, &nb);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(cfg.port));
    if (inet_pton(AF_INET, cfg.listenIp.c_str(), &addr.sin_addr) != 1 ||
        bind(sock.s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        st.SetError("Could not listen on " + cfg.listenIp + ":" + std::to_string(cfg.port) +
                    " (port in use?)");
        st.state = HostStatus::Failed;
        return;
    }

    Capture cap;
    Encoder enc;
    auto initPipeline = [&]() {
        if (!cap.Init()) return false;
        return enc.Init(cap.device(), cap.width(), cap.height(), cfg.fps, cfg.mbps * 1'000'000);
    };
    if (!initPipeline()) {
        st.SetError("Screen capture or hardware H.264 encoder is unavailable on this PC");
        st.state = HostStatus::Failed;
        return;
    }

    detail::Peer peer;
    rd::Session active, pending;
    uint8_t cachedHs1[rd::kHs1Len] = {};
    uint8_t cachedHs2[sizeof(rd::WireHeader) + rd::kHs2Len] = {};
    bool forceKey = true;
    uint32_t frameId = 0;
    std::vector<uint8_t> packet;
    uint64_t statStart = NowUs(), statBytes = 0, statFrames = 0, lastHs = 0, blockUntil = 0;
    // A client the user approved may re-pair (network blip, restart) from the same IP within this
    // window without a new prompt. Kick/deny clears it.
    constexpr uint64_t kApprovalGraceUs = 120'000'000;
    std::string peerIp, approvedIp;
    uint64_t approvedAt = 0, lastKeepalive = 0;
    st.state = HostStatus::Waiting;

    auto dropClient = [&](uint64_t blockMs) {
        active = rd::Session();
        pending = rd::Session();
        peer.valid = false;
        if (blockMs > 0) approvedIp.clear();  // kicked or denied: forget the approval
        blockUntil = NowUs() + blockMs * 1000;
        st.fps = 0;
        st.mbps = 0;
        st.state = HostStatus::Waiting;
        st.SetPeer("");
    };

    while (!stop) {
        // --- UI commands -------------------------------------------------------------------
        if (st.kick.exchange(false)) dropClient(10'000);  // refuse re-pairing for 10 s
        if (st.state == HostStatus::AwaitingApproval) {
            if (st.approve.exchange(false)) {
                st.state = HostStatus::Streaming;
                approvedIp = peerIp;
                forceKey = true;
            } else if (st.deny.exchange(false)) {
                dropClient(30'000);
            }
        }

        // --- Datagrams from clients: handshake or encrypted control packets -----------------
        uint8_t rbuf[512], plain[512];
        sockaddr_in from;
        while (true) {
            int fl = sizeof(from);
            int n = recvfrom(sock.s, reinterpret_cast<char*>(rbuf), sizeof(rbuf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < int(sizeof(rd::WireHeader))) break;
            rd::WireHeader wh;
            memcpy(&wh, rbuf, sizeof(wh));
            if (wh.magic != rd::kWireMagic) continue;

            if (wh.type == uint8_t(rd::WireType::Hs1)) {
                if (n != int(sizeof(wh) + rd::kHs1Len) || NowUs() < blockUntil) continue;
                // Client retransmits the same Hs1 until it sees our Hs2; answer duplicates with the
                // same cached Hs2 so the pending session stays consistent with what the client holds.
                if (pending.valid() && memcmp(rbuf + sizeof(wh), cachedHs1, rd::kHs1Len) == 0) {
                    sendto(sock.s, reinterpret_cast<char*>(cachedHs2), sizeof(cachedHs2), 0,
                           reinterpret_cast<sockaddr*>(&from), sizeof(from));
                    continue;
                }
                if (NowUs() - lastHs < 100'000) continue;  // rate-limit new handshakes
                lastHs = NowUs();
                rd::WireHeader oh = {rd::kWireMagic, uint8_t(rd::WireType::Hs2), {}, 0};
                memcpy(cachedHs2, &oh, sizeof(oh));
                if (!rd::HostRespond(cfg.psk.data(), rbuf + sizeof(wh), cachedHs2 + sizeof(oh), pending))
                    continue;
                memcpy(cachedHs1, rbuf + sizeof(wh), rd::kHs1Len);
                sendto(sock.s, reinterpret_cast<char*>(cachedHs2), sizeof(cachedHs2), 0,
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
                peerIp = detail::IpOnly(from);
                st.SetPeer(detail::AddrToString(from));
                const bool preApproved = !cfg.requireApproval ||
                                         (peerIp == approvedIp && NowUs() - approvedAt < kApprovalGraceUs);
                if (preApproved) {
                    approvedIp = peerIp;
                    st.state = HostStatus::Streaming;
                } else {
                    st.approve = false;
                    st.deny = false;
                    st.state = HostStatus::AwaitingApproval;
                }
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
        if (peer.valid && NowUs() - peer.lastSeenUs > 5'000'000) dropClient(0);  // client vanished
        if (st.state == HostStatus::Streaming) approvedAt = NowUs();

        // Keepalive: a static screen (or a pending approval) sends no video, and the client treats
        // 3 s of silence as "host gone". Tiny encrypted packet once a second.
        if (peer.valid && active.valid() && NowUs() - lastKeepalive > 1'000'000) {
            rd::PacketHeader kh = {};
            kh.magic = rd::kMagic;
            kh.type = uint8_t(rd::PacketType::Hello);
            uint8_t kw[sizeof(rd::WireHeader) + sizeof(kh) + rd::kTagLen];
            size_t kn = active.Seal(reinterpret_cast<uint8_t*>(&kh), sizeof(kh), kw);
            if (kn) sendto(sock.s, reinterpret_cast<char*>(kw), int(kn), 0,
                           reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
            lastKeepalive = NowUs();
        }

        // --- Capture + encode + send --------------------------------------------------------
        bool lost = false;
        ID3D11Texture2D* tex = cap.Grab(peer.valid ? 8 : 100, lost);
        if (lost) {
            if (!initPipeline()) Sleep(500);
            forceKey = true;
            continue;
        }
        if (!tex && forceKey) tex = cap.Last();  // static screen but client needs an IDR
        if (!tex || !peer.valid || st.state != HostStatus::Streaming) continue;

        const uint64_t capUs = NowUs();
        bool key = false;
        if (!enc.Encode(tex, forceKey, packet, key)) continue;
        if (forceKey && key) forceKey = false;

        detail::SendFrame(sock.s, peer, active, packet, frameId++, key, capUs);
        statBytes += packet.size();
        ++statFrames;
        if (NowUs() - statStart >= 1'000'000) {
            double sec = (NowUs() - statStart) / 1e6;
            st.fps = float(statFrames / sec);
            st.mbps = float(statBytes * 8 / sec / 1e6);
            statStart = NowUs();
            statBytes = statFrames = 0;
        }
    }
    st.fps = 0;
    st.mbps = 0;
    st.SetPeer("");
    st.state = HostStatus::Stopped;
}

}  // namespace rdh
