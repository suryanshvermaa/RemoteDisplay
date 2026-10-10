#pragma once
// RemoteDisplay.exe --host-worker
// The headless host. The Windows service launches this into the active user's session (screen
// capture is impossible from Session 0), as that user. It shares the screen using the same saved
// pairing key as the GUI, shows the "Allow this connection?" prompt on the user's desktop, and
// publishes its status for the GUI through shared.h.
#include <atomic>
#include <thread>

#include "common.h"
#include "host.h"
#include "shared.h"

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
    Log(log, "listening on UDP %d, approval %s", hc.port, hc.requireApproval ? "required" : "not required");

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
        int lastState = -1;
        while (!done) {
            status.Publish(st);
            if (status.KickRequested()) st.kick = true;

            const int s = st.state;
            if (s != lastState) {
                Log(log, "state %d %s", s, st.Peer().c_str());
                lastState = s;
            }
            if (s == rdh::HostStatus::AwaitingApproval && !prompting) {
                if (prompt.joinable()) prompt.join();
                prompting = true;
                const std::string peer = st.Peer();
                prompt = std::thread([&, peer] {
                    std::wstring msg = W(peer) + L" has your pairing key and wants to view your screen.\n\nAllow this connection?";
                    int r = MessageBoxW(nullptr, msg.c_str(), L"RemoteDisplay - connection request",
                                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
                    if (st.state == rdh::HostStatus::AwaitingApproval) {  // client may have left meanwhile
                        if (r == IDYES) st.approve = true; else st.deny = true;
                    }
                    prompting = false;
                });
            }
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
