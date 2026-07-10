// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_style_palette.cpp — pure unit test for the Phase-5d style palette + resolver
// (src/style_palette.h): the named styles, the in-box-default / detected-plug-in-preference resolution,
// the limiter-ceiling fill from a target true-peak, and the immersive-aware flags (isLimiter/lfeExempt).
// No REAPER, no SDK — all SDK-free logic.

#include <cstdio>
#include <string>
#include <vector>

#include "style_palette.h"

using namespace reaper_mcp;
using namespace reaper_mcp::style;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

static const ResolvedStep* limiterStep(const ResolvedChain& rc) {
    for (const auto& s : rc.steps) if (s.isLimiter) return &s;
    return nullptr;
}
static double limiterCeiling(const ResolvedStep& s) {
    for (const auto& p : s.params) if (p.fromSpecTruePeak) return p.value;
    return 999.0;
}

int main() {
    // ---- 1. findStyle / knownStyles ----
    {
        check(findStyle("pop-bright") != nullptr, "pop-bright resolves");
        check(findStyle("POP-BRIGHT") != nullptr, "style lookup case-insensitive");
        check(findStyle("nope") == nullptr, "unknown style -> nullptr");
        check(knownStyles().find("immersive-master-atmos") != std::string::npos,
              "knownStyles lists the immersive master");
        check(findStyle("immersive-master-atmos")->immersiveAware, "immersive master flagged aware");
        check(!findStyle("pop-bright")->immersiveAware, "pop-bright not immersive-aware");
    }

    // ---- 2. every style ends in a limiter step ----
    {
        for (const auto& s : styles()) {
            bool hasLimiter = false;
            for (const auto& step : s.chain) if (step.isLimiter) hasLimiter = true;
            check(hasLimiter, "style '" + s.name + "' has a limiter step");
        }
    }

    // ---- 3. resolveChain with NOTHING installed -> in-box fallback everywhere ----
    {
        const Style* pb = findStyle("pop-bright");
        ResolvedChain rc = resolveChain(*pb, {}, -1.0);
        for (const auto& s : rc.steps) check(!s.detected, "step '" + s.role + "' falls back to in-box");
        const ResolvedStep* lim = limiterStep(rc);
        check(lim && lim->fx == "ReaLimit", "in-box limiter = ReaLimit");
        check(lim && lim->inboxFx == "ReaLimit", "resolved step carries the in-box fallback name");
        check(lim && limiterCeiling(*lim) == -1.0, "limiter ceiling filled from ceilingDb (-1.0)");
        // first step is the in-box EQ
        check(rc.steps.front().fx == "ReaEQ", "in-box EQ = ReaEQ");
    }

    // ---- 4. resolveChain prefers a DETECTED third-party plug-in ----
    {
        const Style* pb = findStyle("pop-bright");
        std::vector<std::string> installed = {"vst3: pro-l 2 (fabfilter)", "vst: reacomp (cockos)"};
        ResolvedChain rc = resolveChain(*pb, installed, -1.0);
        const ResolvedStep* lim = limiterStep(rc);
        check(lim && lim->detected && lim->fx == "FabFilter Pro-L",
              "detected FabFilter Pro-L preferred over ReaLimit");
        // the comp preferred (FabFilter Pro-C) is NOT installed -> in-box ReaComp
        for (const auto& s : rc.steps)
            if (s.role == "comp") check(!s.detected && s.fx == "ReaComp",
                                        "comp falls back to in-box ReaComp (Pro-C not installed)");
    }

    // ---- 5. limiter ceiling tracks the passed dBTP ----
    {
        const Style* im = findStyle("immersive-master-atmos");
        ResolvedChain rc = resolveChain(*im, {}, -2.0);
        const ResolvedStep* lim = limiterStep(rc);
        check(lim && limiterCeiling(*lim) == -2.0, "ceiling fill uses the resolved dBTP (-2.0)");
        check(rc.immersiveAware, "resolved chain carries immersiveAware");
        check(rc.ceilingDb == -2.0, "resolved chain records ceilingDb");
    }

    // ---- 6. immersive master keeps dynamics off the LFE ----
    {
        const Style* im = findStyle("immersive-master-atmos");
        for (const auto& step : im->chain)
            if (step.role == "multiband" || step.isLimiter)
                check(step.lfeExempt, "immersive '" + step.role + "' is lfeExempt");
    }

    // ---- 7. toJson shape ----
    {
        const Style* pb = findStyle("pop-bright");
        ResolvedChain rc = resolveChain(*pb, {}, -1.0);
        Json j = toJson(rc);
        check(j.value("style", "") == "pop-bright", "toJson carries the style name");
        check(j.contains("steps") && j["steps"].is_array() && !j["steps"].empty(), "toJson steps array");
        check(j["steps"].back().value("isLimiter", false) == true, "last step marked isLimiter in JSON");
        check(j["steps"].back().contains("params"), "step carries params in JSON");
        check(j["steps"].back().contains("multichannel"), "step carries the multichannel flag in JSON");
    }

    // ---- 8. Phase-6c: wide immersive bed -> the bundled multichannel limiter, else stock ----
    {
        check(!mcLimiterAddNames().empty(), "mcLimiterAddNames offers add-name candidates");
        check(mcLimiterAddNames().front().find("mcp_mc_limiter") != std::string::npos,
              "the first add-name candidate names the bundled JSFX");

        const Style* im = findStyle("immersive-master-atmos");
        // 12-ch (7.1.4) immersive bed -> the limiter step swaps to the multichannel JSFX.
        ResolvedChain wide = resolveChain(*im, {}, -1.0, 12);
        const ResolvedStep* wl = limiterStep(wide);
        check(wl && wl->multichannel, "immersive limiter on a 12-ch bed is multichannel");
        check(wl && wl->fx == mcLimiterDisplay(), "multichannel limiter fx = the bundled JSFX display name");
        check(wl && wl->inboxFx == "ReaLimit", "multichannel limiter keeps ReaLimit as the final fallback");
        check(wl && limiterCeiling(*wl) == -1.0, "multichannel limiter still fills the ceiling from dBTP");
        check(wl && wl->lfeExempt, "multichannel limiter stays LFE-exempt");

        // A STEREO immersive target keeps the stock 2-ch limiter (no multichannel swap).
        ResolvedChain narrow = resolveChain(*im, {}, -1.0, 2);
        const ResolvedStep* nl = limiterStep(narrow);
        check(nl && !nl->multichannel && nl->fx == "ReaLimit", "stereo immersive target keeps stock ReaLimit");

        // A NON-immersive style on a wide bed does NOT get the multichannel limiter (immersive-master first).
        const Style* pb = findStyle("pop-bright");
        ResolvedChain wpb = resolveChain(*pb, {}, -1.0, 12);
        const ResolvedStep* pl = limiterStep(wpb);
        check(pl && !pl->multichannel && pl->fx == "ReaLimit",
              "non-immersive style on a wide bed keeps the stock limiter (no MC swap)");

        // toJson surfaces the multichannel flag on the wide-bed limiter step.
        Json jw = toJson(wide);
        bool sawMc = false;
        for (const auto& s : jw["steps"]) if (s.value("multichannel", false)) sawMc = true;
        check(sawMc, "toJson marks the multichannel limiter step");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL STYLE-PALETTE CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d STYLE-PALETTE CHECK(S) FAILED\n", g_failures);
    return 1;
}
