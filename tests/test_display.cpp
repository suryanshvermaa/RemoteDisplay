// Checks that the capture pipeline can select the virtual monitor's output.
// Run with the virtual monitor installed and extended (see driver\README.md).
#include <cstdio>

#include "virtual_monitor.h"
#include "capture.h"

int main() {
    const std::wstring name = rdapp::FindVirtualMonitorDisplay();
    if (name.empty()) {
        printf("SKIP: no virtual monitor is active in the display configuration\n");
        return 0;
    }
    wprintf(L"virtual monitor display: %ls\n", name.c_str());

    Capture cap;
    cap.SetOutputName(name);
    const bool ok = cap.Init();
    printf("capture init on that output: %s\n", ok ? "ok" : "failed");
    if (!ok) return 1;
    printf("captured size: %ux%u\n", cap.bgraWidth(), cap.bgraHeight());
    // Frames arrive only when something changes on that screen; give it a few seconds with a window there.
    int frames = 0;
    for (int i = 0; i < 300; ++i) {
        bool lost = false;
        if (cap.Grab(16, lost)) ++frames;
        if (lost) { printf("duplication lost\n"); return 1; }
    }
    printf("frames grabbed in ~5 s: %d\n", frames);
    return cap.bgraWidth() == 1920 && cap.bgraHeight() == 1080 ? 0 : 1;
}
