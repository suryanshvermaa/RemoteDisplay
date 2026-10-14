// RemoteDisplayClient: the viewer on its own. Install it on a PC that only needs to view a shared screen.
// It has no host, service, driver or firewall parts, and it needs no administrator rights.
//   RemoteDisplayClient.exe                                   connect window (address, pairing key, recents)
//   RemoteDisplayClient.exe --key <key> [--host <ip>] [--port <n>]   connect at once
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "viewer.h"

namespace {

constexpr UINT kMsgViewerClosed = WM_APP + 1;
enum Ctl { IDC_ADDR = 101, IDC_KEY, IDC_RECENT, IDC_CONNECT, IDC_STATUS };

HINSTANCE gInst = nullptr;
HWND gWnd = nullptr, gAddr = nullptr, gKey = nullptr, gRecent = nullptr, gStatus = nullptr;
std::unique_ptr<rdv::Viewer> gViewer;
rd::DeviceIdentity gIdentity;  // same permanent device key as the app, so "Always allow" works here too
std::vector<rdapp::Recent> gRecents;

std::string GetText(HWND h) {
    const int n = GetWindowTextLengthA(h);
    std::vector<char> buf(size_t(n) + 1, '\0');
    GetWindowTextA(h, buf.data(), n + 1);
    return std::string(buf.data());
}

void SetStatus(const std::string& s) { SetWindowTextA(gStatus, s.c_str()); }

void FillRecents() {
    SendMessageA(gRecent, CB_RESETCONTENT, 0, 0);
    gRecents = rdapp::Config().Recents();
    for (const auto& r : gRecents) SendMessageA(gRecent, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.addr.c_str()));
}

void Connect() {
    const std::string addrText = GetText(gAddr);
    std::array<uint8_t, rd::kKeyLen> psk{};
    if (!rd::ParseKey(GetText(gKey), psk.data())) {
        SetStatus("Enter the pairing key shown in the other PC's RemoteDisplay window.");
        return;
    }
    std::string ip;
    int port = rd::kDefaultPort;
    if (!rdapp::ResolveAddress(addrText, ip, port)) {
        SetStatus("Could not read that address. Use the IP address (or name), optionally with :port.");
        return;
    }
    gViewer.reset();  // one viewer at a time
    rdv::Viewer* v = rdv::Viewer::Create(gInst, gWnd, kMsgViewerClosed, ip, port, psk, &gIdentity, rdapp::DeviceName());
    if (!v) {
        SetStatus("Could not open the viewer window.");
        return;
    }
    gViewer.reset(v);
    rdapp::Recent r;
    r.addr = addrText;
    r.key = psk;
    rdapp::Config().AddRecent(r);
    FillRecents();
    SetStatus("Connecting to " + ip + ":" + std::to_string(port) + ". The viewer window shows the connection state.");
}

HWND Add(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
         DWORD exStyle = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return c;
}

// Called from WM_CREATE, where the window handle is the parent (gWnd is not set until CreateWindowEx returns).
void Build(HWND parent) {
    Add(parent, L"STATIC", L"Computer (IP address or name, optional :port)", SS_LEFT, 16, 16, 400, 18, 0);
    gAddr = Add(parent, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 16, 36, 400, 24, IDC_ADDR, WS_EX_CLIENTEDGE);
    Add(parent, L"STATIC", L"Pairing key", SS_LEFT, 16, 70, 400, 18, 0);
    gKey = Add(parent, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 16, 90, 400, 24, IDC_KEY, WS_EX_CLIENTEDGE);
    Add(parent, L"STATIC", L"Recent computers", SS_LEFT, 16, 124, 400, 18, 0);
    gRecent = Add(parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 16, 144, 400, 200, IDC_RECENT);
    Add(parent, L"BUTTON", L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 178, 400, 32, IDC_CONNECT);
    gStatus = Add(parent, L"STATIC", L"", SS_LEFT, 16, 218, 400, 40, IDC_STATUS);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            gWnd = h;
            Build(h);
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == IDC_CONNECT && HIWORD(w) == BN_CLICKED) {
                Connect();
            } else if (LOWORD(w) == IDC_RECENT && HIWORD(w) == CBN_SELCHANGE) {
                const LRESULT i = SendMessage(gRecent, CB_GETCURSEL, 0, 0);
                if (i >= 0 && size_t(i) < gRecents.size()) {
                    SetWindowTextA(gAddr, gRecents[size_t(i)].addr.c_str());
                    SetWindowTextA(gKey, rd::FormatKey(gRecents[size_t(i)].key.data()).c_str());
                }
            }
            return 0;
        case kMsgViewerClosed:
            if (gViewer && gViewer.get() == reinterpret_cast<rdv::Viewer*>(w)) {
                gViewer.reset();  // its thread is finished: the destructor joins it
                SetStatus("The viewer was closed. Press Connect to view again.");
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    gInst = inst;
    SetProcessDPIAware();  // real pixels: sharp picture, and pointer mapping matches the window
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);
    rdapp::Config().LoadDeviceIdentity(gIdentity);

    std::string host, port, key;
    for (int i = 1; i + 1 < __argc; i += 2) {
        const std::string a = __argv[i];
        if (a == "--host") host = __argv[i + 1];
        else if (a == "--port") port = __argv[i + 1];
        else if (a == "--key") key = __argv[i + 1];
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"RemoteDisplayClient";
    wc.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT rc = {0, 0, 432, 276};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    gWnd = CreateWindowExW(0, L"RemoteDisplayClient", L"RemoteDisplay Client", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!gWnd) return 1;

    FillRecents();
    if (!host.empty()) SetWindowTextA(gAddr, (port.empty() ? host : host + ":" + port).c_str());
    else SetWindowTextA(gAddr, "127.0.0.1");
    if (!key.empty()) SetWindowTextA(gKey, key.c_str());
    ShowWindow(gWnd, show);
    UpdateWindow(gWnd);
    if (!key.empty()) Connect();  // started with --key: connect at once

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    gViewer.reset();
    WSACleanup();
    return 0;
}
