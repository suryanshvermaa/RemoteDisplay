#pragma once
// Elevation helpers.
//
// When RemoteDisplay runs with administrator rights, Windows treats it as a different "integrity
// level" than the same user's normal programs. Two things then break unless handled on purpose:
//  * named objects (shared memory, events, mutexes) created by the elevated copy get a default ACL
//    that locks out the user's normal-rights window, so we give them an explicit one;
//  * window messages from lower-integrity processes (the Windows shell clicking the tray icon, a
//    second launch asking the first to show itself) are dropped, so the window allows the few it needs.
#include <windows.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace rdapp {

inline bool IsElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    const bool r = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(t);
    return r;
}

// SYSTEM, Administrators and interactive users get full access, at Medium integrity so a normal-rights
// process of the same user can use objects created by an elevated one. (The Local\ namespace is per
// session, so "interactive users" here means this session's user.)
inline SECURITY_ATTRIBUTES* IpcSecurity() {
    static SECURITY_ATTRIBUTES sa = [] {
        SECURITY_ATTRIBUTES a = {sizeof(a)};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;ME)",
                                                                 SDDL_REVISION_1, &sd, nullptr))
            a.lpSecurityDescriptor = sd;  // intentionally never freed: lives as long as the process
        return a;
    }();
    return sa.lpSecurityDescriptor ? &sa : nullptr;
}

constexpr wchar_t kAdminKey[] = L"SOFTWARE\\RemoteDisplay";
constexpr wchar_t kElevateValue[] = L"ElevateWorker";
// Machine-wide switch (set by an elevated helper): the service launches its worker elevated.
inline bool GetElevateWorker() {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kAdminKey, kElevateValue, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v != 0;
}

// Lets a lower-integrity process (the shell, another instance of the app) send `msg` to `hwnd`.
inline void AllowMessageFromLowerIntegrity(HWND hwnd, UINT msg) {
    ChangeWindowMessageFilterEx(hwnd, msg, MSGFLT_ALLOW, nullptr);
}

}  // namespace rdapp
