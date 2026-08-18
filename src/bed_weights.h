// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// bed_weights.h — SMPTE bed-layout labeling + the BS.1770-4 channel-weight table (F33).
//
// Extracted verbatim from tools_analysis.cpp (its anonymous namespace) so the weight table is
// host-unit-testable — the render_stems.h relocation idiom: this file only moves the code (and
// fixes the F33-class weights, below) so a second TU can pin it. tools_analysis.cpp includes it;
// all eight loudness call sites are unchanged.
//
// ============================================================================================
// CHANNEL WEIGHTS G — ITU-R BS.1770-4 Tables 4/5, read directly (the F33 adjudication;
// mirrored by iamf-sentinel-pro's CHANNEL_WEIGHTS +
// test_channel_weights_bs1770_conformance):
//
//   G = 1.41 (+1.5 dB, POWER-domain — kWeightedPowerSeries applies w·z²) requires BOTH
//       ear level (|elev| < 30°, the M layer) AND azimuth in the 60°–120° band, INCLUSIVE.
//   U-layer (heights) is unconditionally 1.0. LFE is excluded (0).
//
//   Per label:  Ls/Rs (M±110) 1.41 · Lss/Rss (M±090) 1.41 · Lw/Rw (M±060, boundary-
//   inclusive — Table 5's row M±060 = 1.41) 1.41 · Lsr/Rsr (M±135, OUTSIDE the band)
//   1.00 · all Lt*/Rt* heights 1.00 · C/L/R 1.00 · LFE 0.
//
//   HISTORY (F33): this table previously weighted Lsr/Rsr at 1.41 (rears folded into a
//   generic "surrounds" bucket — the same class as FFmpeg ebur128's BACK_MASK over-weighting,
//   filed upstream as F29/FFmpeg#23968) and Lw/Rw at 1.00 (the ±60° boundary missed).
//   Readings on rear/wide-bearing 7.1-family content move by up to 10·log10(1.41) ≈ 1.49 dB
//   of the affected channels' contribution. 5.1 and stereo were always conformant.
//
//   22.2 (SMPTE ST 2036-2): DELIBERATELY all 1.0 (LFE1/LFE2 excluded). Its interleave order
//   is renderer-dependent (CONVENTIONS §1 caveat), so per-index Table-5 weights would be a
//   guess about channel identity; the honest posture is unweighted + documented, not
//   silently "conformant" against an assumed order.
//
//   Label spellings: this table says Lsr/Rsr (rear surround) and Ltr/Rtr (top rear);
//   tools_spatial.cpp's bedLayouts() + CONVENTIONS §1 spell the same physical channels
//   Lrs/Rrs, and Loom/BS.2051 spells the top pair Ltb/Rtb (see loom_manifest.h's verified
//   identity pin). Spelling unification is deliberately NOT done here,
//   because the labels are agent-visible tool output.
// ============================================================================================
//
// SDK-free (no REAPER). Host-unit-tested (tests/unit/test_bed_weights.cpp) with no DAW —
// the expected vectors are hard-coded there as a second witness, negative-control-verified.

#pragma once

#include <string>
#include <vector>

namespace reaper_mcp {

// ---- The canonical accepted-bed-layout table -----------------------------
// The single source of truth for "which bed layouts does the product accept". Every layout-
// dependent table in this file must cover every entry, and `unit.bed_weights` walks this table and
// FAILS if one does not — so the next layout cannot repeat that defect.
//
// Historically: 7.1.2 was the DEFAULT `bedLayout` in three spatial tool schemas while
// bedChannelLabels() had no `case 10`. A 7.1.2 bed therefore fell through to generic "ch0".."ch9"
// labels, lost BOTH its Lss/Rss 1.41 cells (reading 0.350 LU LOW — measured), and
// reported its layout as "multichannel". Nothing failed; the number was quietly wrong, including
// in the B1 intent sidecar's bed.expectLufs, which iamf-sentinel-pro's intent-compare consumes.
//
// The cause was STRUCTURAL, not a typo: bedChannelIsLFE() is a PREDICATE that GENERALISES
// (`nch >= 6 && idx == 3`, so it covered 10 channels for free) sitting directly beside switches
// that ENUMERATE. A layout the product accepts but a switch omits gets correct LFE exclusion for
// free and silently loses its weights. A table + a test that walks it is the guard.
//
// NB tools_spatial.cpp's bedLayouts() does NOT contain 7.1.2 either — it is grafted in by special
// case at each site that needs it (see admBedSpeakers). Deriving bedLayouts() and the schema enums
// from THIS table is the real fix for that class and is deliberately NOT done in this beat.
struct BedLayoutEntry {
    const char* name;
    int channels;
    bool labelled;   // false ONLY for a documented, deliberate exception
};
inline const std::vector<BedLayoutEntry>& bedAcceptedLayouts() {
    static const std::vector<BedLayoutEntry> kAccepted = {
        {"5.1", 6, true},   {"7.1", 8, true},     {"7.1.2", 10, true},
        {"7.1.4", 12, true}, {"9.1.6", 16, true},
        // 22.2 is DELIBERATELY unlabelled and unweighted: its interleave order is renderer-
        // dependent (header banner above), so per-index Table-5 cells would be a guess about
        // channel identity. The honest posture is unweighted + documented, not silently
        // "conformant" against an assumed order.
        {"22.2", 24, false},
    };
    return kAccepted;
}

// Channel width for an accepted bed layout; returns `missing` when the name is not in the table.
//
// THE single width lookup for the whole product. A later change deleted FIVE hand-rolled
// copies of this mapping — admBedChannels, export_adm's and export_damf's hostBedCh,
// orchestrate_sends' and send_layout_inspect's layoutWidth — each of which had to remember
// 7.1.2 separately, and one consumer (spatial.inject_identity_tones) had no copy at all and so
// REFUSED a layout its own schema advertised. Derive, never re-enumerate.
inline int bedLayoutChannels(const std::string& name, int missing = -1) {
    for (const BedLayoutEntry& e : bedAcceptedLayouts())
        if (name == e.name) return e.channels;
    return missing;
}

// Best-effort SMPTE channel labels for the immersive bed widths, so per-channel metering reads as
// "C / LFE / Lss / Ltf" rather than bare indices. LFE lives at channel index 3 (and index 9 for 22.2)
// — those channels are excluded from BS.1770 program loudness.
inline std::string bedLayoutName(int nch) {
    switch (nch) {
        case 1:  return "mono";
        case 2:  return "stereo";
        case 6:  return "5.1";
        case 8:  return "7.1";
        case 10: return "7.1.2";   // was reported as "multichannel"
        case 12: return "7.1.4";
        case 16: return "9.1.6";
        case 24: return "22.2";
        default: return "multichannel";
    }
}
inline std::vector<std::string> bedChannelLabels(int nch) {
    switch (nch) {
        case 1:  return {"M"};
        case 2:  return {"L", "R"};
        case 6:  return {"L", "R", "C", "LFE", "Ls", "Rs"};
        case 8:  return {"L", "R", "C", "LFE", "Lss", "Rss", "Lsr", "Rsr"};
        // 7.1.2. Order matches tools_spatial.cpp's admBedSpeakers("7.1.2")
        // and 7.1.4's first ten channels; the front height pair is the one 7.1.2 carries
        // (: our U±045 top-FRONT pair). Spelling follows THIS file's Lsr/Rsr convention,
        // not admBedSpeakers' Lrs/Rrs — the divergence is documented above and is a founder call.
        case 10: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lsr", "Rsr", "Ltf", "Rtf"};
        case 12: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lsr", "Rsr",
                         "Ltf", "Rtf", "Ltr", "Rtr"};
        case 16: return {"L", "R", "C", "LFE", "Lss", "Rss", "Lsr", "Rsr",
                         "Lw", "Rw", "Ltf", "Rtf", "Ltr", "Rtr", "Ltm", "Rtm"};
        default: break;
    }
    std::vector<std::string> v;
    v.reserve(nch);
    for (int i = 0; i < nch; ++i) v.push_back("ch" + std::to_string(i));
    return v;
}
inline bool bedChannelIsLFE(int nch, int idx) {
    if (nch >= 6 && idx == 3) return true;      // LFE1 (all beds with an LFE)
    if (nch == 24 && idx == 9) return true;     // 22.2 LFE2
    return false;
}

// BS.1770-4 channel weights G for the C++ gated-loudness path — the header-top table, applied by
// label: 1.41 ONLY at ear level within |az| 60°–120° inclusive (Ls/Rs ±110, Lss/Rss ±90, Lw/Rw
// ±60); rear surrounds Lsr/Rsr (±135) and every height 1.0; LFE 0. Unknown/generic layouts (and
// 22.2 — interleave renderer-dependent, see header) weigh every channel 1.0 with LFE excluded
// where detectable.
inline std::vector<double> bedChannelWeights(int nch) {
    const std::vector<std::string> labels = bedChannelLabels(nch);
    std::vector<double> w((size_t)std::max(nch, 0), 1.0);
    for (int i = 0; i < nch; ++i) {
        if (bedChannelIsLFE(nch, i)) { w[i] = 0.0; continue; }
        const std::string& L = labels[i];
        if (L == "Ls" || L == "Rs" || L == "Lss" || L == "Rss" || L == "Lw" || L == "Rw")
            w[i] = 1.41;  // M layer, |az| 60–120 inclusive (110 / 90 / 60)
        // Lsr/Rsr (M±135) and all Lt*/Rt* heights stay 1.0 (F33 — see header table).
    }
    return w;
}

}  // namespace reaper_mcp
