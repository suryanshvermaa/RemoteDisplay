// Duplicate and Extend switch the display layout, and RestoreDisplays puts the saved layout back.
#include <cstdio>
#include <string>

#include "display_mode.h"

static int CountActiveMonitors() {
    int n = 0;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT, LPARAM p) -> BOOL { ++*reinterpret_cast<int*>(p); return TRUE; },
                        reinterpret_cast<LPARAM>(&n));
    return n;
}

int main(int argc, char** argv) {
    using namespace rdapp;
    // --begin-and-die: switch to Duplicate and exit without restoring, like a killed worker.
    if (argc > 1 && std::string(argv[1]) == "--begin-and-die") {
        static DisplayModeSession session;
        session.Begin("duplicate");
        ExitProcess(0);  // no destructors, as with TerminateProcess
    }
    if (argc > 1 && std::string(argv[1]) == "--recover") {
        DisplayModeSession::RecoverIfNeeded();
        Sleep(2500);
        printf("after recovery: %d active display(s)\n", CountActiveMonitors());
        return CountActiveMonitors() == 2 ? 0 : 1;
    }
    SavedDisplays before;
    if (!SaveDisplays(before)) {
        printf("FAIL: cannot read the display layout\n");
        return 1;
    }
    const int start = CountActiveMonitors();
    printf("layout before: %d active display(s), %u path(s)\n", start, UINT(before.paths.size()));
    if (start < 2) {
        // Duplicate and Extend need a second display (the virtual monitor, or a real one).
        printf("SKIP: needs two active displays; there is %d\n", start);
        return 0;
    }

    int fails = 0;
    if (!ApplyDuplicate()) { printf("FAIL: Duplicate was refused\n"); ++fails; }
    Sleep(2500);
    printf("in Duplicate: %d active display(s)\n", CountActiveMonitors());

    if (!RestoreDisplays(before)) { printf("FAIL: restore was refused\n"); ++fails; }
    Sleep(2500);
    const int restored = CountActiveMonitors();
    printf("after restore: %d active display(s)\n", restored);
    if (restored != start) { printf("FAIL: restored layout differs from the saved one\n"); ++fails; }

    if (!ApplyExtend()) { printf("FAIL: Extend was refused\n"); ++fails; }
    Sleep(2500);
    printf("in Extend: %d active display(s)\n", CountActiveMonitors());
    if (!RestoreDisplays(before)) { printf("FAIL: restore after Extend was refused\n"); ++fails; }
    Sleep(2500);
    printf("final: %d active display(s)\n", CountActiveMonitors());

    printf(fails ? "%d FAILED\n" : "display mode tests passed\n", fails);
    return fails ? 1 : 0;
}
