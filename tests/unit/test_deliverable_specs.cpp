// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_deliverable_specs.cpp — pure unit test for the Phase-5c deliverable-spec table + evaluator
// (src/deliverable_specs.h): the named specs, name/alias resolution, and the per-metric
// pass/fail judgement analysis.check_deliverable runs. No REAPER, no SDK — all SDK-free logic.

#include <cstdio>
#include <string>

#include "deliverable_specs.h"

using namespace reaper_mcp;
using namespace reaper_mcp::deliverable;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Find a metric's check object in an evaluateSpec result.
static Json metric(const Json& res, const std::string& name) {
    for (const auto& c : res.at("checks"))
        if (c.value("metric", "") == name) return c;
    return Json(nullptr);
}

int main() {
    // ---- 1. findSpec: names + aliases + case-insensitivity ----
    {
        check(findSpec("ebu-r128") != nullptr, "ebu-r128 resolves");
        check(findSpec("broadcast-r128") == findSpec("ebu-r128"), "broadcast-r128 aliases ebu-r128");
        check(findSpec("EBU-R128") == findSpec("ebu-r128"), "spec lookup is case-insensitive");
        check(findSpec("calm-act") == findSpec("atsc-a85"), "calm-act aliases atsc-a85");
        check(findSpec("theatrical") == findSpec("cinema-theatrical"), "theatrical aliases cinema");
        check(findSpec("nope") == nullptr, "unknown spec -> nullptr");
        check(knownSpecs().find("atmos-music") != std::string::npos, "knownSpecs lists atmos-music");
        check((int)specs().size() == 8, "8 named specs");
    }

    // ---- 2. reference numbers ----
    {
        const Spec* a = findSpec("atmos-music");
        check(a->lufsTarget == -18.0 && a->truePeakMaxDb == -1.0, "atmos-music -18 / -1 dBTP");
        const Spec* ss = findSpec("streaming-stereo");
        check(ss->lufsTarget == -14.0 && ss->truePeakMaxDb == -1.0, "streaming-stereo -14 / -1");
        const Spec* ap = findSpec("apple-music-stereo");
        check(ap->lufsTarget == -16.0, "apple-music-stereo -16");
        const Spec* eb = findSpec("ebu-r128");
        check(eb->lufsTarget == -23.0 && eb->lufsTolLU == 0.5 && eb->truePeakMaxDb == -1.0,
              "ebu-r128 -23 ±0.5 / -1");
        const Spec* s1 = findSpec("ebu-r128-s1");
        check(s1->hasShortTermMax && s1->shortTermMaxLufs == -18.0, "ebu-r128-s1 Max-ST <= -18");
        const Spec* at = findSpec("atsc-a85");
        check(at->lufsTarget == -24.0 && at->lufsTolLU == 2.0 && at->truePeakMaxDb == -2.0,
              "atsc-a85 -24 ±2 LK / -2 dBTP (looser TP)");
        const Spec* pod = findSpec("podcast");
        check(pod->hasMono && pod->lufsTarget == -16.0 && pod->lufsTargetMono == -19.0,
              "podcast -16 stereo / -19 mono");
        const Spec* cin = findSpec("cinema-theatrical");
        check(!cin->lufsGated && cin->truePeakMaxDb == -3.0, "cinema-theatrical SPL (not LUFS-gated) / -3");
    }

    // ---- 3. evaluateSpec — integrated LUFS band ----
    {
        const Spec* eb = findSpec("ebu-r128");
        Measured m; m.hasLufs = true; m.lufs = -23.0; m.hasTruePeak = true; m.truePeak = -1.5; m.channels = 2;
        Json r = evaluateSpec(*eb, m);
        check(r.value("pass", false) == true, "ebu-r128 -23.0/-1.5 -> pass");
        check(metric(r, "integratedLufs").value("pass", false) == true, "lufs within ±0.5 passes");

        m.lufs = -22.0;  // delta +1.0 > 0.5
        r = evaluateSpec(*eb, m);
        check(metric(r, "integratedLufs").value("pass", true) == false, "lufs +1.0 over target fails ±0.5");
        check(r.value("pass", true) == false, "over-loud -> overall fail");
        check(metric(r, "integratedLufs").contains("hint"), "over-loud carries a hint");

        // tolerance override widens the band.
        r = evaluateSpec(*eb, m, /*tolOverrideLU=*/1.5);
        check(metric(r, "integratedLufs").value("pass", false) == true, "±1.5 override lets -22.0 pass");
    }

    // ---- 4. evaluateSpec — true-peak ceiling ----
    {
        const Spec* eb = findSpec("ebu-r128");
        Measured m; m.hasLufs = true; m.lufs = -23.2; m.hasTruePeak = true; m.truePeak = -0.5; m.channels = 2;
        Json r = evaluateSpec(*eb, m);  // lufs ok, but TP -0.5 exceeds -1 ceiling
        check(metric(r, "truePeak").value("pass", true) == false, "TP -0.5 exceeds -1 dBTP -> fail");
        check(r.value("pass", true) == false, "TP over ceiling -> overall fail");

        const Spec* at = findSpec("atsc-a85");
        m.lufs = -24.0; m.truePeak = -1.5;  // -1.5 within atsc's looser -2 ceiling? -1.5 > -2 -> fail
        r = evaluateSpec(*at, m);
        check(metric(r, "truePeak").value("pass", true) == false, "atsc TP -1.5 exceeds -2 dBTP -> fail");
        m.truePeak = -2.3;
        r = evaluateSpec(*at, m);
        check(metric(r, "truePeak").value("pass", false) == true, "atsc TP -2.3 under -2 dBTP -> pass");
        check(r.value("pass", false) == true, "atsc -24.0/-2.3 -> overall pass (±2 LK)");
    }

    // ---- 5. cinema-theatrical: LUFS reported, not gated; TP still gates ----
    {
        const Spec* cin = findSpec("cinema-theatrical");
        Measured m; m.hasLufs = true; m.lufs = -30.0; m.hasTruePeak = true; m.truePeak = -3.5; m.channels = 6;
        Json r = evaluateSpec(*cin, m);
        check(metric(r, "integratedLufs").value("evaluated", true) == false, "cinema LUFS not evaluated");
        check(metric(r, "integratedLufs").contains("measured"), "cinema still reports measured LUFS");
        check(r.value("pass", false) == true, "cinema pass rests on TP only (TP -3.5 under -3)");
        m.truePeak = -2.5;
        r = evaluateSpec(*cin, m);
        check(r.value("pass", true) == false, "cinema TP -2.5 over -3 -> fail");
    }

    // ---- 6. ebu-r128-s1: short-term max gate ----
    {
        const Spec* s1 = findSpec("ebu-r128-s1");
        Measured m; m.hasLufs = true; m.lufs = -23.0; m.hasTruePeak = true; m.truePeak = -1.2;
        m.hasShortTermMax = true; m.shortTermMax = -17.0; m.channels = 2;  // -17 > -18 -> fail
        Json r = evaluateSpec(*s1, m);
        check(metric(r, "shortTermMax").value("pass", true) == false, "Max-ST -17 exceeds -18 -> fail");
        check(r.value("pass", true) == false, "s1 overall fails on Max-ST");
        m.shortTermMax = -19.0;
        r = evaluateSpec(*s1, m);
        check(metric(r, "shortTermMax").value("pass", false) == true, "Max-ST -19 under -18 -> pass");
        check(r.value("pass", false) == true, "s1 -23/-1.2/Max-ST -19 -> overall pass");
    }

    // ---- 7. podcast mono vs stereo target selection ----
    {
        const Spec* pod = findSpec("podcast");
        Measured mono; mono.hasLufs = true; mono.lufs = -19.0; mono.hasTruePeak = true; mono.truePeak = -1.5;
        mono.channels = 1;
        check(evaluateSpec(*pod, mono).value("pass", false) == true, "podcast mono -19 -> pass (mono target)");
        Measured st = mono; st.channels = 2;  // -19 vs -16 stereo target -> delta -3 -> fail
        check(metric(evaluateSpec(*pod, st), "integratedLufs").value("pass", true) == false,
              "podcast -19 in stereo fails -16 target");
    }

    // ---- 8. missing metrics don't gate; no-gate -> pass null ----
    {
        const Spec* eb = findSpec("ebu-r128");
        Measured m; m.hasTruePeak = true; m.truePeak = -1.2; m.channels = 2;  // no LUFS measured
        Json r = evaluateSpec(*eb, m);
        check(metric(r, "integratedLufs").value("evaluated", true) == false, "unmeasured LUFS not evaluated");
        check(r.value("pass", false) == true, "TP-only measurement can still pass on TP");

        const Spec* cin = findSpec("cinema-theatrical");
        Measured only; only.hasLufs = true; only.lufs = -28.0; only.channels = 6;  // no TP, spl gate
        Json rc = evaluateSpec(*cin, only);
        check(rc["pass"].is_null(), "cinema with no gated metric -> pass null");
        check(rc.value("evaluatedAnyGate", true) == false, "no gate evaluated flag");
    }

    // ---- 9. Phase-6b: bed-width detection ----
    {
        check(isBedWidth(6) && isBedWidth(8) && isBedWidth(12) && isBedWidth(16) && isBedWidth(24),
              "5.1/7.1/7.1.4/9.1.6/22.2 widths detected as beds");
        check(!isBedWidth(2) && !isBedWidth(4) && !isBedWidth(10) && !isBedWidth(0),
              "stereo / non-standard widths are not beds");
    }

    // ---- 10. Phase-6b: per-bed sub-metering (N measurements -> N evaluations + rollup) ----
    {
        const Spec* atmos = findSpec("atmos-music");   // -18 LUFS ±1 / -1 dBTP
        Measured bedA; bedA.hasLufs = true; bedA.lufs = -18.2; bedA.hasTruePeak = true; bedA.truePeak = -1.4;
        bedA.channels = 12;                             // 7.1.4 bed, conformant
        Measured bedB; bedB.hasLufs = true; bedB.lufs = -12.0; bedB.hasTruePeak = true; bedB.truePeak = -0.3;
        bedB.channels = 12;                             // too loud + over the ceiling -> fails
        Json r = evaluatePerBed(*atmos, {bedA, bedB}, {"Music Bed", "FX Bed"});
        check(r.contains("perBed") && r["perBed"].is_array() && r["perBed"].size() == 2,
              "perBed reports one entry per bed");
        check(r["perBed"][0].value("target", "") == "Music Bed", "per-bed entry carries its label");
        check(r["perBed"][0].value("channels", 0) == 12, "per-bed entry carries channel width");
        check(r["perBed"][0].value("pass", false) == true, "conformant bed passes");
        check(r["perBed"][0]["metrics"].value("truePeak", 0.0) == -1.4, "per-bed metrics surface true peak");
        check(r["perBed"][1].value("pass", true) == false, "hot bed fails");

        const Json& roll = r["rollup"];
        check(roll.value("pass", true) == false, "rollup fails because one bed fails");
        check(roll.value("bedsEvaluated", 0) == 2, "rollup counts beds");
        check(roll["failing"].is_array() && roll["failing"].size() == 1 &&
              roll["failing"][0] == "FX Bed", "rollup names the failing bed");
        check(roll.value("worstTruePeak", -9.0) == -0.3, "rollup worstTruePeak = the highest measured dBTP");
        check(roll.value("worstLufs", 0.0) == -12.0, "rollup worstLufs = the least-conformant integrated LUFS");
    }

    // ---- 11. per-bed rollup: all pass, and label fallback ----
    {
        const Spec* atmos = findSpec("atmos-music");
        Measured a; a.hasLufs = true; a.lufs = -18.0; a.hasTruePeak = true; a.truePeak = -1.5; a.channels = 12;
        Measured b; b.hasLufs = true; b.lufs = -17.5; b.hasTruePeak = true; b.truePeak = -1.2; b.channels = 12;
        Json r = evaluatePerBed(*atmos, {a, b}, {});   // no labels -> "bed 0" / "bed 1"
        check(r["rollup"].value("pass", false) == true, "all-conformant beds -> rollup pass");
        check(r["rollup"]["failing"].empty(), "no failing beds");
        check(r["perBed"][0].value("target", "") == "bed 0", "label falls back to bed index");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL DELIVERABLE-SPEC CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d DELIVERABLE-SPEC CHECK(S) FAILED\n", g_failures);
    return 1;
}
