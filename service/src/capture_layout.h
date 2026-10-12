#pragma once
// Layout math for capturing several screens as one picture: which screens to keep, the size of the
// combined desktop, and the size the encoder receives. No DXGI or Direct3D here, so it can be unit tested.
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace rdcap {

// The largest picture the H.264 encoder is given. A bigger combined desktop is scaled down on the GPU.
constexpr UINT kMaxEncodeW = 3840;
constexpr UINT kMaxEncodeH = 2160;

inline LONG RectW(const RECT& r) { return r.right - r.left; }
inline LONG RectH(const RECT& r) { return r.bottom - r.top; }
inline bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

struct CompositeLayout {
    RECT desktop = {};         // bounding box of the kept screens, in desktop coordinates
    std::vector<size_t> keep;  // indices (into the input) of the screens to capture, in input order
};

// Screens with the same area (a mirrored or cloned pair) are kept once: the second one shows nothing new.
// Screens with no area are skipped.
inline CompositeLayout PlanComposite(const std::vector<RECT>& screens) {
    CompositeLayout out;
    bool first = true;
    for (size_t i = 0; i < screens.size(); ++i) {
        const RECT& r = screens[i];
        if (RectW(r) <= 0 || RectH(r) <= 0) continue;
        bool clone = false;
        for (size_t k : out.keep) {
            if (SameRect(screens[k], r)) {
                clone = true;
                break;
            }
        }
        if (clone) continue;
        out.keep.push_back(i);
        if (first) {
            out.desktop = r;
            first = false;
        } else {
            out.desktop.left = (std::min)(out.desktop.left, r.left);
            out.desktop.top = (std::min)(out.desktop.top, r.top);
            out.desktop.right = (std::max)(out.desktop.right, r.right);
            out.desktop.bottom = (std::max)(out.desktop.bottom, r.bottom);
        }
    }
    return out;
}

// The largest size that fits in maxW x maxH with the same aspect ratio, both sides even (NV12 needs that).
// A size that already fits is only rounded down to even.
inline void FitInside(UINT w, UINT h, UINT maxW, UINT maxH, UINT& ow, UINT& oh) {
    if (w == 0 || h == 0) {
        ow = oh = 0;
        return;
    }
    const double scale = (std::min)(1.0, (std::min)(double(maxW) / w, double(maxH) / h));
    ow = (std::max)(2u, UINT(w * scale) & ~1u);
    oh = (std::max)(2u, UINT(h * scale) & ~1u);
}

}  // namespace rdcap
