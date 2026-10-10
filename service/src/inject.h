#pragma once
// Host side: turns input events from an authenticated, approved viewer into real mouse/keyboard
// input with SendInput. Keys use hardware scancodes so the remote keyboard layout is respected.
//
// Limits (Windows UIPI): input from a normal-user process cannot reach elevated (admin) windows,
// UAC prompts, the lock screen or Ctrl+Alt+Del. That needs a SYSTEM-level helper.
#include <windows.h>

#include <set>

#include "protocol.h"

namespace rdh {

class InputInjector {
public:
    // Screen rectangle (virtual-desktop coordinates) that the stream shows.
    void SetDesktopRect(const RECT& r) { rect_ = r; }

    void Move(uint16_t nx, uint16_t ny) {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        FillAbsolute(in.mi, nx, ny);
        SendInput(1, &in, sizeof(in));
    }

    void Apply(const rd::InputEvent& e) {
        switch (static_cast<rd::InputType>(e.type)) {
            case rd::InputType::Button: Button(e); break;
            case rd::InputType::Wheel: Wheel(e); break;
            case rd::InputType::Key: Key(e); break;
        }
    }

    // Called when the viewer goes away: never leave keys or buttons stuck down on this PC.
    void ReleaseAll() {
        for (uint32_t k : keysDown_) SendKey(uint16_t(k & 0xFFFF), (k >> 16) & 1, true);
        keysDown_.clear();
        for (int b = 0; b < 5; ++b)
            if (buttonDown_[b]) SendButton(b, false);
    }

private:
    void FillAbsolute(MOUSEINPUT& mi, uint16_t nx, uint16_t ny) const {
        const LONG w = rect_.right - rect_.left, h = rect_.bottom - rect_.top;
        const LONG px = rect_.left + LONG(uint32_t(nx) * (w > 1 ? w - 1 : 0) / 65535);
        const LONG py = rect_.top + LONG(uint32_t(ny) * (h > 1 ? h - 1 : 0) / 65535);
        const LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const LONG vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        mi.dx = vw > 1 ? LONG((int64_t(px - vx) * 65535 + (vw - 1) / 2) / (vw - 1)) : 0;
        mi.dy = vh > 1 ? LONG((int64_t(py - vy) * 65535 + (vh - 1) / 2) / (vh - 1)) : 0;
        mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }

    void SendButton(int idx, bool down) {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        switch (idx) {
            case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
            case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
            case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
            case 3: case 4:
                in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                in.mi.mouseData = idx == 3 ? XBUTTON1 : XBUTTON2;
                break;
            default: return;
        }
        SendInput(1, &in, sizeof(in));
    }

    void Button(const rd::InputEvent& e) {
        const int idx = e.flags & 0x7F;
        const bool down = (e.flags & 0x80) != 0;
        if (idx > 4) return;
        Move(e.x, e.y);  // land exactly where the viewer clicked
        SendButton(idx, down);
        buttonDown_[idx] = down;
    }

    void Wheel(const rd::InputEvent& e) {
        Move(e.x, e.y);
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = (e.flags & 1) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        in.mi.mouseData = DWORD(int32_t(e.v));
        SendInput(1, &in, sizeof(in));
    }

    static void SendKey(uint16_t scan, bool extended, bool up) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        SendInput(1, &in, sizeof(in));
    }

    void Key(const rd::InputEvent& e) {
        const bool up = (e.flags & 1) != 0, ext = (e.flags & 2) != 0;
        if (e.x == 0) return;
        const uint32_t id = uint32_t(e.x) | (ext ? 1u << 16 : 0);
        SendKey(e.x, ext, up);
        if (up) keysDown_.erase(id); else keysDown_.insert(id);
    }

    RECT rect_ = {0, 0, 1920, 1080};
    std::set<uint32_t> keysDown_;
    bool buttonDown_[5] = {};
};

}  // namespace rdh
