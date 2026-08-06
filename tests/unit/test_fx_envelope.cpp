// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_fx_envelope.cpp — unit test for the doc-136 FX-param envelope addressing core
// (src/fx_envelope.h). No REAPER, no SDK.
//
// The negative control REPRODUCES the defect the tiered resolver exists to kill: the naive
// "first substring wins" rule, applied to the IEM StereoEncoder roster with a request of
// "Angle", returns AZIMUTH — so a caller meaning elevation would get a successful write on the
// wrong axis. That is the same live-observed shape as D3's letter scan (doc 125/126) and B4's
// out-of-domain arg-max (doc 135), and if this control ever stops firing the roster no longer
// exercises the defect and the suite must say so.

#include <cstdio>
#include <string>
#include <vector>

#include "fx_envelope.h"

using namespace reaper_mcp::fxenv;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// The naive rule, verbatim: first roster entry containing the request (case-insensitively)
// wins, ambiguity unexamined. Kept here ONLY as the negative control.
static int naiveFirstSubstring(const std::vector<std::string>& roster, const std::string& req) {
    const std::string lreq = lower(req);
    for (int i = 0; i < (int)roster.size(); ++i)
        if (lower(roster[(std::size_t)i]).find(lreq) != std::string::npos) return i;
    return -1;
}

int main() {
    // The IEM StereoEncoder roster (VST3, live-observed doc 125), in param-index order.
    const std::vector<std::string> iem = {
        "Azimuth Angle",    // 0
        "Elevation Angle",  // 1
        "Roll",             // 2
        "Width",            // 3
        "Quaternion W",     // 4
        "Quaternion X",     // 5
        "Quaternion Y",     // 6
        "Quaternion Z",     // 7
        "High Quality",     // 8
    };

    std::fprintf(stderr, "\n-- 1. NEGATIVE CONTROL: the naive rule silently picks the wrong axis --\n");
    {
        // "Angle" matches both axis params. The naive rule returns azimuth with no signal.
        check(naiveFirstSubstring(iem, "Angle") == 0,
              "negative control: naive first-substring 'Angle' -> Azimuth Angle (0), silently");
        // The same request through the tiered resolver REFUSES.
        const ParamMatch m = resolveParam(iem, "Angle");
        check(m.ambiguous, "'Angle' is reported AMBIGUOUS, not resolved");
        check(m.index == -1, "'Angle' yields index -1 — the resolver does not guess");
        check(m.candidateCount == 2, "'Angle' names exactly 2 candidates");
        check(m.candidates.size() == 2 && m.candidates[0] == 0 && m.candidates[1] == 1,
              "'Angle' candidates are Azimuth(0) and Elevation(1) — the error can name them");
        check(m.kind == MatchKind::Substring, "'Angle' resolved at the substring tier");
    }

    std::fprintf(stderr, "\n-- 2. Tiers do not mix: exact beats a lower-indexed substring --\n");
    {
        // "Elevation Angle" is exact at index 1; "Azimuth Angle" also contains "Angle" and
        // sits at a LOWER index. A single-pass scan would have to be told to prefer exact;
        // the tier rule gets it for free.
        const ParamMatch m = resolveParam(iem, "Elevation Angle");
        check(m.index == 1, "exact 'Elevation Angle' -> 1, despite index 0 also matching 'Angle'");
        check(m.kind == MatchKind::Exact, "matched at the exact tier");
        check(!m.ambiguous, "exact match is unambiguous");
        check(m.candidateCount == 1, "exactly one exact candidate");
    }

    std::fprintf(stderr, "\n-- 3. Case-insensitive equality is its own tier, above substring --\n");
    {
        const ParamMatch m = resolveParam(iem, "azimuth angle");
        check(m.index == 0, "'azimuth angle' -> 0");
        check(m.kind == MatchKind::CaseInsensitive, "matched case-insensitively, NOT as substring");
        check(!m.ambiguous, "unambiguous");
    }
    {
        // A request that is a case-variant of one name and a substring of nothing else must
        // not be demoted to the substring tier.
        const ParamMatch m = resolveParam(iem, "HIGH QUALITY");
        check(m.index == 8 && m.kind == MatchKind::CaseInsensitive, "'HIGH QUALITY' -> 8, ci tier");
    }

    std::fprintf(stderr, "\n-- 4. Unique substring resolves; no match reports None --\n");
    {
        const ParamMatch m = resolveParam(iem, "azim");
        check(m.index == 0 && m.kind == MatchKind::Substring, "'azim' -> 0 at the substring tier");
        check(!m.ambiguous, "'azim' is unique on this roster");
    }
    {
        const ParamMatch m = resolveParam(iem, "Distance");
        check(m.index == -1, "'Distance' is absent -> -1");
        check(m.kind == MatchKind::None, "kind is None");
        check(m.candidateCount == 0, "no candidates");
        check(!m.ambiguous, "absent is NOT ambiguous — the two states are distinguishable");
    }
    {
        // Empty request must not substring-match everything.
        const ParamMatch m = resolveParam(iem, "");
        check(m.index == -1 && m.kind == MatchKind::None,
              "empty request matches nothing (not every param)");
    }
    {
        const ParamMatch m = resolveParam({}, "Azimuth Angle");
        check(m.index == -1 && m.kind == MatchKind::None, "empty roster -> None, no crash");
    }

    std::fprintf(stderr, "\n-- 5. Ambiguity at the EXACT tier is possible and is reported --\n");
    {
        // REAPER does not forbid an FX exposing the same param name twice.
        const std::vector<std::string> dup = {"Gain", "Gain", "Mix"};
        const ParamMatch m = resolveParam(dup, "Gain");
        check(m.kind == MatchKind::Exact, "exact tier reached");
        check(m.ambiguous && m.index == -1, "duplicate EXACT names are ambiguous, not resolved");
        check(m.candidateCount == 2, "both duplicates reported");
    }

    std::fprintf(stderr, "\n-- 6. E-136.5: the envelope-NAME collision that makes add_point unsafe --\n");
    {
        // Two IEM encoders on one track: GetTrackEnvelopeByName("Azimuth Angle") returns ONE
        // of them, and add_point writes to whichever, reporting ok:true.
        const std::vector<std::string> twoEncoders = {
            "Azimuth Angle", "Elevation Angle",   // fx 0
            "Azimuth Angle", "Elevation Angle",   // fx 1
        };
        check(nameCollisionCount(twoEncoders, "Azimuth Angle") == 2,
              "two same-named envelopes on one track are counted");
        check(!nameSeamSafe(twoEncoders, "Azimuth Angle"),
              "the by-name seam is UNSAFE at count 2 — the verb must report, not pick");
        check(nameSeamSafe(twoEncoders, "Roll") == false,
              "count 0 is ALSO unsafe — an envelope that never became name-visible");
        const std::vector<std::string> one = {"Azimuth Angle", "Volume"};
        check(nameSeamSafe(one, "Azimuth Angle"),
              "count 1 is the only safe state");
        check(nameCollisionCount({}, "Azimuth Angle") == 0, "empty envelope list -> 0");
    }

    std::fprintf(stderr, "\n-- 7. matchKindName is total (every enumerator has a string) --\n");
    {
        check(std::string(matchKindName(MatchKind::None)) == "none", "None -> 'none'");
        check(std::string(matchKindName(MatchKind::Exact)) == "exact", "Exact -> 'exact'");
        check(std::string(matchKindName(MatchKind::CaseInsensitive)) == "case-insensitive",
              "CaseInsensitive -> 'case-insensitive'");
        check(std::string(matchKindName(MatchKind::Substring)) == "substring",
              "Substring -> 'substring'");
    }

    if (g_failures) {
        std::fprintf(stderr, "\nunit.fx_envelope: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nunit.fx_envelope: all checks passed\n");
    return 0;
}
