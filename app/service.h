#pragma once
// Windows service support: install/uninstall/start/stop, and the service itself.
//
// Why a worker process: services run in Session 0 with no access to the user's desktop, so screen
// capture cannot happen inside the service. The service (LocalSystem, starts with Windows) only
// supervises: it watches the active console session and launches `RemoteDisplay.exe --host-worker`
// into it as the logged-in user, relaunching it after a crash or when the user/session changes.
#include <windows.h>
#include <shellapi.h>
#include <userenv.h>
#include <winsvc.h>
#include <wtsapi32.h>
#include <tlhelp32.h>

#include <string>

#include "common.h"
#include "worker.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "shell32.lib")

namespace rdapp {

constexpr wchar_t kServiceName[] = L"RemoteDisplayHost";
constexpr wchar_t kServiceDisplay[] = L"RemoteDisplay Host";
constexpr wchar_t kFirewallRule[] = L"RemoteDisplay (UDP 47800)";

enum class SvcState { NotInstalled, Stopped, Running, Transition };

inline std::wstring ExePath() {
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
    return p;
}

inline std::wstring ServiceLogPath() {
    wchar_t pd[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = std::wstring(pd) + L"\\RemoteDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\service.log";
}

// Works without admin rights.
inline SvcState QueryServiceState() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return SvcState::NotInstalled;
    SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    SvcState r = SvcState::NotInstalled;
    if (s) {
        SERVICE_STATUS st = {};
        if (QueryServiceStatus(s, &st)) {
            r = st.dwCurrentState == SERVICE_RUNNING ? SvcState::Running
                : st.dwCurrentState == SERVICE_STOPPED ? SvcState::Stopped
                                                       : SvcState::Transition;
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return r;
}

// Runs this exe elevated (UAC prompt) with `args`, waits, returns its exit code (-1 if the user
// declined the prompt or it could not be started).
inline int RunElevated(HWND parent, const std::wstring& args) {
    const std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = parent;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, 90'000);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

namespace detail {

inline void Netsh(const std::wstring& args) {
    std::wstring cmd = L"netsh.exe " + args;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15'000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

inline void RemoveFirewallRule() {
    Netsh(std::wstring(L"advfirewall firewall delete rule name=\"") + kFirewallRule + L"\"");
}

// Inbound UDP for this program on private/domain networks only (not public Wi-Fi).
inline void AddFirewallRule(const std::wstring& exe) {
    RemoveFirewallRule();
    Netsh(std::wstring(L"advfirewall firewall add rule name=\"") + kFirewallRule + L"\" dir=in action=allow program=\"" +
          exe + L"\" protocol=UDP localport=" + std::to_wstring(rd::kDefaultPort) +
          L" profile=private,domain enable=yes");
}

inline bool WaitForState(SC_HANDLE s, DWORD want, DWORD ms) {
    for (DWORD waited = 0; waited <= ms; waited += 250) {
        SERVICE_STATUS st = {};
        if (QueryServiceStatus(s, &st) && st.dwCurrentState == want) return true;
        Sleep(250);
    }
    return false;
}

}  // namespace detail

// Executed in an elevated copy of the exe: `RemoteDisplay.exe --service-ctl <verb>`.
// Returns a process exit code: 0 on success.
inline int ServiceControl(const std::wstring& verb) {
    const std::wstring log = ServiceLogPath();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        Log(log, "ctl %ls: cannot open service manager (error %lu)", verb.c_str(), GetLastError());
        return 10;
    }
    int rc = 0;
    if (verb == L"install") {
        const std::wstring exe = ExePath();
        const std::wstring cmd = L"\"" + exe + L"\" --service";
        SC_HANDLE s = CreateServiceW(scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                     SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr,
                                     nullptr, nullptr);
        if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {  // reinstall: refresh the path
            s = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
            if (s) ChangeServiceConfigW(s, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd.c_str(),
                                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        if (!s) {
            Log(log, "install failed (error %lu)", GetLastError());
            rc = 11;
        } else {
            wchar_t desc[] = L"Lets you view this PC's screen remotely with RemoteDisplay. Starts with Windows.";
            SERVICE_DESCRIPTIONW d = {desc};
            ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
            SERVICE_DELAYED_AUTO_START_INFO delayed = {TRUE};
            ChangeServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
            SC_ACTION acts[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
            SERVICE_FAILURE_ACTIONSW fa = {86400, nullptr, nullptr, 3, acts};
            ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
            detail::AddFirewallRule(exe);
            if (!StartServiceW(s, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                Log(log, "installed, but start failed (error %lu)", GetLastError());
                rc = 12;
            } else {
                Log(log, "installed and started: %ls", cmd.c_str());
            }
            CloseServiceHandle(s);
        }
    } else if (verb == L"uninstall") {
        SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (s) {
            SERVICE_STATUS st = {};
            ControlService(s, SERVICE_CONTROL_STOP, &st);
            detail::WaitForState(s, SERVICE_STOPPED, 10'000);
            if (!DeleteService(s)) {
                Log(log, "uninstall failed (error %lu)", GetLastError());
                rc = 13;
            } else {
                Log(log, "uninstalled");
            }
            CloseServiceHandle(s);
        }
        detail::RemoveFirewallRule();
    } else if (verb == L"start" || verb == L"stop") {
        SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (!s) {
            rc = 14;
        } else {
            SERVICE_STATUS st = {};
            if (verb == L"start") {
                if (!StartServiceW(s, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) rc = 15;
            } else {
                ControlService(s, SERVICE_CONTROL_STOP, &st);
                detail::WaitForState(s, SERVICE_STOPPED, 10'000);
            }
            CloseServiceHandle(s);
        }
    } else {
        rc = 2;
    }
    CloseServiceHandle(scm);
    return rc;
}

// ---------------------------------------------------------------------------------------------
// The service itself (runs as LocalSystem under the Service Control Manager).

namespace svc {

inline SERVICE_STATUS_HANDLE g_handle = nullptr;
inline HANDLE g_stop = nullptr, g_wake = nullptr;

inline void Report(DWORD state, DWORD accepted = 0, DWORD waitMs = 0) {
    static DWORD checkpoint = 1;
    SERVICE_STATUS st = {};
    st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState = state;
    st.dwControlsAccepted = accepted;
    st.dwWaitHint = waitMs;
    st.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_handle, &st);
}

inline DWORD WINAPI Handler(DWORD ctl, DWORD, LPVOID, LPVOID) {
    switch (ctl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            Report(SERVICE_STOP_PENDING, 0, 8000);
            SetEvent(g_stop);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            SetEvent(g_wake);  // logon/logoff/user switch: re-evaluate now
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

// Keeps exactly one worker alive in the active console session.
// Enables one privilege on this process's own token (LocalSystem has it but starts it disabled).
inline bool EnablePrivilege(const wchar_t* name) {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
                    AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(t);
    return ok;
}

// Process id of `exe` running in session `sid`, or 0.
inline DWORD FindProcessInSession(const wchar_t* exe, DWORD sid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof(pe)};
    DWORD found = 0;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        DWORD s = 0;
        if (_wcsicmp(pe.szExeFile, exe) == 0 && ProcessIdToSessionId(pe.th32ProcessID, &s) && s == sid) {
            found = pe.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

class Supervisor {
public:
    explicit Supervisor(std::wstring log) : log_(std::move(log)) {}
    ~Supervisor() { Kill(); }

    void Tick() {
        const DWORD sid = WTSGetActiveConsoleSessionId();
        if (proc_) {
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(proc_, &code) && code == STILL_ACTIVE;
            if (!alive) {
                Log(log_, "worker exited (code %lu)", code);
                CloseHandle(proc_);
                proc_ = nullptr;
                backoffMs_ = (code == kExitHostBusy) ? 30'000 : 5'000;  // don't spin if it keeps dying
            } else if (sid != session_) {
                Log(log_, "active session changed %lu -> %lu; restarting worker", session_, sid);
                Kill();
                backoffMs_ = 0;
            }
        }
        if (!proc_ && sid != 0xFFFFFFFF && GetTickCount64() - lastStart_ >= backoffMs_) Launch(sid);
        ManageHelper(sid);
    }

    void Kill() {
        KillHelper();
        if (!proc_) return;
        TerminateProcess(proc_, 0);
        WaitForSingleObject(proc_, 3000);
        CloseHandle(proc_);
        proc_ = nullptr;
    }

private:
    // ---- secure-desktop helper (UAC prompt / sign-in screen) -------------------------------------
    // Runs only while the administrator has allowed it AND a worker is running in the same session.
    void ManageHelper(DWORD sid) {
        const bool want = proc_ && sid != 0xFFFFFFFF && sid == session_ && SecureDesktopAllowed();
        if (helper_) {
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(helper_, &code) && code == STILL_ACTIVE;
            if (!want) {
                Log(log_, "secure-desktop helper stopped (consent off or worker stopped)");
                KillHelper();
            } else if (!alive) {
                Log(log_, "secure-desktop helper exited (code %lu); restarting", code);
                KillHelper();
                helperStart_ = GetTickCount64();
                helperBackoff_ = 2000;
            }
        }
        if (want && !helper_ && GetTickCount64() - helperStart_ >= helperBackoff_) LaunchHelper(sid);
    }

    void KillHelper() {
        if (!helper_) return;
        TerminateProcess(helper_, 0);
        WaitForSingleObject(helper_, 3000);
        CloseHandle(helper_);
        helper_ = nullptr;
    }

    // Starts the helper as SYSTEM on the Winlogon desktop, using winlogon.exe's own token. winlogon.exe is
    // the SYSTEM process that owns the secure desktop, so this is the only way an ordinary service can
    // reach it. Nothing else here gets SYSTEM rights.
    void LaunchHelper(DWORD sid) {
        helperStart_ = GetTickCount64();
        helperBackoff_ = 5000;
        if (!EnablePrivilege(L"SeAssignPrimaryTokenPrivilege") || !EnablePrivilege(L"SeIncreaseQuotaPrivilege")) {
            Log(log_, "secure-desktop helper: could not enable privileges (error %lu)", GetLastError());
            return;
        }
        const DWORD pid = FindProcessInSession(L"winlogon.exe", sid);
        if (!pid) {
            Log(log_, "secure-desktop helper: winlogon.exe not found in session %lu", sid);
            return;
        }
        HANDLE wp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!wp) {
            Log(log_, "secure-desktop helper: cannot open winlogon.exe (error %lu)", GetLastError());
            return;
        }
        HANDLE wt = nullptr, tok = nullptr;
        bool ok = OpenProcessToken(wp, TOKEN_DUPLICATE | TOKEN_QUERY, &wt) &&
                  DuplicateTokenEx(wt, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &tok);
        if (wt) CloseHandle(wt);
        CloseHandle(wp);
        if (!ok) {
            Log(log_, "secure-desktop helper: cannot get winlogon token (error %lu)", GetLastError());
            return;
        }
        const std::wstring exe = ExePath();
        std::wstring cmd = L"\"" + exe + L"\" --secure-helper";
        std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
        wchar_t desktop[] = L"winsta0\\winlogon";
        STARTUPINFOW si = {sizeof(si)};
        si.lpDesktop = desktop;
        PROCESS_INFORMATION pi = {};
        ok = CreateProcessAsUserW(tok, nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
        CloseHandle(tok);
        if (!ok) {
            Log(log_, "secure-desktop helper: CreateProcessAsUser failed (error %lu)", GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        helper_ = pi.hProcess;
        Log(log_, "secure-desktop helper started in session %lu (pid %lu)", sid, pi.dwProcessId);
    }

    void Launch(DWORD sid) {
        lastStart_ = GetTickCount64();
        HANDLE userTok = nullptr;
        if (!WTSQueryUserToken(sid, &userTok)) {  // nobody logged in yet (login screen)
            if (!loggedNoUser_) Log(log_, "session %lu has no logged-in user yet (error %lu)", sid, GetLastError());
            loggedNoUser_ = true;
            backoffMs_ = 2000;
            return;
        }
        loggedNoUser_ = false;
        HANDLE tok = nullptr;
        BOOL ok = DuplicateTokenEx(userTok, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &tok);
        bool elevated = false;
        if (ok && GetElevateWorker()) {
            // An administrator signed in with UAC has two tokens: the everyday filtered one (what we
            // got) and a linked full-rights one. Using the linked one needs no prompt and is the same
            // account, so the worker can drive elevated windows. Standard users have no such token.
            TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
            DWORD got = 0;
            if (GetTokenInformation(userTok, TokenElevationType, &type, sizeof(type), &got) && type == TokenElevationTypeLimited) {
                TOKEN_LINKED_TOKEN lt = {};
                if (GetTokenInformation(userTok, TokenLinkedToken, &lt, sizeof(lt), &got)) {
                    HANDLE full = nullptr;
                    if (DuplicateTokenEx(lt.LinkedToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &full)) {
                        CloseHandle(tok);
                        tok = full;
                        elevated = true;
                    }
                    CloseHandle(lt.LinkedToken);
                }
            }
            if (!elevated) Log(log_, "administrator mode is on, but this user has no elevated token; worker runs with normal rights");
        }
        CloseHandle(userTok);
        if (!ok) {
            Log(log_, "DuplicateTokenEx failed (error %lu)", GetLastError());
            backoffMs_ = 5000;
            return;
        }
        LPVOID env = nullptr;
        CreateEnvironmentBlock(&env, tok, FALSE);
        const std::wstring exe = ExePath();
        std::wstring cmd = L"\"" + exe + L"\" --host-worker";
        std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
        wchar_t desktop[] = L"winsta0\\default";
        STARTUPINFOW si = {sizeof(si)};
        si.lpDesktop = desktop;
        PROCESS_INFORMATION pi = {};
        ok = CreateProcessAsUserW(tok, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                  CREATE_UNICODE_ENVIRONMENT, env, dir.c_str(), &si, &pi);
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(tok);
        if (!ok) {
            Log(log_, "CreateProcessAsUser failed (error %lu)", GetLastError());
            backoffMs_ = 5000;
            return;
        }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        session_ = sid;
        backoffMs_ = 5000;  // minimum gap between starts
        Log(log_, "worker launched in session %lu (pid %lu)%s", sid, pi.dwProcessId, elevated ? " with administrator rights" : "");
    }

    std::wstring log_;
    HANDLE proc_ = nullptr;
    HANDLE helper_ = nullptr;
    ULONGLONG helperStart_ = 0, helperBackoff_ = 0;
    DWORD session_ = 0;
    ULONGLONG lastStart_ = 0, backoffMs_ = 0;
    bool loggedNoUser_ = false;
};

inline void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_handle = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
    if (!g_handle) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Report(SERVICE_START_PENDING, 0, 3000);
    const std::wstring log = ServiceLogPath();
    Log(log, "service starting");
    Report(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE);

    {
        Supervisor sup(log);
        HANDLE waits[2] = {g_stop, g_wake};
        for (;;) {
            sup.Tick();
            if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) break;  // stop requested
        }
    }  // ~Supervisor ends the worker
    Log(log, "service stopped");
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    Report(SERVICE_STOPPED);
}

}  // namespace svc

// Called when launched by the Service Control Manager with `--service`.
inline int RunServiceMain() {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), svc::ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

}  // namespace rdapp
