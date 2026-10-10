#pragma once
// The secure-desktop helper. The service starts it as SYSTEM on the Winlogon desktop of the console
// session (service.h), because only SYSTEM can see the UAC prompt and the sign-in screen.
//
// It follows the input desktop with SetThreadDesktop. It captures only while the secure desktop is up;
// otherwise it sleeps and drops its capture, and the worker captures the normal desktop itself.
//
// This thread must own no windows and no hooks, because SetThreadDesktop refuses a thread that does.
// Nothing here creates a window.
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "capture.h"
#include "common.h"
#include "secure_feed.h"

namespace rdapp {

// DESKTOP_ALL (winuser.h), spelled out because WIN32_LEAN_AND_MEAN hides it.
constexpr DWORD kDesktopAllAccess = 0x000F01FF;

inline std::wstring HelperLogPath() {
    wchar_t pd[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = std::wstring(pd) + L"\\RemoteDisplay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\secure-helper.log";
}

inline std::wstring DesktopNameOf(HDESK d) {
    wchar_t name[256] = {};
    DWORD need = 0;
    if (!d || !GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &need)) return L"";
    return name;
}

// `force` is for testing only and is never passed by the service: it captures the normal desktop too.
inline int RunSecureHelper(bool force) {
    const std::wstring log = HelperLogPath();
    SecureFeedWriter out;
    if (!out.Create()) {
        Log(log, "could not create the shared picture");
        return 2;
    }
    Log(log, "secure-desktop helper started, pid %lu%s", GetCurrentProcessId(), force ? " (forced)" : "");

    HDESK cur = nullptr;  // kept open while it is this thread's desktop
    std::wstring curName;
    std::unique_ptr<Capture> cap;
    bool capReady = false, capFailLogged = false;
    std::vector<uint8_t> pixels;

    for (;;) {
        if (HDESK in = OpenInputDesktop(0, FALSE, kDesktopAllAccess)) {
            const std::wstring name = DesktopNameOf(in);
            if (name != curName && SetThreadDesktop(in)) {
                if (cur) CloseDesktop(cur);
                cur = in;
                in = nullptr;
                Log(log, "input desktop is now '%ls'", name.c_str());
                curName = name;
                cap.reset();  // duplication belongs to the desktop it was created on
                capReady = false;
                capFailLogged = false;
            }
            if (in) CloseDesktop(in);
        }

        const bool secure = !curName.empty() && _wcsicmp(curName.c_str(), L"Default") != 0;
        out.Heartbeat(secure || force);
        if (!secure && !force) {
            cap.reset();  // give the GPU back while the normal desktop is up
            capReady = false;
            Sleep(200);
            continue;
        }

        if (!cap) cap = std::make_unique<Capture>();
        if (!capReady) {
            capReady = cap->Init();
            if (!capReady) {
                if (!capFailLogged) Log(log, "capture could not start on '%ls'; retrying", curName.c_str());
                capFailLogged = true;
                Sleep(300);
                continue;
            }
        }
        bool lost = false;
        ID3D11Texture2D* tex = cap->Grab(50, lost);  // paces the loop to the screen's refresh
        if (lost) {                                  // desktop switched under us: rebuild on the new one
            capReady = false;
            Sleep(50);
            continue;
        }
        if (tex) {
            UINT pitch = 0;
            if (cap->ReadBgra(pixels, pitch)) out.Publish(pixels.data(), cap->bgraWidth(), cap->bgraHeight(), pitch);
        }
    }
}

}  // namespace rdapp
