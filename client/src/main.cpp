// rd-client: single-viewer command-line client. The GUI app (RemoteDisplay.exe) uses the same Viewer.
//   rd-client --key <pairing-key> [--host <ip>] [--port <n>]
#include <array>
#include <string>

#include "viewer.h"

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    std::string hostIp = "127.0.0.1";
    int port = rd::kDefaultPort;
    std::string keyText;
    for (int i = 1; i + 1 < __argc; i += 2) {
        std::string a = __argv[i];
        if (a == "--host") hostIp = __argv[i + 1];
        else if (a == "--port") port = atoi(__argv[i + 1]);
        else if (a == "--key") keyText = __argv[i + 1];
    }
    std::array<uint8_t, rd::kKeyLen> psk{};
    if (!rd::ParseKey(keyText, psk.data())) {
        MessageBoxA(nullptr, "Usage: rd-client --key <pairing key shown by the host> [--host <ip>] [--port <n>]",
                    "rd-client", MB_ICONERROR);
        return 1;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);

    std::unique_ptr<rdv::Viewer> v(rdv::Viewer::Create(inst, nullptr, 0, hostIp, port, psk));
    if (!v) return 1;
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
