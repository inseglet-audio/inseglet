// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_decode_timeline.cpp — pure unit test for the Batch T1 per-object decode-coverage timeline math
// (src/decode_timeline.h): the az/el -> direction map, the SN3D encode -> projection-decode per-speaker
// energy (a hard-left object lands on the left speakers, a height-panned object on the top layer), and
// the timeline aggregation (dominant-speaker migration, dwell fractions, angular travel, footprint).
// No REAPER, no SDK — every number is deterministic.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "decode_timeline.h"

using namespace reaper_mcp;
using reaper_mcp::decode_timeline::Sample;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// A 7.1.4 loudspeaker set built exactly like analysis.decode_coverage's namedLayoutDirs table:
// azimuth is +RIGHT (clockwise-from-front), elevation +up; Dir = {sin(az)cos(el), cos(az)cos(el), sin(el)}.
struct Spk { const char* label; double azRight, el; };
static std::vector<meter::Dir> g_dirs;
static std::vector<std::string> g_labels;
static void build714() {
    const std::vector<Spk> spk = {
        {"L", -30, 0}, {"R", 30, 0}, {"C", 0, 0}, {"Lss", -90, 0}, {"Rss", 90, 0},
        {"Lrs", -150, 0}, {"Rrs", 150, 0},
        {"Ltf", -45, 45}, {"Rtf", 45, 45}, {"Ltr", -135, 45}, {"Rtr", 135, 45}};
    const double pi = std::acos(-1.0);
    for (const auto& s : spk) {
        const double a = s.azRight * pi / 180.0, e = s.el * pi / 180.0;
        g_dirs.push_back(meter::Dir{std::sin(a) * std::cos(e), std::cos(a) * std::cos(e), std::sin(e)});
        g_labels.push_back(s.label);
    }
}
static std::string domLabel(const decode_timeline::Slice& sl) {
    return (sl.dominantIdx >= 0 && sl.dominantIdx < (int)g_labels.size()) ? g_labels[sl.dominantIdx] : "?";
}

int main() {
    build714();
    const int order = 3;

    // ---- dirFromAzEl round-trips through meter::vecToAzEl (object az is +LEFT) ----
    {
        meter::Dir d = decode_timeline::dirFromAzEl(90, 0);   // hard left
        check(d.x < -0.99 && std::fabs(d.y) < 0.02, "az=+90 (left) -> x negative (left of frame)");
        double az = 0, el = 0; meter::vecToAzEl(d, az, el);
        check(std::fabs(az - 90) < 0.5 && std::fabs(el) < 0.5, "dirFromAzEl round-trips via vecToAzEl");
        meter::Dir up = decode_timeline::dirFromAzEl(0, 45);
        check(up.z > 0.7 && up.y > 0.6, "az=0 el=45 -> up + front");
    }

    // ---- single-direction decode picks the geometrically nearest speaker ----
    {
        auto basis = decode_timeline::speakerBasis(g_dirs, order);
        auto argmax = [&](const meter::Dir& d) {
            auto E = decode_timeline::objectSpeakerEnergies(basis, d, order, 1.0);
            int best = -1; double bv = -1; for (size_t s = 0; s < E.size(); ++s) if (E[s] > bv) { bv = E[s]; best = (int)s; }
            return best;
        };
        check(g_labels[argmax(decode_timeline::dirFromAzEl(90, 0))] == "Lss",  "object az=+90 -> dominant Lss (left)");
        check(g_labels[argmax(decode_timeline::dirFromAzEl(-90, 0))] == "Rss", "object az=-90 -> dominant Rss (right)");
        check(g_labels[argmax(decode_timeline::dirFromAzEl(0, 0))] == "C",     "object az=0 -> dominant C (front)");
        const std::string up = g_labels[argmax(decode_timeline::dirFromAzEl(0, 45))];
        check(up == "Ltf" || up == "Rtf", "object el=+45 front -> dominant a top-front speaker");
        // gain scales energy but not the winner.
        auto e1 = decode_timeline::objectSpeakerEnergies(basis, decode_timeline::dirFromAzEl(90, 0), order, 1.0);
        auto e2 = decode_timeline::objectSpeakerEnergies(basis, decode_timeline::dirFromAzEl(90, 0), order, 2.0);
        check(std::fabs(e2[3] - 4.0 * e1[3]) < 1e-9, "gain=2 quadruples a speaker's energy (gain^2)");
    }

    // ---- static object -> one slice, one migration run, isStatic ----
    {
        std::vector<Sample> s = {{0.0, 90.0, 0.0, 1.0, 1.0}};
        auto tl = decode_timeline::computeTimeline(s, g_dirs, order);
        check(tl.slices.size() == 1 && tl.isStatic, "static object -> 1 slice, isStatic");
        check(tl.migration.size() == 1 && domLabel(tl.slices[0]) == "Lss", "static left object dominant Lss");
        check(tl.footprint.valid && tl.speakersTouched == 1, "static footprint valid, 1 speaker touched");
        double sum = 0; for (double f : tl.dwellFrac) sum += f;
        check(std::fabs(sum - 1.0) < 1e-9, "dwell fractions sum to 1");
    }

    // ---- L->R azimuth sweep: dominant migrates left -> centre -> right ----
    {
        std::vector<Sample> s = {
            {0.0,  90.0, 0.0, 1.0, 1.0}, {1.0,  45.0, 0.0, 1.0, 1.0}, {2.0, 0.0, 0.0, 1.0, 1.0},
            {3.0, -45.0, 0.0, 1.0, 1.0}, {4.0, -90.0, 0.0, 1.0, 1.0}};
        auto tl = decode_timeline::computeTimeline(s, g_dirs, order);
        check(tl.slices.size() == 5 && !tl.isStatic, "sweep -> 5 slices, not static");
        check(domLabel(tl.slices.front()) == "Lss" && domLabel(tl.slices[2]) == "C" &&
              domLabel(tl.slices.back()) == "Rss", "sweep dominant migrates Lss -> C -> Rss");
        check(tl.speakersTouched >= 3 && tl.migration.size() >= 3, ">=3 distinct dominant speakers over the sweep");
        check(std::fabs(tl.angularTravelDeg - 180.0) < 2.0, "great-circle travel ~180 deg for a hard L->R sweep");
        check(tl.footprint.valid && tl.footprint.horizFrac > 0.75 && tl.footprint.upperFrac < 0.25 &&
              tl.footprint.lowerFrac < 1e-9, "planar sweep footprint: ear-level dominates, no below-floor energy");
        check(std::fabs(tl.maxElevationDeg) < 1e-9 && std::fabs(tl.minElevationDeg) < 1e-9, "planar sweep elevation stays 0");
    }

    // ---- elevation climb: max elevation tracked, footprint gains upper energy ----
    {
        std::vector<Sample> s = {
            {0.0, 0.0,  0.0, 1.0, 1.0}, {1.0, 0.0, 15.0, 1.0, 1.0},
            {2.0, 0.0, 30.0, 1.0, 1.0}, {3.0, 0.0, 45.0, 1.0, 1.0}};
        auto tl = decode_timeline::computeTimeline(s, g_dirs, order);
        check(std::fabs(tl.maxElevationDeg - 45.0) < 1e-9, "climb reaches max elevation 45");
        check(tl.footprint.valid && tl.footprint.upperFrac > 0.1, "climb footprint carries upper-hemisphere energy");
        const std::string upLast = domLabel(tl.slices.back());
        check(upLast == "Ltf" || upLast == "Rtf", "top of the climb is dominated by a top-front speaker");
    }

    // ---- time weighting: an uneven grid weights the long dwell more ----
    {
        // 9 s parked hard-left (0..9), then a brief flick to hard-right at t=10.
        std::vector<Sample> s = {{0.0, 90.0, 0.0, 1.0, 1.0}, {9.0, 90.0, 0.0, 1.0, 1.0}, {10.0, -90.0, 0.0, 1.0, 1.0}};
        auto tl = decode_timeline::computeTimeline(s, g_dirs, order);
        // Lss (index 3) should dominate dwell (9s of 10) far more than Rss (index 4).
        check(tl.dwellFrac[3] > 0.8 && tl.dwellFrac[4] < 0.2, "long left dwell dominates the time-weighted dwell");
        check(tl.footprint.dominantIdx == 3, "time-weighted footprint dominant = the long-dwell left speaker");
    }

    // ---- degenerate: empty samples / no speakers -> empty result, no crash ----
    {
        auto empty = decode_timeline::computeTimeline({}, g_dirs, order);
        check(empty.slices.empty() && !empty.footprint.valid, "empty samples -> empty timeline");
        std::vector<Sample> one = {{0.0, 0.0, 0.0, 1.0, 1.0}};
        auto noSpk = decode_timeline::computeTimeline(one, {}, order);
        check(noSpk.slices.empty(), "no speakers -> empty timeline");
    }

    if (g_failures == 0) std::fprintf(stderr, "\nALL DECODE-TIMELINE UNIT TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d DECODE-TIMELINE UNIT TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
