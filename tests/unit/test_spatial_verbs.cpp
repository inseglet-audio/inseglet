// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_spatial_verbs.cpp — pure unit test for the Phase-5b spatial composite planning core
// (src/placement_defaults.h + src/spatial_verbs.h): the descriptor->geometry table,
// target parsing (bed layout vs ambisonic order), per-source placement resolution, and the dryRun
// Plan step lists for the three verbs. No REAPER, no SDK — all SDK-free logic.

#include <cstdio>
#include <string>
#include <vector>

#include "spatial_verbs.h"  // pulls placement_defaults.h + composite_support.h

using namespace reaper_mcp;
using namespace reaper_mcp::spatial_verbs;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Count plan steps whose action == a.
static int countAction(const Plan& p, const std::string& a) {
    int n = 0;
    for (const auto& s : p.steps()) if (s.action == a) ++n;
    return n;
}

int main() {
    // ---- 1. placement_defaults: the reference numbers ----
    {
        placement::Placement p;
        check(placement::lookup("front-center", p) && p.az == 0.0 && p.el == 0.0,
              "front-center = 0/0");
        check(placement::lookup("front-left", p) && p.az == 30.0, "front-left = +30 (CCW/left)");
        check(placement::lookup("front-right", p) && p.az == -30.0, "front-right = -30");
        check(placement::lookup("side", p) && p.az == 105.0, "side (bare) defaults to +105 (left)");
        check(placement::lookup("surround-right", p) && p.az == -105.0, "surround-right = -105");
        check(placement::lookup("rear", p) && p.az == 135.0, "rear = +135");
        check(placement::lookup("back-right", p) && p.az == -135.0, "back-right = -135");
        check(placement::lookup("overhead", p) && p.el == 45.0 && p.usesSourceAz,
              "overhead = +45 el, rides source az");
        check(placement::lookup("top", p) && p.az == 0.0 && p.el == 90.0, "top/zenith = 0/+90");
        check(placement::lookup("zenith", p) && p.el == 90.0, "zenith synonym of top");
        check(placement::lookup("lfe", p) && p.lfe, "lfe -> lfe flag");
        check(placement::lookup("sub", p) && p.lfe, "sub is an lfe synonym");
        check(placement::lookup("diffuse", p) && p.diffuse && p.width == placement::Width::Max,
              "diffuse = max width, diffuse flag");
        check(placement::lookup("wide", p) && p.usesSourceAz && p.width == placement::Width::Large,
              "wide rides source az, large width");
        check(!placement::lookup("banana", p), "unknown descriptor rejected");
        // normalization is case/format insensitive
        check(placement::lookup("Front Center", p) && p.az == 0.0, "case + space folding");
        check(placement::lookup("front_left", p) && p.az == 30.0, "underscore folding");
    }

    // ---- 2. parseTarget ----
    {
        Target t;
        check(parseTarget(Json{{"layout", "7.1.4"}}, t).is_null() && !t.ambisonic && t.channels == 12,
              "layout 7.1.4 -> bed, 12 ch");
        check(parseTarget(Json{{"layout", "5.1"}}, t).is_null() && t.channels == 6, "layout 5.1 -> 6 ch");
        check(parseTarget(Json{{"layout", "22.2"}}, t).is_null() && t.channels == 24, "layout 22.2 -> 24 ch");

        Json e = parseTarget(Json{{"layout", "9.9.9"}}, t);
        check(isError(e) && e.value("error", "") == "unknown_layout", "unknown layout -> structured error");

        Target a;
        check(parseTarget(Json{{"ambisonicOrder", 1}}, a).is_null() && a.ambisonic && a.order == 1 &&
              a.channels == 4 && a.normalization == "SN3D", "order 1 -> FOA, 4 ch, SN3D default");
        Target a3;
        check(parseTarget(Json{{"ambisonicOrder", 3}, {"normalization", "N3D"}}, a3).is_null() &&
              a3.channels == 16 && a3.normalization == "N3D", "order 3 -> 16 ch, N3D honored");
        // 2nd order = 9 ch -> even-clamped to 10
        Target a2;
        check(parseTarget(Json{{"ambisonicOrder", 2}}, a2).is_null() && a2.channels == 10,
              "order 2 -> 9 ch even-clamped to 10");

        check(isError(parseTarget(Json{{"ambisonicOrder", 9}}, a)), "order 9 out of range -> error");
        check(isError(parseTarget(Json{{"normalization", "SN3D"}}, a)), "no target key -> error");
        check(isError(parseTarget(Json{{"layout", "7.1.4"}, {"ambisonicOrder", 1}}, a)),
              "both layout + order -> ambiguous error");
        check(isError(parseTarget(Json("nope"), a)), "non-object target -> error");
    }

    // ---- 3. resolvePlacementSpec ----
    {
        ResolvedPlacement r;
        check(resolvePlacementSpec(Json("front-center"), 0.0, r).is_null() && r.az == 0 && r.el == 0,
              "descriptor string front-center");
        check(resolvePlacementSpec(Json("overhead"), 60.0, r).is_null() && r.el == 45 && r.az == 60,
              "overhead substitutes the source's intrinsic az (60)");
        // explicit numeric wins
        check(resolvePlacementSpec(Json{{"az", 30}, {"el", 10}}, 0.0, r).is_null() &&
              r.az == 30 && r.el == 10 && r.explicitNumeric, "explicit numeric az/el");
        // descriptor base + explicit override
        check(resolvePlacementSpec(Json{{"descriptor", "side-right"}}, 0.0, r).is_null() && r.az == -105,
              "object with descriptor side-right = -105");
        check(resolvePlacementSpec(Json{{"descriptor", "front-center"}, {"az", 12.5}}, 0.0, r).is_null() &&
              r.az == 12.5, "explicit az overrides descriptor base");
        check(resolvePlacementSpec(Json{{"lfe", true}}, 0.0, r).is_null() && r.lfe, "explicit lfe flag");
        check(isError(resolvePlacementSpec(Json("banana"), 0.0, r)), "unknown descriptor -> error");
        check(isError(resolvePlacementSpec(Json(42), 0.0, r)), "numeric placement -> error");
    }

    // ---- 4. planSpatialize (bed target) ----
    {
        Target t; parseTarget(Json{{"layout", "7.1.4"}}, t);
        std::vector<SourceRef> srcs = {{3, "Drums", 2}, {4, "Vocal", 1}, {5, "Sub", 1}};
        std::vector<ResolvedPlacement> pl(3);
        resolvePlacementSpec(Json("wide"), 0.0, pl[0]);
        resolvePlacementSpec(Json("front-center"), 0.0, pl[1]);
        resolvePlacementSpec(Json("lfe"), 0.0, pl[2]);

        Plan p = planSpatialize(t, srcs, pl, "7.1.4 Bed", /*monitor=*/true, /*busExists=*/false);
        check(countAction(p, "build_bed") == 1, "bed target builds one bed");
        check(countAction(p, "insert_panner") == 2, "two non-LFE sources get a panner");
        check(countAction(p, "create_send") == 3, "every source gets a send (incl. the LFE mono send)");
        check(countAction(p, "add_binaural_monitor") == 1, "monitor step present when requested");
        // idempotent re-run: bus already exists -> reuse, no second build
        Plan p2 = planSpatialize(t, srcs, pl, "7.1.4 Bed", false, /*busExists=*/true);
        check(countAction(p2, "build_bed") == 0 && countAction(p2, "reuse_bus") == 1,
              "existing bed is reused, not rebuilt (idempotent)");
        check(countAction(p2, "add_binaural_monitor") == 0, "no monitor step when not requested");
    }

    // ---- 5. planSpatialize (ambisonic target) ----
    {
        Target t; parseTarget(Json{{"ambisonicOrder", 3}}, t);
        std::vector<SourceRef> srcs = {{2, "Pad", 2}, {6, "FX", 2}};
        std::vector<ResolvedPlacement> pl(2);
        resolvePlacementSpec(Json("overhead"), 0.0, pl[0]);
        resolvePlacementSpec(Json{{"az", -90}, {"el", 0}}, 0.0, pl[1]);
        Plan p = planSpatialize(t, srcs, pl, "3rd-order Scene", false, false);
        check(countAction(p, "create_ambisonic_bus") == 1, "ambisonic target creates a scene bus");
        check(countAction(p, "insert_encoder") == 2, "each source gets an encoder");
        check(countAction(p, "set_channels") == 2, "each source is set to the HOA channel count");
        check(countAction(p, "create_send") == 2, "each source sends into the scene bus");
    }

    // ---- 6. planStereoToAmbi ----
    {
        SourceRef s{1, "Mix", 2};
        Plan p = planStereoToAmbi(s, 1, 4, "SN3D", 30.0, true);
        check(countAction(p, "set_channels") == 1 && countAction(p, "insert_encoder") == 1 &&
              countAction(p, "set_param") == 1 && countAction(p, "add_binaural_monitor") == 1,
              "stereo_to_ambisonic plan: channels + encoder + L/R spread + monitor");
    }

    // ---- 7. planSetupImmersive ----
    {
        Plan p = planSetupImmersive("7.1.4", 12, 4, /*monitor=*/true, /*renderer=*/true);
        check(countAction(p, "build_bed") == 1, "one bed");
        check(countAction(p, "add_object_track") == 4, "four object tracks");
        check(countAction(p, "add_binaural_monitor") == 1, "monitor");
        check(countAction(p, "atmos_send_layout") == 1, "renderer send layout");
        Plan p2 = planSetupImmersive("5.1", 6, 0, false, false);
        check(countAction(p2, "add_object_track") == 0 && countAction(p2, "atmos_send_layout") == 0,
              "no objects / no renderer when not requested");
    }

    // ---- 8. dryRunResult envelope shape (Plan reused from composite_support) ----
    {
        Target t; parseTarget(Json{{"layout", "5.1"}}, t);
        std::vector<SourceRef> srcs = {{0, "A", 2}};
        std::vector<ResolvedPlacement> pl(1);
        resolvePlacementSpec(Json("front-center"), 0.0, pl[0]);
        Plan p = planSpatialize(t, srcs, pl, "5.1 Bed", false, false);
        Json env = p.dryRunResult();
        check(env.value("dryRun", false) == true, "dryRunResult marks dryRun:true");
        check(env.value("stepCount", 0) == (int)p.size(), "stepCount matches plan size");
        check(env.contains("plan") && env["plan"].is_array(), "plan array present");
        check(env.contains("diff") && env["diff"].is_string() && !env["diff"].get<std::string>().empty(),
              "human diff present");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL SPATIAL-VERB CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d SPATIAL-VERB CHECK(S) FAILED\n", g_failures);
    return 1;
}
