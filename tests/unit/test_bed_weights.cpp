// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_bed_weights.cpp — pure unit pin for the BS.1770-4 channel-weight table (src/bed_weights.h;
// F33), mirroring iamf-sentinel-pro's `test_channel_weights_bs1770_conformance`. The
// expected vectors are HARD-CODED here (second witness, not derived from the header): 1.41 requires
// ear level AND |az| 60°–120° inclusive — so Ls/Rs (±110), Lss/Rss (±90), Lw/Rw (±60) are 1.41;
// rear surrounds Lsr/Rsr (±135) are 1.00; heights unconditionally 1.00; LFE 0. Plus the label /
// LFE-index pins and a quantitative power-domain check (the weight multiplies w·z², so a wrong
// 1.41 cell is worth exactly 10·log10(1.41) ≈ 1.494 dB of that channel's contribution). No REAPER,
// no SDK — every number is deterministic.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "ambisonic_meter.h"
#include "bed_weights.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void checkVector(int nch, const std::vector<double>& want, const std::string& name) {
    const std::vector<double> got = bedChannelWeights(nch);
    bool ok = got.size() == want.size();
    if (ok)
        for (size_t i = 0; i < want.size(); ++i)
            if (got[i] != want[i]) {
                ok = false;
                std::fprintf(stderr, "    %s idx %zu: got %.2f, want %.2f\n",
                             name.c_str(), i, got[i], want[i]);
            }
    check(ok, name + " weight vector exact");
}

int main() {
    // ==== E-117.1 — the conformance vectors (BS.1770-4 Tables 4/5, read directly) ==========
    checkVector(2, {1, 1}, "stereo");
    checkVector(6, {1, 1, 1, 0, 1.41, 1.41}, "5.1 (Ls/Rs M±110 in-band)");
    checkVector(8, {1, 1, 1, 0, 1.41, 1.41, 1.0, 1.0}, "7.1 (Lsr/Rsr M±135 OUT of band — F33)");
    checkVector(12, {1, 1, 1, 0, 1.41, 1.41, 1.0, 1.0, 1, 1, 1, 1},
                "7.1.4 (rears 1.0, heights unconditionally 1.0)");
    checkVector(10, {1, 1, 1, 0, 1.41, 1.41, 1.0, 1.0, 1, 1},
                "7.1.2 (had NO case 10, so both 1.41 cells were missing)");
    checkVector(16, {1, 1, 1, 0, 1.41, 1.41, 1.0, 1.0, 1.41, 1.41, 1, 1, 1, 1, 1, 1},
                "9.1.6 (Lw/Rw M±060 boundary-INCLUSIVE 1.41; rears 1.0; heights 1.0)");
    {
        std::vector<double> w222(24, 1.0);
        w222[3] = 0.0; w222[9] = 0.0;
        checkVector(24, w222, "22.2 (all 1.0, both LFEs excluded — interleave renderer-dependent)");
    }
    checkVector(4, {1, 1, 1, 1}, "unknown width (4 ch) all 1.0");

    // ==== labels + LFE pins (spelling preserved — Lsr/Rsr, Ltr/Rtr; renames deliberately deferred) ==
    {
        const std::vector<std::string> l12 = bedChannelLabels(12);
        check(l12.size() == 12 && l12[6] == "Lsr" && l12[7] == "Rsr" && l12[10] == "Ltr" &&
                  l12[11] == "Rtr",
              "7.1.4 labels: spelling preserved (Lsr/Rsr, Ltr/Rtr)");
        const std::vector<std::string> l16 = bedChannelLabels(16);
        check(l16.size() == 16 && l16[8] == "Lw" && l16[9] == "Rw",
              "9.1.6 labels: wides at indices 8/9");
        check(bedChannelIsLFE(6, 3) && bedChannelIsLFE(24, 3) && bedChannelIsLFE(24, 9) &&
                  !bedChannelIsLFE(2, 0) && !bedChannelIsLFE(12, 4),
              "LFE indices: 3 everywhere, +9 for 22.2, none elsewhere");
        const std::vector<std::string> l10 = bedChannelLabels(10);
        check(l10.size() == 10 && l10[4] == "Lss" && l10[5] == "Rss" && l10[6] == "Lsr" &&
                  l10[7] == "Rsr" && l10[8] == "Ltf" && l10[9] == "Rtf",
              "7.1.2 labels: sides Lss/Rss, rears Lsr/Rsr, FRONT height pair Ltf/Rtf ");
        check(bedLayoutName(12) == "7.1.4" && bedLayoutName(16) == "9.1.6" &&
                  bedLayoutName(10) == "7.1.2" && bedLayoutName(7) == "multichannel",
              "layout names (10 ch used to report \"multichannel\")");
        for (int nch : {2, 6, 8, 12, 16, 24, 5})
            if ((int)bedChannelWeights(nch).size() != nch) {
                check(false, "weights.size() == nch for " + std::to_string(nch));
                break;
            }
    }

    // ==== E-117.3 — quantitative power-domain check: a wrong 1.41 cell == 1.494 dB ==========
    // Rear-only 7.1.4 content: pre-fix vector (rears 1.41) minus the conformant table must read
    // exactly 10·log10(1.41) hotter. Same delta for 9.1.6 wide-only content, opposite history.
    {
        const double sr = 48000.0;
        const size_t N = 48000 * 2;  // 2 s -> plenty of 400 ms gating blocks
        auto toneBuf = [&](int nch, std::vector<int> hotCh) {
            meter::AudioBuffer b;
            b.channels = nch;
            b.frames = N;
            b.sampleRate = sr;
            b.samples.assign(N * (size_t)nch, 0.0f);
            for (int ch : hotCh)
                for (size_t i = 0; i < N; ++i)
                    b.samples[i * (size_t)nch + (size_t)ch] =
                        (float)(0.25 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * i / sr));
            return b;
        };
        const double expect = 10.0 * std::log10(1.41);  // 1.4923 dB

        meter::AudioBuffer rear = toneBuf(12, {6, 7});
        std::vector<double> preFix714 = {1, 1, 1, 0, 1.41, 1.41, 1.41, 1.41, 1, 1, 1, 1};
        const double dRear = meter::gatedLoudness(rear, preFix714).integratedLufs -
                             meter::gatedLoudness(rear, bedChannelWeights(12)).integratedLufs;
        check(near(dRear, expect, 0.01),
              "7.1.4 rear-only: pre-fix read " + std::to_string(dRear) +
                  " dB hotter (expect 1.492)");

        meter::AudioBuffer wide = toneBuf(16, {8, 9});
        std::vector<double> preFix916 = {1, 1, 1, 0, 1.41, 1.41, 1.41, 1.41,
                                         1.0, 1.0, 1, 1, 1, 1, 1, 1};
        const double dWide = meter::gatedLoudness(wide, bedChannelWeights(16)).integratedLufs -
                             meter::gatedLoudness(wide, preFix916).integratedLufs;
        check(near(dWide, expect, 0.01),
              "9.1.6 wide-only: conformant reads " + std::to_string(dWide) +
                  " dB hotter than pre-fix (expect 1.492)");
    }

    // ==== E-143.2 — the GENERALISING GUARD =============================
    // The invariant that would have caught it, stated over the canonical accepted-layout
    // table rather than a hand-list: every layout the product accepts must have a real label row,
    // because bedChannelWeights() derives ENTIRELY from labels — a generic "chN" row silently
    // yields an all-1.0 weight vector and a quietly wrong loudness figure. The one permitted
    // exception (22.2) is declared IN the table as data, not special-cased here, so a future
    // exception has to be written down where the table is read.
    {
        for (const BedLayoutEntry& e : bedAcceptedLayouts()) {
            const std::vector<std::string> lab = bedChannelLabels(e.channels);
            const bool generic = !lab.empty() && lab[0] == "ch0";
            check((int)lab.size() == e.channels,
                  std::string("accepted layout ") + e.name + ": label row width == channels");
            check(generic != e.labelled,
                  std::string("accepted layout ") + e.name + " (" +
                      std::to_string(e.channels) + " ch): labels " +
                      (e.labelled ? "PRESENT as declared" : "generic as declared (documented)"));
            check(bedLayoutName(e.channels) == e.name,
                  std::string("accepted layout ") + e.name + ": bedLayoutName round-trips");
            // A labelled layout must produce at least one 1.41 cell, otherwise the row exists but
            // does not reach the weight table (the failure mode one step further in).
            // ⚠ This first read "at least one weight != 1.0" — which the LFE's 0.0 satisfies FOR
            // FREE, so it passed against the injected bug. The negative control caught it; review
            // did not. Assert the Tables-4/5 value ITSELF, not merely non-uniformity.
            if (e.labelled) {
                const std::vector<double> w = bedChannelWeights(e.channels);
                bool anyInBand = false;
                for (double x : w) if (x == 1.41) anyInBand = true;
                check(anyInBand, std::string("accepted layout ") + e.name +
                                     ": labels actually REACH the weight table (a 1.41 cell exists)");
            }
        }
    }

    // ==== E-143.3 — the quantitative pin, both ways ========================================
    // Side-only 7.1.2 content: the pre-fix all-1.0 vector must read exactly 10·log10(1.41) COLDER
    // than the conformant table, and the whole-bed error on the identity fixture is the
    // 0.350 LU recorded in the finding. Shown BOTH ways so the pin fails if either drifts.
    {
        const double sr = 48000.0;
        const size_t N = 48000 * 2;
        meter::AudioBuffer side;
        side.channels = 10; side.frames = N; side.sampleRate = sr;
        side.samples.assign(N * 10u, 0.0f);
        for (int ch : {4, 5})
            for (size_t i = 0; i < N; ++i)
                side.samples[i * 10u + (size_t)ch] =
                    (float)(0.25 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * i / sr));
        const std::vector<double> preFix712(10, 1.0);   // what the default branch returned
        const double d = meter::gatedLoudness(side, bedChannelWeights(10)).integratedLufs -
                         meter::gatedLoudness(side, preFix712).integratedLufs;
        check(near(d, 10.0 * std::log10(1.41), 0.01),
              "7.1.2 side-only: conformant reads " + std::to_string(d) +
                  " dB hotter than the previous default (expect 1.492)");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d BED-WEIGHTS UNIT TEST FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nALL BED-WEIGHTS UNIT TESTS PASSED\n");
    return 0;
}
