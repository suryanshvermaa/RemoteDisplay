// Unit tests for the multi-screen layout math (service/src/capture_layout.h). No display is needed.
#include <cstdio>

#include "capture_layout.h"

static int fails = 0;

#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) printf("  ok   %s\n", what);                         \
        else { printf("  FAIL %s (line %d)\n", what, __LINE__); ++fails; } \
    } while (0)

static RECT R(LONG l, LONG t, LONG r, LONG b) { return RECT{l, t, r, b}; }

static bool Same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

int main() {
    using namespace rdcap;

    printf("single screen\n");
    {
        auto p = PlanComposite({R(0, 0, 1920, 1080)});
        CHECK(Same(p.desktop, R(0, 0, 1920, 1080)), "desktop is the screen");
        CHECK(p.keep.size() == 1 && p.keep[0] == 0, "the one screen is kept");
    }

    printf("two screens side by side\n");
    {
        auto p = PlanComposite({R(0, 0, 1920, 1080), R(1920, 0, 3840, 1080)});
        CHECK(Same(p.desktop, R(0, 0, 3840, 1080)), "desktop spans both screens");
        CHECK(p.keep.size() == 2, "both screens are kept");
    }

    printf("screen placed left of the primary (negative x)\n");
    {
        auto p = PlanComposite({R(0, 0, 1920, 1080), R(-1280, 0, 0, 1024)});
        CHECK(Same(p.desktop, R(-1280, 0, 1920, 1080)), "desktop starts at the leftmost screen");
    }

    printf("screens stacked vertically with different heights\n");
    {
        auto p = PlanComposite({R(0, 0, 1920, 1080), R(0, 1080, 1280, 1800)});
        CHECK(Same(p.desktop, R(0, 0, 1920, 1800)), "desktop is the bounding box");
    }

    printf("mirrored (cloned) screens are kept once\n");
    {
        auto p = PlanComposite({R(0, 0, 1920, 1080), R(0, 0, 1920, 1080)});
        CHECK(p.keep.size() == 1 && p.keep[0] == 0, "the duplicate is dropped");
        CHECK(Same(p.desktop, R(0, 0, 1920, 1080)), "desktop is the single screen");
    }

    printf("mirror pair plus an extra screen\n");
    {
        auto p = PlanComposite({R(0, 0, 1920, 1080), R(0, 0, 1920, 1080), R(1920, 0, 3840, 1080)});
        CHECK(p.keep.size() == 2 && p.keep[0] == 0 && p.keep[1] == 2, "the mirror is kept once, the extra screen kept");
        CHECK(Same(p.desktop, R(0, 0, 3840, 1080)), "desktop spans the kept screens");
    }

    printf("screens with no area are skipped\n");
    {
        auto p = PlanComposite({R(0, 0, 0, 0), R(0, 0, 1280, 720)});
        CHECK(p.keep.size() == 1 && p.keep[0] == 1, "the empty rectangle is not kept");
        CHECK(Same(p.desktop, R(0, 0, 1280, 720)), "desktop is the real screen");
    }

    printf("no screens\n");
    {
        auto p = PlanComposite({});
        CHECK(p.keep.empty(), "nothing is kept");
    }

    printf("FitInside\n");
    {
        UINT w = 0, h = 0;
        FitInside(1920, 1080, kMaxEncodeW, kMaxEncodeH, w, h);
        CHECK(w == 1920 && h == 1080, "a size that fits is unchanged");
        FitInside(3840, 2160, kMaxEncodeW, kMaxEncodeH, w, h);
        CHECK(w == 3840 && h == 2160, "4K fits exactly");
        FitInside(7680, 2160, kMaxEncodeW, kMaxEncodeH, w, h);
        CHECK(w == 3840 && h == 1080, "three-wide desktop scales to the width limit, aspect kept");
        FitInside(3840, 4320, kMaxEncodeW, kMaxEncodeH, w, h);
        CHECK(w == 1920 && h == 2160, "tall desktop scales to the height limit");
        FitInside(1921, 1081, kMaxEncodeW, kMaxEncodeH, w, h);
        CHECK(w == 1920 && h == 1080, "odd sizes are rounded down to even");
        FitInside(0, 0, kMaxEncodeW, kMaxEncodeH, w, h);
        CHECK(w == 0 && h == 0, "empty size stays empty");
    }

    printf(fails ? "%d FAILED\n" : "all capture layout tests passed\n", fails);
    return fails ? 1 : 0;
}
