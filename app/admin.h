#pragma once
// "Run with administrator rights": what it takes and the one-time elevated helper that sets it up.
//
// Why: Windows (UIPI) refuses input injected by a normal-rights process into an elevated window
// (Task Manager as admin, Registry Editor, installers...). An elevated RemoteDisplay can drive them.
// It still cannot touch the UAC consent screen or the lock screen: those live on the secure desktop.
//
// Three ways to run elevated, all optional:
//  1. "Restart as administrator" now (one UAC click).
//  2. Start at sign-in elevated: a Scheduled Task with "highest privileges", created once (UAC) and
//     then started by Windows with no further prompts. (A HKCU Run entry can never start elevated.)
//  3. The Windows service launches its worker with the user's own elevated token (if the user is an
//     administrator), flag read from HKLM\SOFTWARE\RemoteDisplay\ElevateWorker.
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "common.h"
#include "ipc_security.h"
#include "secure.h"
#include "virtual_monitor.h"
#include "service.h"

#pragma comment(lib, "secur32.lib")

namespace rdapp {

constexpr wchar_t kTaskName[] = L"RemoteDisplay";

inline std::wstring CurrentUserName() {  // DOMAIN\\user or PC\\user
    wchar_t n[256] = {};
    ULONG len = ULONG(std::size(n));
    if (!GetUserNameExW(NameSamCompatible, n, &len)) return L"";
    return n;
}

inline bool SetElevateWorker(bool on) {  // needs administrator rights
    if (!on) {
        const LSTATUS r = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kAdminKey, kElevateValue);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    const DWORD v = 1;
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kAdminKey, kElevateValue, REG_DWORD, &v, sizeof(v)) == ERROR_SUCCESS;
}

namespace detail {

inline std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': o += L"&amp;"; break;
            case L'<': o += L"&lt;"; break;
            case L'>': o += L"&gt;"; break;
            case L'"': o += L"&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

// Runs a command line hidden, waits, returns its exit code (or -1).
inline int RunHidden(const std::wstring& cmdline) {
    std::wstring cmd = cmdline;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 20'000);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

}  // namespace detail

// The task's definition. The defaults of `schtasks /Create` would stop the app after 72 hours and
// whenever the laptop goes on battery, so it is written out in full instead.
inline std::wstring BuildTaskXml(const std::wstring& user, const std::wstring& exe, const wchar_t* runLevel = L"HighestAvailable") {
    const std::wstring u = detail::XmlEscape(user);
    return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
           L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
           L"  <RegistrationInfo><Description>Starts RemoteDisplay hidden when you sign in, with administrator rights.</Description></RegistrationInfo>\r\n"
           L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + u + L"</UserId></LogonTrigger></Triggers>\r\n"
           L"  <Principals><Principal id=\"Author\"><UserId>" + u + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>" +
           runLevel + L"</RunLevel></Principal></Principals>\r\n"
           L"  <Settings>\r\n"
           L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
           L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
           L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
           L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
           L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
           L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
           L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
           L"    <Enabled>true</Enabled>\r\n"
           L"    <Hidden>false</Hidden>\r\n"
           L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
           L"    <Priority>7</Priority>\r\n"
           L"  </Settings>\r\n"
           L"  <Actions Context=\"Author\"><Exec><Command>" + detail::XmlEscape(exe) + L"</Command><Arguments>--background</Arguments></Exec></Actions>\r\n"
           L"</Task>\r\n";
}

inline bool WriteUtf16File(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint16_t bom = 0xFEFF;  // schtasks wants UTF-16 with a byte-order mark
    DWORD w = 0;
    bool ok = WriteFile(f, &bom, sizeof(bom), &w, nullptr) &&
              WriteFile(f, text.data(), DWORD(text.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
    return ok;
}

inline bool TaskExists(const wchar_t* name = kTaskName) {
    return detail::RunHidden(std::wstring(L"schtasks.exe /Query /TN \"") + name + L"\"") == 0;
}

inline bool CreateStartupTask(const std::wstring& user, const std::wstring& exe, const wchar_t* runLevel = L"HighestAvailable",
                              const wchar_t* name = kTaskName) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring xml = std::wstring(tmp) + L"remotedisplay-task.xml";
    if (!WriteUtf16File(xml, BuildTaskXml(user, exe, runLevel))) return false;
    const int rc = detail::RunHidden(std::wstring(L"schtasks.exe /Create /TN \"") + name + L"\" /XML \"" + xml + L"\" /F");
    DeleteFileW(xml.c_str());
    return rc == 0;
}

inline bool DeleteStartupTask(const wchar_t* name = kTaskName) {
    const int rc = detail::RunHidden(std::wstring(L"schtasks.exe /Delete /TN \"") + name + L"\" /F");
    return rc == 0 || !TaskExists(name);  // already gone is fine
}

// "Start with Windows" is on if either mechanism is set up.
inline bool IsAutostartEnabled() { return IsStartupEnabled() || TaskExists(); }

// Executed in an elevated copy: `RemoteDisplay.exe --admin-ctl <verb[,verb...]> --user <DOMAIN\\user>`.
// Verbs: worker-on, worker-off (service worker elevation flag), task-on, task-off (elevated autostart).
inline int AdminControl(const std::wstring& verbs, const std::wstring& user) {
    const std::wstring log = ServiceLogPath();
    int rc = 0;
    size_t pos = 0;
    while (pos <= verbs.size()) {
        size_t comma = verbs.find(L',', pos);
        if (comma == std::wstring::npos) comma = verbs.size();
        const std::wstring v = verbs.substr(pos, comma - pos);
        pos = comma + 1;
        if (v.empty()) continue;
        bool ok = false;
        if (v == L"worker-on") ok = SetElevateWorker(true);
        else if (v == L"worker-off") ok = SetElevateWorker(false);
        else if (v == L"task-on") ok = !user.empty() && CreateStartupTask(user, SelfPath());
        else if (v == L"task-off") ok = DeleteStartupTask();
        else if (v == L"secure-on") { ok = SetSecureDesktopAllowed(true); Audit("secure-desktop-policy", "allowed=1"); }
        else if (v == L"secure-off") { ok = SetSecureDesktopAllowed(false); Audit("secure-desktop-policy", "allowed=0"); }
        else if (v.rfind(L"vm-add=", 0) == 0) {  // vm-add=<path to the driver package INF>
            std::wstring e;
            bool reboot = false;
            ok = AddVirtualMonitor(v.substr(7), e, reboot);
            if (!ok) Log(log, "vm-add: %ls", e.c_str());
            if (ok && reboot) Log(log, "vm-add: a restart is needed before the virtual monitor appears");
        } else if (v == L"vm-remove") {
            std::wstring e;
            ok = RemoveVirtualMonitor(e);
            if (!ok) Log(log, "vm-remove: %ls", e.c_str());
        } else if (v == L"driver-install") {  // installer: the driver package shipped next to this program
            const std::wstring self = SelfPath();
            const std::wstring inf = self.substr(0, self.find_last_of(L'\\')) + L"\\driver\\RemoteDisplayVirtual.inf";
            std::string out = RunCapture(L"pnputil /add-driver \"" + inf + L"\" /install");
            ok = out.find("successfully") != std::string::npos;
            if (!ok) Log(log, "driver-install: pnputil failed: %s", out.c_str());
            if (ok && !HasVirtualMonitorDevice()) {
                std::wstring e;
                bool reboot = false;
                ok = AddVirtualMonitor(inf, e, reboot);
                if (!ok) Log(log, "driver-install: %ls", e.c_str());
                if (ok && reboot) Log(log, "driver-install: a restart is needed before the virtual monitor appears");
            }
        } else if (v == L"driver-purge") {  // uninstall: remove the device and every driver package of it
            std::wstring e;
            bool failed = false;
            ok = RemoveVirtualMonitor(e);
            const int n = RemoveDriverPackages(failed);
            ok = ok && !failed;
            Log(log, "driver-purge: %d package(s) removed%s", n, failed ? " (one could not be removed)" : "");
        }
        Log(log, "admin-ctl %ls: %s", v.c_str(), ok ? "ok" : "FAILED");
        if (!ok) rc = 20;
    }
    return rc;
}

// Relaunches this app elevated and quits this copy once the new one has taken over (it waits for us).
// Returns false if the user declined the UAC prompt.
inline bool RestartAsAdmin(HWND parent, bool resumeSharing) {
    std::wstring args = L"--wait-pid " + std::to_wstring(GetCurrentProcessId());
    if (resumeSharing) args += L" --share";
    const std::wstring exe = SelfPath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.hwnd = parent;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

}  // namespace rdapp
