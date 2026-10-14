#pragma once
// RemoteDisplay.exe --host-worker
// The headless host. The Windows service launches this into the active user's session (screen
// capture is impossible from Session 0), as that user. It shares the screen using the same saved
// pairing key as the GUI, shows the "Allow this connection?" prompt on the user's desktop, and
// publishes its status for the GUI through shared.h.
#include <shellapi.h>

#include <atomic>
#include <thread>

#include "common.h"
#include "secure.h"
#include "secure_feed.h"
#include "host.h"
#include "shared.h"
#include "virtual_monitor.h"
#include "display_mode.h"

namespace rdapp {

// Test options for running the worker by hand (the service starts it without them):
//   --port <n>      listen on another UDP port (e.g. when the installed service holds 47800)
//   --key <key>    use this pairing key instead of the saved one
//   --no-approval  do not show the "Allow this connection?" prompt
inline void ApplyWorkerTestOptions(rdh::HostConfig& hc) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--no-approval") {
            hc.requireApproval = false;
        } else if (a == L"--port" && i + 1 < argc) {
            hc.port = _wtoi(argv[++i]);
        } else if (a == L"--key" && i + 1 < argc) {
            std::string text;  // keys are plain ASCII
            for (const wchar_t* p = argv[++i]; *p; ++p) text += char(*p);
            std::array<uint8_t, rd::kKeyLen> k{};
            if (rd::ParseKey(text, k.data())) hc.psk = k;
        }
    }
    LocalFree(argv);
}

}  // namespace rdapp

namespace rdapp {

constexpr int kExitHostBusy = 3;  // another host (the GUI) already owns the port in this session

inline int RunWorker() {
    const std::wstring log = UserDataDir() + L"\\worker.log";
    Log(log, "worker started (pid %lu)", GetCurrentProcessId());

    HostActiveLock lock;
    if (!lock.Acquire()) {
        Log(log, "another RemoteDisplay host is already active in this session; exiting");
        return kExitHostBusy;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);

    Config cfg;
    rdh::HostConfig hc;
    hc.listenIp = "0.0.0.0";
    hc.psk = cfg.HostKey();
    hc.requireApproval = cfg.RequireApproval();
    ApplyWorkerTestOptions(hc);
    hc.allowControl = cfg.AllowControl();
    // [host] capture = auto | dxgi | gdi (auto: GDI when the GPU has no Direct3D 11, e.g. the KMDOD driver).
    const std::string capture = cfg.Get(L"host", L"capture");
    if (!capture.empty()) hc.capture = capture;
    // Duplicate or Extend while sharing. A layout left by a worker that was killed is put back first.
    DisplayModeSession displaySession;
    DisplayModeSession::RecoverIfNeeded();
    const std::string displayMode = cfg.DisplayMode();
    if (displaySession.Begin(displayMode)) Log(log, "display layout: %s while sharing", displayMode.c_str());
    // [host] display=virtual, or display_mode=extend, captures the virtual monitor instead of the first screen.
    if (cfg.Get(L"host", L"display") == "virtual" || displayMode == "extend") {
        hc.displayName = rdapp::FindVirtualMonitorDisplay();
        Log(log, "capture target: %ls", hc.displayName.empty() ? L"virtual monitor not found (using the first screen)" : hc.displayName.c_str());
    }
    ConfigTrust trust;
    hc.trust = &trust;
    SecureFeedReader secureFeed;  // the UAC / sign-in screen picture from the helper, if it runs
    hc.secure = &secureFeed;
    hc.log = [log](const std::string& m) { Log(log, "%s", m.c_str()); };  // host diagnostics
    Log(log, "listening on UDP %d, approval %s, control %s", hc.port, hc.requireApproval ? "required" : "not required",
        hc.allowControl ? "allowed" : "view only");

    StatusWriter status;
    if (!status.Create()) Log(log, "could not create shared status (GUI will not see this worker)");

    for (;;) {  // restart the host if it fails (e.g. port briefly in use, GPU reset)
        std::atomic<bool> stop{false}, done{false};
        rdh::HostStatus st;
        st.state = rdh::HostStatus::Starting;
        std::thread host([&] {
            rdh::RunHost(hc, stop, st);
            done = true;
        });

        std::thread prompt;
        std::atomic<bool> prompting{false};
        PromptHandle promptHandle;
        int lastState = -1;
        bool wasSecure = false;
        SecureFeedReader auditFeed;
        while (!done) {
            status.Publish(st);
            // Opt-in secure-desktop audit: record every time the UAC prompt / sign-in screen takes over.
            const bool secure = auditFeed.Active();  // a normal process cannot open the Winlogon desktop itself
            if (secure != wasSecure) {
                wasSecure = secure;
                Audit(secure ? "secure-desktop-entered" : "secure-desktop-left",
                      SecureDesktopAllowed() ? "policy=allowed" : "policy=not-allowed");
            }
            if (status.KickRequested()) st.kick = true;
            if (status.RestartRequested()) {  // settings changed (key, approval, admin mode): the service relaunches us
                Log(log, "restart requested by the app");
                displaySession.End();  // ExitProcess skips destructors: put the layout back first
                ExitProcess(0);
            }

            const int s = st.state;
            if (s != lastState) {
                Log(log, "state %d %s", s, st.Peer().c_str());
                lastState = s;
            }
            if (s == rdh::HostStatus::AwaitingApproval && !prompting) {
                if (prompt.joinable()) prompt.join();
                prompting = true;
                std::string who = st.Peer();
                if (st.PeerKnown()) who = st.PeerName() + " (" + who + ")";
                const bool canRemember = st.PeerKnown();
                prompt = std::thread([&, who, canRemember] {
                    const int r = AskApproval(nullptr, W(who), hc.allowControl, canRemember, promptHandle);
                    if (st.state == rdh::HostStatus::AwaitingApproval) {  // viewer may have left meanwhile
                        if (r == 0) {
                            st.deny = true;
                        } else {
                            if (r == 2) st.remember = true;
                            st.approve = true;
                        }
                    }
                    prompting = false;
                });
            }
            if (s != rdh::HostStatus::AwaitingApproval && prompting) DismissPrompt(promptHandle);  // no longer needed
            Sleep(250);
        }
        host.join();
        if (prompt.joinable()) prompt.join();
        Log(log, "host stopped: %s", st.Error().empty() ? "(no error)" : st.Error().c_str());
        status.Publish(st);
        Sleep(5000);
    }
}

}  // namespace rdapp
