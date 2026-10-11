#pragma once
// Helpers shared by the GUI, the background worker and the service.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "identity.h"
#include "protocol.h"
#include "session.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comctl32.lib")

namespace rdapp {

inline std::wstring UserDataDir();

// helpers

inline std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
inline std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::string Trim(std::string s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Secrets at rest are protected with the Windows user's DPAPI key (unreadable by other users).
inline std::string Protect(const uint8_t* d, size_t n) {
    DATA_BLOB in{DWORD(n), const_cast<BYTE*>(d)}, out{};
    if (!CryptProtectData(&in, L"RemoteDisplay", nullptr, nullptr, nullptr, 0, &out)) return "";
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (DWORD i = 0; i < out.cbData; ++i) { s += hex[out.pbData[i] >> 4]; s += hex[out.pbData[i] & 15]; }
    LocalFree(out.pbData);
    return s;
}
inline bool Unprotect(const std::string& hexs, std::vector<uint8_t>& result) {
    if (hexs.empty() || hexs.size() % 2) return false;
    std::vector<uint8_t> raw;
    for (size_t i = 0; i < hexs.size(); i += 2) {
        auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        int a = nib(hexs[i]), b = nib(hexs[i + 1]);
        if (a < 0 || b < 0) return false;
        raw.push_back(uint8_t(a * 16 + b));
    }
    DATA_BLOB in{DWORD(raw.size()), raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return false;
    result.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
}

struct TrustedDevice {
    rd::DeviceId id{};
    std::string name;
    std::string added;  // date text, for display
};

struct Recent {
    std::string addr;
    std::array<uint8_t, rd::kKeyLen> key{};
};

class Config {
public:
    Config() : path_(UserDataDir() + L"\\config.ini") {}

    std::string Get(const wchar_t* sec, const std::wstring& key) const {
        wchar_t buf[1024] = {};
        GetPrivateProfileStringW(sec, key.c_str(), L"", buf, 1024, path_.c_str());
        return U(buf);
    }
    void Set(const wchar_t* sec, const std::wstring& key, const std::string& v) const {
        WritePrivateProfileStringW(sec, key.c_str(), W(v).c_str(), path_.c_str());
    }

    std::array<uint8_t, rd::kKeyLen> HostKey() {
        std::vector<uint8_t> k;
        std::array<uint8_t, rd::kKeyLen> out{};
        if (Unprotect(Get(L"host", L"key"), k) && k.size() == rd::kKeyLen) {
            std::copy(k.begin(), k.end(), out.begin());
        } else {
            rd::crypto::Random(out.data(), out.size());
            SetHostKey(out);
        }
        return out;
    }
    void SetHostKey(const std::array<uint8_t, rd::kKeyLen>& k) { Set(L"host", L"key", Protect(k.data(), k.size())); }
    bool RequireApproval() const { return Get(L"host", L"approve") != "0"; }
    bool AllowControl() const { return Get(L"host", L"control") != "0"; }
    bool ShowTray() const { return Get(L"app", L"tray") != "0"; }  // default: show the tray icon
    bool AdminMode() const { return Get(L"app", L"admin") == "1"; }  // default: normal rights
    void SetAdminMode(bool v) const { Set(L"app", L"admin", v ? "1" : "0"); }
    void SetShowTray(bool v) const { Set(L"app", L"tray", v ? "1" : "0"); }
    void SetAllowControl(bool v) const { Set(L"host", L"control", v ? "1" : "0"); }
    // What the screens do while sharing: "duplicate", "extend", or empty (leave them alone).
    std::string DisplayMode() const { return Get(L"host", L"display_mode"); }
    void SetDisplayMode(const std::string& v) const { Set(L"host", L"display_mode", v); }
    void SetRequireApproval(bool v) const { Set(L"host", L"approve", v ? "1" : "0"); }

    std::vector<Recent> Recents() const {
        std::vector<Recent> r;
        int count = atoi(Get(L"recent", L"count").c_str());
        for (int i = 0; i < count && i < 8; ++i) {
            Recent x;
            x.addr = Get(L"recent", L"addr" + std::to_wstring(i));
            std::vector<uint8_t> k;
            if (x.addr.empty() || !Unprotect(Get(L"recent", L"key" + std::to_wstring(i)), k) || k.size() != rd::kKeyLen) continue;
            std::copy(k.begin(), k.end(), x.key.begin());
            r.push_back(x);
        }
        return r;
    }
    void AddRecent(const Recent& x) const {
        std::vector<Recent> r = Recents();
        r.erase(std::remove_if(r.begin(), r.end(), [&](const Recent& e) { return e.addr == x.addr; }), r.end());
        r.insert(r.begin(), x);
        if (r.size() > 8) r.resize(8);
        Set(L"recent", L"count", std::to_string(r.size()));
        for (size_t i = 0; i < r.size(); ++i) {
            Set(L"recent", L"addr" + std::to_wstring(i), r[i].addr);
            Set(L"recent", L"key" + std::to_wstring(i), Protect(r[i].key.data(), r[i].key.size()));
        }
    }

    // ---- trusted devices (approved with "Always allow this device") -------------------------------
    std::vector<TrustedDevice> TrustedDevices() const {
        std::vector<TrustedDevice> out;
        const int count = atoi(Get(L"trusted", L"count").c_str());
        for (int i = 0; i < count && i < 256; ++i) {
            TrustedDevice d;
            if (!rd::IdFromHex(Get(L"trusted", L"id" + std::to_wstring(i)), d.id)) continue;
            d.name = Get(L"trusted", L"name" + std::to_wstring(i));
            d.added = Get(L"trusted", L"added" + std::to_wstring(i));
            out.push_back(d);
        }
        return out;
    }
    bool IsTrusted(const rd::DeviceId& id) const {
        for (const auto& d : TrustedDevices())
            if (d.id == id) return true;
        return false;
    }
    void SaveTrusted(const std::vector<TrustedDevice>& list) const {
        Set(L"trusted", L"count", std::to_string(list.size()));
        for (size_t i = 0; i < list.size(); ++i) {
            Set(L"trusted", L"id" + std::to_wstring(i), rd::IdToHex(list[i].id));
            Set(L"trusted", L"name" + std::to_wstring(i), list[i].name);
            Set(L"trusted", L"added" + std::to_wstring(i), list[i].added);
        }
    }
    void AddTrusted(const rd::DeviceId& id, const std::string& name) const {
        auto list = TrustedDevices();
        for (auto& d : list)
            if (d.id == id) { d.name = name; SaveTrusted(list); return; }
        SYSTEMTIME t;
        GetLocalTime(&t);
        char date[32];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
        list.push_back({id, name, date});
        SaveTrusted(list);
    }
    void RemoveTrusted(const rd::DeviceId& id) const {
        auto list = TrustedDevices();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const TrustedDevice& d) { return d.id == id; }), list.end());
        SaveTrusted(list);
    }
    void ClearTrusted() const { SaveTrusted({}); }

    // ---- this PC's identity when it acts as a viewer (permanent key, DPAPI-protected) ---------------
    bool LoadDeviceIdentity(rd::DeviceIdentity& id) const {
        std::vector<uint8_t> blob;
        if (Unprotect(Get(L"device", L"key"), blob) && id.Load(blob)) return true;
        if (!id.Generate()) return false;
        const auto priv = id.ExportPrivate();
        Set(L"device", L"key", Protect(priv.data(), priv.size()));
        return true;
    }

private:
    std::wstring path_;
};

inline std::vector<std::string> LocalIPv4() {
    std::vector<std::string> out;
    ULONG len = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &len) != NO_ERROR)
        return out;
    std::vector<std::string> withGw, withoutGw;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[INET_ADDRSTRLEN];
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            if (strncmp(ip, "169.254.", 8) == 0) continue;
            (a->FirstGatewayAddress ? withGw : withoutGw).push_back(ip);
        }
    }
    out = withGw;
    out.insert(out.end(), withoutGw.begin(), withoutGw.end());
    return out;
}

// "host", "host:port" -> resolved IPv4 string + port. Returns false if it cannot be resolved.
inline bool ResolveAddress(std::string text, std::string& ip, int& port) {
    port = rd::kDefaultPort;
    size_t c = text.rfind(':');
    if (c != std::string::npos) {
        port = atoi(text.c_str() + c + 1);
        text = text.substr(0, c);
        if (port <= 0 || port > 65535) return false;
    }
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(text.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    ip = buf;
    return true;
}

inline void CopyToClipboard(HWND h, const std::wstring& text) {
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        memcpy(GlobalLock(g), text.c_str(), bytes);
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}


// Appends a timestamped line to a log file (best effort). Services have no console, so this is how
// you find out why one misbehaves.
inline void Log(const std::wstring& path, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[640];
    int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d  %s\r\n", t.wYear, t.wMonth, t.wDay,
                     t.wHour, t.wMinute, t.wSecond, msg);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, line, DWORD(n), &w, nullptr);
    CloseHandle(h);
}

// "Start with Windows" for the current user only: a value in HKCU\...\Run. No administrator
// rights are needed and it can be removed any time from Settings > Apps > Startup.
inline std::wstring SelfPath() {
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
    return p;
}
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"RemoteDisplay";

inline bool IsStartupEnabled() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

inline bool SetStartupEnabled(bool on) {
    if (!on) {
        LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring cmd = L"\"" + SelfPath() + L"\" --background";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                           DWORD((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

inline std::string DeviceName() {
    wchar_t n[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = DWORD(std::size(n));
    GetComputerNameW(n, &len);
    return U(n);
}

// ---- connection-approval prompt -------------------------------------------------------------------
// A Task Dialog with three choices. `ph.hwnd` lets another thread dismiss it if the situation changes
// (the viewer left, or turned out to be a trusted device).
struct PromptHandle {
    std::atomic<HWND> hwnd{nullptr};
};
inline HRESULT CALLBACK ApprovalCallback(HWND h, UINT msg, WPARAM, LPARAM, LONG_PTR data) {
    auto* ph = reinterpret_cast<PromptHandle*>(data);
    if (msg == TDN_CREATED) ph->hwnd = h;
    if (msg == TDN_DESTROYED) ph->hwnd = nullptr;
    return S_OK;
}
inline void DismissPrompt(PromptHandle& ph) {
    if (HWND h = ph.hwnd) PostMessage(h, TDM_CLICK_BUTTON, IDCANCEL, 0);
}
// Returns 0 = deny, 1 = allow once, 2 = always allow this device.
inline int AskApproval(HWND owner, const std::wstring& who, bool control, bool canRemember, PromptHandle& ph) {
    const std::wstring content = who + L" has your pairing key and wants to connect.";
    TASKDIALOG_BUTTON buttons[3];
    int n = 0;
    buttons[n++] = {1002, L"Allow once"};
    if (canRemember) buttons[n++] = {1003, L"Always allow this device"};
    buttons[n++] = {1001, L"Deny"};
    TASKDIALOGCONFIG tc = {sizeof(tc)};
    tc.hwndParent = owner;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"RemoteDisplay - connection request";
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = control ? L"Allow this device to view and control your PC?" : L"Allow this device to view your PC?";
    tc.pszContent = content.c_str();
    tc.cButtons = UINT(n);
    tc.pButtons = buttons;
    tc.nDefaultButton = 1001;  // Deny is the default: an accidental Enter never grants access
    tc.pfCallback = ApprovalCallback;
    tc.lpCallbackData = reinterpret_cast<LONG_PTR>(&ph);
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) return 0;
    return pressed == 1002 ? 1 : pressed == 1003 ? 2 : 0;
}

inline std::wstring UserDataDir() {
    wchar_t appdata[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring dir = std::wstring(appdata) + L"\\RemoteDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

}  // namespace rdapp
