#pragma once
// The look shared by the app and the client: light and dark palettes, anti-aliased drawing (GDI+), and
// controls drawn by us so they follow the theme: buttons, toggle switches, text fields, drop-downs and
// status pills. Windows draws none of these in dark colours by itself, so all of them are painted here.
//
// Usage: rdui::GdiplusScope at start-up, rdui::ApplyTheme(mode), then route WM_DRAWITEM, WM_MEASUREITEM and
// WM_CTLCOLOR* to the helpers below. Toggles answer BM_GETCHECK / BM_SETCHECK like real check boxes, so code
// using IsDlgButtonChecked / CheckDlgButton keeps working; the parent flips them on BN_CLICKED (FlipIfToggle).
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace rdui {

// ---------------------------------------------------------------------------------------------
// Palettes

struct Palette {
    bool dark;
    COLORREF bg;             // window background
    COLORREF surface;        // cards, top bar
    COLORREF surfaceHover;   // hovered / secondary surfaces
    COLORREF border;         // card and divider lines
    COLORREF text, muted, subtle;
    COLORREF accent, accentHover, accentPressed, onAccent;
    COLORREF field, fieldBorder;  // text boxes and drop-downs
    COLORREF track;               // toggle switch when off
    COLORREF success, danger, warning;
    COLORREF selection;           // highlighted list row
};

inline Palette LightPalette() {
    Palette p{};
    p.dark = false;
    p.bg = RGB(246, 247, 251);
    p.surface = RGB(255, 255, 255);
    p.surfaceHover = RGB(243, 245, 250);
    p.border = RGB(229, 232, 240);
    p.text = RGB(15, 23, 42);
    p.muted = RGB(100, 116, 139);
    p.subtle = RGB(148, 163, 184);
    p.accent = RGB(99, 102, 241);
    p.accentHover = RGB(79, 70, 229);
    p.accentPressed = RGB(67, 56, 202);
    p.onAccent = RGB(255, 255, 255);
    p.field = RGB(250, 251, 253);
    p.fieldBorder = RGB(221, 225, 235);
    p.track = RGB(203, 213, 225);
    p.success = RGB(22, 163, 74);
    p.danger = RGB(220, 38, 38);
    p.warning = RGB(217, 119, 6);
    p.selection = RGB(238, 240, 254);
    return p;
}

inline Palette DarkPalette() {
    Palette p{};
    p.dark = true;
    p.bg = RGB(11, 14, 20);
    p.surface = RGB(20, 24, 33);
    p.surfaceHover = RGB(30, 35, 47);
    p.border = RGB(37, 43, 57);
    p.text = RGB(231, 234, 243);
    p.muted = RGB(140, 150, 168);
    p.subtle = RGB(96, 106, 124);
    p.accent = RGB(108, 112, 245);
    p.accentHover = RGB(129, 133, 250);
    p.accentPressed = RGB(88, 92, 230);
    p.onAccent = RGB(255, 255, 255);
    p.field = RGB(14, 18, 26);
    p.fieldBorder = RGB(46, 53, 69);
    p.track = RGB(55, 63, 80);
    p.success = RGB(52, 199, 105);
    p.danger = RGB(248, 113, 113);
    p.warning = RGB(251, 191, 36);
    p.selection = RGB(38, 42, 74);
    return p;
}

enum class Mode { System, Light, Dark };

inline Palette& P() {
    static Palette p = LightPalette();
    return p;
}
inline Mode& CurrentMode() {
    static Mode m = Mode::System;
    return m;
}

// Windows' own setting: Settings > Personalization > Colors > "Choose your app mode".
inline bool SystemPrefersDark() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

inline Mode ModeFromString(const std::string& s) {
    if (s == "light") return Mode::Light;
    if (s == "dark") return Mode::Dark;
    return Mode::System;
}
inline const char* ModeToString(Mode m) { return m == Mode::Light ? "light" : m == Mode::Dark ? "dark" : "system"; }

// Brushes for WM_CTLCOLOR*, one per colour, freed when the theme changes.
inline std::vector<std::pair<COLORREF, HBRUSH>>& BrushCache() {
    static std::vector<std::pair<COLORREF, HBRUSH>> cache;
    return cache;
}
inline HBRUSH Brush(COLORREF c) {
    for (auto& e : BrushCache())
        if (e.first == c) return e.second;
    HBRUSH b = CreateSolidBrush(c);
    BrushCache().push_back({c, b});
    return b;
}

inline void ApplyTheme(Mode m) {
    CurrentMode() = m;
    const bool dark = m == Mode::Dark || (m == Mode::System && SystemPrefersDark());
    P() = dark ? DarkPalette() : LightPalette();
    for (auto& e : BrushCache()) DeleteObject(e.second);
    BrushCache().clear();
}

// Title bar in the theme's colours (Windows 11; older Windows ignores the attributes it does not know).
inline void ThemeTitleBar(HWND h) {
    const BOOL dark = P().dark;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF caption = P().surface, text = P().text;
    DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(h, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

// Drop-down lists and scroll bars: Windows' dark control theme when the palette is dark.
inline void ThemeControl(HWND h) { SetWindowTheme(h, P().dark ? L"DarkMode_Explorer" : L"Explorer", nullptr); }

// ---------------------------------------------------------------------------------------------
// Drawing

class GdiplusScope {
public:
    GdiplusScope() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token_, &in, nullptr);
    }
    ~GdiplusScope() { Gdiplus::GdiplusShutdown(token_); }

private:
    ULONG_PTR token_ = 0;
};

inline COLORREF Mix(COLORREF a, COLORREF b, double t) {  // t = 0 -> a, 1 -> b
    auto ch = [t](int x, int y) { return int(x + (y - x) * t + 0.5); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

inline Gdiplus::Color GC(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

inline void RoundPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    float d = std::min({radius * 2.f, r.Width, r.Height});
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

inline void Smooth(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

// Rounded rectangle, anti-aliased. `line` = CLR_INVALID for no outline.
inline void FillRound(HDC dc, const RECT& rc, float radius, COLORREF fill, COLORREF line = CLR_INVALID, float lineW = 1.f,
                      BYTE alpha = 255) {
    Gdiplus::Graphics g(dc);
    Smooth(g);
    const float inset = line == CLR_INVALID ? 0.f : lineW / 2.f;
    Gdiplus::RectF r(rc.left + inset, rc.top + inset, float(rc.right - rc.left) - 2 * inset,
                     float(rc.bottom - rc.top) - 2 * inset);
    Gdiplus::GraphicsPath path;
    RoundPath(path, r, radius);
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(GC(fill, alpha));
        g.FillPath(&b, &path);
    }
    if (line != CLR_INVALID) {
        Gdiplus::Pen pen(GC(line, alpha), lineW);
        g.DrawPath(&pen, &path);
    }
}

// A soft drop shadow under a rounded card (light theme only; dark cards use their border).
inline void SoftShadow(HDC dc, const RECT& rc, float radius, int scale96) {
    if (P().dark) return;
    Gdiplus::Graphics g(dc);
    Smooth(g);
    const int steps = 6;
    for (int i = steps; i >= 1; --i) {
        const float grow = float(i) * scale96 / 96.f * 1.6f;
        Gdiplus::RectF r(rc.left - grow, rc.top - grow + 4.f * scale96 / 96.f, float(rc.right - rc.left) + 2 * grow,
                         float(rc.bottom - rc.top) + 2 * grow);
        Gdiplus::GraphicsPath path;
        RoundPath(path, r, radius + grow);
        Gdiplus::SolidBrush b(Gdiplus::Color(BYTE(5 + (steps - i)), 15, 23, 42));
        g.FillPath(&b, &path);
    }
}

inline void FillCircle(HDC dc, float cx, float cy, float r, COLORREF c, BYTE alpha = 255) {
    Gdiplus::Graphics g(dc);
    Smooth(g);
    Gdiplus::SolidBrush b(GC(c, alpha));
    g.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r);
}

inline void Line(HDC dc, float x1, float y1, float x2, float y2, COLORREF c, float w) {
    Gdiplus::Graphics g(dc);
    Smooth(g);
    Gdiplus::Pen pen(GC(c), w);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, x1, y1, x2, y2);
}

inline void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, s.c_str(), -1, &r, fmt);
    SelectObject(dc, old);
}

inline int Scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

// The app mark: a rounded tile with an indigo-to-violet gradient and a white monitor.
inline void DrawLogo(HDC dc, int x, int y, int size) {
    Gdiplus::Graphics g(dc);
    Smooth(g);
    const Gdiplus::RectF r{float(x), float(y), float(size), float(size)};
    Gdiplus::GraphicsPath path;
    RoundPath(path, r, size * 0.28f);
    Gdiplus::LinearGradientBrush grad(Gdiplus::PointF(r.X, r.Y), Gdiplus::PointF(r.X + r.Width, r.Y + r.Height),
                                      GC(RGB(99, 102, 241)), GC(RGB(168, 85, 247)));
    g.FillPath(&grad, &path);
    const float s = float(size);
    Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), s * 0.075f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::GraphicsPath screen;
    RoundPath(screen, Gdiplus::RectF(x + s * 0.22f, y + s * 0.25f, s * 0.56f, s * 0.38f), s * 0.06f);
    g.DrawPath(&pen, &screen);
    g.DrawLine(&pen, x + s * 0.5f, y + s * 0.63f, x + s * 0.5f, y + s * 0.74f);
    g.DrawLine(&pen, x + s * 0.36f, y + s * 0.76f, x + s * 0.64f, y + s * 0.76f);
}

// Draws an owner-drawn control off-screen and copies it in one step, so it never shows half-painted.
template <typename F>
void Buffered(const DRAWITEMSTRUCT* d, F&& draw) {
    const int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    DRAWITEMSTRUCT c = *d;
    c.hDC = mem;
    OffsetRect(&c.rcItem, -d->rcItem.left, -d->rcItem.top);
    draw(&c);
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------------------------------------
// Hover tracking (all our controls): a window property is set while the mouse is over the control.

inline bool Hovered(HWND h) { return GetPropW(h, L"rdhover") != nullptr; }

inline void TrackHover(HWND h, UINT m) {
    if (m == WM_MOUSEMOVE && !Hovered(h)) {
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        SetPropW(h, L"rdhover", reinterpret_cast<HANDLE>(1));
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        RemovePropW(h, L"rdhover");
        InvalidateRect(h, nullptr, FALSE);
    }
}

inline LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    TrackHover(h, m);
    if (GetPropW(h, L"rdtoggle")) {  // toggle switches behave like check boxes for the rest of the code
        if (m == BM_GETCHECK) return GetPropW(h, L"rdon") ? BST_CHECKED : BST_UNCHECKED;
        if (m == BM_SETCHECK) {
            if (w == BST_CHECKED) SetPropW(h, L"rdon", reinterpret_cast<HANDLE>(1));
            else RemovePropW(h, L"rdon");
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
    }
    if (m == WM_NCDESTROY) {
        RemovePropW(h, L"rdhover");
        RemovePropW(h, L"rdtoggle");
        RemovePropW(h, L"rdon");
        RemoveWindowSubclass(h, ButtonProc, 1);
    }
    return DefSubclassProc(h, m, w, l);
}

// Call on every owner-drawn button; with `toggle`, it becomes a switch that keeps its own checked state.
inline void MakeButton(HWND h, bool toggle = false) {
    if (toggle) SetPropW(h, L"rdtoggle", reinterpret_cast<HANDLE>(1));
    SetWindowSubclass(h, ButtonProc, 1, 0);
}
inline bool IsToggle(HWND h) { return h && GetPropW(h, L"rdtoggle") != nullptr; }

// Owner-drawn buttons do not toggle themselves: the parent calls this for every BN_CLICKED / BN_DOUBLECLICKED.
// True if `h` is a toggle (it has been flipped).
inline bool FlipIfToggle(HWND h, UINT code) {
    if (!IsToggle(h) || (code != BN_CLICKED && code != BN_DOUBLECLICKED)) return false;
    SendMessageW(h, BM_SETCHECK, SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Buttons

enum class ButtonKind { Primary, Success, Danger, Secondary, DangerOutline, Ghost };

inline void DrawButton(const DRAWITEMSTRUCT* d, ButtonKind kind, const std::wstring& label, HFONT font, int dpi,
                       COLORREF behind) {
    const Palette& p = P();
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    const bool hover = Hovered(d->hwndItem);
    COLORREF fill = behind, line = CLR_INVALID, ink = p.text;
    switch (kind) {
        case ButtonKind::Primary:
            fill = pressed ? p.accentPressed : hover ? p.accentHover : p.accent;
            ink = p.onAccent;
            break;
        case ButtonKind::Success: {
            const COLORREF base = RGB(22, 163, 74);
            fill = pressed ? Mix(base, RGB(0, 0, 0), 0.25) : hover ? Mix(base, RGB(0, 0, 0), 0.12) : base;
            ink = RGB(255, 255, 255);
            break;
        }
        case ButtonKind::Danger: {
            const COLORREF base = RGB(220, 38, 38);
            fill = pressed ? Mix(base, RGB(0, 0, 0), 0.25) : hover ? Mix(base, RGB(0, 0, 0), 0.12) : base;
            ink = RGB(255, 255, 255);
            break;
        }
        case ButtonKind::Secondary:
            fill = p.dark ? (hover ? Mix(p.surfaceHover, p.text, 0.06) : p.surfaceHover) : (hover ? p.surfaceHover : p.surface);
            if (pressed) fill = Mix(fill, p.text, 0.06);
            line = hover ? Mix(p.fieldBorder, p.accent, 0.45) : p.fieldBorder;
            break;
        case ButtonKind::DangerOutline:
            fill = hover || pressed ? Mix(behind, p.danger, p.dark ? 0.16 : 0.08) : behind;
            line = Mix(behind, p.danger, 0.45);
            ink = p.danger;
            break;
        case ButtonKind::Ghost:
            fill = hover || pressed ? p.surfaceHover : behind;
            line = p.border;
            break;
    }
    if (disabled) {
        fill = p.dark ? p.surfaceHover : Mix(p.surfaceHover, p.border, 0.3);
        line = CLR_INVALID;
        ink = p.subtle;
    }
    RECT rc = d->rcItem;
    FillRect(d->hDC, &rc, Brush(behind));
    const float radius = float(Scale(10, dpi));
    FillRound(d->hDC, rc, radius, fill, line);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = rc;
        FillRound(d->hDC, f, radius, CLR_INVALID, p.accent, 2.f, 150);
    }
    RECT tr = rc;
    if (pressed) OffsetRect(&tr, 0, 1);
    Text(d->hDC, label, tr, ink, font, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------------------------
// Toggle switch: label on the left, switch on the right.

inline void DrawToggle(const DRAWITEMSTRUCT* d, HFONT font, int dpi, COLORREF behind) {
    const Palette& p = P();
    const bool on = SendMessageW(d->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;
    const bool hover = Hovered(d->hwndItem);
    RECT rc = d->rcItem;
    FillRect(d->hDC, &rc, Brush(behind));

    const int tw = Scale(40, dpi), th = Scale(22, dpi);
    RECT track = {rc.right - tw, (rc.top + rc.bottom - th) / 2, rc.right, (rc.top + rc.bottom - th) / 2 + th};
    COLORREF tc = on ? (hover ? p.accentHover : p.accent) : (hover ? Mix(p.track, p.text, 0.12) : p.track);
    if (disabled) tc = Mix(tc, behind, 0.55);
    FillRound(d->hDC, track, th / 2.f, tc);
    const float knob = float(th - Scale(6, dpi));
    const float kx = on ? float(track.right - Scale(3, dpi)) - knob : float(track.left + Scale(3, dpi));
    const float ky = float(track.top + Scale(3, dpi));
    FillCircle(d->hDC, kx + knob / 2.f, ky + knob / 2.f + 0.6f, knob / 2.f + 0.5f, RGB(0, 0, 0), 40);  // knob shadow
    FillCircle(d->hDC, kx + knob / 2.f, ky + knob / 2.f, knob / 2.f, RGB(255, 255, 255));
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = track;
        InflateRect(&f, Scale(3, dpi), Scale(3, dpi));
        FillRound(d->hDC, f, (f.bottom - f.top) / 2.f, CLR_INVALID, p.accent, 2.f, 140);
    }

    wchar_t text[160] = {};
    GetWindowTextW(d->hwndItem, text, 160);
    RECT tr = rc;
    tr.right = track.left - Scale(12, dpi);
    Text(d->hDC, text, tr, disabled ? p.subtle : p.text, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------------------------
// Text fields: the parent draws the rounded box; the borderless EDIT sits inside it.

inline void DrawField(HDC dc, const RECT& rc, bool focused, bool hover, int dpi) {
    const Palette& p = P();
    const COLORREF line = focused ? p.accent : hover ? Mix(p.fieldBorder, p.text, 0.18) : p.fieldBorder;
    FillRound(dc, rc, float(Scale(10, dpi)), p.field, line, focused ? 1.6f : 1.f);
    if (focused) {  // a faint outer glow in the accent colour
        RECT g = rc;
        InflateRect(&g, Scale(3, dpi), Scale(3, dpi));
        FillRound(dc, g, float(Scale(13, dpi)), CLR_INVALID, p.accent, 3.f, 40);
    }
}

// WM_CTLCOLOREDIT (and WM_CTLCOLORSTATIC for read-only edits): field colours.
inline LRESULT FieldColors(HDC dc) {
    SetTextColor(dc, P().text);
    SetBkColor(dc, P().field);
    return reinterpret_cast<LRESULT>(Brush(P().field));
}

// WM_CTLCOLORLISTBOX: the open list of a drop-down.
inline LRESULT ListColors(HDC dc) {
    SetTextColor(dc, P().text);
    SetBkColor(dc, P().surface);
    return reinterpret_cast<LRESULT>(Brush(P().surface));
}

// ---------------------------------------------------------------------------------------------
// Drop-downs: owner-drawn combo boxes (CBS_OWNERDRAWFIXED | CBS_HASSTRINGS) whose closed face is painted here.

inline void PaintCombo(HWND h, HDC dc) {
    const Palette& p = P();
    const int dpi = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(h, L"rddpi")));
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, Brush(static_cast<COLORREF>(reinterpret_cast<INT_PTR>(GetPropW(h, L"rdbehind")))));
    HWND focus = GetFocus();
    const bool focused = focus == h || IsChild(h, focus) || SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0);
    DrawField(dc, rc, focused, Hovered(h), dpi);

    const bool editable = (GetWindowLongW(h, GWL_STYLE) & 3) == CBS_DROPDOWN;
    if (!editable) {  // a drop-down list shows its selected item
        const LRESULT sel = SendMessageW(h, CB_GETCURSEL, 0, 0);
        std::wstring s;
        if (sel >= 0) {
            const LRESULT n = SendMessageW(h, CB_GETLBTEXTLEN, sel, 0);
            if (n > 0) {
                s.resize(size_t(n) + 1);
                SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(s.data()));
                s.resize(size_t(n));
            }
        }
        RECT tr = rc;
        tr.left += Scale(12, dpi);
        tr.right -= Scale(36, dpi);
        Text(dc, s, tr, IsWindowEnabled(h) ? p.text : p.subtle, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // Chevron.
    const float cx = float(rc.right - Scale(18, dpi)), cy = float((rc.top + rc.bottom) / 2);
    const float a = float(Scale(4, dpi));
    Line(dc, cx - a, cy - a / 2.f, cx, cy + a / 2.f, p.muted, 1.6f * dpi / 96.f);
    Line(dc, cx, cy + a / 2.f, cx + a, cy - a / 2.f, p.muted, 1.6f * dpi / 96.f);
}

inline LRESULT CALLBACK ComboProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    TrackHover(h, m);
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            PaintCombo(h, mem);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            RemovePropW(h, L"rdhover");
            RemovePropW(h, L"rddpi");
            RemovePropW(h, L"rdbehind");
            RemoveWindowSubclass(h, ComboProc, 1);
            break;
    }
    const LRESULT r = DefSubclassProc(h, m, w, l);
    // The combo box redraws parts of itself outside WM_PAINT on these: paint our face over them at once.
    switch (m) {
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_ENABLE:
        case CB_SETCURSEL: case CB_SHOWDROPDOWN: case WM_KEYDOWN: case WM_MOUSEWHEEL:
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
            break;
    }
    return r;
}

// Call after creating an owner-drawn combo box. `behind` is the colour around its rounded corners.
inline void MakeCombo(HWND h, int dpi, COLORREF behind) {
    SetPropW(h, L"rddpi", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(dpi)));
    SetPropW(h, L"rdbehind", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(behind)));
    SetWindowSubclass(h, ComboProc, 1, 0);
    COMBOBOXINFO ci = {sizeof(ci)};
    if (GetComboBoxInfo(h, &ci)) {
        if (ci.hwndList) ThemeControl(ci.hwndList);
        if (ci.hwndItem && ci.hwndItem != h) {  // editable: move the edit inside the rounded face, with padding
            RECT rc;
            GetClientRect(h, &rc);
            HDC dc = GetDC(ci.hwndItem);
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
            TEXTMETRICW tm = {};
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, old);
            ReleaseDC(ci.hwndItem, dc);
            const int eh = tm.tmHeight + 2;
            MoveWindow(ci.hwndItem, Scale(12, dpi), (rc.bottom - eh) / 2, rc.right - Scale(12, dpi) - Scale(36, dpi), eh, TRUE);
        }
    }
}

// Re-applies the theme to a combo box after a theme change.
inline void RethemeCombo(HWND h, COLORREF behind) {
    SetPropW(h, L"rdbehind", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(behind)));
    COMBOBOXINFO ci = {sizeof(ci)};
    if (GetComboBoxInfo(h, &ci) && ci.hwndList) ThemeControl(ci.hwndList);
    InvalidateRect(h, nullptr, TRUE);
}

// WM_MEASUREITEM for our combo boxes: the closed face and the rows of the open list.
inline void MeasureCombo(MEASUREITEMSTRUCT* m, int faceHeight, int rowHeight) {
    m->itemHeight = m->itemID == UINT(-1) ? UINT(faceHeight) : UINT(rowHeight);
}

// WM_DRAWITEM for a combo box row (the closed face is painted by PaintCombo).
inline void DrawComboItem(const DRAWITEMSTRUCT* d, int dpi) {
    if (d->itemState & ODS_COMBOBOXEDIT) {
        InvalidateRect(d->hwndItem, nullptr, FALSE);
        return;
    }
    const Palette& p = P();
    RECT rc = d->rcItem;
    FillRect(d->hDC, &rc, Brush(p.surface));
    if (d->itemID == UINT(-1)) return;
    if (d->itemState & ODS_SELECTED) {
        RECT s = rc;
        InflateRect(&s, -Scale(4, dpi), -Scale(2, dpi));
        FillRound(d->hDC, s, float(Scale(6, dpi)), p.selection);
    }
    const LRESULT n = SendMessageW(d->hwndItem, CB_GETLBTEXTLEN, d->itemID, 0);
    std::wstring s(n > 0 ? size_t(n) + 1 : 1, L'\0');
    if (n > 0) SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, reinterpret_cast<LPARAM>(s.data()));
    s.resize(n > 0 ? size_t(n) : 0);
    RECT tr = rc;
    tr.left += Scale(12, dpi);
    tr.right -= Scale(8, dpi);
    Text(d->hDC, s, tr, p.text, reinterpret_cast<HFONT>(SendMessageW(d->hwndItem, WM_GETFONT, 0, 0)),
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------------------------
// Status pill: a tinted rounded badge with a coloured dot and text (an SS_OWNERDRAW static).

inline void DrawPill(const DRAWITEMSTRUCT* d, std::wstring text, COLORREF color, HFONT font, int dpi, COLORREF behind) {
    RECT rc = d->rcItem;
    FillRect(d->hDC, &rc, Brush(behind));
    if (!text.empty() && text[0] == L'●') text = text.substr(text.size() > 1 && text[1] == L' ' ? 2 : 1);  // our own dot
    if (text.empty()) return;
    HGDIOBJ old = SelectObject(d->hDC, font);
    SIZE sz = {};
    GetTextExtentPoint32W(d->hDC, text.c_str(), int(text.size()), &sz);
    SelectObject(d->hDC, old);
    const int padL = Scale(26, dpi), padR = Scale(12, dpi);
    RECT pill = rc;
    pill.right = std::min(LONG(rc.left + padL + sz.cx + padR), rc.right);
    FillRound(d->hDC, pill, (pill.bottom - pill.top) / 2.f, Mix(behind, color, P().dark ? 0.18 : 0.11));
    FillCircle(d->hDC, float(pill.left + Scale(14, dpi)), float(pill.top + pill.bottom) / 2.f, Scale(4, dpi) * 1.f, color);
    RECT tr = pill;
    tr.left += padL;
    tr.right -= Scale(8, dpi);
    Text(d->hDC, text, tr, P().dark ? Mix(color, P().text, 0.35) : Mix(color, P().text, 0.3), font,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------------------------
// Theme switch button (top bar): an icon for the current mode and its name.

inline void DrawThemeButton(const DRAWITEMSTRUCT* d, HFONT font, int dpi, COLORREF behind) {
    const Palette& p = P();
    const Mode m = CurrentMode();
    const std::wstring label = m == Mode::System ? L"Auto" : m == Mode::Light ? L"Light" : L"Dark";
    DrawButton(d, ButtonKind::Ghost, L"", font, dpi, behind);
    RECT rc = d->rcItem;
    const float cx = float(rc.left + Scale(22, dpi)), cy = float(rc.top + rc.bottom) / 2.f, r = float(Scale(6, dpi));
    if (m == Mode::Light) {  // sun
        FillCircle(d->hDC, cx, cy, r * 0.7f, p.warning);
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265 / 4;
            Line(d->hDC, cx + float(cos(a)) * r * 1.15f, cy + float(sin(a)) * r * 1.15f, cx + float(cos(a)) * r * 1.6f,
                 cy + float(sin(a)) * r * 1.6f, p.warning, 1.5f * dpi / 96.f);
        }
    } else if (m == Mode::Dark) {  // moon
        FillCircle(d->hDC, cx, cy, r, p.accentHover);
        FillCircle(d->hDC, cx + r * 0.55f, cy - r * 0.45f, r * 0.85f, Hovered(d->hwndItem) ? p.surfaceHover : behind);
    } else {  // auto: half light, half dark
        FillCircle(d->hDC, cx, cy, r, p.text);
        Gdiplus::Graphics g(d->hDC);
        Smooth(g);
        Gdiplus::SolidBrush b(GC(p.surface));
        g.FillPie(&b, cx - r + 1.2f, cy - r + 1.2f, 2 * r - 2.4f, 2 * r - 2.4f, 90.f, 180.f);
    }
    RECT tr = rc;
    tr.left += Scale(36, dpi);
    Text(d->hDC, label, tr, p.text, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

}  // namespace rdui
