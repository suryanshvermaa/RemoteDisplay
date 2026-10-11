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
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "capture.h"
#include "encoder.h"
#include "identity.h"
#include "inject.h"
#include "protocol.h"
#include "session.h"

namespace rdh {

// Where "approved once, trusted from now on" devices are kept (implemented by the app).
struct TrustStore {
    virtual ~TrustStore() = default;
    virtual bool IsTrusted(const rd::DeviceId& id) = 0;
    virtual void Remember(const rd::DeviceId& id, const std::string& name) = 0;
};

// Frames from the secure-desktop helper (UAC prompt / sign-in screen). Set only when the service runs
// the helper and the administrator allowed it; otherwise the host captures the desktop itself.
struct SecureSource {
    virtual ~SecureSource() = default;
    // True while the helper reports the secure desktop is the input desktop (and is alive).
    virtual bool Active() = 0;
    // Uploads the newest secure frame into `cap` and returns it. Without `needFrame` it returns nullptr
    // unless the frame is new; with it (a viewer needs a picture now) it returns the current frame.
    virtual ID3D11Texture2D* Upload(Capture& cap, bool needFrame) = 0;
};

struct HostConfig {
    std::string listenIp = "0.0.0.0";
    int port = rd::kDefaultPort;
    int fps = 60;
    int mbps = 20;
    std::array<uint8_t, rd::kKeyLen> psk{};
    bool requireApproval = true;  // hold video until the local user clicks Allow
    bool allowControl = true;     // accept mouse/keyboard input from the (approved) viewer
    TrustStore* trust = nullptr;  // devices that skip the approval prompt; may be null
    SecureSource* secure = nullptr;  // UAC / sign-in screen frames from the helper; may be null
    std::function<void(const std::string&)> log;  // optional diagnostics (the app writes them to its log)
    std::wstring displayName;  // GDI name of the output to capture (e.g. \\.\DISPLAY22); empty = first output
};

struct HostStatus {
    enum State { Stopped, Starting, Waiting, AwaitingApproval, Streaming, Failed };
    std::atomic<int> state{Stopped};
    std::atomic<float> fps{0}, mbps{0};
    std::atomic<bool> approve{false}, deny{false}, kick{false};
    std::atomic<bool> remember{false};      // the user chose "Always allow this device"
    std::atomic<bool> controlling{false};  // the viewer has used mouse/keyboard in the last 2 s

    void SetText(std::string& dst, const std::string& v) { std::lock_guard<std::mutex> l(m_); dst = v; }
    std::string Peer() { std::lock_guard<std::mutex> l(m_); return peer_; }
    std::string Error() { std::lock_guard<std::mutex> l(m_); return error_; }
    void SetPeer(const std::string& v) { SetText(peer_, v); }
    std::string PeerName() { std::lock_guard<std::mutex> l(m_); return peerName_; }
    void SetPeerName(const std::string& v) { SetText(peerName_, v); }
    bool PeerKnown() { std::lock_guard<std::mutex> l(m_); return !peerName_.empty(); }
    void SetError(const std::string& v) { SetText(error_, v); }

private:
    std::mutex m_;
    std::string peer_, error_, peerName_;
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

inline void SendCursor(SOCKET s, const Peer& peer, rd::Session& sess, const std::vector<uint8_t>& blob,
                       uint32_t id) {
    const size_t total = (blob.size() + rd::kMaxPayload - 1) / rd::kMaxPayload;
    uint8_t plain[sizeof(rd::PacketHeader) + rd::kMaxPayload];
    uint8_t wire[sizeof(rd::WireHeader) + sizeof(plain) + rd::kTagLen];
    for (size_t i = 0; i < total; ++i) {
        const size_t off = i * rd::kMaxPayload;
        const size_t n = std::min(rd::kMaxPayload, blob.size() - off);
        rd::PacketHeader h{rd::kMagic, uint8_t(rd::PacketType::Cursor), 0, uint16_t(i), uint16_t(total),
                           uint16_t(n), id, 0};
        memcpy(plain, &h, sizeof(h));
        memcpy(plain + sizeof(h), blob.data() + off, n);
        size_t wn = sess.Seal(plain, sizeof(h) + n, wire);
        if (!wn) return;
        sendto(s, reinterpret_cast<char*>(wire), int(wn), 0,
               reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
    }
}

inline void SendAck(SOCKET s, const Peer& peer, rd::Session& sess, uint32_t nextSeq) {
    rd::PacketHeader h = {};
    h.magic = rd::kMagic;
    h.type = uint8_t(rd::PacketType::InputAck);
    h.frameId = nextSeq;
    uint8_t wire[sizeof(rd::WireHeader) + sizeof(h) + rd::kTagLen];
    size_t wn = sess.Seal(reinterpret_cast<uint8_t*>(&h), sizeof(h), wire);
    if (wn) sendto(s, reinterpret_cast<char*>(wire), int(wn), 0,
                   reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
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
    SetProcessDPIAware();  // physical-pixel virtual-screen metrics for mapping viewer input
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
    cap.SetOutputName(cfg.displayName);
    Encoder enc;
    InputInjector inject;
    bool forceKey = true;
    uint32_t encGen = ~0u;   // capture device generation the encoder was built for
    bool encReady = false;
    bool dupDown = false;    // duplication is currently unavailable (logged once per outage)
    auto note = [&](const std::string& m) {
        if (cfg.log) cfg.log(m);
    };
    // The encoder follows the capture device: rebuilt whenever the device is (new GPU, new mode).
    auto followDevice = [&]() {
        if (cap.Generation() == encGen) return encReady;
        encGen = cap.Generation();
        encReady = false;
        if (cap.HasDevice()) {
            encReady = enc.Init(cap.device(), cap.width(), cap.height(), cfg.fps, cfg.mbps * 1'000'000);
            inject.SetDesktopRect(cap.desktopRect());
            forceKey = true;
            note(encReady ? "encoder ready " + std::to_string(cap.width()) + "x" + std::to_string(cap.height())
                          : std::string("encoder setup failed"));
        }
        return encReady;
    };
    // Duplication may be unavailable (secure desktop up); the device is kept either way.
    auto refresh = [&]() {
        const bool dup = cap.Init();
        if (!dup && !dupDown) {
            dupDown = true;
            note("screen duplication unavailable; retrying (secure desktop or mode change)");
        } else if (dup && dupDown) {
            dupDown = false;
            note("screen duplication restored");
        }
        return dup && followDevice();
    };
    cap.Init();  // duplication may be unavailable at start (secure desktop up); the device still works
    if (!followDevice()) {
        st.SetError("Screen capture or hardware H.264 encoder is unavailable on this PC");
        st.state = HostStatus::Failed;
        return;
    }

    detail::Peer peer;
    rd::Session active, pending;
    uint8_t cachedHs1[rd::kHs1Len] = {};
    uint8_t cachedHs2[sizeof(rd::WireHeader) + rd::kHs2Len] = {};
    uint64_t secureFrames = 0, secureStatStart = 0;  // diagnostics for the secure-desktop picture
    uint32_t frameId = 0;
    std::vector<uint8_t> packet;
    uint64_t statStart = NowUs(), statBytes = 0, statFrames = 0, lastHs = 0, blockUntil = 0;
    // A client the user approved may re-pair (network blip) from the same IP within this short window
    // without a new prompt. Devices the user chose to trust are remembered for good (cfg.trust).
    // Kick/deny clears the grace.
    constexpr uint64_t kApprovalGraceUs = 20'000'000;
    std::string peerIp, approvedIp, peerName;
    rd::DeviceId peerDev{};
    bool haveDev = false;  // the viewer proved which device it is (Identify)
    uint64_t approvedAt = 0, lastKeepalive = 0, lastControlUs = 0, lastCursorSend = 0;
    uint32_t inputNext = 0;  // next reliable input sequence number we expect from the viewer
    bool cursorSendNow = false;
    st.state = HostStatus::Waiting;

    auto dropClient = [&](uint64_t blockMs) {
        active = rd::Session();
        pending = rd::Session();
        peer.valid = false;
        if (blockMs > 0) approvedIp.clear();  // kicked or denied: forget the approval
        blockUntil = NowUs() + blockMs * 1000;
        inject.ReleaseAll();  // never leave keys or mouse buttons stuck down
        haveDev = false;
        peerName.clear();
        st.SetPeerName("");
        st.remember = false;
        st.controlling = false;
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
                if (st.remember.exchange(false) && haveDev && cfg.trust) cfg.trust->Remember(peerDev, peerName);
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
                inputNext = 0;
                cursorSendNow = true;
                peerIp = detail::IpOnly(from);
                st.SetPeer(detail::AddrToString(from));
                // Which device is this? The first packet of a viewer is its signed Identify.
                haveDev = false;
                peerName.clear();
                st.SetPeerName("");
                st.remember = false;
                if (pl >= sizeof(rd::PacketHeader)) {
                    rd::PacketHeader fh;
                    memcpy(&fh, plain, sizeof(fh));
                    if (fh.magic == rd::kMagic && fh.type == uint8_t(rd::PacketType::Identify) &&
                        rd::ParseIdentify(plain + sizeof(fh), pl - sizeof(fh), active, peerDev, peerName)) {
                        haveDev = true;
                        st.SetPeerName(peerName.empty() ? "unnamed device" : peerName);
                    }
                }
                const bool trustedDevice = haveDev && cfg.trust && cfg.trust->IsTrusted(peerDev);
                const bool preApproved = !cfg.requireApproval || trustedDevice ||
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
            if (h.type == uint8_t(rd::PacketType::KeyReq)) {
                forceKey = true;
            } else if (h.type == uint8_t(rd::PacketType::Identify)) {
                if (!haveDev && rd::ParseIdentify(plain + sizeof(h), pl - sizeof(h), active, peerDev, peerName)) {
                    haveDev = true;
                    st.SetPeerName(peerName.empty() ? "unnamed device" : peerName);
                    if (st.state == HostStatus::AwaitingApproval && cfg.trust && cfg.trust->IsTrusted(peerDev)) {
                        approvedIp = peerIp;  // a trusted device: no need to ask
                        st.state = HostStatus::Streaming;
                        forceKey = true;
                    }
                }
            } else if (h.type == uint8_t(rd::PacketType::InputMove) || h.type == uint8_t(rd::PacketType::Input)) {
                // Input only counts after the local user approved this viewer AND allows control.
                if (st.state != HostStatus::Streaming || !cfg.allowControl) continue;
                // The consent covers viewing the UAC prompt / sign-in screen, not typing into it: input
                // is received and acknowledged there but never injected.
                const bool injectOk = !(cfg.secure && cfg.secure->Active());
                const uint8_t* body = plain + sizeof(h);
                const size_t bl = pl - sizeof(h);
                if (h.type == uint8_t(rd::PacketType::InputMove)) {
                    if (bl < sizeof(rd::MoveBody)) continue;
                    rd::MoveBody mb;
                    memcpy(&mb, body, sizeof(mb));
                    if (injectOk) inject.Move(mb.x, mb.y);
                } else {
                    if (bl < sizeof(rd::InputBody)) continue;
                    rd::InputBody ib;
                    memcpy(&ib, body, sizeof(ib));
                    if (ib.count == 0 || ib.count > rd::kMaxInputEventsPerPacket ||
                        bl < sizeof(ib) + size_t(ib.count) * sizeof(rd::InputEvent))
                        continue;
                    for (unsigned i = 0; i < ib.count; ++i) {
                        const uint32_t seq = ib.firstSeq + i;
                        if (int32_t(seq - inputNext) < 0) continue;  // already applied (retransmit)
                        if (seq != inputNext) break;                 // gap: viewer resends from its oldest unacked
                        rd::InputEvent ev;
                        memcpy(&ev, body + sizeof(ib) + i * sizeof(ev), sizeof(ev));
                        if (injectOk) inject.Apply(ev);
                        ++inputNext;
                    }
                    detail::SendAck(sock.s, peer, active, inputNext);
                }
                lastControlUs = NowUs();
                st.controlling = true;
            }
        }
        if (st.controlling && NowUs() - lastControlUs > 2'000'000) st.controlling = false;
        if (peer.valid && NowUs() - peer.lastSeenUs > 5'000'000) dropClient(0);  // client vanished
        if (st.state == HostStatus::Streaming) approvedAt = NowUs();

        // Keepalive: a static screen (or a pending approval) sends no video, and the client treats
        // 3 s of silence as "host gone". Tiny encrypted packet once a second.
        if (peer.valid && active.valid() && NowUs() - lastKeepalive > 1'000'000) {
            rd::PacketHeader kh = {};
            kh.magic = rd::kMagic;
            kh.type = uint8_t(rd::PacketType::Hello);
            kh.flags = cfg.allowControl ? uint8_t(rd::kFlagControlAllowed) : uint8_t(0);
            uint8_t kw[sizeof(rd::WireHeader) + sizeof(kh) + rd::kTagLen];
            size_t kn = active.Seal(reinterpret_cast<uint8_t*>(&kh), sizeof(kh), kw);
            if (kn) sendto(sock.s, reinterpret_cast<char*>(kw), int(kn), 0,
                           reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
            lastKeepalive = NowUs();
        }

        // --- Capture + encode + send --------------------------------------------------------
        // While the UAC prompt or sign-in screen is up, our own duplication cannot see it: take the
        // helper's frames instead. The pointer is not available there, so no cursor is sent.
        const bool secureNow = cfg.secure && cfg.secure->Active();
        bool lost = false;
        ID3D11Texture2D* tex = nullptr;
        if (secureNow) {
            if (!peer.valid) {
                Sleep(50);  // nobody is watching yet: don't copy frames
            } else if (followDevice()) {
                tex = cfg.secure->Upload(cap, forceKey);
                if (tex) ++secureFrames;
            } else {
                Sleep(100);
            }
            if (NowUs() - secureStatStart >= 10'000'000) {
                if (secureStatStart) note("secure picture: " + std::to_string(secureFrames) + " new frames in 10 s");
                secureFrames = 0;
                secureStatStart = NowUs();
            }
        } else {
            tex = cap.Grab(peer.valid ? 4 : 100, lost);
            if (lost) {
                if (!refresh()) Sleep(300);  // no duplication yet (lock screen, mode change): retry shortly
                forceKey = true;
                continue;
            }
        }
        // The cursor shape travels separately (the picture has no pointer in it): send on change,
        // on a new client, and every 2 s so a lost fragment heals itself.
        if (!secureNow && cap.TakeCursorChange()) cursorSendNow = true;
        if (!secureNow && peer.valid && active.valid() && st.state == HostStatus::Streaming && !cap.CursorBlob().empty() &&
            (cursorSendNow || NowUs() - lastCursorSend > 2'000'000)) {
            detail::SendCursor(sock.s, peer, active, cap.CursorBlob(), cap.CursorId());
            lastCursorSend = NowUs();
            cursorSendNow = false;
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
    inject.ReleaseAll();
    st.controlling = false;
    st.fps = 0;
    st.mbps = 0;
    st.SetPeer("");
    st.state = HostStatus::Stopped;
}

}  // namespace rdh
