// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_position_params.cpp — unit test for the D3 representation-aware positional
// param discovery + angle composition core (src/position_params.h). No REAPER, no SDK.
//
// The negative control REPRODUCES the pre-D3 defect in-test: the old bare-letter scan,
// applied to the IEM StereoEncoder roster, picks "Quaternion X" — the exact live-observed
// mis-map the classifier exists to kill (E-126.1). If that control ever stops firing, the
// roster no longer exercises the defect and the suite must say so.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "position_params.h"

using namespace reaper_mcp::posparam;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) < tol; }

// The pre-D3 rule, verbatim (set_source_position's old discover lambda, minus the explicit
// override): first name containing the letter is the source-0 fallback; a name also
// containing the 1-based source token wins. Kept here ONLY as the negative control.
static int preD3LetterScan(const std::vector<std::string>& lowerNames, char axis, int source) {
    const std::string want(1, axis);
    const std::string srcTok = std::to_string(source + 1);
    int fallback = -1;
    for (int p = 0; p < (int)lowerNames.size(); ++p) {
        const std::string& nm = lowerNames[p];
        if (nm.find(want) == std::string::npos) continue;
        if (source == 0 && fallback < 0) fallback = p;
        if (nm.find(srcTok) != std::string::npos) return p;
    }
    return fallback;
}

int main() {
    // The IEM StereoEncoder roster (VST3, live-observed), lowercased in param order.
    const std::vector<std::string> iem = {
        "azimuth angle",   // 0
        "elevation angle", // 1
        "roll",            // 2
        "width",           // 3
        "quaternion w",    // 4
        "quaternion x",    // 5
        "quaternion y",    // 6
        "quaternion z",    // 7
        "high quality",    // 8
    };

    // ---- 1. E-126.1 negative control: the OLD rule reproduces (and exceeds) the live-observed
    //      defect. 'x'/'y' land on quaternion components as live-observed — and 'z' never
    //      even reaches Quaternion Z: it hits "a-Z-imuth angle" first, so a pre-D3 height
    //      write silently REWROTE AZIMUTH. (Found by this test; the live session had recorded only
    //      the quaternion half. E-126.1's "z -> Quaternion Z (7)" expectation was refuted
    //      in the worse direction.) ----
    {
        check(preD3LetterScan(iem, 'x', 0) == 5, "negative control: pre-D3 'x' scan hits Quaternion X (5)");
        check(preD3LetterScan(iem, 'y', 0) == 6, "negative control: pre-D3 'y' scan hits Quaternion Y (6)");
        check(preD3LetterScan(iem, 'z', 0) == 0, "negative control: pre-D3 'z' scan hits aZimuth angle (0)");
    }

    // ---- 2. E-126.1: the classifier on the IEM roster — az/el-first, letter scan not run
    //      (single letters collide with angle names themselves: 'z' ⊂ azimuth, 'y' ⊂ quality;
    //      representation choice is all-or-nothing) ----
    {
        const Discovery d = discover(iem, 0);
        check(d.azElFirst(), "IEM: azElFirst");
        check(d.azP == 0, "IEM: azimuth angle at 0");
        check(d.elP == 1, "IEM: elevation angle at 1");
        check(d.distP == -1, "IEM: no distance param");
        check(d.xP == -1 && d.yP == -1 && d.zP == -1, "IEM: letter scan not run on an angle roster");
        check(!d.skippedQuaternion, "IEM: nothing scanned, nothing skipped");
    }

    // ---- 2b. Quaternion-ONLY roster (no angle params): the letter scan runs, refuses the
    //      quaternions, and REPORTS the refusal so the tool can explain itself ----
    {
        const std::vector<std::string> quatOnly = {"quaternion w", "quaternion x",
                                                   "quaternion y", "quaternion z", "gain"};
        const Discovery d = discover(quatOnly, 0);
        check(!d.azElFirst(), "quat-only: no angle representation");
        check(d.xP == -1 && d.yP == -1 && d.zP == -1, "quat-only: quaternions refused");
        check(d.skippedQuaternion, "quat-only: skippedQuaternion reported");
    }

    // ---- 3. E-126.5: ReaSurroundPan-style roster — old behavior preserved exactly ----
    {
        const std::vector<std::string> rsp = {
            "in 1 x", "in 1 y", "in 1 z",   // 0..2
            "in 2 x", "in 2 y", "in 2 z",   // 3..5
            "lfe mix", "smoothing",         // 6..7
        };
        const Discovery d0 = discover(rsp, 0);
        check(!d0.azElFirst(), "RSP: cartesian representation");
        check(d0.xP == 0 && d0.yP == 1 && d0.zP == 2, "RSP source 0: in 1 x/y/z");
        check(d0.xP == preD3LetterScan(rsp, 'x', 0) &&
              d0.yP == preD3LetterScan(rsp, 'y', 0) &&
              d0.zP == preD3LetterScan(rsp, 'z', 0),
              "RSP source 0: classifier == pre-D3 scan (regression pin)");
        check(!d0.skippedQuaternion, "RSP: nothing skipped");

        const Discovery d1 = discover(rsp, 1);
        check(d1.xP == 3 && d1.yP == 4 && d1.zP == 5, "RSP source 1: in 2 x/y/z (token preference)");
        check(d1.xP == preD3LetterScan(rsp, 'x', 1), "RSP source 1: classifier == pre-D3 scan");

        // Strict multi-source contract: source > 0 with no numbered match -> -1 (never
        // silently drives source 1) — the tool's pre-D3 behavior, preserved.
        const std::vector<std::string> bare = {"pos x", "pos y", "pos z"};
        const Discovery d2 = discover(bare, 2);
        check(d2.xP == -1 && d2.yP == -1 && d2.zP == -1, "strict source: no numbered match -> -1");
        check(discover(bare, 0).xP == 0, "strict source: source 0 falls back to first match");
    }

    // ---- 4. MultiEncoder-style numbered angle roster ----
    {
        const std::vector<std::string> multi = {
            "azimuth 1", "elevation 1",   // 0..1
            "azimuth 2", "elevation 2",   // 2..3
            "azimuth 3", "elevation 3",   // 4..5
            "master azimuth",             // 6
        };
        check(discover(multi, 0).azP == 0, "MultiEncoder source 0: azimuth 1");
        check(discover(multi, 2).azP == 4, "MultiEncoder source 2: azimuth 3");
        check(discover(multi, 2).elP == 5, "MultiEncoder source 2: elevation 3");
    }

    // ---- 5. E-126.2: composeTarget — direct angle args win verbatim ----
    {
        const TargetAngles t = composeTarget(true, 47.5, true, -12.25,
                                             true, 0.9, true, 0.1, true, 0.5, 0.0);
        check(t.writeAz && near(t.azDeg, 47.5), "direct azimuthDeg wins verbatim");
        check(t.writeEl && near(t.elDeg, -12.25), "direct elevationDeg wins verbatim");
    }

    // ---- 6. E-126.2: cartesian -> angle inverses of the tool's forward map ----
    {
        // Forward map: x = -sin(az)cos(el), y = cos(az)cos(el), z = sin(el); +az = left.
        auto roundTrip = [&](double azDeg, double elDeg) {
            const double az = azDeg / kDegPerRad, el = elDeg / kDegPerRad;
            const double x = -std::sin(az) * std::cos(el);
            const double y = std::cos(az) * std::cos(el);
            const double z = std::sin(el);
            const TargetAngles t = composeTarget(false, 0, false, 0,
                                                 true, x, true, y, true, z, 0.0);
            check(t.writeAz && near(t.azDeg, azDeg, 1e-9),
                  "round trip az " + std::to_string(azDeg));
            check(t.writeEl && near(t.elDeg, elDeg, 1e-9),
                  "round trip el " + std::to_string(elDeg));
        };
        roundTrip(0.0, 0.0);
        roundTrip(90.0, 0.0);     // hard left
        roundTrip(-30.0, 15.0);   // front-right, raised
        roundTrip(135.0, -20.0);  // rear-left, lowered
        roundTrip(180.0, 0.0);    // dead rear

        // x = +1 (right) alone: az = atan2(-1, cur-dir). At current az 0 (dir (0,1)):
        // atan2(-1, 1) = -45 is WRONG to expect — the missing y comes from the unit
        // direction, cos(0) = 1 -> az = atan2(-1, 1) = -45. Requested semantics: a
        // half-right nudge from front. (The pre-D3 cartesian path had no defined angle
        // semantics at all; this pins the composition rule.)
        const TargetAngles tx = composeTarget(false, 0, false, 0,
                                              true, 1.0, false, 0, false, 0, 0.0);
        check(tx.writeAz && near(tx.azDeg, -45.0), "x=+1 from front: az -45 (right of front)");
        check(!tx.writeEl, "x-only never writes elevation");

        // Same x = +1 nudge with the source currently at hard left (+90, dir (-1, 0)):
        // az = atan2(-1, 0) = -90... composed against the CURRENT direction, not front.
        const TargetAngles tx90 = composeTarget(false, 0, false, 0,
                                                true, 1.0, false, 0, false, 0, 90.0);
        check(tx90.writeAz && near(tx90.azDeg, -90.0, 1e-9),
              "x=+1 at cur az +90: horizontal member from current direction");
    }

    // ---- 7. E-126.2: partial writes keep their meaning ----
    {
        const TargetAngles tz = composeTarget(false, 0, false, 0,
                                              false, 0, false, 0, true, 0.5, 123.0);
        check(!tz.writeAz, "z-only never recenters azimuth");
        check(tz.writeEl && near(tz.elDeg, 30.0, 1e-9), "z=0.5 -> el 30 (asin)");

        const TargetAngles tclamp = composeTarget(false, 0, false, 0,
                                                  false, 0, false, 0, true, 1.75, 0.0);
        check(near(tclamp.elDeg, 90.0, 1e-9), "z clamps to +90");
        const TargetAngles tclampn = composeTarget(false, 0, false, 0,
                                                   false, 0, false, 0, true, -2.0, 0.0);
        check(near(tclampn.elDeg, -90.0, 1e-9), "z clamps to -90");

        const TargetAngles tnone = composeTarget(false, 0, false, 0,
                                                 false, 0, false, 0, false, 0, 45.0);
        check(!tnone.writeAz && !tnone.writeEl, "empty request writes nothing");

        // azimuthDeg alone: elevation untouched (the arg conversion's old el=0 default
        // must NOT leak into the angle path).
        const TargetAngles taz = composeTarget(true, 60.0, false, 0,
                                               false, 0, false, 0, false, 0, 0.0);
        check(taz.writeAz && !taz.writeEl, "azimuthDeg alone never flattens elevation");
    }

    if (g_failures) { std::fprintf(stderr, "%d FAILURE(S)\n", g_failures); return 1; }
    std::fprintf(stderr, "all position_params checks passed\n");
    return 0;
}
