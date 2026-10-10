// RemoteDisplay.exe: one window to share this PC and to connect to another PC.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <wincrypt.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "host.h"
#include "service.h"
#include "shared.h"
#include "viewer.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace rdapp;

namespace {

// ---------------------------------------------------------------------------------------------
// UI

enum Ctl {
    IDC_IPS = 100, IDC_COPY_IP, IDC_KEY, IDC_COPY_KEY, IDC_NEW_KEY, IDC_APPROVE, IDC_SHARE, IDC_STATUS,
    IDC_DISCONNECT, IDC_REMOTE, IDC_REMOTE_KEY, IDC_CONNECT, IDC_MSG, IDC_HINT,
    IDC_LBL_ADDR, IDC_LBL_KEY, IDC_LBL_RADDR, IDC_LBL_RKEY, IDC_TITLE_L, IDC_TITLE_R,
    IDC_SVC_TITLE, IDC_SVC_STATUS, IDC_SVC_BTN, IDC_SVC_HINT,
};
constexpr UINT kMsgViewerClosed = WM_APP + 10, kMsgSvcDone = WM_APP + 11;
constexpr UINT_PTR kTimerUi = 1, kTimerCopied = 2;

constexpr COLORREF kBg = RGB(238, 241, 245), kCard = RGB(255, 255, 255), kBorder = RGB(217, 222, 231),
                   kNavy = RGB(24, 34, 58), kAccent = RGB(37, 99, 235), kGreen = RGB(22, 163, 74),
                   kRed = RGB(220, 38, 38), kAmber = RGB(217, 119, 6), kText = RGB(31, 41, 55),
                   kMuted = RGB(107, 114, 128);

class App {
public:
    static int Run(HINSTANCE inst) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        timeBeginPeriod(1);
        SetProcessDPIAware();
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);

        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"RemoteDisplayApp";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        RegisterClassW(&wc);

        App app;
        app.inst_ = inst;
        app.dpi_ = GetDeviceCaps(GetDC(nullptr), LOGPIXELSX);
        RECT rc = {0, 0, app.S(820), app.S(548)};
        AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        HWND h = CreateWindowW(L"RemoteDisplayApp", L"RemoteDisplay",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                               nullptr, inst, &app);
        if (!h) return 1;
        ShowWindow(h, SW_SHOW);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (!IsDialogMessageW(h, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return 0;
    }

private:
    int S(int v) const { return MulDiv(v, dpi_, 96); }

    HFONT Font(int pt, bool bold) {
        return CreateFontW(-MulDiv(pt, dpi_, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    }

    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
             DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), hwnd_,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), inst_, nullptr);
        SendMessage(c, WM_SETFONT, reinterpret_cast<WPARAM>(fontNormal_), TRUE);
        return c;
    }
    HWND Ctrl(int id) { return GetDlgItem(hwnd_, id); }
    void SetText(int id, const std::wstring& t) { SetWindowTextW(Ctrl(id), t.c_str()); }
    std::string GetText(int id) {
        wchar_t buf[512] = {};
        GetWindowTextW(Ctrl(id), buf, 512);
        return Trim(U(buf));
    }

    void OnCreate() {
        fontNormal_ = Font(10, false);
        fontBold_ = Font(10, true);
        fontTitle_ = Font(14, true);
        fontSmall_ = Font(9, false);
        fontBig_ = Font(11, true);

        cfg_ = std::make_unique<Config>();
        hostCfg_.psk = cfg_->HostKey();
        hostCfg_.requireApproval = cfg_->RequireApproval();

        // ---- left card: share this PC
        Add(L"STATIC", L"THIS PC", SS_LEFT, 44, 108, 200, 16, IDC_LBL_ADDR + 100);
        Add(L"STATIC", L"Share your screen", SS_LEFT, 44, 126, 320, 28, IDC_TITLE_L);
        Add(L"STATIC", L"Your address", SS_LEFT, 44, 166, 200, 18, IDC_LBL_ADDR);
        Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 44, 186, 252, 200, IDC_IPS);
        Add(L"BUTTON", L"Copy", BS_PUSHBUTTON | WS_TABSTOP, 304, 186, 60, 28, IDC_COPY_IP);
        Add(L"STATIC", L"Pairing key", SS_LEFT, 44, 224, 200, 18, IDC_LBL_KEY);
        Add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 44, 244, 252, 28, IDC_KEY, WS_EX_CLIENTEDGE);
        Add(L"BUTTON", L"Copy", BS_PUSHBUTTON | WS_TABSTOP, 304, 244, 60, 28, IDC_COPY_KEY);
        Add(L"BUTTON", L"Generate a new key", BS_PUSHBUTTON | WS_TABSTOP, 44, 278, 150, 24, IDC_NEW_KEY);
        Add(L"BUTTON", L"Ask me before allowing a connection", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 308, 320, 24, IDC_APPROVE);
        Add(L"BUTTON", L"Start sharing", BS_OWNERDRAW | WS_TABSTOP, 44, 340, 320, 40, IDC_SHARE);
        Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 44, 390, 226, 20, IDC_STATUS);
        Add(L"BUTTON", L"Disconnect", BS_PUSHBUTTON | WS_TABSTOP, 276, 388, 88, 24, IDC_DISCONNECT);

        // ---- background service (bottom of the left card)
        Add(L"STATIC", L"BACKGROUND SERVICE", SS_LEFT, 44, 428, 200, 16, IDC_SVC_TITLE);
        Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 44, 450, 190, 22, IDC_SVC_STATUS);
        Add(L"BUTTON", L"Install", BS_PUSHBUTTON | WS_TABSTOP, 244, 446, 120, 28, IDC_SVC_BTN);
        Add(L"STATIC", L"Starts with Windows - stays shareable after a reboot.",
            SS_LEFT, 44, 478, 320, 18, IDC_SVC_HINT);

        // ---- right card: connect
        Add(L"STATIC", L"REMOTE PC", SS_LEFT, 436, 108, 200, 16, IDC_LBL_RADDR + 100);
        Add(L"STATIC", L"Connect to a computer", SS_LEFT, 436, 126, 340, 28, IDC_TITLE_R);
        Add(L"STATIC", L"Remote address", SS_LEFT, 436, 166, 200, 18, IDC_LBL_RADDR);
        Add(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 436, 186, 340, 220, IDC_REMOTE);
        Add(L"STATIC", L"Pairing key", SS_LEFT, 436, 224, 200, 18, IDC_LBL_RKEY);
        Add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 436, 244, 340, 28, IDC_REMOTE_KEY, WS_EX_CLIENTEDGE);
        Add(L"STATIC", L"Enter the address and pairing key shown in RemoteDisplay on the other PC. "
                       L"This version is view-only.", SS_LEFT, 436, 282, 340, 44, IDC_HINT);
        Add(L"BUTTON", L"Connect", BS_OWNERDRAW | WS_TABSTOP, 436, 340, 340, 40, IDC_CONNECT);
        Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 436, 390, 340, 20, IDC_MSG);

        for (const auto& ip : LocalIPv4()) SendMessageW(Ctrl(IDC_IPS), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(ip).c_str()));
        if (SendMessage(Ctrl(IDC_IPS), CB_GETCOUNT, 0, 0) == 0)
            SendMessageW(Ctrl(IDC_IPS), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(no network found)"));
        SendMessage(Ctrl(IDC_IPS), CB_SETCURSEL, 0, 0);
        SetText(IDC_KEY, W(rd::FormatKey(hostCfg_.psk.data())));
        CheckDlgButton(hwnd_, IDC_APPROVE, hostCfg_.requireApproval ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(Ctrl(IDC_DISCONNECT), SW_HIDE);

        recents_ = cfg_->Recents();
        for (const auto& r : recents_) SendMessageW(Ctrl(IDC_REMOTE), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(r.addr).c_str()));
        if (!recents_.empty()) {
            SendMessage(Ctrl(IDC_REMOTE), CB_SETCURSEL, 0, 0);
            FillRemoteKey(0);
        }
        SetTimer(hwnd_, kTimerUi, 250, nullptr);
        UpdateUi();
    }

    void FillRemoteKey(size_t i) {
        if (i < recents_.size()) SetText(IDC_REMOTE_KEY, W(rd::FormatKey(recents_[i].key.data())));
    }

    // ---- sharing --------------------------------------------------------------------------
    // What the status area shows: this window's own host, or the background worker's status.
    struct View {
        int state = rdh::HostStatus::Stopped;
        float fps = 0;
        std::string peer, error;
        bool viaService = false;
        uint32_t workerPid = 0;
    };

    bool Sharing() const { return hostThread_.joinable() && host_.state != rdh::HostStatus::Failed; }

    View CurrentView() {
        View v;
        if (!Sharing() && svcState_ == SvcState::Running) {
            v.viaService = true;
            SharedStatus s;
            if (StatusReader::Read(s)) {
                v.state = s.state;
                v.fps = s.fps;
                v.peer = s.peer;
                v.error = s.error;
                v.workerPid = s.pid;
            } else {
                v.state = rdh::HostStatus::Starting;  // service is up, its worker is still starting
            }
            return v;
        }
        v.state = host_.state;
        v.fps = host_.fps;
        v.peer = host_.Peer();
        v.error = host_.Error();
        return v;
    }

    void StopSharing() {
        stopHost_ = true;
        if (hostThread_.joinable()) hostThread_.join();
        stopHost_ = false;
        host_.state = rdh::HostStatus::Stopped;
        ownLock_.Release();
    }

    // The background worker reads the key and approval setting at start; make it pick up changes.
    void ApplyToService() {
        View v = CurrentView();
        if (v.viaService && v.workerPid) {
            StatusReader::RestartWorker(v.workerPid);
            Message(L"Applying to the background service...", kMuted);
        }
    }

    void ServiceAction(const wchar_t* verb) {
        if (svcBusy_) return;
        if (svcThread_.joinable()) svcThread_.join();
        if (std::wstring(verb) == L"install" && Sharing()) StopSharing();  // free the port for the service
        svcBusy_ = true;
        SetText(IDC_SVC_STATUS, L"Waiting for administrator approval...");
        EnableWindow(Ctrl(IDC_SVC_BTN), FALSE);
        svcVerb_ = verb;
        svcThread_ = std::thread([this, verb] {
            int rc = RunElevated(hwnd_, std::wstring(L"--service-ctl ") + verb);
            PostMessage(hwnd_, kMsgSvcDone, WPARAM(rc), 0);
        });
    }

    void OnServiceDone(int rc) {
        if (svcThread_.joinable()) svcThread_.join();
        svcBusy_ = false;
        svcState_ = QueryServiceState();
        if (rc == 0) {
            Message(svcVerb_ == L"install" ? L"Background service installed and running."
                                           : L"Background service removed.", kGreen);
        } else if (rc == -1) {
            Message(L"Cancelled. Administrator approval is needed for this.");
        } else {
            Message(L"Service command failed (code " + std::to_wstring(rc) + L"). See ProgramData\\RemoteDisplay\\service.log");
        }
        UpdateUi();
    }

    void ToggleSharing() {
        if (Sharing()) {
            StopSharing();
        } else {
            StopSharing();  // reap a failed run, if any
            if (!ownLock_.Acquire()) {
                Message(L"Another RemoteDisplay host is already running (the background service?).");
                UpdateUi();
                return;
            }
            hostCfg_.requireApproval = IsDlgButtonChecked(hwnd_, IDC_APPROVE) == BST_CHECKED;
            host_.state = rdh::HostStatus::Starting;
            host_.approve = host_.deny = host_.kick = false;
            promptShown_ = false;
            hostThread_ = std::thread([this] { rdh::RunHost(hostCfg_, stopHost_, host_); });
        }
        UpdateUi();
    }

    void UpdateUi() {
        if (++tick_ % 4 == 1 && !svcBusy_) svcState_ = QueryServiceState();  // about once a second
        const View v = CurrentView();
        const int s = v.state;
        const bool sharing = Sharing();
        const bool connected = s == rdh::HostStatus::Streaming;
        std::wstring status;
        COLORREF c = kMuted;
        switch (s) {
            case rdh::HostStatus::Starting: status = L"Starting..."; c = kAmber; break;
            case rdh::HostStatus::Waiting: status = L"● Ready - waiting for a connection"; c = kGreen; break;
            case rdh::HostStatus::AwaitingApproval:
                status = L"● Connection request from " + W(v.peer); c = kAmber; break;
            case rdh::HostStatus::Streaming: {
                std::string peer = v.peer;
                peer = peer.substr(0, peer.rfind(':'));  // IP only, the port is noise here
                wchar_t b[160];
                swprintf_s(b, L"● %s viewing - %.0f fps", W(peer).c_str(), v.fps);
                status = b; c = kGreen; break;
            }
            case rdh::HostStatus::Failed: status = W(v.error); c = kRed; break;
            default: status = L"Not sharing"; break;
        }
        if (v.viaService && s != rdh::HostStatus::Failed && s != rdh::HostStatus::Starting && !connected &&
            s != rdh::HostStatus::AwaitingApproval)
            status = L"● Sharing in background - waiting for a connection";
        statusColor_ = c;
        if (status != lastStatus_) {
            lastStatus_ = status;
            SetText(IDC_STATUS, status);
        }
        InvalidateRect(Ctrl(IDC_STATUS), nullptr, TRUE);
        ShowWindow(Ctrl(IDC_DISCONNECT), connected ? SW_SHOW : SW_HIDE);
        EnableWindow(Ctrl(IDC_NEW_KEY), !svcBusy_);
        EnableWindow(Ctrl(IDC_APPROVE), !sharing || true);
        EnableWindow(Ctrl(IDC_SHARE), !v.viaService);
        InvalidateRect(Ctrl(IDC_SHARE), nullptr, FALSE);

        // background-service section
        std::wstring ss, btn;
        COLORREF sc = kMuted;
        switch (svcState_) {
            case SvcState::NotInstalled: ss = L"Not installed"; btn = L"Install"; break;
            case SvcState::Running: ss = L"● Installed and running"; btn = L"Remove"; sc = kGreen; break;
            case SvcState::Stopped: ss = L"● Installed, not running"; btn = L"Remove"; sc = kAmber; break;
            case SvcState::Transition: ss = L"Changing state..."; btn = L"Remove"; sc = kAmber; break;
        }
        if (!svcBusy_) {
            svcColor_ = sc;
            if (ss != lastSvcStatus_) {
                lastSvcStatus_ = ss;
                SetText(IDC_SVC_STATUS, ss);
                InvalidateRect(Ctrl(IDC_SVC_STATUS), nullptr, TRUE);
            }
            SetText(IDC_SVC_BTN, btn);
            EnableWindow(Ctrl(IDC_SVC_BTN), TRUE);
        }
    }

    void MaybePromptForApproval() {
        if (!Sharing() || host_.state != rdh::HostStatus::AwaitingApproval) { promptShown_ = false; return; }
        if (promptShown_) return;
        promptShown_ = true;
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        std::wstring msg = W(host_.Peer()) + L" has your pairing key and wants to view your screen.\n\nAllow this connection?";
        int r = MessageBoxW(hwnd_, msg.c_str(), L"RemoteDisplay - connection request",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
        if (host_.state == rdh::HostStatus::AwaitingApproval) {  // client may have left meanwhile
            if (r == IDYES) host_.approve = true; else host_.deny = true;
        }
    }

    // ---- connecting -----------------------------------------------------------------------
    void Connect() {
        SetText(IDC_MSG, L"");
        std::string text = GetText(IDC_REMOTE);
        std::array<uint8_t, rd::kKeyLen> psk{};
        if (text.empty()) { Message(L"Enter the remote address."); return; }
        if (!rd::ParseKey(GetText(IDC_REMOTE_KEY), psk.data())) {
            Message(L"That pairing key doesn't look right (26 letters/digits)."); return;
        }
        std::string ip;
        int port = 0;
        if (!ResolveAddress(text, ip, port)) { Message(L"Could not find that address."); return; }

        auto* v = rdv::Viewer::Create(inst_, hwnd_, kMsgViewerClosed, ip, port, psk);
        if (!v) { Message(L"Could not open the viewer window."); return; }
        viewers_.emplace_back(v);
        Recent r;
        r.addr = text;
        r.key = psk;
        cfg_->AddRecent(r);
        recents_ = cfg_->Recents();
        SendMessage(Ctrl(IDC_REMOTE), CB_RESETCONTENT, 0, 0);
        for (const auto& x : recents_) SendMessageW(Ctrl(IDC_REMOTE), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(x.addr).c_str()));
        SetText(IDC_REMOTE, W(text));
        Message(L"Connecting... a wrong key simply never connects.", kMuted);
    }

    void Message(const std::wstring& t, COLORREF c = kRed) {
        msgColor_ = c;
        SetText(IDC_MSG, t);
    }

    // ---- painting -------------------------------------------------------------------------
    void Paint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(mem, &cr, bg);
        DeleteObject(bg);
        RECT hdr = {0, 0, cr.right, S(78)};
        HBRUSH nv = CreateSolidBrush(kNavy);
        FillRect(mem, &hdr, nv);
        DeleteObject(nv);

        SetBkMode(mem, TRANSPARENT);
        SelectObject(mem, Font(18, true));
        SetTextColor(mem, RGB(255, 255, 255));
        TextOutW(mem, S(24), S(12), L"RemoteDisplay", 13);
        SelectObject(mem, fontNormal_);
        SetTextColor(mem, RGB(170, 182, 207));
        TextOutW(mem, S(26), S(46), L"Low-latency, end-to-end encrypted remote desktop", 48);

        HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
        HBRUSH card = CreateSolidBrush(kCard);
        SelectObject(mem, pen);
        SelectObject(mem, card);
        RoundRect(mem, S(24), S(96), S(404), S(508), S(14), S(14));
        RoundRect(mem, S(416), S(96), S(796), S(508), S(14), S(14));
        DeleteObject(pen);
        DeleteObject(card);

        SetTextColor(mem, kMuted);
        SelectObject(mem, fontSmall_);
        TextOutW(mem, S(26), S(520), L"AES-128-GCM encrypted. Connections need your pairing key.", 56);

        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
    }

    void DrawButton(const DRAWITEMSTRUCT* d) {
        std::wstring label;
        COLORREF fill;
        if (d->CtlID == IDC_SHARE) {
            bool sharing = Sharing();
            label = sharing ? L"Stop sharing" : L"Start sharing";
            fill = sharing ? kRed : kGreen;
            if (d->itemState & ODS_DISABLED) {
                label = L"Sharing via background service";
                fill = RGB(156, 163, 175);
            }
        } else {
            label = L"Connect";
            fill = kAccent;
        }
        if (d->itemState & ODS_SELECTED) fill = RGB(GetRValue(fill) * 3 / 4, GetGValue(fill) * 3 / 4, GetBValue(fill) * 3 / 4);
        HBRUSH bg = CreateSolidBrush(kCard);
        FillRect(d->hDC, &d->rcItem, bg);
        DeleteObject(bg);
        HBRUSH b = CreateSolidBrush(fill);
        HPEN p = CreatePen(PS_SOLID, 1, fill);
        SelectObject(d->hDC, b);
        SelectObject(d->hDC, p);
        RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, S(10), S(10));
        DeleteObject(b);
        DeleteObject(p);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, RGB(255, 255, 255));
        SelectObject(d->hDC, fontBig_);
        RECT r = d->rcItem;
        DrawTextW(d->hDC, label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (d->itemState & ODS_FOCUS) {
            RECT f = d->rcItem;
            InflateRect(&f, -S(4), -S(4));
            DrawFocusRect(d->hDC, &f);
        }
    }

    // ---- window procedure -----------------------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            auto* app = static_cast<App*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams);
            app->hwnd_ = h;
            SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        App* a = reinterpret_cast<App*>(GetWindowLongPtr(h, GWLP_USERDATA));
        return a ? a->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    LRESULT Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_CREATE: OnCreate(); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: Paint(); return 0;
            case WM_DRAWITEM: DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(w);
                int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
                SetBkColor(dc, kCard);
                SetTextColor(dc, kText);
                SelectObject(dc, fontNormal_);
                if (id == IDC_STATUS) SetTextColor(dc, statusColor_);
                else if (id == IDC_SVC_STATUS) SetTextColor(dc, svcColor_);
                else if (id == IDC_SVC_TITLE) { SetTextColor(dc, kAccent); SelectObject(dc, fontSmall_); }
                else if (id == IDC_SVC_HINT) { SetTextColor(dc, kMuted); SelectObject(dc, fontSmall_); }
                else if (id == IDC_MSG) SetTextColor(dc, msgColor_);
                else if (id == IDC_HINT || id == IDC_LBL_ADDR || id == IDC_LBL_KEY || id == IDC_LBL_RADDR ||
                         id == IDC_LBL_RKEY) SetTextColor(dc, kMuted);
                else if (id == IDC_TITLE_L || id == IDC_TITLE_R) SelectObject(dc, fontTitle_);
                else if (id == IDC_LBL_ADDR + 100 || id == IDC_LBL_RADDR + 100) { SetTextColor(dc, kAccent); SelectObject(dc, fontSmall_); }
                if (!cardBrush_) cardBrush_ = CreateSolidBrush(kCard);
                return reinterpret_cast<LRESULT>(cardBrush_);
            }
            case WM_TIMER:
                if (w == kTimerUi) {
                    UpdateUi();
                    MaybePromptForApproval();
                } else if (w == kTimerCopied) {
                    KillTimer(h, kTimerCopied);
                    SetText(IDC_COPY_IP, L"Copy");
                    SetText(IDC_COPY_KEY, L"Copy");
                }
                return 0;
            case WM_COMMAND:
                switch (LOWORD(w)) {
                    case IDC_SHARE: ToggleSharing(); break;
                    case IDC_CONNECT: Connect(); break;
                    case IDC_DISCONNECT:
                        if (CurrentView().viaService) StatusReader::Kick(); else host_.kick = true;
                        break;
                    case IDC_SVC_BTN:
                        ServiceAction(svcState_ == SvcState::NotInstalled ? L"install" : L"uninstall");
                        break;
                    case IDC_COPY_IP: {
                        wchar_t b[64] = {};
                        GetWindowTextW(Ctrl(IDC_IPS), b, 64);
                        CopyToClipboard(h, b);
                        SetText(IDC_COPY_IP, L"Copied");
                        SetTimer(h, kTimerCopied, 1500, nullptr);
                        break;
                    }
                    case IDC_COPY_KEY:
                        CopyToClipboard(h, W(rd::FormatKey(hostCfg_.psk.data())));
                        SetText(IDC_COPY_KEY, L"Copied");
                        SetTimer(h, kTimerCopied, 1500, nullptr);
                        break;
                    case IDC_NEW_KEY:
                        if (MessageBoxW(h, L"Generate a new pairing key? Computers that saved the old key will need the new one.",
                                        L"RemoteDisplay", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                            rd::crypto::Random(hostCfg_.psk.data(), hostCfg_.psk.size());
                            cfg_->SetHostKey(hostCfg_.psk);
                            SetText(IDC_KEY, W(rd::FormatKey(hostCfg_.psk.data())));
                            ApplyToService();
                        }
                        break;
                    case IDC_APPROVE:
                        cfg_->SetRequireApproval(IsDlgButtonChecked(h, IDC_APPROVE) == BST_CHECKED);
                        ApplyToService();
                        break;
                    case IDC_REMOTE:
                        if (HIWORD(w) == CBN_SELCHANGE) {
                            LRESULT i = SendMessage(Ctrl(IDC_REMOTE), CB_GETCURSEL, 0, 0);
                            if (i >= 0) FillRemoteKey(size_t(i));
                        }
                        break;
                }
                return 0;
            case kMsgSvcDone:
                OnServiceDone(int(w));
                return 0;
            case kMsgViewerClosed: {
                auto* v = reinterpret_cast<rdv::Viewer*>(w);
                viewers_.erase(std::remove_if(viewers_.begin(), viewers_.end(),
                                              [&](const std::unique_ptr<rdv::Viewer>& p) { return p.get() == v; }),
                               viewers_.end());  // destructor joins the viewer's thread
                return 0;
            }
            case WM_DESTROY:
                KillTimer(h, kTimerUi);
                viewers_.clear();
                StopSharing();
                if (svcThread_.joinable()) svcThread_.join();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    HFONT fontNormal_ = nullptr, fontBold_ = nullptr, fontTitle_ = nullptr, fontSmall_ = nullptr, fontBig_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    COLORREF statusColor_ = kMuted, msgColor_ = kRed, svcColor_ = kMuted;
    std::wstring lastStatus_, lastSvcStatus_, svcVerb_;
    SvcState svcState_ = SvcState::NotInstalled;
    bool svcBusy_ = false;
    std::thread svcThread_;
    unsigned tick_ = 0;
    HostActiveLock ownLock_;

    std::unique_ptr<Config> cfg_;
    std::vector<Recent> recents_;

    rdh::HostConfig hostCfg_;
    rdh::HostStatus host_;
    std::atomic<bool> stopHost_{false};
    std::thread hostThread_;
    bool promptShown_ = false;

    std::vector<std::unique_ptr<rdv::Viewer>> viewers_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    const std::wstring arg2 = argc > 2 ? argv[2] : L"";
    if (argv) LocalFree(argv);
    if (mode == L"--service") return RunServiceMain();        // started by the Service Control Manager
    if (mode == L"--host-worker") return RunWorker();         // started by the service, in the user session
    if (mode == L"--service-ctl") return ServiceControl(arg2);  // elevated helper: install/uninstall/start/stop
    return App::Run(inst);
}
