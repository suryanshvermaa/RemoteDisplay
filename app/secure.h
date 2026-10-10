#pragma once
// Secure desktop support, opt-in only.
//
// Windows draws the UAC consent prompt and the sign-in screen on a separate "secure desktop" (the
// Winlogon desktop). An ordinary process cannot read it, so while it is shown the viewer sees a
// frozen picture. This module does three things:
//  1. the machine-wide opt-in flag (HKLM, changeable only by an administrator, so a connected viewer
//     cannot switch it on),
//  2. detects when the input desktop is the secure one,
//  3. writes an audit trail of every time it happens.
// Capturing the secure desktop itself needs a SYSTEM-level helper; see docs/ARCHITECTURE.md.
#include <windows.h>

#include <string>

#include "common.h"
#include "ipc_security.h"

namespace rdapp {

constexpr wchar_t kSecureValue[] = L"AllowSecureDesktop";

// True only if an administrator turned the feature on for this PC.
inline bool SecureDesktopAllowed() {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kAdminKey, kSecureValue, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v != 0;
}

// Needs administrator rights (HKLM). Returns false for normal users.
inline bool SetSecureDesktopAllowed(bool on) {
    if (!on) {
        const LSTATUS r = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kAdminKey, kSecureValue);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    const DWORD v = 1;
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kAdminKey, kSecureValue, REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
}

// Name of the desktop that currently receives input: "Default" for the user's desktop, "Winlogon" for
// the UAC prompt and sign-in screen. Empty if it cannot be read (e.g. in a service or locked session).
inline std::wstring InputDesktopName() {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) return L"";
    wchar_t name[256] = {};
    DWORD need = 0;
    std::wstring out;
    if (GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &need)) out = name;
    CloseDesktop(d);
    return out;
}

inline bool OnSecureDesktop() {
    const std::wstring n = InputDesktopName();
    return !n.empty() && _wcsicmp(n.c_str(), L"Default") != 0;
}

// Append-only audit trail: one line per event, with time, Windows user and process. Lives in
// %ProgramData%, which normal users cannot rewrite, so a connected viewer cannot erase it.
inline std::wstring AuditLogPath() {
    wchar_t pd[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = std::wstring(pd) + L"\\RemoteDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\audit.log";
}

inline void Audit(const char* event, const std::string& detail) {
    wchar_t user[256] = {};
    DWORD n = DWORD(std::size(user));
    GetUserNameW(user, &n);
    Log(AuditLogPath(), "AUDIT %s user=%ls pid=%lu %s", event, user, GetCurrentProcessId(), detail.c_str());
}

}  // namespace rdapp
