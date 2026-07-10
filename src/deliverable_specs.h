// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// deliverable_specs.h — the named deliverable-spec table + the pure pass/fail evaluator that
// analysis.check_deliverable runs.
//
// A measured master's integrated LUFS / true-peak (/ short-term max) is compared against a NAMED spec
// and each metric returns pass/fail. The table and the evaluator are SDK-free and JSON-only (no REAPER),
// so the whole conformance judgement is unit-tested with no DAW (test_deliverable_specs.cpp); the tool
// (tools_analysis.cpp) only has to *measure* — via the shared RENDER_STATS loudness path — and hand the
// numbers here.
//
// Standards basis: all integrated-LUFS and
// true-peak measurement follows ITU-R BS.1770-5 (K-weighted LKFS/LUFS + inter-sample true peak), metered
// per EBU Tech 3341 ("EBU Mode"). Immersive `layout` values reference ITU-R BS.2051-3 / BS.775. ITU gives
// the measurement method + layouts, not a consumer target — the regional targets (EBU −23, ATSC −24) all
// sit on BS.1770.
//
// Deliverable-spec table:
//   atmos-music        −18 LUFS-I           −1 dBTP   7.1.4 bed+objects   Apple/Dolby streaming immersive
//   streaming-stereo   −14                  −1        2.0                 Spotify / YT / Tidal / Amazon
//   apple-music-stereo −16                  −1        2.0                 Apple Music (Sound Check)
//   ebu-r128           −23 (±0.5 LU; ±1 live) −1      2.0 / 5.1           EBU broadcast (alias broadcast-r128)
//   ebu-r128-s1        −23 target; Max-ST ≤ −18  −1   2.0                 EBU short-form (adverts/promos)
//   atsc-a85           −24 LKFS (±2 LK)     −2        2.0 / 5.1           US digital TV / CALM Act
//   podcast            −16 (−19 mono)       −1        mono / 2.0          spoken word (Apple Podcasts)
//   cinema-theatrical  SPL-referenced (not LUFS-gated) −3   5.1 / 7.1     dubbing-stage 85 dB SPL reference
//
// This header is SDK-free: it pulls composite_support.h (Json + makeError, reaper_api.h de-fangs SWELL
// before <json>), so include it the same way. Everything is inline/header-only.

#pragma once

#include "composite_support.h"  // Json + makeError; reaper_api.h before <json> (SWELL min/max)

#include <cmath>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace deliverable {

// A named loudness/true-peak deliverable spec. LUFS values are LUFS-I; truePeakMaxDb is a dBTP ceiling.
struct Spec {
    std::string name;
    std::vector<std::string> aliases;
    std::string layout;            // informational (which layouts the spec is written for)
    std::string use;               // human description

    bool   lufsGated = true;       // false => SPL-referenced (cinema): report LUFS, do not pass/fail it
    double lufsTarget = -23.0;     // integrated-LUFS target
    double lufsTolLU = 1.0;        // ± conformance band (LU) around the target
    bool   hasMono = false;        // a distinct mono integrated target (podcast)
    double lufsTargetMono = -19.0; // used when the measured render is 1 channel and hasMono

    bool   hasShortTermMax = false;   // ebu-r128-s1: Max short-term must not exceed…
    double shortTermMaxLufs = -18.0;

    double truePeakMaxDb = -1.0;   // dBTP ceiling (measured true-peak must be <= this)

    std::string toleranceNote;     // e.g. "±0.5 LU (±1 live)", "±2 LK", "platform-normalized"
    std::string note;              // extra guidance (SPL note for cinema, etc.)
};

// The deliverable-spec table. One place; any future tuning is a one-file change.
inline const std::vector<Spec>& specs() {
    static const std::vector<Spec> table = {
        {"atmos-music", {}, "7.1.4 bed + objects", "Apple/Dolby streaming immersive",
         true, -18.0, 1.0, false, -19.0, false, -18.0, -1.0,
         "platform-normalized; ±1 LU conformance band", "Apple Music / Dolby Atmos music target."},
        {"streaming-stereo", {}, "2.0", "Spotify / YouTube / Tidal / Amazon",
         true, -14.0, 1.0, false, -19.0, false, -18.0, -1.0,
         "platform-normalized; ±1 LU conformance band", "Common loudness-normalized streaming target."},
        {"apple-music-stereo", {}, "2.0", "Apple Music (Sound Check)",
         true, -16.0, 1.0, false, -19.0, false, -18.0, -1.0,
         "platform-normalized; ±1 LU conformance band", "Apple Music Sound Check reference."},
        {"ebu-r128", {"broadcast-r128"}, "2.0 / 5.1", "EBU broadcast",
         true, -23.0, 0.5, false, -19.0, false, -18.0, -1.0,
         "±0.5 LU (±1 LU tolerated on live/unpredictable programme)", "EBU R128 broadcast delivery."},
        {"ebu-r128-s1", {}, "2.0", "EBU short-form (adverts / promos, <=~2 min)",
         true, -23.0, 0.5, false, -19.0, true, -18.0, -1.0,
         "±0.5 LU target; Max short-term <= −18 LUFS (EBU R128 s1)",
         "Short-form uses Max-ST as the gate since integrated gating is unreliable under ~2 min."},
        {"atsc-a85", {"calm-act"}, "2.0 / 5.1", "US digital TV / CALM Act (dialog-anchored)",
         true, -24.0, 2.0, false, -19.0, false, -18.0, -2.0,
         "±2 LK (ATSC A/85)", "−2 dBTP is looser than EBU's −1 dBTP, leaving headroom for lossy codecs."},
        {"podcast", {}, "mono / 2.0", "spoken word (Apple Podcasts)",
         true, -16.0, 1.0, true, -19.0, false, -18.0, -1.0,
         "−16 LUFS stereo, −19 LUFS mono; ±1 LU", "Apple Podcasts spoken-word target."},
        {"cinema-theatrical", {"theatrical"}, "5.1 / 7.1", "dubbing-stage 85 dB SPL reference",
         false, 0.0, 0.0, false, -19.0, false, -18.0, -3.0,
         "SPL-referenced (85 dB SPL, pink-noise-calibrated) — not LUFS-gated",
         "Theatrical is monitored at a fixed SPL; a LUFS pass/fail is meaningless. TP still reported."},
    };
    return table;
}

// Resolve a spec by name or alias (case-insensitive). nullptr if unknown.
inline const Spec* findSpec(const std::string& nameRaw) {
    std::string n;
    n.reserve(nameRaw.size());
    for (char c : nameRaw) n.push_back((char)std::tolower((unsigned char)c));
    for (const auto& s : specs()) {
        if (s.name == n) return &s;
        for (const auto& a : s.aliases) if (a == n) return &s;
    }
    return nullptr;
}

// A comma-separated list of the canonical spec names, for error remediation / a "list specs" reply.
inline std::string knownSpecs() {
    std::string out;
    for (const auto& s : specs()) { if (!out.empty()) out += ", "; out += s.name; }
    return out;
}

// Standard immersive bed widths (channels): 5.1 / 7.1 / 7.1.4 / 9.1.6 / 22.2 = 6 / 8 / 12 / 16 / 24.
// Matches the bed-builder / bedLfe table; used to auto-detect bed buses for per-bed sub-metering.
inline bool isBedWidth(int nchan) {
    return nchan == 6 || nchan == 8 || nchan == 12 || nchan == 16 || nchan == 24;
}

// The measured loudness of a rendered master (whatever subset is available). Units match REAPER's
// native RENDER_STATS (LUFS-I / short-term-max in LUFS; true peak in dBTP per BS.1770 inter-sample).
struct Measured {
    bool hasLufs = false;    double lufs = 0.0;
    bool hasTruePeak = false; double truePeak = 0.0;
    bool hasShortTermMax = false; double shortTermMax = 0.0;
    int  channels = 2;       // used to pick the mono vs stereo target (podcast)
};

// Evaluate `m` against `spec`, returning per-metric pass/fail + an overall verdict. `tolOverrideLU`
// (>= 0) overrides the spec's integrated-LUFS tolerance band. A metric with no measurement is reported
// as {"measured": null, "evaluated": false} and does not gate the overall pass.
inline Json evaluateSpec(const Spec& spec, const Measured& m, double tolOverrideLU = -1.0) {
    const double eps = 1e-6;
    Json checks = Json::array();
    bool overall = true;
    bool anyGate = false;

    // --- integrated LUFS ---
    if (spec.lufsGated) {
        const bool mono = spec.hasMono && m.channels == 1;
        const double target = mono ? spec.lufsTargetMono : spec.lufsTarget;
        const double tol = (tolOverrideLU >= 0.0) ? tolOverrideLU : spec.lufsTolLU;
        Json c{{"metric", "integratedLufs"}, {"target", target}, {"toleranceLU", tol},
               {"comparison", "within ±tol"}, {"unit", "LUFS"}};
        if (m.hasLufs) {
            const double delta = m.lufs - target;
            const bool pass = std::fabs(delta) <= tol + eps;
            c["measured"] = m.lufs; c["deltaLU"] = delta; c["evaluated"] = true; c["pass"] = pass;
            if (delta > tol) c["hint"] = "too loud — platform will turn it down / clips headroom";
            else if (delta < -tol) c["hint"] = "too quiet vs target";
            anyGate = true; overall = overall && pass;
        } else { c["measured"] = Json(nullptr); c["evaluated"] = false; }
        checks.push_back(std::move(c));
    } else {
        Json c{{"metric", "integratedLufs"}, {"evaluated", false}, {"gate", "spl-referenced"},
               {"note", spec.note}, {"unit", "LUFS"}};
        if (m.hasLufs) c["measured"] = m.lufs; else c["measured"] = Json(nullptr);
        checks.push_back(std::move(c));
    }

    // --- short-term max (ebu-r128-s1) ---
    if (spec.hasShortTermMax) {
        Json c{{"metric", "shortTermMax"}, {"limit", spec.shortTermMaxLufs},
               {"comparison", "<= limit"}, {"unit", "LUFS"}};
        if (m.hasShortTermMax) {
            const bool pass = m.shortTermMax <= spec.shortTermMaxLufs + eps;
            c["measured"] = m.shortTermMax; c["evaluated"] = true; c["pass"] = pass;
            anyGate = true; overall = overall && pass;
        } else { c["measured"] = Json(nullptr); c["evaluated"] = false; }
        checks.push_back(std::move(c));
    }

    // --- true peak (every spec has a dBTP ceiling) ---
    {
        Json c{{"metric", "truePeak"}, {"limit", spec.truePeakMaxDb}, {"comparison", "<= limit"},
               {"unit", "dBTP"}};
        if (m.hasTruePeak) {
            const bool pass = m.truePeak <= spec.truePeakMaxDb + eps;
            c["measured"] = m.truePeak; c["evaluated"] = true; c["pass"] = pass;
            anyGate = true; overall = overall && pass;
        } else { c["measured"] = Json(nullptr); c["evaluated"] = false; }
        checks.push_back(std::move(c));
    }

    Json out{{"spec", spec.name}, {"layout", spec.layout}, {"use", spec.use},
             {"lufsGated", spec.lufsGated}, {"toleranceNote", spec.toleranceNote},
             {"checks", std::move(checks)},
             {"standardsBasis",
              "ITU-R BS.1770-5 (K-weighted LKFS + inter-sample true peak); EBU Tech 3341 metering; "
              "layouts ITU-R BS.2051-3 / BS.775"}};
    // Overall pass only means something if at least one metric was actually gated.
    if (anyGate) out["pass"] = overall;
    else out["pass"] = Json(nullptr);
    out["evaluatedAnyGate"] = anyGate;
    if (!spec.note.empty()) out["note"] = spec.note;
    return out;
}

// Per-bed conformance: evaluate EACH bed's Measured against the SAME spec and roll up.
// `labels` runs parallel to `beds` (a missing/empty entry falls back to "bed <i>"). Returns
//   { "perBed":[ <evaluateSpec result + target/channels/metrics>, … ],
//     "rollup":{ "pass":<all gated beds pass | null>, "bedsEvaluated", "failing":[…],
//                "worstTruePeak"(+Bed), "worstLufs"(+Bed) } }.
// A bed with no gated metric reports pass:null and does NOT fail the rollup; rollup.pass is null only if
// NO bed gated anything. worstTruePeak = the highest measured dBTP (closest to / over the ceiling);
// worstLufs = the measured integrated LUFS with the greatest |delta| from the spec target (least
// conformant). Pure/SDK-free: the tool just measures each bed and hands the numbers here.
inline Json evaluatePerBed(const Spec& spec, const std::vector<Measured>& beds,
                           const std::vector<std::string>& labels, double tolOverrideLU = -1.0) {
    Json perBed = Json::array();
    bool anyGate = false, allGatedPass = true;
    bool haveTp = false;   double worstTp = 0.0;   std::string worstTpBed;
    bool haveLufs = false; double worstLufs = 0.0; double worstAbsDelta = -1.0; std::string worstLufsBed;
    Json failing = Json::array();
    for (size_t i = 0; i < beds.size(); ++i) {
        const Measured& m = beds[i];
        const std::string label = (i < labels.size() && !labels[i].empty())
                                      ? labels[i] : ("bed " + std::to_string(i));
        Json r = evaluateSpec(spec, m, tolOverrideLU);
        r["target"] = label;
        r["channels"] = m.channels;
        Json metrics = Json::object();
        if (m.hasLufs)         metrics["lufsIntegrated"] = m.lufs;
        if (m.hasTruePeak)     metrics["truePeak"] = m.truePeak;
        if (m.hasShortTermMax) metrics["shortTermMax"] = m.shortTermMax;
        r["metrics"] = metrics;
        perBed.push_back(r);

        if (r.contains("pass") && r["pass"].is_boolean()) {
            anyGate = true;
            if (!r["pass"].get<bool>()) { allGatedPass = false; failing.push_back(label); }
        }
        if (m.hasTruePeak) {
            if (!haveTp || m.truePeak > worstTp) { worstTp = m.truePeak; worstTpBed = label; }
            haveTp = true;
        }
        if (spec.lufsGated && m.hasLufs) {
            const bool mono = spec.hasMono && m.channels == 1;
            const double target = mono ? spec.lufsTargetMono : spec.lufsTarget;
            const double ad = std::fabs(m.lufs - target);
            if (ad > worstAbsDelta) { worstAbsDelta = ad; worstLufs = m.lufs; worstLufsBed = label; }
            haveLufs = true;
        }
    }
    Json rollup{{"bedsEvaluated", (int)beds.size()}, {"failing", failing},
                {"evaluatedAnyGate", anyGate}};
    rollup["pass"] = anyGate ? Json(allGatedPass) : Json(nullptr);
    if (haveTp) { rollup["worstTruePeak"] = worstTp; rollup["worstTruePeakBed"] = worstTpBed; }
    else          rollup["worstTruePeak"] = Json(nullptr);
    if (haveLufs) { rollup["worstLufs"] = worstLufs; rollup["worstLufsBed"] = worstLufsBed; }
    else            rollup["worstLufs"] = Json(nullptr);
    return Json{{"perBed", std::move(perBed)}, {"rollup", std::move(rollup)}};
}

}  // namespace deliverable
}  // namespace reaper_mcp
