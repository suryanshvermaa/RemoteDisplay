#pragma once
// Screen capture with GDI (BitBlt from the desktop DC into system memory). Used when Direct3D 11 is not
// available on the GPU, for example while a display-only driver owns it: DXGI Desktop Duplication then
// cannot run, but GDI can still read the screen. It captures the whole virtual desktop, so every
// attached screen is in the picture. The hardware cursor is not drawn into it.
#include <windows.h>

#include <cstdint>
#include <vector>

class GdiCapture {
public:
    ~GdiCapture() { Release(); }

    // Sizes the capture to the virtual desktop (all screens). False if the desktop cannot be read.
    bool Init() {
        Release();
        rect_.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        rect_.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        rect_.right = rect_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        rect_.bottom = rect_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        w_ = UINT(rect_.right - rect_.left);
        h_ = UINT(rect_.bottom - rect_.top);
        if (w_ == 0 || h_ == 0) return false;

        screen_ = GetDC(nullptr);
        if (!screen_) return false;
        mem_ = CreateCompatibleDC(screen_);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = LONG(w_);
        bi.bmiHeader.biHeight = -LONG(h_);  // negative: top-down rows, same order as the picture
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(screen_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits_), nullptr, 0);
        if (!mem_ || !bitmap_ || !bits_) {
            Release();
            return false;
        }
        old_ = SelectObject(mem_, bitmap_);
        return true;
    }

    // Copies the desktop into the internal buffer, with the mouse pointer drawn in. False if the desktop
    // could not be read this time.
    bool Grab() {
        if (!bits_) return false;
        if (!BitBlt(mem_, 0, 0, LONG(w_), LONG(h_), screen_, rect_.left, rect_.top, SRCCOPY | CAPTUREBLT)) return false;
        DrawCursor();
        return true;
    }

    // The last grabbed picture: BGRA, top-down, pitch bytes per row.
    const uint8_t* Bgra() const { return bits_; }
    UINT Pitch() const { return w_ * 4; }

    // The area covered, in desktop coordinates (for pointer mapping).
    const RECT& rect() const { return rect_; }
    UINT width() const { return w_; }
    UINT height() const { return h_; }

private:
    // The pointer is not in the desktop bitmap: draw the current cursor at its position and hotspot.
    void DrawCursor() {
        CURSORINFO ci = {};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
        ICONINFO ii = {};
        if (!GetIconInfo(ci.hCursor, &ii)) return;
        if (ii.hbmMask) DeleteObject(ii.hbmMask);  // GetIconInfo hands these bitmaps to the caller
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        const int x = int(ci.ptScreenPos.x - rect_.left) - int(ii.xHotspot);
        const int y = int(ci.ptScreenPos.y - rect_.top) - int(ii.yHotspot);
        DrawIconEx(mem_, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    }

    void Release() {
        if (mem_ && old_) SelectObject(mem_, old_);
        if (bitmap_) DeleteObject(bitmap_);
        if (mem_) DeleteDC(mem_);
        if (screen_) ReleaseDC(nullptr, screen_);
        bitmap_ = nullptr;
        mem_ = nullptr;
        screen_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
    }

    RECT rect_ = {};
    UINT w_ = 0, h_ = 0;
    HDC screen_ = nullptr, mem_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    uint8_t* bits_ = nullptr;
};
