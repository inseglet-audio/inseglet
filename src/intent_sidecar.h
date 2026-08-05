// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// intent_sidecar.h — the intent-sidecar emitter (B1 R1/R2).
//
// An intent sidecar is the authoring session's own prediction of what it just exported —
// geometry, levels, activity — written beside the deliverable at export time as one JSON file
// (schema v0, envelope `"intent": 0`; ratified-frozen after the consumer-side acceptance
// round). The consumer half is `sentinel intent-compare` (iamf-sentinel-pro
// `sentinel_pro/intent_compare.py`, S-340…S-346): it decodes the delivered file, measures the
// same quantities, and answers "does the file render as the session intended?". This header is
// the PRODUCER half: `spatial.export_adm` (R1) and `spatial.export_loom_manifest` (R2) fill the
// model from buffers/trajectories they already have and emit it with `intentSidecar: true`.
//
// DESIGN RULES (all load-bearing):
//   * The sidecar carries PREDICTIONS TO COMPARE, never values to inject. No key ever carries
//     deliverable-loudness semantics (Loom's "loudness is measured, never typed" invariant) —
//     every level key is expect* (a claim about a STEM, checked by measurement downstream).
//     The unit suite schema-scans emissions for the forbidden key set.
//   * Predictions come from machinery this extension already ships: trajectories are the same
//     adm::Block samples export_adm writes; decode predictions reuse decode_timeline.h's
//     SN3D projection-decode (analysis.object_decode_timeline's core, order 3); level/activity
//     mirrors are bit-for-bit re-implementations of the consumer's formulas.
//   * Absent block = absent claim. A bed outside the consumer's judged layout set emits NO bed
//     block (never a roster the consumer would false-fail); VO stems carry no block in v0.
//   * Deterministic: same model -> same bytes (goldens, E-116.3 idiom). Hand-ordered JSON,
//     one shared float formatter, values rounded (levels/activity 3 dp, coverage 4 dp) so the
//     goldens hold byte-identical across the 3-OS CI matrix.
//
// ============================================================================================
// SCHEMA v0 EMISSION CONTRACT (pinned against sentinel_pro/intent_compare.py @ cc6d8cf;
// pre-registered before implementation):
//   envelope   "intent": 0 (the consumer hard-rejects anything else, exit 2)
//   producer   {tool, version, export} — audit trail; the consumer does not judge it
//   program    {sampleRate, start: 0, end: frames/rate, sliceSec: 0.1} in DELIVERED-FILE time
//              (never project time; the consumer checks frames/rate vs end-start at ±0.05 s)
//   bed        only for layouts the consumer judges ("2.0", "5.1", "7.1", "7.1.4"); roster
//              channels are CANONICAL BS.2051 labels (M+030…U-135); track = 1-based delivered
//              track; per-channel expectLufs = K-weighted gated @ weight 1.0; whole-bed
//              bed.expectLufs = the F33-conformant positional weights (bed_weights.h)
//   objects    trajectory points at t = block reach time (rtime + effective duration, the
//              adm_bwf writer's fill rule) => the consumer's first-point-hold + cartesian-
//              chord sampling reproduces the delivered semantics EXACTLY (0.000° clean loop
//              by construction); decode = {layout, dominant[per slice], coverage{label:frac}}
//   scene      {order, norm: "SN3D", expectRmsDb: [per-ACN plain RMS]} — no BS.1770 weights
//              on ACN tracks (they are not speaker feeds)
//   tolerances emitted explicitly = the consumer's defaults (auditability; floors are the
//              consumer's job): posDeg 5, levelLu 0.5, dominantFrac 0.9, covSilent 0.01,
//              silentDb -40
// ============================================================================================
//
// SDK-free (no REAPER). Pulls adm_bwf.h for fmtNum (SWELL-safe include order) and
// decode_timeline.h for the SN3D projection-decode. Host-unit-tested
// (tests/unit/test_intent_sidecar.cpp) with no DAW; the expectations were pre-registered
// before implementation.

#pragma once

#include "adm_bwf.h"         // fmtNum (deterministic float formatting)
#include "decode_timeline.h" // dirFromAzEl, speakerBasis, objectSpeakerEnergies (SN3D decode)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace reaper_mcp {
namespace intent {

// Schema + prediction constants (v0; the consumer's mirrors are pinned in its module docstring).
inline int schemaVersion() { return 0; }
inline double defaultSliceSec() { return 0.1; }
inline int decodeOrder() { return 3; }          // object_decode_timeline's default SH order
// Emitted tolerance block == the consumer's TOLERANCE_DEFAULTS (intent_compare.py @ cc6d8cf).
inline double tolPosDeg() { return 5.0; }
inline double tolLevelLu() { return 0.5; }
inline double tolDominantFrac() { return 0.9; }
inline double tolCovSilent() { return 0.01; }
inline double tolSilentDb() { return -40.0; }
// Activity mirror constants (consumer: ACTIVITY_WIN_S / ACTIVITY_FLOOR_DB).
inline double activityWinSec() { return 0.100; }
inline double activityFloorDb() { return -60.0; }

// ============================================================================================
// The judged layout set — a deliberate MIRROR of intent_compare.py's LAYOUT_LABELS +
// LABEL_POSITIONS (@ cc6d8cf), pinned by a hard-coded second witness in the unit suite
// (the E-116.1 idiom). Do not edit without re-pinning both sides.
// ============================================================================================
struct IntentSpeaker {
    const char* label;   // canonical BS.2051-style label
    double az, el;       // az +left/CCW deg, el +up deg (LFE: position unused)
    bool lfe;
};
struct IntentLayout {
    const char* name;
    std::vector<IntentSpeaker> speakers;  // delivered/rendered channel order
};

inline const std::vector<IntentLayout>& intentLayouts() {
    static const std::vector<IntentLayout> kL = {
        {"2.0", {{"M+030", 30, 0, false}, {"M-030", -30, 0, false}}},
        {"5.1", {{"M+030", 30, 0, false}, {"M-030", -30, 0, false}, {"M+000", 0, 0, false},
                 {"LFE1", 0, 0, true}, {"M+110", 110, 0, false}, {"M-110", -110, 0, false}}},
        {"7.1", {{"M+030", 30, 0, false}, {"M-030", -30, 0, false}, {"M+000", 0, 0, false},
                 {"LFE1", 0, 0, true}, {"M+090", 90, 0, false}, {"M-090", -90, 0, false},
                 {"M+135", 135, 0, false}, {"M-135", -135, 0, false}}},
        {"7.1.4", {{"M+030", 30, 0, false}, {"M-030", -30, 0, false}, {"M+000", 0, 0, false},
                   {"LFE1", 0, 0, true}, {"M+090", 90, 0, false}, {"M-090", -90, 0, false},
                   {"M+135", 135, 0, false}, {"M-135", -135, 0, false},
                   {"U+045", 45, 45, false}, {"U-045", -45, 45, false},
                   {"U+135", 135, 45, false}, {"U-135", -135, 45, false}}},
    };
    return kL;
}

inline const IntentLayout* findIntentLayout(const std::string& name) {
    for (const auto& l : intentLayouts())
        if (name == l.name) return &l;
    return nullptr;
}

// Map an export-side bed layout name to the sidecar/consumer name ("stereo" -> "2.0");
// returns "" when the layout is outside the judged set (=> emit NO bed block).
inline std::string sidecarBedLayout(const std::string& exportLayout) {
    const std::string n = (exportLayout == "stereo") ? std::string("2.0") : exportLayout;
    return findIntentLayout(n) ? n : std::string();
}

// ============================================================================================
// Model — exactly what the emitter needs. The tools fill it from the session; the unit tests
// build it synthetically.
// ============================================================================================
struct RosterEntry {
    std::string channel;         // canonical label (M+030…)
    int track = 0;               // 1-based delivered track
    bool haveRms = false;   double rmsDb = 0.0;
    bool haveLufs = false;  double lufs = 0.0;
    bool haveActive = false; double active = 0.0;
};

struct TrajPoint {
    double t = 0.0;              // time the value is reached (delivered-file seconds)
    double az = 0.0, el = 0.0, dist = 1.0;
    double gain = 1.0;
};

struct ObjectEntry {
    int id = 0;                  // stable 1-based object number
    std::string label;
    int track = 0;               // 1-based delivered track
    std::vector<TrajPoint> trajectory;
    bool haveDecode = false;
    std::string decodeLayout;    // judged-set name; computeDecode() fills the two below
    std::vector<std::string> dominant;                     // per slice
    std::vector<std::pair<std::string, double>> coverage;  // (label, frac) in layout order
    bool haveRms = false;   double rmsDb = 0.0;
    bool haveLufs = false;  double lufs = 0.0;
    bool haveActive = false; double active = 0.0;
};

struct SceneBlock {
    int order = 0;
    std::vector<double> rmsDb;   // per ACN, plain dBFS RMS
};

struct Sidecar {
    std::string producerTool = "inseglet";
    std::string producerVersion;             // kInsegletVersion at the call sites
    std::string producerExport;              // the emitting tool's name
    int sampleRate = 48000;
    double start = 0.0, end = 0.0;           // delivered-file time
    double sliceSec = 0.1;
    bool haveBed = false;
    std::string bedLayout;                   // sidecar name (judged set)
    bool haveBedLufs = false; double bedLufs = 0.0;
    std::vector<RosterEntry> roster;
    std::vector<ObjectEntry> objects;
    bool haveScene = false;
    SceneBlock scene;
};

// ============================================================================================
// Measurement mirrors — bit-for-bit re-implementations of the consumer's formulas
// (_rms_db / _active_fraction, intent_compare.py @ cc6d8cf), so a clean loop agrees.
// ============================================================================================
inline double rmsDb(const std::vector<float>& x) {
    if (x.empty()) return -120.0;
    double e = 0.0;
    for (float v : x) e += (double)v * (double)v;
    e /= (double)x.size();
    return e > 0.0 ? 10.0 * std::log10(e) : -120.0;
}

// BS.1770-4 absolute-gate silence convention: the gated loudness of silence is -70 LKFS
// (the consumer's dsp.integrated_loudness returns exactly -70.0 when no block passes the
// absolute gate). Inseglet's meter uses an internal -144 sentinel (meter::kMinDb) for
// silence; that sentinel must NOT leak into a BS.1770-labeled claim — the live cross-loop
// gate caught it as a false S-344 (+74 LU) on silent bed channels (C/LFE unfed). Every
// emitted expectLufs is clamped through this floor.
inline double lufsSilenceFloor() { return -70.0; }
inline double clampLufs(double lufs) {
    return lufs < lufsSilenceFloor() ? lufsSilenceFloor() : lufs;
}

inline double activeFraction(const std::vector<float>& x, int rate) {
    const std::size_t w = (std::size_t)std::max(1, (int)(rate * activityWinSec()));
    const std::size_t n = (x.size() / w) * w;
    if (n == 0) return 0.0;
    std::size_t active = 0, wins = 0;
    for (std::size_t o = 0; o + w <= n; o += w) {
        double e = 0.0;
        for (std::size_t i = 0; i < w; ++i) e += (double)x[o + i] * (double)x[o + i];
        e /= (double)w;
        const double db = 10.0 * std::log10(std::max(e, 1e-12));
        if (db > activityFloorDb()) ++active;
        ++wins;
    }
    return wins ? (double)active / (double)wins : 0.0;
}

// ============================================================================================
// Trajectory sampling + decode prediction. The sampler is a cartesian-chord mirror of the
// consumer's sample_trajectory (hold before the first point; chord-interpolate az/el/dist as
// unit*dist vectors + gain between points; hold after the last).
// ============================================================================================
struct XyzG { double x = 0, y = 1, z = 0, g = 1; };

inline XyzG trajXyz(const TrajPoint& p) {
    const meter::Dir d = decode_timeline::dirFromAzEl(p.az, p.el);
    return XyzG{d.x * p.dist, d.y * p.dist, d.z * p.dist, p.gain};
}

inline XyzG sampleTrajectory(const std::vector<TrajPoint>& pts, double t) {
    if (pts.empty()) return XyzG{};
    if (t <= pts.front().t) return trajXyz(pts.front());
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (t <= pts[i].t) {
            const XyzG a = trajXyz(pts[i - 1]);
            const XyzG b = trajXyz(pts[i]);
            const double t0 = pts[i - 1].t, t1 = pts[i].t;
            const double f = (t1 > t0) ? std::max(0.0, std::min(1.0, (t - t0) / (t1 - t0)))
                                       : 1.0;
            return XyzG{a.x + f * (b.x - a.x), a.y + f * (b.y - a.y),
                        a.z + f * (b.z - a.z), a.g + f * (b.g - a.g)};
        }
    }
    return trajXyz(pts.back());
}

// Trajectory points from an object's authored blocks: one point per block at its REACH time —
// rtime + effective duration, mirroring the adm_bwf writer's open-ended fill rule (duration 0
// runs to the next block's rtime, the last to programme end); a jump block applies at rtime.
// Emitting reach times makes the consumer's sidecar sampling (first-point hold +
// cartesian-chord) reproduce the delivered BS.2076 semantics exactly on a clean loop.
inline std::vector<TrajPoint> trajectoryFromBlocks(const std::vector<adm::Block>& blocks,
                                                   double programmeDur) {
    std::vector<TrajPoint> out;
    out.reserve(blocks.size());
    for (std::size_t k = 0; k < blocks.size(); ++k) {
        const adm::Block& b = blocks[k];
        double bdur = b.duration;
        if (bdur <= 0) {
            const double next = (k + 1 < blocks.size()) ? blocks[k + 1].rtime : programmeDur;
            bdur = (next > b.rtime) ? (next - b.rtime) : 0.0;
        }
        TrajPoint p;
        p.t = b.jump ? b.rtime : b.rtime + bdur;
        p.az = b.az; p.el = b.el; p.dist = b.dist; p.gain = b.gain;
        out.push_back(p);
    }
    return out;
}

// The consumer's slice grid: n = max(1, round((end-start)/sliceSec)) midpoints.
inline int sliceCount(double start, double end, double sliceSec) {
    if (sliceSec <= 0) sliceSec = defaultSliceSec();
    return std::max(1, (int)std::llround((end - start) / sliceSec));
}

// Fill o.dominant + o.coverage from o.trajectory over decodeLayout's non-LFE speakers at
// decodeOrder(): per grid slice, SN3D-encode the sampled direction and projection-decode onto
// the speaker basis (decode_timeline machinery, reused verbatim); dominant = argmax label,
// coverage = equal-weight mean of per-slice energy fractions (rounded at emission).
inline bool computeDecode(ObjectEntry& o, double start, double end, double sliceSec) {
    const IntentLayout* lay = findIntentLayout(o.decodeLayout);
    if (!lay || o.trajectory.empty()) return false;
    std::vector<meter::Dir> dirs;
    std::vector<std::string> labels;
    for (const auto& s : lay->speakers) {
        if (s.lfe) continue;
        dirs.push_back(decode_timeline::dirFromAzEl(s.az, s.el));
        labels.push_back(s.label);
    }
    if (dirs.empty()) return false;
    const int order = decodeOrder();
    const std::vector<std::vector<double>> basis = decode_timeline::speakerBasis(dirs, order);
    const int n = sliceCount(start, end, sliceSec);
    std::vector<double> cov(dirs.size(), 0.0);
    o.dominant.clear();
    o.coverage.clear();
    for (int i = 0; i < n; ++i) {
        const double t = start + ((double)i + 0.5) * sliceSec;
        const XyzG p = sampleTrajectory(o.trajectory, t);
        const double norm = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        const meter::Dir d = (norm > 1e-12) ? meter::Dir{p.x / norm, p.y / norm, p.z / norm}
                                            : meter::Dir{0, 1, 0};
        const std::vector<double> E =
            decode_timeline::objectSpeakerEnergies(basis, d, order, p.g);
        double tot = 0.0;
        for (double e : E) tot += e;
        std::size_t dom = 0;
        for (std::size_t s = 1; s < E.size(); ++s)
            if (E[s] > E[dom]) dom = s;
        o.dominant.push_back(labels[dom]);
        if (tot > 1e-30)
            for (std::size_t s = 0; s < E.size(); ++s) cov[s] += (E[s] / tot) / (double)n;
    }
    for (std::size_t s = 0; s < labels.size(); ++s)
        o.coverage.emplace_back(labels[s], cov[s]);
    return true;
}

// ============================================================================================
// Emission. Hand-ordered JSON; adm::fmtNum for every number (deterministic, trailing-zero
// trimmed); levels/activity rounded to 3 dp and coverage to 4 dp before formatting so the
// byte-goldens hold across the CI matrix.
// ============================================================================================
inline double roundTo(double v, int places) {
    double m = 1.0;
    for (int i = 0; i < places; ++i) m *= 10.0;
    return std::round(v * m) / m;
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

inline std::string num(double v) { return adm::fmtNum(v); }
inline std::string num3(double v) { return adm::fmtNum(roundTo(v, 3)); }
inline std::string num4(double v) { return adm::fmtNum(roundTo(v, 4)); }
inline std::string jstr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

// The sidecar path beside an export: swap the final extension (after the last path separator)
// for ".intent.json"; extensionless paths get it appended. "FOO.wav" -> "FOO.intent.json".
inline std::string sidecarPathFor(const std::string& outPath) {
    const std::size_t slash = outPath.find_last_of("/\\");
    const std::size_t dot = outPath.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return outPath.substr(0, dot) + ".intent.json";
    return outPath + ".intent.json";
}

inline void emitLevels(std::string& j, bool haveRms, double rmsDbV, bool haveLufs, double lufs,
                       bool haveActive, double active) {
    if (haveRms) j += ", \"expectRmsDb\": " + num3(rmsDbV);
    if (haveLufs) j += ", \"expectLufs\": " + num3(lufs);
    if (haveActive) j += ", \"expectActive\": " + num3(active);
}

inline std::string writeSidecarJson(const Sidecar& m) {
    std::string j;
    j.reserve(2048);
    j += "{\n";
    j += "  \"intent\": 0,\n";
    j += "  \"producer\": {\"tool\": " + jstr(m.producerTool) + ", \"version\": " +
         jstr(m.producerVersion) + ", \"export\": " + jstr(m.producerExport) + "},\n";
    j += "  \"program\": {\"sampleRate\": " + std::to_string(m.sampleRate) +
         ", \"start\": " + num(m.start) + ", \"end\": " + num(m.end) +
         ", \"sliceSec\": " + num(m.sliceSec) + "}";
    if (m.haveBed) {
        j += ",\n  \"bed\": {\n    \"layout\": " + jstr(m.bedLayout);
        if (m.haveBedLufs) j += ",\n    \"expectLufs\": " + num3(m.bedLufs);
        j += ",\n    \"roster\": [\n";
        for (std::size_t i = 0; i < m.roster.size(); ++i) {
            const RosterEntry& r = m.roster[i];
            j += "      {\"channel\": " + jstr(r.channel) +
                 ", \"track\": " + std::to_string(r.track);
            emitLevels(j, r.haveRms, r.rmsDb, r.haveLufs, r.lufs, r.haveActive, r.active);
            j += (i + 1 < m.roster.size()) ? "},\n" : "}\n";
        }
        j += "    ]\n  }";
    }
    if (!m.objects.empty()) {
        j += ",\n  \"objects\": [\n";
        for (std::size_t oi = 0; oi < m.objects.size(); ++oi) {
            const ObjectEntry& o = m.objects[oi];
            j += "    {\n      \"id\": " + std::to_string(o.id) +
                 ", \"label\": " + jstr(o.label) +
                 ", \"track\": " + std::to_string(o.track) + ",\n";
            j += "      \"trajectory\": [\n";
            for (std::size_t pi = 0; pi < o.trajectory.size(); ++pi) {
                const TrajPoint& p = o.trajectory[pi];
                j += "        {\"t\": " + num(p.t) + ", \"az\": " + num(p.az) +
                     ", \"el\": " + num(p.el) + ", \"dist\": " + num(p.dist) +
                     ", \"gain\": " + num(p.gain);
                j += (pi + 1 < o.trajectory.size()) ? "},\n" : "}\n";
            }
            j += "      ]";
            if (o.haveDecode && !o.dominant.empty()) {
                j += ",\n      \"decode\": {\"layout\": " + jstr(o.decodeLayout) +
                     ",\n        \"dominant\": [";
                for (std::size_t di = 0; di < o.dominant.size(); ++di) {
                    j += jstr(o.dominant[di]);
                    if (di + 1 < o.dominant.size()) j += ", ";
                }
                j += "],\n        \"coverage\": {";
                for (std::size_t ci = 0; ci < o.coverage.size(); ++ci) {
                    j += jstr(o.coverage[ci].first) + ": " + num4(o.coverage[ci].second);
                    if (ci + 1 < o.coverage.size()) j += ", ";
                }
                j += "}}";
            }
            const bool anyLevel = o.haveRms || o.haveLufs || o.haveActive;
            if (anyLevel) {
                j += ",\n      ";
                std::string lv;
                emitLevels(lv, o.haveRms, o.rmsDb, o.haveLufs, o.lufs, o.haveActive, o.active);
                j += lv.substr(2);  // drop the leading ", "
            }
            j += "\n    }";
            j += (oi + 1 < m.objects.size()) ? ",\n" : "\n";
        }
        j += "  ]";
    }
    if (m.haveScene) {
        j += ",\n  \"scene\": {\"order\": " + std::to_string(m.scene.order) +
             ", \"norm\": \"SN3D\", \"expectRmsDb\": [";
        for (std::size_t i = 0; i < m.scene.rmsDb.size(); ++i) {
            j += num3(m.scene.rmsDb[i]);
            if (i + 1 < m.scene.rmsDb.size()) j += ", ";
        }
        j += "]}";
    }
    j += ",\n  \"tolerances\": {\"posDeg\": " + num(tolPosDeg()) +
         ", \"levelLu\": " + num(tolLevelLu()) +
         ", \"dominantFrac\": " + num(tolDominantFrac()) +
         ", \"covSilent\": " + num(tolCovSilent()) +
         ", \"silentDb\": " + num(tolSilentDb()) + "}\n";
    j += "}\n";
    return j;
}

}  // namespace intent
}  // namespace reaper_mcp
