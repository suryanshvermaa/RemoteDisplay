#pragma once
// Helpers shared by the GUI, the background worker and the service.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "protocol.h"
#include "session.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "crypt32.lib")

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

inline std::wstring UserDataDir() {
    wchar_t appdata[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    std::wstring dir = std::wstring(appdata) + L"\\RemoteDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

}  // namespace rdapp
