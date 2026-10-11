// rd-host: headless host (CLI). The GUI app (RemoteDisplay.exe) uses the same RunHost().
//   rd-host [--listen <ip>] [--port <n>] [--fps <n>] [--mbps <n>] [--key <pairing-key>] [--control 0|1]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "host.h"
#include "secure_feed.h"  // test mode only, see RD_SECURE_TEST below
#include "virtual_monitor.h"  // RD_DISPLAY=virtual: capture the virtual monitor

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    timeBeginPeriod(1);
    rdh::HostConfig cfg;
    cfg.listenIp = "127.0.0.1";  // CLI stays loopback-only unless asked
    cfg.requireApproval = false;
    std::string keyText;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--listen") cfg.listenIp = argv[i + 1];
        else if (a == "--port") cfg.port = atoi(argv[i + 1]);
        else if (a == "--fps") cfg.fps = atoi(argv[i + 1]);
        else if (a == "--mbps") cfg.mbps = atoi(argv[i + 1]);
        else if (a == "--key") keyText = argv[i + 1];
        else if (a == "--control") cfg.allowControl = atoi(argv[i + 1]) != 0;  // 0 = view only
    }
    if (keyText.empty()) {
        rd::crypto::Random(cfg.psk.data(), cfg.psk.size());
    } else if (!rd::ParseKey(keyText, cfg.psk.data())) {
        fprintf(stderr, "invalid --key (expected 26 base32 characters)\n");
        return 1;
    }
    printf("Pairing key: %s\nListening on %s:%d (all traffic is encrypted)\n",
           rd::FormatKey(cfg.psk.data()).c_str(), cfg.listenIp.c_str(), cfg.port);

    std::atomic<bool> stop{false};
    rdh::HostStatus st;
    st.state = rdh::HostStatus::Starting;  // set before the thread runs so "Stopped" means finished
    // Test mode (RD_SECURE_TEST=1): frames come from the secure-desktop helper's shared picture instead of
    // our own capture, so viewer checks cover the handoff, upload and encode of that path. Start the helper
    // yourself first: RemoteDisplay.exe --secure-helper --force (a separate process, as in production).
    // RD_DISPLAY=virtual captures the virtual monitor (the one named "RD Virtual") instead of the first screen.
    if (const char* d = getenv("RD_DISPLAY"); d && std::string(d) == "virtual") {
        cfg.displayName = rdapp::FindVirtualMonitorDisplay();
        printf(cfg.displayName.empty() ? "capture: virtual monitor not found, using the first screen\n"
                                       : "capture: virtual monitor\n");
    }
    static rdapp::SecureFeedReader secureFeed;
    if (getenv("RD_SECURE_TEST")) {
        cfg.secure = &secureFeed;
        printf("TEST MODE: frames come through the secure-desktop path\n");
    }
    std::thread t([&] { rdh::RunHost(cfg, stop, st); });
    static const char* names[] = {"stopped", "starting", "waiting for client", "awaiting approval",
                                  "streaming", "error"};
    int lastState = -1;
    for (;;) {
        int s = st.state;
        if (s == rdh::HostStatus::Failed || s == rdh::HostStatus::Stopped) break;
        if (s != lastState) {
            printf("[%s] %s\n", names[s], st.Peer().c_str());
            lastState = s;
        }
        if (s == rdh::HostStatus::Streaming) printf("%.0f fps, %.1f Mbps\n", st.fps.load(), st.mbps.load());
        Sleep(2000);
    }
    stop = true;
    t.join();
    if (!st.Error().empty()) fprintf(stderr, "error: %s\n", st.Error().c_str());
    return st.Error().empty() ? 0 : 1;
}
