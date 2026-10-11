// RemoteDisplay.exe: one window to share this PC and to connect to another PC.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "admin.h"
#include "secure.h"
#include "secure_helper.h"
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
    IDC_SVC_TITLE, IDC_SVC_STATUS, IDC_SVC_BTN, IDC_SVC_HINT, IDC_CONTROL, IDC_AUTOSTART, IDC_SHOWTRAY, IDC_TRUSTED, IDC_ADMIN, IDC_ADMIN_STATE, IDC_SVC_REMOVE, IDC_SECURE, IDC_DISPLAY_DUP, IDC_DISPLAY_EXT,
};
enum class Elev { SvcInstall, SvcUninstall, AdminOn, AdminOff, TaskOn, TaskOff, SecureOn, SecureOff };
enum TrayCmd { IDM_OPEN = 2001, IDM_TOGGLE, IDM_DISCONNECT, IDM_EXIT };
constexpr UINT kMsgViewerClosed = WM_APP + 10, kMsgSvcDone = WM_APP + 11, kMsgTray = WM_APP + 20,
               kMsgShow = WM_APP + 21;
constexpr UINT_PTR kTimerUi = 1, kTimerCopied = 2;

constexpr COLORREF kBg = RGB(238, 241, 245), kCard = RGB(255, 255, 255), kBorder = RGB(217, 222, 231),
                   kNavy = RGB(24, 34, 58), kAccent = RGB(37, 99, 235), kGreen = RGB(22, 163, 74),
                   kRed = RGB(220, 38, 38), kAmber = RGB(217, 119, 6), kText = RGB(31, 41, 55),
                   kMuted = RGB(107, 114, 128);

// Draws the app icon at any size: navy tile, white monitor, optional status dot in the corner.
HICON MakeIcon(int n, COLORREF dot, bool showDot) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    if (!color || !mask || !bits) return nullptr;
    auto argb = [](COLORREF c) { return 0xFF000000u | (uint32_t(GetRValue(c)) << 16) | (uint32_t(GetGValue(c)) << 8) | GetBValue(c); };
    const double r = n * 0.22, t = std::max(1.0, n / 14.0);
    const double L = n * 0.17, T = n * 0.2, R = n * 0.83, B = n * 0.62;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double fx = x + 0.5, fy = y + 0.5;
            uint32_t out = 0;
            const double cx = std::clamp(fx, r, n - r), cy = std::clamp(fy, r, n - r);
            if (std::hypot(fx - cx, fy - cy) <= r) {
                out = argb(kNavy);
                const bool outer = fx >= L && fx <= R && fy >= T && fy <= B;
                const bool inner = fx >= L + t && fx <= R - t && fy >= T + t && fy <= B - t;
                if (inner) out = argb(kAccent);
                else if (outer) out = argb(RGB(255, 255, 255));
                if ((fx >= n * 0.42 && fx <= n * 0.58 && fy > B && fy <= n * 0.74) ||
                    (fx >= n * 0.3 && fx <= n * 0.7 && fy > n * 0.74 && fy <= n * 0.8))
                    out = argb(RGB(255, 255, 255));
            }
            if (showDot) {
                const double d = std::hypot(fx - n * 0.75, fy - n * 0.72);
                if (d <= n * 0.27) out = argb(RGB(255, 255, 255));
                if (d <= n * 0.2) out = argb(dot);
            }
            px[size_t(y) * n + x] = out;
        }
    }
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

class App {
public:
    static int Run(HINSTANCE inst, bool background, bool autoShare) {
        DisplayModeSession::RecoverIfNeeded();  // a previous run killed mid-session must not leave the screens switched
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
        RegisterClassW(&wc);

        App app;
        app.inst_ = inst;
        app.background_ = background;
        app.autoShare_ = autoShare;
        app.dpi_ = GetDeviceCaps(GetDC(nullptr), LOGPIXELSX);
        RECT rc = {0, 0, app.S(820), app.S(688)};
        AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        HWND h = CreateWindowW(L"RemoteDisplayApp", L"RemoteDisplay",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                               nullptr, inst, &app);
        if (!h) return 1;
        HICON big = MakeIcon(GetSystemMetrics(SM_CXICON), kGreen, false);
        HICON smallIcon = MakeIcon(GetSystemMetrics(SM_CXSMICON), kGreen, false);
        SendMessage(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        SendMessage(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        ShowWindow(h, background ? SW_HIDE : SW_SHOW);  // started with Windows: tray only, no window
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
        hostCfg_.allowControl = cfg_->AllowControl();
        hostCfg_.trust = &trust_;               // devices approved with "Always allow" skip the prompt
        cfg_->LoadDeviceIdentity(identity_);    // this PC's permanent key when it connects out

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
        Add(L"BUTTON", L"Trusted devices...", BS_PUSHBUTTON | WS_TABSTOP, 204, 278, 160, 24, IDC_TRUSTED);
        Add(L"BUTTON", L"Ask me before allowing a connection", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 306, 320, 24, IDC_APPROVE);
        Add(L"BUTTON", L"Allow mouse and keyboard control", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 332, 320, 24, IDC_CONTROL);
        Add(L"BUTTON", L"Duplicate displays while sharing", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 364, 200, 24, IDC_DISPLAY_DUP);
        Add(L"BUTTON", L"Extend: stream virtual monitor", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 388, 260, 24, IDC_DISPLAY_EXT);
        Add(L"BUTTON", L"Start sharing", BS_OWNERDRAW | WS_TABSTOP, 44, 420, 320, 40, IDC_SHARE);
        Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 44, 442, 226, 20, IDC_STATUS);
        Add(L"BUTTON", L"Disconnect", BS_PUSHBUTTON | WS_TABSTOP, 276, 440, 88, 24, IDC_DISCONNECT);

        // ---- background service (bottom of the left card)
        Add(L"STATIC", L"RUN IN BACKGROUND", SS_LEFT, 44, 472, 200, 16, IDC_SVC_TITLE);
        Add(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 492, 170, 24, IDC_AUTOSTART);
        Add(L"BUTTON", L"Show tray icon", BS_AUTOCHECKBOX | WS_TABSTOP, 224, 492, 140, 24, IDC_SHOWTRAY);
        Add(L"BUTTON", L"Run as administrator", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 520, 190, 24, IDC_ADMIN);
        Add(L"BUTTON", L"Restart as admin", BS_PUSHBUTTON | WS_TABSTOP, 244, 518, 120, 26, IDC_ADMIN_STATE);
        Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 44, 580, 320, 22, IDC_SVC_STATUS);
        Add(L"BUTTON", L"Install service", BS_PUSHBUTTON | WS_TABSTOP, 44, 608, 150, 28, IDC_SVC_BTN);
        Add(L"BUTTON", L"Uninstall service", BS_PUSHBUTTON | WS_TABSTOP, 204, 608, 160, 28, IDC_SVC_REMOVE);
        Add(L"BUTTON", L"Allow viewing UAC and sign-in screens (admin)", BS_AUTOCHECKBOX | WS_TABSTOP, 44, 548, 360, 24, IDC_SECURE);

        // ---- right card: connect
        Add(L"STATIC", L"REMOTE PC", SS_LEFT, 436, 108, 200, 16, IDC_LBL_RADDR + 100);
        Add(L"STATIC", L"Connect to a computer", SS_LEFT, 436, 126, 340, 28, IDC_TITLE_R);
        Add(L"STATIC", L"Remote address", SS_LEFT, 436, 166, 200, 18, IDC_LBL_RADDR);
        Add(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 436, 186, 340, 220, IDC_REMOTE);
        Add(L"STATIC", L"Pairing key", SS_LEFT, 436, 224, 200, 18, IDC_LBL_RKEY);
        Add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 436, 244, 340, 28, IDC_REMOTE_KEY, WS_EX_CLIENTEDGE);
        Add(L"STATIC", L"Enter the address and pairing key shown in the other PC's RemoteDisplay window.", SS_LEFT, 436, 282, 340, 44, IDC_HINT);
        Add(L"BUTTON", L"Connect", BS_OWNERDRAW | WS_TABSTOP, 436, 364, 340, 40, IDC_CONNECT);
        Add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 436, 414, 340, 20, IDC_MSG);

        for (const auto& ip : LocalIPv4()) SendMessageW(Ctrl(IDC_IPS), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(ip).c_str()));
        if (SendMessage(Ctrl(IDC_IPS), CB_GETCOUNT, 0, 0) == 0)
            SendMessageW(Ctrl(IDC_IPS), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(no network found)"));
        SendMessage(Ctrl(IDC_IPS), CB_SETCURSEL, 0, 0);
        SetText(IDC_KEY, W(rd::FormatKey(hostCfg_.psk.data())));
        CheckDlgButton(hwnd_, IDC_APPROVE, hostCfg_.requireApproval ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd_, IDC_CONTROL, hostCfg_.allowControl ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd_, IDC_DISPLAY_DUP, cfg_->DisplayMode() == "duplicate" ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd_, IDC_DISPLAY_EXT, cfg_->DisplayMode() == "extend" ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(Ctrl(IDC_DISCONNECT), SW_HIDE);

        recents_ = cfg_->Recents();
        for (const auto& r : recents_) SendMessageW(Ctrl(IDC_REMOTE), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(r.addr).c_str()));
        if (!recents_.empty()) {
            SendMessage(Ctrl(IDC_REMOTE), CB_SETCURSEL, 0, 0);
            FillRemoteKey(0);
        }
        showTray_ = cfg_->ShowTray();
        CheckDlgButton(hwnd_, IDC_SHOWTRAY, showTray_ ? BST_CHECKED : BST_UNCHECKED);
        adminPref_ = cfg_->AdminMode();
        CheckDlgButton(hwnd_, IDC_AUTOSTART, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd_, IDC_ADMIN, adminPref_ ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd_, IDC_SECURE, SecureDesktopAllowed() ? BST_CHECKED : BST_UNCHECKED);
        if (IsElevated()) SetWindowTextW(hwnd_, L"RemoteDisplay (Administrator)");
        UpdateAdminUi();
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");  // Explorer restarted: re-add the icon
        // When elevated, messages from the normal-rights shell are dropped unless allowed on purpose.
        AllowMessageFromLowerIntegrity(hwnd_, kMsgTray);
        AllowMessageFromLowerIntegrity(hwnd_, kMsgShow);
        AllowMessageFromLowerIntegrity(hwnd_, taskbarCreated_);
        for (int i = 0; i < 3; ++i) {
            const COLORREF cols[3] = {RGB(156, 163, 175), kGreen, kAmber};
            trayIcons_[i] = MakeIcon(GetSystemMetrics(SM_CXSMICON), cols[i], true);
        }
        SetTimer(hwnd_, kTimerUi, 250, nullptr);
        UpdateUi();
        AddTray();
        if ((background_ || autoShare_) && svcState_ != SvcState::Running && !Sharing()) {  // started with Windows / restarted
            ToggleSharing();
            if (background_) Balloon(L"RemoteDisplay is running", L"Sharing is on. Open it from the tray icon.");
        }
    }

    // ---- tray icon ------------------------------------------------------------------------
    NOTIFYICONDATAW TrayData(UINT flags) {
        NOTIFYICONDATAW nid = {sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = 1;
        nid.uFlags = flags;
        nid.uCallbackMessage = kMsgTray;
        nid.hIcon = trayIcons_[trayState_];
        wcsncpy_s(nid.szTip, trayTip_.c_str(), _TRUNCATE);
        return nid;
    }
    void AddTray() {
        if (!showTray_) return;  // user chose to run with no tray icon
        NOTIFYICONDATAW nid = TrayData(NIF_ICON | NIF_MESSAGE | NIF_TIP);
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }
    void RemoveTray() {
        if (!trayAdded_) return;
        NOTIFYICONDATAW nid = TrayData(0);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
    }
    void UpdateTray(int state, const std::wstring& tip) {
        if (state == trayState_ && tip == trayTip_) return;
        trayState_ = state;
        trayTip_ = tip;
        if (!trayAdded_) return;
        NOTIFYICONDATAW nid = TrayData(NIF_ICON | NIF_TIP);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    void Balloon(const wchar_t* title, const wchar_t* text) {
        if (!trayAdded_) return;
        NOTIFYICONDATAW nid = TrayData(NIF_INFO);
        wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
        wcsncpy_s(nid.szInfo, text, _TRUNCATE);
        nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    void ShowMain() {
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
    void ShowTrayMenu() {
        const View v = CurrentView();
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open RemoteDisplay");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (v.viaService ? MF_GRAYED : 0), IDM_TOGGLE, Sharing() ? L"Stop sharing" : L"Start sharing");
        AppendMenuW(m, MF_STRING | (v.state == rdh::HostStatus::Streaming ? 0 : MF_GRAYED), IDM_DISCONNECT, L"Disconnect viewer");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);  // required so the menu closes when you click elsewhere
        TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessage(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(m);
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
        bool controlling = false;
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
                v.controlling = s.controlling != 0;
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
        v.controlling = host_.controlling;
        return v;
    }

    void StopSharing() {
        stopHost_ = true;
        if (hostThread_.joinable()) hostThread_.join();
        stopHost_ = false;
        displaySession_.End();  // put the screens back as they were
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

    // One mechanism for everything that needs an administrator: run an elevated copy of this exe with
    // `args` (UAC prompt), wait for it on a helper thread, then finish in OnElevatedDone().
    void ElevatedAction(const std::wstring& args, Elev tag) {
        if (svcBusy_) return;
        if (svcThread_.joinable()) svcThread_.join();
        if (tag == Elev::SvcInstall && Sharing()) StopSharing();  // free the port for the service
        svcBusy_ = true;
        elevTag_ = tag;
        SetText(IDC_SVC_STATUS, L"Waiting for administrator approval...");
        EnableWindow(Ctrl(IDC_SVC_BTN), FALSE);
        EnableWindow(Ctrl(IDC_SVC_REMOVE), FALSE);
        EnableWindow(Ctrl(IDC_SECURE), FALSE);
        svcThread_ = std::thread([this, args] {
            const int rc = RunElevated(hwnd_, args);
            PostMessage(hwnd_, kMsgSvcDone, WPARAM(rc), 0);
        });
    }

    std::wstring UserArg() { return L" --user \"" + CurrentUserName() + L"\""; }

    void UpdateAdminUi() {
        const bool el = IsElevated();
        SetText(IDC_ADMIN_STATE, el ? L"Running as admin" : L"Restart as admin");
        EnableWindow(Ctrl(IDC_ADMIN_STATE), !el);
    }

    void RestartElevatedNow() {
        if (IsElevated()) return;
        if (RestartAsAdmin(hwnd_, Sharing())) {  // the new copy waits for this one to exit
            forceExit_ = true;
            DestroyWindow(hwnd_);
        } else {
            Message(L"Cancelled. Administrator approval is needed for this.");
        }
    }

    void OnElevatedDone(int rc) {
        if (svcThread_.joinable()) svcThread_.join();
        svcBusy_ = false;
        svcState_ = QueryServiceState();
        const bool ok = rc == 0;
        auto fail = [&] {
            if (rc == -1) Message(L"Cancelled. Administrator approval is needed for this.");
            else Message(L"That did not work (code " + std::to_wstring(rc) + L"). See ProgramData\\RemoteDisplay\\service.log");
        };
        switch (elevTag_) {
            case Elev::SvcInstall:
                if (ok) Message(L"Background service installed and running.", kGreen); else fail();
                break;
            case Elev::SvcUninstall:
                if (ok) Message(L"Background service removed.", kGreen); else fail();
                break;
            case Elev::AdminOn:
                if (ok) {
                    adminPref_ = true;
                    cfg_->SetAdminMode(true);
                    if (IsDlgButtonChecked(hwnd_, IDC_AUTOSTART) == BST_CHECKED) SetStartupEnabled(false);  // the elevated task starts it now
                    ApplyToService();  // the service relaunches its worker elevated
                    Message(L"Administrator mode is on for start-up and the service.", kGreen);
                    if (!IsElevated() &&
                        MessageBoxW(hwnd_, L"Restart RemoteDisplay as administrator now?\n\nSharing pauses for a moment while it restarts.",
                                    L"RemoteDisplay", MB_YESNO | MB_ICONQUESTION) == IDYES)
                        RestartElevatedNow();
                } else {
                    CheckDlgButton(hwnd_, IDC_ADMIN, BST_UNCHECKED);
                    fail();
                }
                break;
            case Elev::AdminOff:
                if (ok) {
                    adminPref_ = false;
                    cfg_->SetAdminMode(false);
                    if (IsDlgButtonChecked(hwnd_, IDC_AUTOSTART) == BST_CHECKED) SetStartupEnabled(true);  // back to the normal entry
                    ApplyToService();
                    Message(L"Administrator mode is off. It applies the next time RemoteDisplay starts.", kMuted);
                } else {
                    CheckDlgButton(hwnd_, IDC_ADMIN, BST_CHECKED);
                    fail();
                }
                break;
            case Elev::TaskOn:
                if (ok) {
                    SetStartupEnabled(false);  // only the elevated task may start it, or the normal copy would win the race
                    Message(L"RemoteDisplay will start as administrator when you sign in.", kGreen);
                } else {
                    CheckDlgButton(hwnd_, IDC_AUTOSTART, BST_UNCHECKED);
                    fail();
                }
                break;
            case Elev::SecureOn:
                if (ok) {
                    Message(L"Consent recorded. Every use of the UAC / sign-in screen is written to the audit log.", kGreen);
                } else {
                    CheckDlgButton(hwnd_, IDC_SECURE, BST_UNCHECKED);
                    fail();
                }
                break;
            case Elev::SecureOff:
                if (ok) Message(L"Consent withdrawn.", kMuted);
                else { CheckDlgButton(hwnd_, IDC_SECURE, BST_CHECKED); fail(); }
                break;
            case Elev::TaskOff:
                if (ok) Message(L"Will no longer start with Windows.", kMuted);
                else { CheckDlgButton(hwnd_, IDC_AUTOSTART, BST_CHECKED); fail(); }
                break;
        }
        UpdateUi();
        UpdateAdminUi();
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
            hostCfg_.allowControl = IsDlgButtonChecked(hwnd_, IDC_CONTROL) == BST_CHECKED;
            host_.state = rdh::HostStatus::Starting;
            host_.approve = host_.deny = host_.kick = false;
            promptShown_ = false;
            const std::string mode = cfg_->DisplayMode();
            const bool applied = displaySession_.Begin(mode);  // once only: a second Begin would save the changed layout
            hostCfg_.displayName = applied && mode == "extend" ? rdapp::FindVirtualMonitorDisplay() : std::wstring();
            hostThread_ = std::thread([this] { rdh::RunHost(hostCfg_, stopHost_, host_); });
        }
        UpdateUi();
    }

    void UpdateUi() {
        if (++tick_ % 4 == 1 && !svcBusy_) svcState_ = QueryServiceState();  // about once a second
        if (host_.state != rdh::HostStatus::AwaitingApproval) DismissPrompt(promptHandle_);  // stale: viewer left / trusted
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
                swprintf_s(b, v.controlling ? L"● %s controlling - %.0f fps" : L"● %s viewing - %.0f fps", W(peer).c_str(), v.fps);
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

        // tray: grey = not sharing, green = sharing, amber = someone is connecting/connected
        {
            std::wstring text = status;
            if (!text.empty() && text[0] == L'\u25CF') text = text.substr(2);
            const int ts = (connected || s == rdh::HostStatus::AwaitingApproval) ? 2 : (sharing || v.viaService) ? 1 : 0;
            UpdateTray(ts, L"RemoteDisplay - " + text);
        }

        // background-service section
        std::wstring ss, btn;
        COLORREF sc = kMuted;
        switch (svcState_) {
            case SvcState::NotInstalled: ss = L"Windows service: not installed"; btn = L"Install service"; break;
            case SvcState::Running: ss = L"● Windows service: running"; btn = L"Remove service"; sc = kGreen; break;
            case SvcState::Stopped: ss = L"● Windows service: stopped"; btn = L"Remove service"; sc = kAmber; break;
            case SvcState::Transition: ss = L"Windows service: changing..."; btn = L"Remove service"; sc = kAmber; break;
        }
        if (!svcBusy_) {
            svcColor_ = sc;
            if (ss != lastSvcStatus_) {
                lastSvcStatus_ = ss;
                SetText(IDC_SVC_STATUS, ss);
                InvalidateRect(Ctrl(IDC_SVC_STATUS), nullptr, TRUE);
            }
            const bool installed = svcState_ != SvcState::NotInstalled;
            EnableWindow(Ctrl(IDC_SVC_BTN), !installed);
            EnableWindow(Ctrl(IDC_SVC_REMOVE), installed);
            EnableWindow(Ctrl(IDC_SECURE), !svcBusy_);
        }
    }

    void MaybePromptForApproval() {
        if (!Sharing() || host_.state != rdh::HostStatus::AwaitingApproval) { promptShown_ = false; return; }
        if (promptShown_) return;
        promptShown_ = true;
        const bool visible = IsWindowVisible(hwnd_) != FALSE;
        if (visible && IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        if (visible) SetForegroundWindow(hwnd_);
        std::string who = host_.Peer();
        const bool known = host_.PeerKnown();
        if (known) who = host_.PeerName() + " (" + who + ")";
        const int r = AskApproval(visible ? hwnd_ : nullptr, W(who), hostCfg_.allowControl, known, promptHandle_);
        if (host_.state == rdh::HostStatus::AwaitingApproval) {  // the viewer may have left meanwhile
            if (r == 0) {
                host_.deny = true;
            } else {
                if (r == 2) host_.remember = true;  // trusted from now on
                host_.approve = true;
            }
        }
    }

    // ---- trusted devices dialog ------------------------------------------------------------------
    enum { IDT_LIST = 301, IDT_REMOVE, IDT_CLEAR, IDT_CLOSE };
    struct TrustedCtx {
        App* app = nullptr;
        std::vector<TrustedDevice> list;
        HWND listbox = nullptr;
    };

    static void FillTrusted(TrustedCtx* c, HWND dlg) {
        SendMessage(c->listbox, LB_RESETCONTENT, 0, 0);
        c->list = c->app->cfg_->TrustedDevices();
        for (const auto& d : c->list) {
            const std::wstring line = W(d.name.empty() ? "unnamed device" : d.name) + L"    (added " + W(d.added) + L")";
            SendMessageW(c->listbox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }
        EnableWindow(GetDlgItem(dlg, IDT_REMOVE), FALSE);
        EnableWindow(GetDlgItem(dlg, IDT_CLEAR), !c->list.empty());
    }

    static LRESULT CALLBACK TrustedProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams));
        auto* c = reinterpret_cast<TrustedCtx*>(GetWindowLongPtr(h, GWLP_USERDATA));
        if (!c) return DefWindowProcW(h, m, w, l);
        App* a = c->app;
        switch (m) {
            case WM_CREATE: {
                auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int ww, int hh, int id) {
                    HWND ch = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, a->S(x), a->S(y), a->S(ww), a->S(hh), h,
                                              reinterpret_cast<HMENU>(INT_PTR(id)), a->inst_, nullptr);
                    SendMessage(ch, WM_SETFONT, reinterpret_cast<WPARAM>(a->fontNormal_), TRUE);
                    return ch;
                };
                mk(L"STATIC", L"Devices you chose to \"Always allow\". They connect without asking, but still need your pairing key.",
                   SS_LEFT, 16, 12, 420, 36, 0);
                c->listbox = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 16, 56, 420, 180, IDT_LIST);
                mk(L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, 16, 248, 130, 28, IDT_REMOVE);
                mk(L"BUTTON", L"Remove all", BS_PUSHBUTTON | WS_TABSTOP, 156, 248, 110, 28, IDT_CLEAR);
                mk(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 336, 248, 100, 28, IDT_CLOSE);
                FillTrusted(c, h);
                return 0;
            }
            case WM_COMMAND:
                switch (LOWORD(w)) {
                    case IDT_LIST:
                        if (HIWORD(w) == LBN_SELCHANGE) EnableWindow(GetDlgItem(h, IDT_REMOVE), SendMessage(c->listbox, LB_GETCURSEL, 0, 0) >= 0);
                        break;
                    case IDT_REMOVE: {
                        const LRESULT i = SendMessage(c->listbox, LB_GETCURSEL, 0, 0);
                        if (i >= 0 && size_t(i) < c->list.size()) {
                            a->cfg_->RemoveTrusted(c->list[size_t(i)].id);
                            FillTrusted(c, h);
                        }
                        break;
                    }
                    case IDT_CLEAR:
                        if (MessageBoxW(h, L"Remove every trusted device? They will have to be approved again.", L"RemoteDisplay",
                                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                            a->cfg_->ClearTrusted();
                            FillTrusted(c, h);
                        }
                        break;
                    case IDT_CLOSE: DestroyWindow(h); break;
                }
                return 0;
            case WM_CLOSE: DestroyWindow(h); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void ShowTrustedDialog() {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = TrustedProc;
            wc.hInstance = inst_;
            wc.lpszClassName = L"RDTrusted";
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        TrustedCtx ctx;
        ctx.app = this;
        RECT rc = {0, 0, S(452), S(292)};
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRect(&rc, style, FALSE);
        RECT pr;
        GetWindowRect(hwnd_, &pr);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RDTrusted", L"Trusted devices", style, pr.left + ((pr.right - pr.left) - w) / 2,
                                   pr.top + ((pr.bottom - pr.top) - h) / 2, w, h, hwnd_, nullptr, inst_, &ctx);
        if (!dlg) return;
        EnableWindow(hwnd_, FALSE);  // modal
        ShowWindow(dlg, SW_SHOW);
        MSG msg;
        while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_QUIT) { PostQuitMessage(int(msg.wParam)); break; }
            if (!IsDialogMessageW(dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        EnableWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
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

        auto* v = rdv::Viewer::Create(inst_, hwnd_, kMsgViewerClosed, ip, port, psk, &identity_, DeviceName());
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
        RoundRect(mem, S(24), S(96), S(404), S(648), S(14), S(14));
        RoundRect(mem, S(416), S(96), S(796), S(648), S(14), S(14));
        DeleteObject(pen);
        DeleteObject(card);

        SetTextColor(mem, kMuted);
        SelectObject(mem, fontSmall_);
        TextOutW(mem, S(26), S(660), L"AES-128-GCM encrypted. Connections need your pairing key.", 56);

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
        if (taskbarCreated_ && m == taskbarCreated_) {
            trayAdded_ = false;
            AddTray();
            return 0;
        }
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
            case kMsgTray:
                if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_LBUTTONDBLCLK) ShowMain();
                else if (LOWORD(l) == WM_RBUTTONUP) ShowTrayMenu();
                return 0;
            case kMsgShow:
                ShowMain();
                return 0;
            case WM_CLOSE:
                // Closing the window while sharing keeps sharing, from the tray (Exit is in its menu).
                if (!forceExit_ && Sharing() && !showTray_) {
                    // No tray icon: never keep sharing invisibly by accident, ask first.
                    const int r = MessageBoxW(h,
                        L"Keep sharing in the background?\n\nYes: keep running with no icon (launch RemoteDisplay again to open it)\n"
                        L"No: stop sharing and exit\nCancel: go back to the window",
                        L"RemoteDisplay", MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (r == IDCANCEL) return 0;
                    if (r == IDYES) {
                        ShowWindow(h, SW_HIDE);
                        return 0;
                    }
                    DestroyWindow(h);  // No: stops sharing and exits
                    return 0;
                }
                if (!forceExit_ && Sharing()) {
                    ShowWindow(h, SW_HIDE);
                    if (!tipShown_) {
                        tipShown_ = true;
                        Balloon(L"RemoteDisplay is still running", L"Sharing continues from the tray. Right-click the icon to exit.");
                    }
                    return 0;
                }
                DestroyWindow(h);
                return 0;
            case WM_COMMAND:
                switch (LOWORD(w)) {
                    case IDC_TRUSTED: ShowTrustedDialog(); break;
                    case IDM_OPEN: ShowMain(); break;
                    case IDM_TOGGLE: ToggleSharing(); break;
                    case IDM_DISCONNECT:
                        if (CurrentView().viaService) StatusReader::Kick(); else host_.kick = true;
                        break;
                    case IDM_EXIT:
                        forceExit_ = true;
                        DestroyWindow(h);
                        break;
                    case IDC_SHOWTRAY: {
                        const bool on = IsDlgButtonChecked(h, IDC_SHOWTRAY) == BST_CHECKED;
                        if (!on) {
                            const int r = MessageBoxW(h,
                                L"Hide the tray icon?\n\nRemoteDisplay will keep running with nothing visible on screen. "
                                L"To open it again, launch RemoteDisplay from the Start menu or a shortcut. "
                                L"To stop it, open it and click Stop sharing, or close its window and choose to exit.\n\n"
                                L"While sharing, you will still be asked before anyone can connect (unless you turned that off).",
                                L"RemoteDisplay", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
                            if (r != IDYES) {
                                CheckDlgButton(h, IDC_SHOWTRAY, BST_CHECKED);
                                break;
                            }
                        }
                        showTray_ = on;
                        cfg_->SetShowTray(on);
                        if (on) AddTray(); else RemoveTray();
                        Message(on ? L"Tray icon shown." : L"Tray icon hidden. Launch RemoteDisplay again to open it.", kMuted);
                        break;
                    }
                    case IDC_ADMIN: {
                        const bool on = IsDlgButtonChecked(h, IDC_ADMIN) == BST_CHECKED;
                        if (svcBusy_) { CheckDlgButton(h, IDC_ADMIN, on ? BST_UNCHECKED : BST_CHECKED); break; }
                        const bool autostart = IsDlgButtonChecked(h, IDC_AUTOSTART) == BST_CHECKED;
                        const wchar_t* verbs = on ? (autostart ? L"worker-on,task-on" : L"worker-on") : L"worker-off,task-off";
                        ElevatedAction(std::wstring(L"--admin-ctl ") + verbs + UserArg(), on ? Elev::AdminOn : Elev::AdminOff);
                        break;
                    }
                    case IDC_SECURE: {
                        // Changing the consent needs administrator rights (UAC), so a connected viewer cannot
                        // switch it on. Declining the prompt puts the checkbox back.
                        const bool on = IsDlgButtonChecked(h, IDC_SECURE) == BST_CHECKED;
                        if (svcBusy_) { CheckDlgButton(h, IDC_SECURE, on ? BST_UNCHECKED : BST_CHECKED); break; }
                        ElevatedAction(std::wstring(L"--admin-ctl ") + (on ? L"secure-on" : L"secure-off") + UserArg(),
                                       on ? Elev::SecureOn : Elev::SecureOff);
                        break;
                    }
                    case IDC_ADMIN_STATE:
                        RestartElevatedNow();
                        break;
                    case IDC_AUTOSTART: {
                        const bool on = IsDlgButtonChecked(h, IDC_AUTOSTART) == BST_CHECKED;
                        if (adminPref_) {  // administrator mode: start-up goes through an elevated Scheduled Task
                            if (svcBusy_) { CheckDlgButton(h, IDC_AUTOSTART, on ? BST_UNCHECKED : BST_CHECKED); break; }
                            ElevatedAction(std::wstring(L"--admin-ctl ") + (on ? L"task-on" : L"task-off") + UserArg(),
                                           on ? Elev::TaskOn : Elev::TaskOff);
                            break;
                        }
                        if (!SetStartupEnabled(on)) {
                            CheckDlgButton(h, IDC_AUTOSTART, on ? BST_UNCHECKED : BST_CHECKED);
                            Message(L"Could not change the startup setting.");
                        } else {
                            Message(on ? L"RemoteDisplay will start sharing when you sign in." : L"Will no longer start with Windows.", kMuted);
                        }
                        break;
                    }
                    case IDC_SHARE: ToggleSharing(); break;
                    case IDC_CONNECT: Connect(); break;
                    case IDC_DISCONNECT:
                        if (CurrentView().viaService) StatusReader::Kick(); else host_.kick = true;
                        break;
                    case IDC_SVC_BTN:
                        if (svcState_ == SvcState::NotInstalled) ElevatedAction(L"--service-ctl install", Elev::SvcInstall);
                        break;
                    case IDC_SVC_REMOVE:
                        if (svcState_ != SvcState::NotInstalled &&
                            MessageBoxW(h, L"Uninstall the RemoteDisplay background service?\n\n"
                                           L"This PC will no longer start sharing when Windows starts. It also removes the firewall rule "
                                           L"the installer added. The app itself and your settings stay.",
                                        L"RemoteDisplay", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                            ElevatedAction(L"--service-ctl uninstall", Elev::SvcUninstall);
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
                    case IDC_DISPLAY_DUP:
                    case IDC_DISPLAY_EXT: {
                        // One choice at a time: ticking one clears the other; clearing both leaves the screens alone.
                        const bool dup = IsDlgButtonChecked(h, IDC_DISPLAY_DUP) == BST_CHECKED;
                        const bool ext = IsDlgButtonChecked(h, IDC_DISPLAY_EXT) == BST_CHECKED;
                        if (LOWORD(w) == IDC_DISPLAY_DUP && dup) CheckDlgButton(h, IDC_DISPLAY_EXT, BST_UNCHECKED);
                        if (LOWORD(w) == IDC_DISPLAY_EXT && ext) CheckDlgButton(h, IDC_DISPLAY_DUP, BST_UNCHECKED);
                        cfg_->SetDisplayMode(IsDlgButtonChecked(h, IDC_DISPLAY_DUP) == BST_CHECKED ? "duplicate"
                                             : IsDlgButtonChecked(h, IDC_DISPLAY_EXT) == BST_CHECKED ? "extend" : "");
                        Message(IsDlgButtonChecked(h, IDC_DISPLAY_DUP) == BST_CHECKED
                                    ? L"Screens will be duplicated while sharing, then put back."
                                : IsDlgButtonChecked(h, IDC_DISPLAY_EXT) == BST_CHECKED
                                    ? L"The virtual monitor will be streamed while sharing."
                                    : L"The screens are left as they are.", kMuted);
                        break;
                    }
                    case IDC_CONTROL:
                        cfg_->SetAllowControl(IsDlgButtonChecked(h, IDC_CONTROL) == BST_CHECKED);
                        hostCfg_.allowControl = IsDlgButtonChecked(h, IDC_CONTROL) == BST_CHECKED;  // live for this window's host
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
                OnElevatedDone(int(w));
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
                RemoveTray();
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
    ConfigTrust trust_;
    rd::DeviceIdentity identity_;
    PromptHandle promptHandle_;

    bool adminPref_ = false, autoShare_ = false;
    Elev elevTag_ = Elev::SvcInstall;
    DisplayModeSession displaySession_;  // Duplicate / Extend while sharing
    bool background_ = false, forceExit_ = false, tipShown_ = false, trayAdded_ = false, showTray_ = true;
    UINT taskbarCreated_ = 0;
    HICON trayIcons_[3] = {};
    int trayState_ = 0;
    std::wstring trayTip_ = L"RemoteDisplay";

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
    bool background = false, multiple = false, share = false, forceHelper = false;
    DWORD waitPid = 0;
    std::wstring user;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--background") background = true;
        else if (a == L"--allow-multiple") multiple = true;
        else if (a == L"--share") share = true;
        else if (a == L"--force") forceHelper = true;  // with --secure-helper, for testing only
        else if (a == L"--wait-pid" && i + 1 < argc) waitPid = DWORD(_wtoi(argv[++i]));
        else if (a == L"--user" && i + 1 < argc) user = argv[++i];
    }
    if (argv) LocalFree(argv);
    if (mode == L"--service") return RunServiceMain();          // started by the Service Control Manager
    if (mode == L"--host-worker") return RunWorker();           // started by the service, in the user session
    if (mode == L"--secure-helper") return rdapp::RunSecureHelper(forceHelper);  // started by the service as SYSTEM
    if (mode == L"--service-ctl") return ServiceControl(arg2);  // elevated helper: install/uninstall/start/stop
    if (mode == L"--admin-ctl") return AdminControl(arg2, user);  // elevated helper: administrator-mode settings

    if (waitPid) {  // restarted by a previous copy (e.g. "Restart as administrator"): let it finish exiting first
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) {
            WaitForSingleObject(p, 15'000);
            CloseHandle(p);
        }
    }

    // One app per user session: a second launch just brings the first one to the front. The mutex has an
    // explicit ACL so a normal-rights launch can still see an elevated copy (otherwise it gets "access
    // denied" and would start a duplicate).
    HANDLE single = nullptr;
    if (!multiple) {
        single = CreateMutexW(IpcSecurity(), FALSE, L"Local\\RemoteDisplayApp");
        const DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS || (!single && err == ERROR_ACCESS_DENIED)) {
            if (!background) {
                if (HWND existing = FindWindowW(L"RemoteDisplayApp", nullptr)) PostMessage(existing, kMsgShow, 0, 0);
            }
            return 0;
        }
    }
    return App::Run(inst, background, share);
}
