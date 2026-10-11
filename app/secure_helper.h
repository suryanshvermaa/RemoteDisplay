#pragma once
// The secure-desktop helper. The service starts it as SYSTEM on the Winlogon desktop of the console
// session (service.h), because only SYSTEM can see the UAC prompt, the sign-in screen and the lock screen.
//
// It follows the input desktop with SetThreadDesktop. It captures only while the secure desktop is up;
// otherwise it sleeps, drops its capture, and the worker captures the normal desktop itself.
//
// This thread must own no windows and no hooks, because SetThreadDesktop refuses a thread that does.
// Nothing here creates a window. Every failure is logged, so a problem on a real lock screen or UAC
// prompt shows up in secure-helper.log.
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
// Read, write and switch: the smaller set, tried if the full one is refused.
constexpr DWORD kDesktopMinAccess = 0x00000181;

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

// The desktop that receives input, or nullptr. Logs the error when it changes (not on every try).
inline HDESK OpenInputDesk(const std::wstring& log, DWORD& lastErr) {
    const DWORD masks[] = {kDesktopAllAccess, kDesktopMinAccess, GENERIC_ALL};
    for (DWORD m : masks) {
        if (HDESK d = OpenInputDesktop(0, FALSE, m)) {
            if (lastErr) Log(log, "OpenInputDesktop works again (access 0x%08lx)", m);
            lastErr = 0;
            return d;
        }
    }
    const DWORD e = GetLastError();
    if (e != lastErr) {
        lastErr = e;
        Log(log, "OpenInputDesktop failed (error %lu)", e);
    }
    return nullptr;
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

    HDESK cur = nullptr;      // the desktop this thread is attached to; kept open while it is in use
    std::wstring curName;     // its name
    bool attachNeeded = true; // re-attach even if the name is the same: the desktop object may be new
    DWORD openErr = 0;
    DWORD attachFails = 0, lostCount = 0;
    std::unique_ptr<Capture> cap;
    bool capReady = false, capFailLogged = false, firstFrameLogged = false;
    std::vector<uint8_t> pixels;

    for (;;) {
        if (HDESK in = OpenInputDesk(log, openErr)) {
            const std::wstring name = DesktopNameOf(in);
            if (name != curName || attachNeeded) {
                // Device and duplication must be released before the thread changes desktop.
                cap.reset();
                capReady = false;
                if (SetThreadDesktop(in)) {
                    if (cur) CloseDesktop(cur);
                    cur = in;
                    in = nullptr;  // now owned by `cur`
                    if (name != curName) Log(log, "input desktop is now '%ls'", name.c_str());
                    curName = name;
                    attachNeeded = false;
                    attachFails = 0;
                    capFailLogged = false;
                    firstFrameLogged = false;
                } else {
                    const DWORD e = GetLastError();
                    if (attachFails == 0 || attachFails % 50 == 0)
                        Log(log, "SetThreadDesktop('%ls') failed (error %lu), attempt %lu", name.c_str(), e, attachFails + 1);
                    if (++attachFails >= 100) {
                        // About 20 s of refusals: exit. The service starts us again on the Winlogon desktop.
                        Log(log, "giving up on attaching to '%ls'; exiting so the service restarts the helper", name.c_str());
                        CloseDesktop(in);
                        return 4;
                    }
                    Sleep(200);
                }
            }
            if (in) CloseDesktop(in);
        }

        const bool secure = !curName.empty() && _wcsicmp(curName.c_str(), L"Default") != 0;
        out.Heartbeat(secure || force);
        if (!secure && !force) {
            if (cap) {  // give the GPU back while the normal desktop is up
                cap.reset();
                capReady = false;
            }
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
        ID3D11Texture2D* tex = cap->Grab(50, lost);  // also paces the loop to the screen's refresh
        if (lost) {                                  // desktop switched or locked again: start over
            if (lostCount++ % 50 == 0)
                Log(log, "duplication lost on '%ls' (%lu times so far)", curName.c_str(), lostCount);
            capReady = false;
            attachNeeded = true;
            Sleep(50);
            continue;
        }
        if (tex) {
            UINT pitch = 0;
            if (cap->ReadBgra(pixels, pitch)) {
                out.Publish(pixels.data(), cap->bgraWidth(), cap->bgraHeight(), pitch);
                if (!firstFrameLogged) {
                    Log(log, "first picture from '%ls': %ux%u", curName.c_str(), cap->bgraWidth(), cap->bgraHeight());
                    firstFrameLogged = true;
                }
            } else {
                Log(log, "could not read the picture from '%ls'", curName.c_str());
                Sleep(100);
            }
        }
    }
}

}  // namespace rdapp
