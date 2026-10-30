// RemoteDisplayClient: the viewer on its own. Install it on a PC that only needs to view a shared screen.
// It has no host, service, driver or firewall parts, and it needs no administrator rights.
//   RemoteDisplayClient.exe                                   connect window (address, pairing key, recents)
//   RemoteDisplayClient.exe --key <key> [--host <ip>] [--port <n>]   connect at once
#include <array>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "discovery.h"
#include "viewer.h"
#include "ui_theme.h"

// Modern (visual-style) common controls for the drop-down lists and scroll bars.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

enum Ctl { IDC_ADDR = 101, IDC_KEY, IDC_RECENT, IDC_CONNECT, IDC_STATUS, IDC_DEVICES, IDC_FIND, IDC_LBL };

// Same look as the main app (ui_theme.h), with the same light / dark / auto choice.
constexpr int kTopBar = 56, kWinW = 432, kWinH = 452;  // 96-dpi pixels
int gDpi = 96;
HFONT gFont = nullptr, gFontBold = nullptr, gFontBig = nullptr, gFontHeader = nullptr, gFontSmall = nullptr,
      gFontMono = nullptr;
std::vector<std::pair<HWND, RECT>> gFields;  // text fields: the edit and the rounded box painted around it
int S(int v) { return MulDiv(v, gDpi, 96); }

const wchar_t* UiFontFace() {
    static const wchar_t* face = [] {
        using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW vi = {sizeof(vi)};
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&vi);
        return vi.dwBuildNumber >= 22000 ? L"Segoe UI Variable Text" : L"Segoe UI";
    }();
    return face;
}
HFONT MakeFont(int pt, bool bold, const wchar_t* face = nullptr) {
    return CreateFontW(-MulDiv(pt, gDpi, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face ? face : UiFontFace());
}

constexpr UINT kMsgViewerClosed = WM_APP + 1, kMsgDevices = WM_APP + 2;

HINSTANCE gInst = nullptr;
HWND gWnd = nullptr, gAddr = nullptr, gKey = nullptr, gRecent = nullptr, gStatus = nullptr, gDevices = nullptr;
std::unique_ptr<rdv::Viewer> gViewer;
rd::DeviceIdentity gIdentity;  // same permanent device key as the app, so "Always allow" works here too
std::vector<rdapp::Recent> gRecents;
std::vector<rdapp::FoundDevice> gFound;  // PCs that answered the last network search
bool gFinding = false;

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

// Asks the network which PCs are sharing. Runs in the background; the answer arrives as kMsgDevices.
void FindDevicesAsync() {
    if (gFinding) return;
    gFinding = true;
    SetStatus("Searching the network...");
    const HWND target = gWnd;
    std::thread([target] {
        auto* found = new std::vector<rdapp::FoundDevice>(rdapp::FindDevices());
        if (!PostMessageW(target, kMsgDevices, 0, reinterpret_cast<LPARAM>(found))) delete found;
    }).detach();
}

void ShowDevices(const std::vector<rdapp::FoundDevice>& found) {
    gFound = found;
    SendMessageA(gDevices, CB_RESETCONTENT, 0, 0);
    for (const auto& d : gFound) {
        const std::string label = d.name + "  (" + d.ip + ")";
        SendMessageA(gDevices, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SetStatus(gFound.empty() ? "No PCs found. Make sure the other PC is sharing, on the same network."
                             : "Found " + std::to_string(gFound.size()) + " PC(s). Pick one, then enter its pairing key.");
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
         HFONT font = nullptr) {
    // Layout is written in 96-dpi pixels; scaled here to the real DPI.
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : gFont), TRUE);
    if (wcscmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW) rdui::MakeButton(c);
    return c;
}

// A text field: the rounded box is painted by the window, the borderless edit sits inside it.
HWND AddField(HWND parent, int x, int y, int w, int h, int id, HFONT font) {
    HDC dc = GetDC(parent);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(parent, dc);
    const RECT box = {S(x), S(y), S(x + w), S(y + h)};
    const int eh = tm.tmHeight + 2;
    HWND c = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, box.left + S(12),
                             (box.top + box.bottom - eh) / 2, box.right - box.left - S(24), eh, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    gFields.push_back({c, box});
    return c;
}

HWND AddCombo(HWND parent, int x, int y, int w, int id) {
    HWND c = Add(parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                 x, y, w, 260, id);
    rdui::MakeCombo(c, gDpi, rdui::P().bg);
    return c;
}

// Called from WM_CREATE, where the window handle is the parent (gWnd is not set until CreateWindowEx returns).
void Build(HWND parent) {
    Add(parent, L"STATIC", L"Computer (IP address or name, optional :port)", SS_LEFT, 24, 72, 384, 18, IDC_LBL, gFontSmall);
    gAddr = AddField(parent, 24, 92, 384, 36, IDC_ADDR, gFont);
    Add(parent, L"STATIC", L"Pairing key", SS_LEFT, 24, 138, 384, 18, IDC_LBL, gFontSmall);
    gKey = AddField(parent, 24, 158, 384, 36, IDC_KEY, gFontMono);
    Add(parent, L"STATIC", L"Recent computers", SS_LEFT, 24, 204, 384, 18, IDC_LBL, gFontSmall);
    gRecent = AddCombo(parent, 24, 222, 384, IDC_RECENT);
    Add(parent, L"STATIC", L"PCs found on this network", SS_LEFT, 24, 268, 384, 18, IDC_LBL, gFontSmall);
    gDevices = AddCombo(parent, 24, 286, 288, IDC_DEVICES);
    Add(parent, L"BUTTON", L"Find", BS_OWNERDRAW | WS_TABSTOP, 320, 286, 88, 36, IDC_FIND, gFontBold);
    Add(parent, L"BUTTON", L"Connect", BS_OWNERDRAW | WS_TABSTOP, 24, 338, 384, 44, IDC_CONNECT, gFontBig);
    gStatus = Add(parent, L"STATIC", L"", SS_LEFT, 24, 392, 384, 40, IDC_STATUS);
}

void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    const rdui::Palette& p = rdui::P();
    FillRect(mem, &cr, rdui::Brush(p.bg));
    RECT bar = {0, 0, cr.right, S(kTopBar)};
    FillRect(mem, &bar, rdui::Brush(p.surface));
    RECT line = {0, S(kTopBar) - 1, cr.right, S(kTopBar)};
    FillRect(mem, &line, rdui::Brush(p.border));
    rdui::DrawLogo(mem, S(24), S(10), S(36));
    rdui::Text(mem, L"RemoteDisplay Client", RECT{S(72), S(8), S(420), S(29)}, p.text, gFontHeader,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    rdui::Text(mem, L"View a computer that is sharing its screen", RECT{S(72), S(28), S(420), S(47)}, p.muted, gFontSmall,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(h, &pt);
    for (const auto& f : gFields) rdui::DrawField(mem, f.second, GetFocus() == f.first, PtInRect(&f.second, pt) != FALSE, gDpi);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

void InvalidateField(HWND c) {
    for (const auto& f : gFields) {
        if (f.first != c) continue;
        RECT r = f.second;
        InflateRect(&r, S(4), S(4));
        InvalidateRect(gWnd, &r, FALSE);
    }
}

void Retheme() {
    rdui::ApplyTheme(rdui::CurrentMode());
    rdui::ThemeTitleBar(gWnd);
    rdui::RethemeCombo(gRecent, rdui::P().bg);
    rdui::RethemeCombo(gDevices, rdui::P().bg);
    RedrawWindow(gWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            gWnd = h;
            Build(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // everything is painted in WM_PAINT
        case WM_PAINT:
            Paint(h);
            return 0;
        case WM_CTLCOLOREDIT:
            return rdui::FieldColors(reinterpret_cast<HDC>(w));
        case WM_CTLCOLORLISTBOX:
            return rdui::ListColors(reinterpret_cast<HDC>(w));
        case WM_CTLCOLORSTATIC: {  // labels sit on the page colour; the status line uses the main text colour
            HDC dc = reinterpret_cast<HDC>(w);
            SetBkColor(dc, rdui::P().bg);
            SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(l)) == IDC_STATUS ? rdui::P().text : rdui::P().muted);
            return reinterpret_cast<LRESULT>(rdui::Brush(rdui::P().bg));
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if (mi->CtlType == ODT_COMBOBOX) rdui::MeasureCombo(mi, S(30), S(34));
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (d->CtlType == ODT_COMBOBOX && (d->itemState & ODS_COMBOBOXEDIT)) {
                rdui::DrawComboItem(d, gDpi);  // the closed face: the combo repaints itself
                return TRUE;
            }
            rdui::Buffered(d, [](const DRAWITEMSTRUCT* b) {  // drawn off-screen, then copied: no flicker
                if (b->CtlType == ODT_COMBOBOX) {
                    rdui::DrawComboItem(b, gDpi);
                    return;
                }
                wchar_t text[64] = {};
                GetWindowTextW(b->hwndItem, text, 64);
                const bool primary = b->CtlID == IDC_CONNECT;
                rdui::DrawButton(b, primary ? rdui::ButtonKind::Primary : rdui::ButtonKind::Secondary, text,
                                 primary ? gFontBig : gFontBold, gDpi, rdui::P().bg);
            });
            return TRUE;
        }
        case WM_SETTINGCHANGE:
            // Windows switched between light and dark apps: follow it when the theme is "Auto".
            if (l && wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0 &&
                rdui::CurrentMode() == rdui::Mode::System)
                Retheme();
            break;
        case WM_COMMAND:
            if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateField(reinterpret_cast<HWND>(l));
            if (LOWORD(w) == IDC_CONNECT && HIWORD(w) == BN_CLICKED) {
                Connect();
            } else if (LOWORD(w) == IDC_FIND && HIWORD(w) == BN_CLICKED) {
                FindDevicesAsync();
            } else if (LOWORD(w) == IDC_DEVICES && HIWORD(w) == CBN_SELCHANGE) {
                const LRESULT i = SendMessage(gDevices, CB_GETCURSEL, 0, 0);
                if (i >= 0 && size_t(i) < gFound.size()) SetWindowTextA(gAddr, gFound[size_t(i)].ip.c_str());
            } else if (LOWORD(w) == IDC_RECENT && HIWORD(w) == CBN_SELCHANGE) {
                const LRESULT i = SendMessage(gRecent, CB_GETCURSEL, 0, 0);
                if (i >= 0 && size_t(i) < gRecents.size()) {
                    SetWindowTextA(gAddr, gRecents[size_t(i)].addr.c_str());
                    SetWindowTextA(gKey, rd::FormatKey(gRecents[size_t(i)].key.data()).c_str());
                }
            }
            return 0;
        case kMsgDevices: {
            std::unique_ptr<std::vector<rdapp::FoundDevice>> found(reinterpret_cast<std::vector<rdapp::FoundDevice>*>(l));
            gFinding = false;
            ShowDevices(*found);
            return 0;
        }
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
    rdapp::Config cfg;
    cfg.LoadDeviceIdentity(gIdentity);
    rdui::GdiplusScope gdiplus;
    rdui::ApplyTheme(rdui::ModeFromString(cfg.Get(L"app", L"theme")));  // the choice made in the main app

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
    wc.hbrBackground = nullptr;  // painted in WM_PAINT
    wc.lpszClassName = L"RemoteDisplayClient";
    wc.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HDC screen = GetDC(nullptr);
    gDpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    gFont = MakeFont(10, false);
    gFontBold = MakeFont(10, true);
    gFontBig = MakeFont(11, true);
    gFontHeader = MakeFont(13, true);
    gFontSmall = MakeFont(9, false);
    gFontMono = MakeFont(10, false, wcscmp(UiFontFace(), L"Segoe UI") == 0 ? L"Consolas" : L"Cascadia Mono");

    RECT rc = {0, 0, S(kWinW), S(kWinH)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    gWnd = CreateWindowExW(0, L"RemoteDisplayClient", L"RemoteDisplay Client",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!gWnd) return 1;
    rdui::ThemeTitleBar(gWnd);

    FillRecents();
    if (!host.empty()) SetWindowTextA(gAddr, (port.empty() ? host : host + ":" + port).c_str());
    else SetWindowTextA(gAddr, "127.0.0.1");
    if (!key.empty()) SetWindowTextA(gKey, key.c_str());
    ShowWindow(gWnd, show);
    UpdateWindow(gWnd);
    if (!key.empty()) Connect();  // started with --key: connect at once
    else FindDevicesAsync();      // otherwise look for PCs to connect to

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(gWnd, &msg)) continue;  // Tab moves between the fields and buttons
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    gViewer.reset();
    WSACleanup();
    return 0;
}
