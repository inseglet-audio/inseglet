// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_groove.cpp — pure unit test for the Batch B2 groove/quantize math (src/groove.h): quantize
// snapping + swing, velocity scaling, transpose bounds, stretch, groove-template extract/apply, legato,
// and the deterministic PRNG behind midi.humanize. No REAPER, no SDK — every number is deterministic.

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "groove.h"

using namespace reaper_mcp::groove;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

int main() {
    // ---- quantizePos ----
    check(near(quantizePos(0.24, 0.25, 1.0, 0.0), 0.25), "quantize full snaps 0.24 -> 0.25");
    check(near(quantizePos(0.26, 0.25, 1.0, 0.0), 0.25), "quantize full snaps 0.26 -> 0.25");
    check(near(quantizePos(0.30, 0.25, 0.0, 0.0), 0.30), "quantize strength 0 leaves position");
    check(near(quantizePos(0.20, 0.25, 0.5, 0.0), 0.225), "quantize strength 0.5 moves halfway to grid");
    check(near(quantizePos(0.51, 0.25, 1.0, 0.0), 0.50), "quantize snaps to nearest (even) slot 0.50");
    // swing pushes ODD grid slots later; slot 1 (0.25) -> 0.25 + 0.5*0.25 = 0.375
    check(near(quantizePos(0.24, 0.25, 1.0, 0.5), 0.375), "swing 0.5 pushes odd slot to 0.375");
    check(near(quantizePos(0.01, 0.25, 1.0, 0.5), 0.0), "swing leaves even slot 0 in place");
    check(near(quantizePos(0.3, 0.0, 1.0, 0.0), 0.3), "quantize grid<=0 is a no-op");

    // ---- scaleVelocity ----
    check(scaleVelocity(100, 1.0, 0.0, 64, 1, 127) == 100, "scaleVelocity identity");
    check(scaleVelocity(64, 2.0, 0.0, 64, 1, 127) == 64, "scaleVelocity keeps the center fixed");
    check(scaleVelocity(100, 0.5, 0.0, 64, 1, 127) == 82, "scaleVelocity compresses toward center");
    check(scaleVelocity(120, 2.0, 0.0, 64, 1, 127) == 127, "scaleVelocity clamps to hi");
    check(scaleVelocity(10, 1.0, -50.0, 64, 1, 127) == 1, "scaleVelocity clamps to lo");

    // ---- transposePitch ----
    int np = -1;
    check(transposePitch(60, 12, np) && np == 72, "transpose +12 -> 72");
    check(transposePitch(60, -60, np) && np == 0, "transpose down to 0 is in range");
    check(!transposePitch(120, 12, np), "transpose past 127 is rejected");
    check(!transposePitch(5, -6, np), "transpose below 0 is rejected");

    // ---- stretchPos ----
    check(near(stretchPos(2.0, 1.0, 2.0), 3.0), "stretch x2 around anchor 1: 2 -> 3");
    check(near(stretchPos(1.0, 1.0, 5.0), 1.0), "stretch leaves the anchor fixed");

    // ---- grooveSlot wrap ----
    check(grooveSlot(0.0, 0.25, 2) == 0 && grooveSlot(0.25, 0.25, 2) == 1 &&
          grooveSlot(0.5, 0.25, 2) == 0, "grooveSlot wraps every nSteps");

    // ---- extract + apply round-trip ----
    // A reference where every ODD 1/16 slot is 0.05 beats late and 20% louder; extract should recover it.
    std::vector<std::pair<double, int>> ref = {
        {0.00, 96}, {0.30, 115}, {0.50, 96}, {0.80, 115}, {1.00, 96}, {1.30, 115}};
    auto tmpl = extractGroove(ref, 0.25, 2, 96);
    check(tmpl.size() == 2, "extractGroove returns nSteps slots");
    check(near(tmpl[0].timeOffset, 0.0, 1e-6), "slot 0 has ~0 timing offset");
    check(near(tmpl[1].timeOffset, 0.05, 1e-6), "slot 1 recovers the +0.05 beat push");
    check(near(tmpl[1].velScale, 115.0 / 96.0, 1e-6), "slot 1 recovers the velocity ratio");
    double ob; int ov;
    applyGroove(0.25, 96, 0.25, tmpl, 1.0, ob, ov);   // on-grid odd slot -> gets the offset + louder
    check(near(ob, 0.30, 1e-6), "applyGroove stamps slot-1 timing (0.25 -> 0.30)");
    check(ov == 115, "applyGroove stamps slot-1 velocity (96 -> 115)");
    applyGroove(0.25, 96, 0.25, tmpl, 0.0, ob, ov);   // strength 0 = untouched
    check(near(ob, 0.25, 1e-9) && ov == 96, "applyGroove strength 0 is a no-op");

    // ---- legatoLengths ----
    std::vector<std::pair<double, double>> notes = {{0.0, 0.1}, {1.0, 0.1}, {2.0, 0.1}};
    auto lens = legatoLengths(notes, 0.0, 0.03125);
    check(near(lens[0], 1.0) && near(lens[1], 1.0), "legato extends each note to the next start");
    check(near(lens[2], 0.1), "legato leaves the last note unchanged");
    auto lensGap = legatoLengths(notes, 0.25, 0.03125);
    check(near(lensGap[0], 0.75), "legato honors the gap");
    std::vector<std::pair<double, double>> tight = {{0.0, 0.5}, {0.5, 0.5}};
    auto lensMin = legatoLengths({{0.0, 0.5}, {0.02, 0.5}}, 0.1, 0.03125);
    check(near(lensMin[0], 0.03125), "legato floors at minLength when notes overlap");
    (void)tight;

    // ---- Rng determinism ----
    Rng r1(42), r2(42);
    check(r1.next() == r2.next() && r1.next() == r2.next(), "Rng is deterministic for a given seed");
    Rng r3(1), r4(2);
    check(r3.next() != r4.next(), "different seeds diverge");
    Rng rs(7);
    bool inRange = true;
    for (int i = 0; i < 1000; ++i) { double s = rs.sym(); if (s < -1.0 || s >= 1.0) inRange = false; }
    check(inRange, "Rng.sym() stays within [-1,1)");

    if (g_failures == 0) std::fprintf(stderr, "\nALL GROOVE UNIT TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d GROOVE UNIT TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
