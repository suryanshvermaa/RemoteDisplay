// Integration test for the host's input path. Needs a running host:
//   rd-host --key <KEY> --listen 127.0.0.1
//   test-input <KEY>
// It connects like a real viewer, then drives the mouse and keyboard of THIS machine through the
// host. It only types after confirming its own test window is the foreground window, so keys can
// never land in another application.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "protocol.h"
#include "session.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } else { printf("ok   %s\n", #c); } } while (0)

struct Conn {
    SOCKET s = INVALID_SOCKET;
    sockaddr_in host = {};
    rd::Session sess;
    bool controlAllowed = false;
    uint32_t acked = 0;
    // reassembled cursor
    uint32_t curId = 0, curCount = 0, curGot = 0;
    std::vector<uint8_t> curBuf, curDone;
    std::vector<bool> curSeen;
    uint32_t nextSeq = 0;
};

static void SendInner(Conn& c, uint8_t type, uint32_t frameId, const void* body, size_t n) {
    uint8_t plain[256], wire[512];
    rd::PacketHeader h = {};
    h.magic = rd::kMagic;
    h.type = type;
    h.frameId = frameId;
    h.payloadLen = uint16_t(n);
    memcpy(plain, &h, sizeof(h));
    if (n) memcpy(plain + sizeof(h), body, n);
    size_t wn = c.sess.Seal(plain, sizeof(h) + n, wire);
    sendto(c.s, reinterpret_cast<char*>(wire), int(wn), 0, reinterpret_cast<sockaddr*>(&c.host), sizeof(c.host));
}

static void Pump(Conn& c, DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        uint8_t buf[1600], plain[1600];
        for (;;) {
            int n = recv(c.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) break;
            size_t pl = 0;
            if (!c.sess.valid() || !c.sess.Open(buf, size_t(n), plain, pl) || pl < sizeof(rd::PacketHeader)) continue;
            rd::PacketHeader h;
            memcpy(&h, plain, sizeof(h));
            if (h.magic != rd::kMagic) continue;
            if (h.type == uint8_t(rd::PacketType::Hello)) {
                c.controlAllowed = (h.flags & rd::kFlagControlAllowed) != 0;
            } else if (h.type == uint8_t(rd::PacketType::InputAck)) {
                if (int32_t(h.frameId - c.acked) > 0) c.acked = h.frameId;
            } else if (h.type == uint8_t(rd::PacketType::Cursor) && h.fragCount) {
                if (h.frameId != c.curId || h.fragCount != c.curCount) {
                    c.curId = h.frameId; c.curCount = h.fragCount; c.curGot = 0;
                    c.curBuf.assign(size_t(h.fragCount) * rd::kMaxPayload, 0);
                    c.curSeen.assign(h.fragCount, false);
                }
                if (h.fragIdx < c.curCount && !c.curSeen[h.fragIdx]) {
                    memcpy(&c.curBuf[size_t(h.fragIdx) * rd::kMaxPayload], plain + sizeof(h), h.payloadLen);
                    c.curSeen[h.fragIdx] = true;
                    if (h.fragIdx + 1 == c.curCount) c.curBuf.resize(size_t(h.fragIdx) * rd::kMaxPayload + h.payloadLen);
                    if (++c.curGot == c.curCount) c.curDone = c.curBuf;
                }
            }
        }
        Sleep(2);
    } while (GetTickCount() < end);
}

static bool Connect(Conn& c, const uint8_t psk[rd::kKeyLen], int port) {
    c.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(c.s, FIONBIO, &nb);
    c.host.sin_family = AF_INET;
    c.host.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, "127.0.0.1", &c.host.sin_addr);
    rd::ClientHandshake hs;
    uint8_t out[sizeof(rd::WireHeader) + rd::kHs1Len];
    rd::WireHeader oh = {rd::kWireMagic, uint8_t(rd::WireType::Hs1), {}, 0};
    memcpy(out, &oh, sizeof(oh));
    if (!hs.Begin(out + sizeof(oh))) return false;
    for (int tries = 0; tries < 20; ++tries) {
        sendto(c.s, reinterpret_cast<char*>(out), sizeof(out), 0, reinterpret_cast<sockaddr*>(&c.host), sizeof(c.host));
        Sleep(150);
        uint8_t buf[512];
        int n = recv(c.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n == int(sizeof(rd::WireHeader) + rd::kHs2Len)) {
            rd::WireHeader wh;
            memcpy(&wh, buf, sizeof(wh));
            if (wh.type == uint8_t(rd::WireType::Hs2) && hs.Finish(psk, buf + sizeof(wh), c.sess)) {
                SendInner(c, uint8_t(rd::PacketType::Hello), 0, nullptr, 0);  // activates the host session
                return true;
            }
        }
    }
    return false;
}

static void SendInput(Conn& c, const std::vector<rd::InputEvent>& evs, uint32_t firstSeq) {
    uint8_t body[sizeof(rd::InputBody) + rd::kMaxInputEventsPerPacket * sizeof(rd::InputEvent)];
    rd::InputBody ib = {firstSeq, uint8_t(evs.size())};
    memcpy(body, &ib, sizeof(ib));
    memcpy(body + sizeof(ib), evs.data(), evs.size() * sizeof(rd::InputEvent));
    SendInner(c, uint8_t(rd::PacketType::Input), 0, body, sizeof(ib) + evs.size() * sizeof(rd::InputEvent));
}

static void Send(Conn& c, const std::vector<rd::InputEvent>& evs) {  // reliable: advances our sequence
    SendInput(c, evs, c.nextSeq);
    c.nextSeq += uint32_t(evs.size());
}

static uint16_t Norm(int px, int extent) { return uint16_t(uint64_t(px) * 65535 / (extent - 1)); }

static rd::InputEvent Key(UINT vk, bool up) {
    rd::InputEvent e = {};
    e.type = uint8_t(rd::InputType::Key);
    e.flags = up ? 1 : 0;
    e.x = uint16_t(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    e.y = uint16_t(vk);
    return e;
}

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char** argv) {
    SetProcessDPIAware();
    if (argc < 2) { printf("usage: test-input <pairing key> [port]\n"); return 2; }
    uint8_t psk[rd::kKeyLen];
    if (!rd::ParseKey(argv[1], psk)) { printf("bad key\n"); return 2; }
    const int port = argc > 2 ? atoi(argv[2]) : rd::kDefaultPort;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // A topmost test window with an edit box, in a known place.
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"rdinputtest";
    RegisterClassW(&wc);
    HWND win = CreateWindowExW(WS_EX_TOPMOST, L"rdinputtest", L"input test", WS_POPUP | WS_VISIBLE | WS_BORDER, 300, 300,
                               420, 220, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 0, 0, 418, 218, win, nullptr,
                                wc.hInstance, nullptr);
    const int SW_ = GetSystemMetrics(SM_CXSCREEN), SH_ = GetSystemMetrics(SM_CYSCREEN);

    Conn c;
    CHECK(Connect(c, psk, port), "could not pair with the host (is rd-host running with this key?)");
    if (g_fail) return 1;
    Pump(c, 1500);
    CHECK(c.controlAllowed, "host should advertise control");
    CHECK(!c.curDone.empty(), "cursor shape should have arrived");
    if (c.curDone.size() >= sizeof(rd::CursorBlobHeader)) {
        rd::CursorBlobHeader ch;
        memcpy(&ch, c.curDone.data(), sizeof(ch));
        printf("     cursor %ux%u hotspot %d,%d, %zu bytes\n", ch.w, ch.h, ch.hotX, ch.hotY, c.curDone.size());
        CHECK(ch.w > 0 && ch.w <= 256 && ch.h > 0 && ch.h <= 256 &&
                  c.curDone.size() == sizeof(ch) + size_t(ch.w) * ch.h * 4,
              "cursor blob size must match its header");
    }

    // 1) unreliable mouse move lands on the right pixel
    rd::MoveBody mb = {Norm(500, SW_), Norm(400, SH_)};
    SendInner(c, uint8_t(rd::PacketType::InputMove), 0, &mb, sizeof(mb));
    Pump(c, 300);
    POINT p;
    GetCursorPos(&p);
    CHECK(abs(p.x - 500) <= 2 && abs(p.y - 400) <= 2, "cursor at %ld,%ld, wanted 500,400", p.x, p.y);

    // 2) click in the middle of the test window makes it foreground and focuses the edit
    const int cx = 510, cy = 410;
    rd::InputEvent down = {uint8_t(rd::InputType::Button), uint8_t(0x80 | 0), Norm(cx, SW_), Norm(cy, SH_), 0};
    rd::InputEvent up = down;
    up.flags = 0;
    Send(c, {down, up});
    Pump(c, 400);
    GetCursorPos(&p);
    CHECK(abs(p.x - cx) <= 2 && abs(p.y - cy) <= 2, "cursor after click %ld,%ld", p.x, p.y);
    CHECK(c.acked == c.nextSeq, "host must ack reliable input (acked %u of %u)", c.acked, c.nextSeq);
    const bool foreground = GetForegroundWindow() == win;
    CHECK(foreground, "click should have activated the test window");

    if (foreground) {
        // 3) typing: only the scancodes travel, the host's layout produces the characters
        std::vector<rd::InputEvent> evs;
        for (char ch : std::string("hello 123")) {
            UINT vk = ch == ' ' ? VK_SPACE : (ch >= '0' && ch <= '9') ? UINT(ch) : UINT(toupper(ch));
            evs.push_back(Key(vk, false));
            evs.push_back(Key(vk, true));
        }
        for (size_t i = 0; i < evs.size(); i += rd::kMaxInputEventsPerPacket) {
            size_t n = std::min(rd::kMaxInputEventsPerPacket, evs.size() - i);
            Send(c, std::vector<rd::InputEvent>(evs.begin() + i, evs.begin() + i + n));
        }
        Pump(c, 500);
        wchar_t text[64] = {};
        GetWindowTextW(edit, text, 64);
        CHECK(std::wstring(text) == L"hello 123", "edit contains '%ls'", text);

        // 4) a retransmitted (duplicate) packet must not type twice
        const uint32_t before = c.nextSeq;
        SendInput(c, {Key('A', false), Key('A', true)}, before);  // new events, seq = before
        c.nextSeq += 2;
        Pump(c, 200);
        SendInput(c, {Key('A', false), Key('A', true)}, before);  // same seq again: duplicate
        Pump(c, 300);
        GetWindowTextW(edit, text, 64);
        CHECK(std::wstring(text) == L"hello 123a", "duplicate must be ignored, edit has '%ls'", text);

        // 5) a gap (events from the future) must be ignored until the missing ones arrive
        SendInput(c, {Key('B', false), Key('B', true)}, c.nextSeq + 5);
        Pump(c, 300);
        GetWindowTextW(edit, text, 64);
        CHECK(std::wstring(text) == L"hello 123a", "out-of-order events must wait, edit has '%ls'", text);
    }

    // 6) viewer vanishes with Ctrl held: the host must release it (no stuck keys)
    Send(c, {Key(VK_LCONTROL, false)});
    Pump(c, 300);
    CHECK((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0, "Ctrl should be held while the viewer is connected");
    printf("     (viewer goes silent; waiting for the host to notice...)\n");
    Pump(c, 0);
    Sleep(6500);  // host drops a silent client after 5 s and releases everything it injected
    CHECK((GetAsyncKeyState(VK_LCONTROL) & 0x8000) == 0, "Ctrl must be released after the viewer disappears");

    printf(g_fail ? "%d FAILED\n" : "all input tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
