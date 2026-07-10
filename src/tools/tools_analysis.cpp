// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_analysis.cpp — the immersive / loudness analysis tool suite (READ-ONLY metering).
//
// Registers the analysis tools that measure a master, bus, take, or set of stems and report
// structured results without altering the project:
//   * check_deliverable — pass/fail of a measured master against a named deliverable spec (integrated
//     LUFS, short-term max, true-peak), via REAPER's native RENDER_STATS loudness engine and the pure,
//     unit-tested deliverable_specs.h evaluator.
//   * meter — full multichannel program + per-channel metering (RMS / peak / true-peak / K-level +
//     L/R correlation) with SMPTE bed-layout labels.
//   * spatial_field — ambisonic DirAC direction-of-arrival / diffuseness + Gerzon rV/rE vectors.
//   * stem_loudness / dialog_loudness / object_loudness — per-stem, dialog-gated, and per-object
//     BS.1770-4 gated loudness in one bounded render pass.
//   * downmix_check / binaural_check — ITU-R BS.775 loudspeaker fold-down and headphone/binaural QC.
//   * loudness_timeline — momentary / short-term series + EBU Tech 3342 loudness range.
//   * decode_coverage — ambisonic spatial coverage / balance map.
//   * adm_inspect / adm_profile_check — parse + summarize an ADM (BS.2076) BWF and validate it against
//     the Dolby Atmos Master ADM Profile.
//   * read_samples / accessor_meter / detect_silence — render-free direct sample reads of a track's
//     item content or a take's source via a REAPER audio accessor (per-channel stats + waveform
//     overview; in-box per-channel meter + gated loudness; frame-accurate silence detection).
//
// Every tool is framed as READ-ONLY analysis: it snapshots + restores every RENDER_* field and the
// track selection (via the shared render_stats.h / render_stems.h path), so it measures without
// shipping and leaves the project exactly as it found it. Program loudness comes from REAPER's native
// RENDER_STATS; the per-channel, gated, spatial, and downmix DSP lives in ambisonic_meter.h and is
// host-unit-tested with synthetic buffers.
//
// Dual-path: real measurement under REAPER_MCP_HAVE_SDK; the host build resolves specs, evaluates any
// supplied measurements, and otherwise returns the measurement plan it *would* run.

#include <algorithm>
#include <cstdio>                // std::snprintf (compact two-decimal number formatting)
#include <fstream>               // analysis.adm_inspect — read an ADM BWF from disk
#include <iterator>              // std::istreambuf_iterator (whole-file read)
#include <string>
#include <vector>

#include "tool_helpers.h"        // reaper_api.h (SWELL de-fang) + arg helpers + kCur/requireTrack
#include "../render_stats.h"     // shared RENDER_* config + RENDER_STATS parsing + snapshot/restore
#include "../render_stems.h"     // shared bounded stem-render helpers (temp render + sample read-back)
#include "../deliverable_specs.h"// the named deliverable specs + the pure pass/fail evaluator
#include "../ambisonic_meter.h"  // metering DSP (WAV read + per-channel + ambisonic field)
#include "../audio_accessor.h"   // render-free direct sample reads (accessor -> meter::AudioBuffer)
#include "../adm_bwf.h"          // ADM (BS.2076) parse + summarize for analysis.adm_inspect
#include "../adm_profile.h"      // Dolby Atmos Master ADM Profile conformance validator
#include "../damf.h"             // DAMF triad parse + summarize for analysis.damf_inspect
#include "../send_layout.h"      // SDK-free send-layout decode/inspect (analysis.send_layout_inspect)
#include "../decode_timeline.h"  // SDK-free per-object decode-coverage timeline (object_decode_timeline)
#include "../tool_registry.h"

namespace reaper_mcp {

namespace {

// ---- Bed-layout labeling (SDK-free; CONVENTIONS §1/§2) ------------------------------------------
// Best-effort SMPTE channel labels for the immersive bed widths, so per-channel metering reads as
// "C / LFE / Lss / Ltf" rather than bare indices. LFE lives at channel index 3 (and index 9 for 22.2)
// — those channels are excluded from BS.1770 program loudness.
inline std::string bedLayoutName(int nch) {
    switch (nch) {
        case 1:  return "mono";
        case 2:  return "stereo";
        case 6:  return "5.1";
        case 8:  return "7.1";
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

// BS.1770-4 channel weights G for the C++ gated-loudness path: fronts + heights 1.0, surrounds 1.41,
// LFE excluded (0). Unknown/generic layouts weigh every channel 1.0 (no LFE detectable).
inline std::vector<double> bedChannelWeights(int nch) {
    const std::vector<std::string> labels = bedChannelLabels(nch);
    std::vector<double> w((size_t)std::max(nch, 0), 1.0);
    for (int i = 0; i < nch; ++i) {
        if (bedChannelIsLFE(nch, i)) { w[i] = 0.0; continue; }
        const std::string& L = labels[i];
        if (L == "Ls" || L == "Rs" || L == "Lss" || L == "Rss" || L == "Lsr" || L == "Rsr")
            w[i] = 1.41;
    }
    return w;
}

// ITU-R BS.775-style stereo fold-down matrix from the SMPTE bed labels: L/R pass at 0 dB, C at -3 dB
// to both sides, every other labeled channel folds to its side at -3 dB (surrounds per BS.775;
// heights/wides use the same -3 dB practical convention — BS.775 predates them, documented as such).
// LFE is omitted by default (standard practice; includeLfe folds it at -3 dB to both).
inline bool buildStereoDownmixMatrix(int nch, bool includeLfe,
                                     std::vector<std::vector<double>>& m, Json& coefficients,
                                     std::string& err) {
    const double q = 1.0 / std::sqrt(2.0);
    if (nch != 2 && nch != 6 && nch != 8 && nch != 12 && nch != 16) {
        err = "unsupported width for a labeled fold-down (" + std::to_string(nch) +
              " ch) — supported: 2 (stereo), 6 (5.1), 8 (7.1), 12 (7.1.4), 16 (9.1.6)";
        return false;
    }
    const std::vector<std::string> labels = bedChannelLabels(nch);
    m.assign(2, std::vector<double>((size_t)nch, 0.0));
    coefficients = Json::array();
    for (int i = 0; i < nch; ++i) {
        const std::string& L = labels[i];
        double toLo = 0.0, toRo = 0.0;
        if (bedChannelIsLFE(nch, i)) {
            if (includeLfe) { toLo = q; toRo = q; }
        } else if (L == "L") toLo = 1.0;
        else if (L == "R") toRo = 1.0;
        else if (L == "C") { toLo = q; toRo = q; }
        else if (!L.empty() && L[0] == 'L') toLo = q;   // Ls / Lss / Lsr / Lw / Ltf / Ltr / Ltm
        else if (!L.empty() && L[0] == 'R') toRo = q;   // Rs / Rss / Rsr / Rw / Rtf / Rtr / Rtm
        m[0][i] = toLo; m[1][i] = toRo;
        coefficients.push_back(Json{{"index", i}, {"label", L}, {"toLo", toLo}, {"toRo", toRo}});
    }
    return true;
}

// Round for compact JSON series (0.01 precision keeps a timeline payload readable).
inline double r2(double v) { return std::round(v * 100.0) / 100.0; }
// Finer rounding for raw sample-domain values (DC offset, waveform overview min/max in [-1,1]).
inline double r4(double v) { return std::round(v * 10000.0) / 10000.0; }

// Two-decimal number for prose strings (std::to_string's fixed six decimals read badly).
inline std::string fmt2(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

// A named virtual loudspeaker (label + internal-frame direction) for analysis.decode_coverage.
struct NamedSpeaker { std::string label; meter::Dir dir; };

// Nominal loudspeaker directions (ITU-R BS.2051-style) for the common immersive delivery layouts, in
// the §4 listener frame (azimuth degrees clockwise-from-front, +right; elevation +up). decode_coverage
// decodes the scene TO these directions, so the renderer-dependent channel INTERLEAVE order does not
// matter — only the standardized directions do. LFE is non-directional and omitted. Returns false for
// an unknown layout.
inline bool namedLayoutDirs(const std::string& layout, std::vector<NamedSpeaker>& out,
                            std::string& err) {
    struct Spk { const char* label; double az, el; };
    std::vector<Spk> spk;
    const std::string L = toLower(layout);
    if (L == "5.1" || L == "5.0")
        spk = {{"L", -30, 0}, {"R", 30, 0}, {"C", 0, 0}, {"Ls", -110, 0}, {"Rs", 110, 0}};
    else if (L == "7.1" || L == "7.0")
        spk = {{"L", -30, 0}, {"R", 30, 0}, {"C", 0, 0}, {"Lss", -90, 0}, {"Rss", 90, 0},
               {"Lrs", -150, 0}, {"Rrs", 150, 0}};
    else if (L == "7.1.4")
        spk = {{"L", -30, 0}, {"R", 30, 0}, {"C", 0, 0}, {"Lss", -90, 0}, {"Rss", 90, 0},
               {"Lrs", -150, 0}, {"Rrs", 150, 0},
               {"Ltf", -45, 45}, {"Rtf", 45, 45}, {"Ltr", -135, 45}, {"Rtr", 135, 45}};
    else if (L == "9.1.6")
        spk = {{"L", -30, 0}, {"R", 30, 0}, {"C", 0, 0}, {"Lss", -90, 0}, {"Rss", 90, 0},
               {"Lrs", -150, 0}, {"Rrs", 150, 0}, {"Lw", -60, 0}, {"Rw", 60, 0},
               {"Ltf", -45, 45}, {"Rtf", 45, 45}, {"Ltm", -90, 45}, {"Rtm", 90, 45},
               {"Ltr", -135, 45}, {"Rtr", 135, 45}};
    else {
        err = "unknown layout '" + layout + "' — use 5.1, 7.1, 7.1.4, or 9.1.6 (or omit for a "
              "uniform sphere)";
        return false;
    }
    const double pi = std::acos(-1.0);
    out.clear();
    for (const auto& s : spk) {
        const double azr = s.az * pi / 180.0, elr = s.el * pi / 180.0;
        out.push_back(NamedSpeaker{std::string(s.label),
            meter::Dir{std::sin(azr) * std::cos(elr), std::cos(azr) * std::cos(elr), std::sin(elr)}});
    }
    return true;
}

// Pull a Measured out of an explicit `measured` argument object (any subset of the three metrics).
deliverable::Measured measuredFromArg(const Json& mo, int channels) {
    deliverable::Measured m;
    m.channels = channels;
    if (mo.is_object()) {
        if (mo.contains("lufsIntegrated") && mo["lufsIntegrated"].is_number()) {
            m.hasLufs = true; m.lufs = mo["lufsIntegrated"].get<double>();
        } else if (mo.contains("lufs") && mo["lufs"].is_number()) {
            m.hasLufs = true; m.lufs = mo["lufs"].get<double>();
        }
        if (mo.contains("truePeak") && mo["truePeak"].is_number()) {
            m.hasTruePeak = true; m.truePeak = mo["truePeak"].get<double>();
        }
        if (mo.contains("shortTermMax") && mo["shortTermMax"].is_number()) {
            m.hasShortTermMax = true; m.shortTermMax = mo["shortTermMax"].get<double>();
        } else if (mo.contains("lufsShortTermMax") && mo["lufsShortTermMax"].is_number()) {
            m.hasShortTermMax = true; m.shortTermMax = mo["lufsShortTermMax"].get<double>();
        }
        if (mo.contains("channels") && mo["channels"].is_number())
            m.channels = mo["channels"].get<int>();
    }
    return m;
}

#ifdef REAPER_MCP_HAVE_SDK
// Fill a Measured from one parseRenderStats() file block (its channel count is set by the caller).
void fillMeasuredFromFile(deliverable::Measured& m, const Json& f) {
    if (f.contains("lufsIntegrated"))   { m.hasLufs = true; m.lufs = f["lufsIntegrated"].get<double>(); }
    if (f.contains("truePeak"))         { m.hasTruePeak = true; m.truePeak = f["truePeak"].get<double>(); }
    if (f.contains("lufsShortTermMax")) { m.hasShortTermMax = true;
                                          m.shortTermMax = f["lufsShortTermMax"].get<double>(); }
}

// Shared target resolution for the analysis meters: index, name, or "master" (default).
struct TargetInfo {
    bool ok = false;
    Json err;                    // makeError() payload when !ok
    std::string label = "master";
    bool isTrack = false;
    int track = -1;
    int channels = 2;
};
inline TargetInfo resolveAnalysisTarget(const Json& a, const char* key = "target") {
    TargetInfo ti;
    if (a.contains(key)) {
        const Json& t = a[key];
        if (t.is_number()) { ti.track = t.get<int>(); ti.isTrack = true; }
        else if (t.is_string() && toLower(t.get<std::string>()) != "master") {
            const int n = CountTracks(kCur);
            const std::string want = t.get<std::string>();
            for (int i = 0; i < n; ++i) {
                MediaTrack* tr = GetTrack(kCur, i);
                if (tr && trackNameStr(tr) == want) { ti.track = i; ti.isTrack = true; break; }
            }
            if (!ti.isTrack) {
                ti.err = makeError("target_not_found", "no track named '" + want + "'",
                                   "pass a valid track index, a track name, or 'master'");
                return ti;
            }
        }
    }
    if (ti.isTrack) {
        MediaTrack* t = GetTrack(kCur, ti.track);
        if (!t) {
            ti.err = makeError("target_not_found",
                               "track index out of range: " + std::to_string(ti.track),
                               "pass a valid track index, a track name, or 'master'");
            return ti;
        }
        ti.channels = trackChannels(t);
        ti.label = "track " + std::to_string(ti.track);
    } else {
        MediaTrack* mt = GetMasterTrack(kCur);
        ti.channels = mt ? (int)GetMediaTrackInfo_Value(mt, "I_NCHAN") : 2;
    }
    if (ti.channels < 1) ti.channels = 2;
    ti.ok = true;
    return ti;
}


// Best-effort read of an object's spatial position from its panner/encoder FX chain. Scans
// for a plug-in exposing positional params (azimuth / elevation / distance) and returns their FORMATTED
// values (REAPER maps IEM/JUCE angle params as normalized [0,1]; the formatted string is the human
// degree/metre value). Returns null when no positional panner is found — position is a best-effort
// bonus on analysis.object_loudness, never a hard failure. Reads only; no project mutation.
Json readObjectPosition(MediaTrack* t) {
    if (!t) return Json(nullptr);
    const int nfx = TrackFX_GetCount(t);
    for (int fx = 0; fx < nfx; ++fx) {
        const int np = TrackFX_GetNumParams(t, fx);
        Json pos = Json::object();
        auto grab = [&](const char* wantLower, const char* outKey) -> bool {
            for (int p = 0; p < np; ++p) {
                char pn[128] = {0};
                if (!TrackFX_GetParamName(t, fx, p, pn, (int)sizeof(pn))) continue;
                if (toLower(pn).find(wantLower) != std::string::npos) {
                    const double nv = TrackFX_GetParamNormalized(t, fx, p);
                    char fbuf[128] = {0};
                    if (TrackFX_FormatParamValueNormalized(t, fx, p, nv, fbuf, (int)sizeof(fbuf)) &&
                        fbuf[0])
                        pos[outKey] = std::string(fbuf);
                    else
                        pos[outKey] = r2(nv);
                    return true;
                }
            }
            return false;
        };
        bool any = false;
        if (grab("azim", "azimuth")) any = true;
        if (grab("elev", "elevation")) any = true;
        if (grab("dist", "distance")) any = true;
        if (any) {
            char fxname[256] = {0};
            TrackFX_GetFXName(t, fx, fxname, (int)sizeof(fxname));
            pos["fx"] = std::string(fxname);
            return pos;
        }
    }
    return Json(nullptr);
}

// ---- Audio-accessor read target: a track's item content, or a specific take ---------------------
// Shared by analysis.read_samples / accessor_meter / detect_silence. Resolves `target` (track index or
// name) and, when itemIndex is present, a specific take (takeIndex<0 => active take). No project mutation.
struct AccTarget {
    bool ok = false;
    Json err;                    // makeError() payload when !ok
    bool isTake = false;
    int track = -1;
    MediaItem_Take* take = nullptr;
    std::string label;
    std::string source;          // "track" | "take"
};
inline AccTarget resolveAccessorTarget(const Json& a) {
    AccTarget t;
    int trackIdx = -1;
    if (a.contains("target")) {
        const Json& tg = a["target"];
        if (tg.is_number()) trackIdx = tg.get<int>();
        else if (tg.is_string()) {
            const std::string want = tg.get<std::string>();
            const int n = CountTracks(kCur);
            for (int i = 0; i < n; ++i) {
                MediaTrack* tr = GetTrack(kCur, i);
                if (tr && trackNameStr(tr) == want) { trackIdx = i; break; }
            }
            if (trackIdx < 0) {
                t.err = makeError("target_not_found", "no track named '" + want + "'",
                                  "pass a valid track index or name");
                return t;
            }
        }
    }
    if (trackIdx < 0) {
        t.err = makeError("missing_target", "no target track given",
                          "pass target as a track index or name (add itemIndex to read a take)");
        return t;
    }
    MediaTrack* tr = GetTrack(kCur, trackIdx);
    if (!tr) {
        t.err = makeError("target_not_found", "track index out of range: " + std::to_string(trackIdx),
                          "pass a valid track index or name");
        return t;
    }
    t.track = trackIdx;
    if (a.contains("itemIndex") && a["itemIndex"].is_number()) {
        const int itemIdx = a["itemIndex"].get<int>();
        const int takeIdx = optInt(a, "takeIndex", -1);
        MediaItem* it = GetTrackMediaItem(tr, itemIdx);
        if (!it) {
            t.err = makeError("item_not_found",
                              "item index out of range on track " + std::to_string(trackIdx) + ": " +
                              std::to_string(itemIdx), "pass a valid itemIndex");
            return t;
        }
        MediaItem_Take* tk = (takeIdx < 0) ? GetActiveTake(it) : GetTake(it, takeIdx);
        if (!tk) {
            t.err = makeError("take_not_found",
                              "no take at index " + std::to_string(takeIdx) + " on that item",
                              "pass a valid takeIndex or omit for the active take");
            return t;
        }
        t.isTake = true; t.take = tk; t.source = "take";
        t.label = "track " + std::to_string(trackIdx) + " item " + std::to_string(itemIdx) +
                  (takeIdx < 0 ? " (active take)" : (" take " + std::to_string(takeIdx)));
    } else {
        t.source = "track";
        t.label = "track " + std::to_string(trackIdx) + " (item content, pre track-FX)";
    }
    t.ok = true;
    return t;
}

// Run the render-free accessor read for a resolved target from the shared window args.
inline AccessorRead readAccTarget(const AccTarget& t, const Json& a) {
    const int rate = optInt(a, "sampleRate", 0);
    const double start = optNum(a, "start", 0.0);
    const double dur = optNum(a, "duration", 0.0);   // <=0 => whole extent from start
    return t.isTake ? readTakeContent(t.take, rate, start, dur)
                    : readTrackContent(t.track, rate, start, dur);
}

// Common window/echo fields shared by the accessor tools.
inline Json accWindowBlock(const AccessorRead& r) {
    return Json{{"sampleRate", (int)r.buf.sampleRate}, {"channels", r.buf.channels},
                {"frames", (double)r.buf.frames}, {"duration", r2(r.readDur)},
                {"window", Json{{"start", r2(r.readStart)}, {"end", r2(r.readStart + r.readDur)}}},
                {"sourceExtent", Json{{"start", r2(r.accStart)}, {"end", r2(r.accEnd)}}},
                {"clamped", r.clamped}, {"silent", r.silent}};
}
#endif

}  // namespace

void registerAnalysisTools(ToolRegistry& reg) {
    // ---- analysis.check_deliverable — conformance vs a named deliverable spec (read-only) ----
    reg.add(Tool{
        "analysis.check_deliverable",
        "Measure a master (or a specified bus/track) and report pass/fail against a NAMED deliverable "
        "spec — integrated LUFS, short-term max, and true-peak (dBTP) — one of the named deliverable specs "
        "(atmos-music, streaming-stereo, apple-music-stereo, ebu-r128, ebu-r128-s1, atsc-a85, podcast, "
        "cinema-theatrical). Measurement uses REAPER's native RENDER_STATS engine (ITU-R BS.1770-5 "
        "K-weighted LUFS + inter-sample true peak; cross-check ffmpeg ebur128 in verify) and is "
        "READ-ONLY: it snapshots and restores every RENDER_* setting + selection, reporting without "
        "shipping. Pass measured:{lufsIntegrated,truePeak,shortTermMax} to evaluate numbers directly "
        "(no render); otherwise it runs one bounded analysis render — bound it with boundsFlag=0 + "
        "startPos/endPos to fit the call window. cinema-theatrical is SPL-referenced, so LUFS is "
        "reported but not gated (true-peak still is). PER-BED: pass beds:[trackIdx|name,…] (or perBed:true "
        "to auto-detect bed-width buses = 6/8/12/16/24 ch) to render each bed bus as its own stem in ONE "
        "pass and report per-bed pass/fail + a rollup{pass,worstLufs,worstTruePeak,failing}. In supplied "
        "mode pass measured as an ARRAY of measurement blocks for the same per-bed report (no render).",
        jparse(R"({"type":"object","properties":{
            "spec":{"type":"string"},
            "target":{"type":["integer","string"]},
            "measured":{"type":["object","array"]},
            "perBed":{"type":"boolean","default":false},
            "beds":{"type":"array","items":{"type":["integer","string"]}},
            "toleranceLU":{"type":"number","minimum":0},
            "outDir":{"type":"string"},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "required":["spec"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "spec":{"type":"string"},"layout":{"type":"string"},"use":{"type":"string"},
            "target":{"type":"string"},"channels":{"type":"integer"},"measuredSource":{"type":"string"},
            "pass":{"type":["boolean","null"]},"lufsGated":{"type":"boolean"},
            "checks":{"type":"array"},"metrics":{"type":"object"},
            "perBed":{"type":"array"},"rollup":{"type":"object"},"beds":{"type":"array"},
            "toleranceNote":{"type":"string"},"standardsBasis":{"type":"string"},
            "rawStats":{"type":"string"},"warnings":{"type":"array"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "dryRun":{"type":"boolean"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const std::string specName = reqStr(a, "spec");
            const deliverable::Spec* spec = deliverable::findSpec(specName);
            if (!spec)
                return makeError("unknown_spec", "no deliverable spec named '" + specName + "'",
                                 "use one of: " + deliverable::knownSpecs());
            const double tolOverride = optNum(a, "toleranceLU", -1.0);
            const bool perBed = optBool(a, "perBed", false);
            const bool hasBedsArg = a.contains("beds") && a["beds"].is_array() && !a["beds"].empty();
            const bool wantPerBed = perBed || hasBedsArg;
            const bool dryRun = optBool(a, "dryRun", false);
            Json warnings = Json::array();

            // Resolve the target label (master vs a track index/name) — SDK-independent for reporting.
            std::string targetLabel = "master";
            bool targetIsTrack = false;
            int targetTrack = -1;
            if (a.contains("target")) {
                if (a["target"].is_number()) { targetTrack = a["target"].get<int>(); targetIsTrack = true; }
                else if (a["target"].is_string() && toLower(a["target"].get<std::string>()) != "master") {
                    targetIsTrack = true;  // by name; resolved under the SDK below
                }
            }

            // --- per-bed direct evaluation of a SUPPLIED array of measurement blocks (no render) ---
            // The unit/eval-harness path for per-bed: feed N measurement blocks -> N evaluations + rollup.
            if (a.contains("measured") && a["measured"].is_array()) {
                std::vector<deliverable::Measured> beds;
                std::vector<std::string> labels;
                int i = 0;
                for (const auto& blk : a["measured"]) {
                    int ch = 2;
                    if (blk.is_object() && blk.contains("channels") && blk["channels"].is_number())
                        ch = blk["channels"].get<int>();
                    beds.push_back(measuredFromArg(blk, ch));
                    std::string lbl;
                    if (blk.is_object()) {
                        if (blk.contains("target") && blk["target"].is_string())
                            lbl = blk["target"].get<std::string>();
                        else if (blk.contains("label") && blk["label"].is_string())
                            lbl = blk["label"].get<std::string>();
                        else if (blk.contains("name") && blk["name"].is_string())
                            lbl = blk["name"].get<std::string>();
                    }
                    if (lbl.empty()) lbl = "bed " + std::to_string(i);
                    labels.push_back(lbl);
                    ++i;
                }
                Json out = deliverable::evaluatePerBed(*spec, beds, labels, tolOverride);
                out["spec"] = spec->name;
                out["measuredSource"] = "supplied";
                if (!warnings.empty()) out["warnings"] = warnings;
                return out;
            }

            // --- direct evaluation of supplied measurements (no render; host + SDK) ---
            if (a.contains("measured") && a["measured"].is_object()) {
                int ch = 2;
                if (a["measured"].contains("channels") && a["measured"]["channels"].is_number())
                    ch = a["measured"]["channels"].get<int>();
                deliverable::Measured m = measuredFromArg(a["measured"], ch);
                Json out = deliverable::evaluateSpec(*spec, m, tolOverride);
                out["measuredSource"] = "supplied";
                out["channels"] = m.channels;
                out["target"] = targetIsTrack ? (a["target"].is_number()
                                     ? ("track " + std::to_string(targetTrack))
                                     : a["target"].get<std::string>())
                                              : std::string("master");
                if (wantPerBed) warnings.push_back(
                    "perBed/beds requested but a single measurement object was supplied — pass measured "
                    "as an ARRAY of blocks (or omit measured to render each bed bus) for a per-bed report.");
                if (!warnings.empty()) out["warnings"] = warnings;
                return out;
            }

#ifdef REAPER_MCP_HAVE_SDK
            // ---- PER-BED sub-metering: render each bed bus as its own stem in ONE pass, evaluate each ----
            if (wantPerBed) {
                struct BedTgt { int idx; int channels; std::string label; };
                std::vector<BedTgt> bedTgts;
                const int ntr = CountTracks(kCur);
                if (hasBedsArg) {
                    for (const auto& e : a["beds"]) {
                        MediaTrack* bt = nullptr; int bidx = -1;
                        if (e.is_number()) { bidx = e.get<int>(); bt = GetTrack(kCur, bidx); }
                        else if (e.is_string()) {
                            const std::string want = e.get<std::string>();
                            for (int i = 0; i < ntr; ++i) {
                                MediaTrack* t = GetTrack(kCur, i);
                                if (t && trackNameStr(t) == want) { bt = t; bidx = i; break; }
                            }
                        }
                        if (!bt)
                            return makeError("target_not_found", "bed target not found: " + e.dump(),
                                             "pass valid bed-bus track indices or names");
                        BedTgt bg; bg.idx = bidx; bg.channels = trackChannels(bt);
                        const std::string nm = trackNameStr(bt);
                        bg.label = nm.empty() ? ("track " + std::to_string(bidx)) : nm;
                        bedTgts.push_back(bg);
                    }
                } else {
                    for (int i = 0; i < ntr; ++i) {
                        MediaTrack* t = GetTrack(kCur, i);
                        if (!t) continue;
                        const int nc = trackChannels(t);
                        if (deliverable::isBedWidth(nc)) {
                            BedTgt bg; bg.idx = i; bg.channels = nc;
                            const std::string nm = trackNameStr(t);
                            bg.label = nm.empty() ? ("track " + std::to_string(i)) : nm;
                            bedTgts.push_back(bg);
                        }
                    }
                }
                if (bedTgts.empty())
                    return makeError("no_beds_found",
                        "no bed buses to sub-meter (auto-detect looks for channel widths 6/8/12/16/24)",
                        "pass beds:[trackIdx,…] explicitly, or build a bed bus first");

                // REAPER emits selected-track stems top-to-bottom, so match files to beds by index order.
                std::sort(bedTgts.begin(), bedTgts.end(),
                          [](const BedTgt& x, const BedTgt& y) { return x.idx < y.idx; });

                const int boundsFlag = optInt(a, "boundsFlag", 1);
                const int renderAction = optInt(a, "renderAction", 41824);

                if (dryRun) {
                    Json bedsJson = Json::array();
                    for (const auto& b : bedTgts)
                        bedsJson.push_back(Json{{"track", b.idx}, {"label", b.label},
                                                {"channels", b.channels}});
                    return Json{{"dryRun", true}, {"spec", spec->name}, {"perBed", true},
                                {"beds", bedsJson}, {"boundsFlag", boundsFlag},
                                {"plan", "snapshot RENDER_* → select the " + std::to_string(bedTgts.size()) +
                                         " bed bus(es) → render as multichannel stems in one pass → read "
                                         "RENDER_STATS per file → restore → evaluate each vs '" +
                                         spec->name + "'"}};
                }

                const RenderSnapshot snap = snapshotRender();
                const std::string outDirUse = optStr(a, "outDir", snap.file);
                if (outDirUse.empty())
                    warnings.push_back("no outDir and project RENDER_FILE is empty — analysis render may "
                                       "fail; pass outDir or configure a project render path.");
                const std::string useFmt = snap.format.empty() ? std::string("evaw") : snap.format;
                int maxCh = 2;
                for (const auto& b : bedTgts) if (b.channels > maxCh) maxCh = b.channels;

                setProjStr("RENDER_FILE", outDirUse);
                setProjStr("RENDER_PATTERN", std::string("_mcp_analysis - ") + spec->name + " - $track");
                setProjStr("RENDER_FORMAT", useFmt);
                setProjNum("RENDER_SRATE", 0);
                setProjNum("RENDER_CHANNELS", maxCh >= 2 ? maxCh : 2);
                setProjNum("RENDER_ADDTOPROJ", 0);
                setProjNum("RENDER_NORMALIZE", (double)(1 | 64 | 128 | 256));
                setProjNum("RENDER_NORMALIZE_TARGET", 1.0);
                setProjNum("RENDER_BRICKWALL", 1.0);
                setProjNum("RENDER_BOUNDSFLAG", boundsFlag);
                if (a.contains("startPos") && a["startPos"].is_number())
                    setProjNum("RENDER_STARTPOS", a["startPos"].get<double>());
                if (a.contains("endPos") && a["endPos"].is_number())
                    setProjNum("RENDER_ENDPOS", a["endPos"].get<double>());
                std::vector<int> sel;
                for (const auto& b : bedTgts) sel.push_back(b.idx);
                applySelection(sel);
                setProjNum("RENDER_SETTINGS", (double)(3 | 4));  // stems (selected tracks) = &(1|2), multichannel files

                Main_OnCommand(renderAction, 0);
                const std::string statsStr = getProjStrBig("RENDER_STATS");
                restoreRender(snap);                            // <-- project restored regardless

                const Json files = parseRenderStats(statsStr);
                std::vector<deliverable::Measured> ms(bedTgts.size());
                std::vector<std::string> labels(bedTgts.size());
                for (size_t i = 0; i < bedTgts.size(); ++i) {
                    ms[i].channels = bedTgts[i].channels;
                    labels[i] = bedTgts[i].label;
                }
                if (files.size() == bedTgts.size()) {
                    for (size_t i = 0; i < bedTgts.size(); ++i) fillMeasuredFromFile(ms[i], files[i]);
                } else {
                    warnings.push_back("render emitted " + std::to_string(files.size()) + " stem file(s) "
                        "for " + std::to_string(bedTgts.size()) + " bed(s) — matching by track name; check "
                        "bounds/selection if a bed is unmeasured.");
                    for (size_t i = 0; i < bedTgts.size(); ++i) {
                        const std::string lname = toLower(bedTgts[i].label);
                        for (const auto& f : files) {
                            const std::string fp = toLower(f.value("file", std::string()));
                            if (!lname.empty() && fp.find(lname) != std::string::npos) {
                                fillMeasuredFromFile(ms[i], f);
                                break;
                            }
                        }
                    }
                }
                Json out = deliverable::evaluatePerBed(*spec, ms, labels, tolOverride);
                out["spec"] = spec->name;
                out["measuredSource"] = "render-stats";
                out["rawStats"] = statsStr;
                if (!warnings.empty()) out["warnings"] = warnings;
                return out;
            }

            // Determine the measured bus + its channel width.
            int channels = 2;
            MediaTrack* srcTrack = nullptr;
            if (targetIsTrack) {
                if (a["target"].is_number()) srcTrack = requireTrack(targetTrack);
                else {
                    const int n = CountTracks(kCur);
                    const std::string want = a["target"].get<std::string>();
                    for (int i = 0; i < n; ++i) {
                        MediaTrack* t = GetTrack(kCur, i);
                        if (t && trackNameStr(t) == want) { srcTrack = t; targetTrack = i; break; }
                    }
                    if (!srcTrack)
                        return makeError("target_not_found", "no track named '" + want + "'",
                                         "pass a valid track index, a track name, or 'master'");
                }
                channels = trackChannels(srcTrack);
                targetLabel = "track " + std::to_string(targetTrack);
            } else {
                MediaTrack* mt = GetMasterTrack(kCur);
                channels = mt ? (int)GetMediaTrackInfo_Value(mt, "I_NCHAN") : 2;
                targetLabel = "master";
            }
            if (channels < 1) channels = 2;

            const int boundsFlag = optInt(a, "boundsFlag", 1);
            const int renderAction = optInt(a, "renderAction", 41824);

            // dryRun: report the measurement we WOULD run, mutate nothing (and no render).
            if (dryRun) {
                Json out{{"dryRun", true}, {"spec", spec->name}, {"target", targetLabel},
                         {"channels", channels}, {"boundsFlag", boundsFlag},
                         {"plan", "snapshot RENDER_* → configure a normalization-analysis render of the "
                                  "target → read RENDER_STATS (LUFS-I + true-peak) → restore → evaluate "
                                  "vs '" + spec->name + "'"}};
                return out;
            }

            // Non-destructive analysis render: snapshot everything we touch, force a LUFS/TP analysis
            // pass with NO applied gain (normalize enabled + only-if-too-loud against an unreachable
            // 0 LUFS ceiling — identical to render_deliverables), read RENDER_STATS, restore.
            const RenderSnapshot snap = snapshotRender();
            const std::string outDirUse = optStr(a, "outDir", snap.file);
            if (outDirUse.empty())
                warnings.push_back("no outDir and project RENDER_FILE is empty — analysis render may fail; "
                                   "pass outDir or configure a project render path.");
            const std::string useFmt = snap.format.empty() ? std::string("evaw") : snap.format;

            setProjStr("RENDER_FILE", outDirUse);
            setProjStr("RENDER_PATTERN", std::string("_mcp_analysis - ") + spec->name);
            setProjStr("RENDER_FORMAT", useFmt);
            setProjNum("RENDER_SRATE", 0);
            setProjNum("RENDER_CHANNELS", channels >= 2 ? channels : 2);
            setProjNum("RENDER_ADDTOPROJ", 0);
            // RENDER_NORMALIZE bitmask: &1 enable, (LUFS-I type = 0), &64 brickwall-limit, &128 brickwall
            // uses TRUE PEAK, &256 only-normalize-if-too-loud. REAPER computes true peak ONLY via the
            // brickwall true-peak stage — the plain loudness-normalize analysis emits LUFS + sample PEAK
            // only (confirmed live). So we turn on the brickwall true-peak measurement at a high ceiling
            // (RENDER_BRICKWALL 1.0 = 0 dBFS) so it MEASURES + reports true peak without ever limiting a
            // properly-headroomed master, and keep the LUFS-I normalize (unreachable target) for LUFS.
            setProjNum("RENDER_NORMALIZE", (double)(1 | 64 | 128 | 256));
            setProjNum("RENDER_NORMALIZE_TARGET", 1.0);          // 0 LUFS — unreachable, so no LUFS gain
            setProjNum("RENDER_BRICKWALL", 1.0);                 // 0 dBFS true-peak brickwall — measure, don't limit
            setProjNum("RENDER_BOUNDSFLAG", boundsFlag);
            if (a.contains("startPos") && a["startPos"].is_number())
                setProjNum("RENDER_STARTPOS", a["startPos"].get<double>());
            if (a.contains("endPos") && a["endPos"].is_number())
                setProjNum("RENDER_ENDPOS", a["endPos"].get<double>());
            if (targetIsTrack) {
                applySelection({targetTrack});
                int settings = 3;                       // stems (selected tracks) = &(1|2); a bare 2 falls back to the master mix
                if (channels > 2) settings |= 4;        // one multichannel file
                setProjNum("RENDER_SETTINGS", (double)settings);
            } else {
                setProjNum("RENDER_SETTINGS", 0);       // master mix
            }

            Main_OnCommand(renderAction, 0);            // no-dialog render, most-recent settings
            const std::string statsStr = getProjStrBig("RENDER_STATS");
            restoreRender(snap);                        // <-- project restored regardless of what follows

            const Json files = parseRenderStats(statsStr);
            deliverable::Measured m;
            m.channels = channels;
            bool hasSamplePeak = false; double samplePeak = 0.0;
            if (!files.empty()) {
                const Json& f0 = files[0];
                if (f0.contains("lufsIntegrated"))  { m.hasLufs = true; m.lufs = f0["lufsIntegrated"].get<double>(); }
                if (f0.contains("truePeak"))        { m.hasTruePeak = true; m.truePeak = f0["truePeak"].get<double>(); }
                if (f0.contains("lufsShortTermMax")){ m.hasShortTermMax = true; m.shortTermMax = f0["lufsShortTermMax"].get<double>(); }
                if (f0.contains("samplePeak"))      { hasSamplePeak = true; samplePeak = f0["samplePeak"].get<double>(); }
            }
            if (!m.hasLufs && !m.hasTruePeak)
                warnings.push_back("RENDER_STATS returned no loudness tokens — is the render bounded to "
                                   "audio? raw stats included for inspection.");
            if (!m.hasTruePeak && hasSamplePeak)
                warnings.push_back("no true-peak token in RENDER_STATS — reporting sample peak only; the "
                                   "true-peak conformance metric could not be gated.");

            Json out = deliverable::evaluateSpec(*spec, m, tolOverride);
            out["measuredSource"] = "render-stats";
            out["target"] = targetLabel;
            out["channels"] = channels;
            out["rawStats"] = statsStr;
            // Also surface the flat metrics for quick consumption.
            Json metrics = Json::object();
            if (m.hasLufs) metrics["lufsIntegrated"] = m.lufs;
            if (m.hasTruePeak) metrics["truePeak"] = m.truePeak;
            if (hasSamplePeak) metrics["samplePeak"] = samplePeak;
            if (m.hasShortTermMax) metrics["shortTermMax"] = m.shortTermMax;
            out["metrics"] = metrics;
            if (!warnings.empty()) out["warnings"] = warnings;
            return out;
#else
            // Host build: no REAPER to measure with. Resolve the spec + report the measurement plan so
            // the protocol test can exercise dispatch + spec resolution; real numbers need the DAW.
            Json out{{"spec", spec->name}, {"layout", spec->layout}, {"use", spec->use},
                     {"target", targetLabel}, {"lufsGated", spec->lufsGated},
                     {"toleranceNote", spec->toleranceNote}, {"dryRun", dryRun},
                     {"measuredSource", "none (host build)"},
                     {"note", wantPerBed
                        ? std::string("host build cannot render; pass measured as an ARRAY of per-bed "
                                      "measurement blocks to evaluate, or run under REAPER to render each bed.")
                        : std::string("host build cannot render; pass measured:{lufsIntegrated,truePeak,...} "
                                      "to evaluate, or run under REAPER for a live analysis render.")},
                     {"pass", Json(nullptr)}};
            if (!warnings.empty()) out["warnings"] = warnings;
            return out;
#endif
        }});

    // ---- analysis.meter — general multichannel meter (program loudness + per-channel + correlation) --
    reg.add(Tool{
        "analysis.meter",
        "Full multichannel meter for a master or bus (READ-ONLY). Reports PROGRAM loudness from "
        "REAPER's native RENDER_STATS (integrated LUFS, short-term/momentary max, loudness range, "
        "true-peak, sample peak — ITU-R BS.1770) AND per-channel metrics the render engine cannot give: "
        "per-channel RMS, sample peak, oversampled true-peak (dBTP) and K-weighted level, with SMPTE "
        "bed-layout labels (5.1/7.1/7.1.4/9.1.6/22.2) and LFE flagged. Also reports L/R phase "
        "correlation (mono/downmix compatibility). Runs ONE bounded, non-destructive analysis render "
        "(snapshots + restores every RENDER_* field + selection; the temp file is deleted after "
        "reading) using the measure-don't-limit config, so a headroomed master is measured exactly. "
        "Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, "
        "2 = time selection) to fit the call window and avoid a long synchronous render. Complements analysis.check_deliverable (spec pass/fail) and "
        "analysis.spatial_field (ambisonic direction).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"channels":{"type":"integer"},"layout":{"type":"string"},
            "program":{"type":"object"},"channelsDetail":{"type":"array"},
            "downmix":{"type":"object"},"measuredSource":{"type":"string"},
            "rawStats":{"type":"string"},"boundsFlag":{"type":"integer"},
            "plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);

            std::string targetLabel = "master";
            bool targetIsTrack = false; int targetTrack = -1; int channels = 2;
#ifdef REAPER_MCP_HAVE_SDK
            if (a.contains("target")) {
                if (a["target"].is_number()) { targetTrack = a["target"].get<int>(); targetIsTrack = true; }
                else if (a["target"].is_string() && toLower(a["target"].get<std::string>()) != "master") {
                    const int n = CountTracks(kCur);
                    const std::string want = a["target"].get<std::string>();
                    for (int i = 0; i < n; ++i) {
                        MediaTrack* t = GetTrack(kCur, i);
                        if (t && trackNameStr(t) == want) { targetTrack = i; targetIsTrack = true; break; }
                    }
                    if (!targetIsTrack)
                        return makeError("target_not_found", "no track named '" + want + "'",
                                         "pass a valid track index, a track name, or 'master'");
                }
            }
            if (targetIsTrack) {
                MediaTrack* t = requireTrack(targetTrack);
                channels = trackChannels(t);
                targetLabel = "track " + std::to_string(targetTrack);
            } else {
                MediaTrack* mt = GetMasterTrack(kCur);
                channels = mt ? (int)GetMediaTrackInfo_Value(mt, "I_NCHAN") : 2;
            }
            if (channels < 1) channels = 2;

            if (dryRun)
                return Json{{"dryRun", true}, {"target", targetLabel}, {"channels", channels},
                            {"layout", bedLayoutName(channels)}, {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → bounded measure-don't-limit render of the "
                                     "target → RENDER_STATS (program loudness) + read the temp WAV "
                                     "(per-channel level/peak/true-peak/K-level + L/R correlation) → "
                                     "delete temp → restore"}};

            TempRender tr = renderTargetToTempWav(targetTrack, channels, /*measureLoudness*/ true,
                                                  boundsFlag, a);
            if (!tr.ok) return makeError(tr.error, "analysis render/read failed", tr.remediation);

            Json warnings = Json::array();
            const int nc = tr.buf.channels;

            // Program loudness from RENDER_STATS.
            Json program = Json::object();
            const Json& f0 = tr.statsFile;
            if (f0.contains("lufsIntegrated"))   program["lufsIntegrated"]   = f0["lufsIntegrated"];
            if (f0.contains("lufsShortTermMax")) program["lufsShortTermMax"] = f0["lufsShortTermMax"];
            if (f0.contains("lufsMomentaryMax")) program["lufsMomentaryMax"] = f0["lufsMomentaryMax"];
            if (f0.contains("loudnessRange"))    program["loudnessRange"]    = f0["loudnessRange"];
            if (f0.contains("truePeak"))         program["truePeak"]         = f0["truePeak"];
            if (f0.contains("samplePeak"))       program["samplePeak"]       = f0["samplePeak"];
            if (program.empty())
                warnings.push_back("RENDER_STATS returned no loudness tokens — is the render bounded to "
                                   "audio? per-channel metrics below are still valid.");

            // Per-channel metrics — what RENDER_STATS cannot provide.
            const std::vector<std::string> labels = bedChannelLabels(nc);
            Json chDetail = Json::array();
            for (int c = 0; c < nc; ++c) {
                meter::ChannelMetrics m = meter::analyzeChannel(tr.buf, c);
                chDetail.push_back(Json{
                    {"index", c},
                    {"label", c < (int)labels.size() ? labels[c] : ("ch" + std::to_string(c))},
                    {"isLFE", bedChannelIsLFE(nc, c)},
                    {"rmsDb", m.rmsDb}, {"peakDb", m.peakDb},
                    {"truePeakDb", m.truePeakDb}, {"kLevelLkfs", m.kLevelLkfs}});
            }

            Json downmix = Json::object();
            if (nc >= 2) {
                double r = meter::interChannelCorrelation(tr.buf, 0, 1);
                downmix["lrCorrelation"] = r;
                downmix["note"] = r < -0.2
                    ? std::string("L/R strongly out of phase — mono fold-down will partially cancel")
                    : (r < 0.4 ? std::string("wide/decorrelated stereo — check mono compatibility")
                               : std::string("mono-compatible"));
            }

            return Json{{"target", targetLabel}, {"channels", nc}, {"layout", bedLayoutName(nc)},
                        {"boundsFlag", boundsFlag}, {"program", program},
                        {"channelsDetail", chDetail}, {"downmix", downmix},
                        {"measuredSource", "render+samples"},
                        {"rawStats", f0.contains("stats") ? f0["stats"] : Json::object()},
                        {"warnings", warnings}};
#else
            (void)boundsFlag; (void)targetIsTrack; (void)targetTrack; (void)channels;
            if (a.contains("target") && a["target"].is_string()) targetLabel = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                targetLabel = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", targetLabel}, {"measuredSource", "none (host build)"},
                        {"dryRun", dryRun},
                        {"note", "host build cannot render; run under REAPER for a live meter. The "
                                 "per-channel + correlation DSP is unit-tested host-side (unit.meter)."},
                        {"program", Json::object()}, {"channelsDetail", Json::array()}};
#endif
        }});

    // ---- analysis.spatial_field — ambisonic direction / diffuseness / rE / rV of a scene bus --------
    reg.add(Tool{
        "analysis.spatial_field",
        "Ambisonic spatial-field analysis of an ACN/SN3D scene bus (READ-ONLY). Runs ONE bounded, "
        "BIT-EXACT analysis render (no normalization/limiting, so the "
        "inter-channel direction cues are preserved; snapshots + restores every RENDER_* field + "
        "selection; temp file deleted after reading), then computes from the samples: DirAC active-"
        "intensity DIRECTION-OF-ARRIVAL (azimuth/elevation in the ambisonic-standard convention — CCW "
        "from front, +az=left, matching spatial.ambisonic_encode / IEM / SPARTA), "
        "DIFFUSENESS ψ (0 = single plane wave, 1 = fully diffuse) and directivity 1−ψ; the Gerzon "
        "localization vectors — VELOCITY vector rV (|rV|=1 for a plane wave) and ENERGY vector rE via a "
        "36-point virtual-speaker decode (sharper with order); and the per-ACN energy distribution. "
        "Order is inferred from the channel count ((order+1)² = 4/9/16/25…); pass normalization='n3d' if "
        "the bus is N3D. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT "
        "— the default, 2 = time selection). Fails closed if "
        "the target is not a valid ambisonic width (≥4 ch). TIMELINE: pass timeline:true for a WINDOWED "
        "trajectory — per-window DoA az/el, diffuseness, |rV|/|rE| — so a moving source traces its path; "
        "windowSec (default 1.0) and hopSec (default 0.5) shape the windows (hop auto-coarsens to cap "
        "the series at 400 windows).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "normalization":{"type":"string","enum":["sn3d","n3d"],"default":"sn3d"},
            "order":{"type":"integer","minimum":1},
            "reSpeakers":{"type":"integer","minimum":6,"maximum":256,"default":36},
            "timeline":{"type":"boolean","default":false},
            "windowSec":{"type":"number","minimum":0.1,"maximum":30,"default":1.0},
            "hopSec":{"type":"number","minimum":0.05,"maximum":30,"default":0.5},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"channels":{"type":"integer"},"order":{"type":"integer"},
            "acnUsed":{"type":"integer"},"normalization":{"type":"string"},
            "doa":{"type":"object"},"diffuseness":{"type":"number"},"directivity":{"type":"number"},
            "velocityVector":{"type":"object"},"energyVector":{"type":"object"},
            "acnEnergyFraction":{"type":"array"},"interpretation":{"type":"string"},
            "timeline":{"type":"object"},
            "measuredSource":{"type":"string"},"boundsFlag":{"type":"integer"},
            "plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
            const std::string norm = toLower(optStr(a, "normalization", "sn3d"));
            const bool isN3D = (norm == "n3d");
            const int reSpeakers = optInt(a, "reSpeakers", 36);
            const int orderOverride = optInt(a, "order", 0);

            std::string targetLabel = "master";
            bool targetIsTrack = false; int targetTrack = -1; int channels = 2;
#ifdef REAPER_MCP_HAVE_SDK
            if (a.contains("target")) {
                if (a["target"].is_number()) { targetTrack = a["target"].get<int>(); targetIsTrack = true; }
                else if (a["target"].is_string() && toLower(a["target"].get<std::string>()) != "master") {
                    const int n = CountTracks(kCur);
                    const std::string want = a["target"].get<std::string>();
                    for (int i = 0; i < n; ++i) {
                        MediaTrack* t = GetTrack(kCur, i);
                        if (t && trackNameStr(t) == want) { targetTrack = i; targetIsTrack = true; break; }
                    }
                    if (!targetIsTrack)
                        return makeError("target_not_found", "no track named '" + want + "'",
                                         "pass a valid ambisonic-bus track index, name, or 'master'");
                }
            }
            if (targetIsTrack) {
                MediaTrack* t = requireTrack(targetTrack);
                channels = trackChannels(t);
                targetLabel = "track " + std::to_string(targetTrack);
            } else {
                MediaTrack* mt = GetMasterTrack(kCur);
                channels = mt ? (int)GetMediaTrackInfo_Value(mt, "I_NCHAN") : 2;
            }

            const int inferredOrder = meter::ambiOrderFromChannels(channels);
            if (channels < 4 || inferredOrder < 1)
                return makeError("not_ambisonic",
                    "target has " + std::to_string(channels) + " channels — not a valid ambisonic bus",
                    "point at an ACN/SN3D scene bus with (order+1)² channels (4/9/16/25…)");

            if (dryRun)
                return Json{{"dryRun", true}, {"target", targetLabel}, {"channels", channels},
                            {"order", orderOverride > 0 ? orderOverride : inferredOrder},
                            {"normalization", norm}, {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → bounded BIT-EXACT render of the scene bus → "
                                     "read the temp WAV → DirAC intensity DoA + diffuseness, Gerzon "
                                     "rV/rE, per-ACN energy → delete temp → restore"}};

            TempRender tr = renderTargetToTempWav(targetTrack, channels, /*measureLoudness*/ false,
                                                  boundsFlag, a);
            if (!tr.ok) return makeError(tr.error, "analysis render/read failed", tr.remediation);

            meter::AmbiField f = meter::analyzeAmbisonic(tr.buf, orderOverride, isN3D, reSpeakers);
            if (!f.valid)
                return makeError("not_ambisonic", "rendered buffer is not a valid ambisonic scene",
                                 "ensure the bus carries an encoded ACN scene (≥4 channels)");

            Json warnings = Json::array();
            if (f.diffuseness > 0.9)
                warnings.push_back("field is nearly fully diffuse — DoA/rV direction is weakly defined.");

            Json acnE = Json::array();
            for (double v : f.acnEnergyFrac) acnE.push_back(v);

            std::string interp = f.diffuseness < 0.3
                ? ("dominant source near az " + std::to_string((int)std::lround(f.doaAzDeg)) +
                   "°, el " + std::to_string((int)std::lround(f.doaElDeg)) + "° (focused field)")
                : (f.diffuseness < 0.7 ? std::string("mixed directional/diffuse field")
                                       : std::string("diffuse/enveloping field — little dominant direction"));

            Json out{
                {"target", targetLabel}, {"channels", channels}, {"order", f.order},
                {"acnUsed", f.acnUsed}, {"normalization", norm},
                {"doa", Json{{"azimuthDeg", f.doaAzDeg}, {"elevationDeg", f.doaElDeg}}},
                {"diffuseness", f.diffuseness}, {"directivity", f.directivity},
                {"velocityVector", Json{{"magnitude", f.rVmag},
                                        {"azimuthDeg", f.rVazDeg}, {"elevationDeg", f.rVelDeg}}},
                {"energyVector", Json{{"magnitude", f.rEmag},
                                      {"azimuthDeg", f.rEazDeg}, {"elevationDeg", f.rEelDeg},
                                      {"speakers", f.rEspeakers}}},
                {"acnEnergyFraction", acnE}, {"interpretation", interp},
                {"boundsFlag", boundsFlag}, {"measuredSource", "render+samples"}};

            // ---- windowed trajectory (per-window DoA/diffuseness/rV/rE) ----
            if (optBool(a, "timeline", false)) {
                double winS = meter::clampd(optNum(a, "windowSec", 1.0), 0.1, 30.0);
                double hopS = meter::clampd(optNum(a, "hopSec", 0.5), 0.05, 30.0);
                const double dur = tr.buf.sampleRate > 0.0
                                       ? (double)tr.buf.frames / tr.buf.sampleRate : 0.0;
                const int maxWin = 400;
                if (dur > winS && hopS > 0.0 && ((dur - winS) / hopS + 1.0) > (double)maxWin) {
                    hopS = (dur - winS) / (double)(maxWin - 1);
                    warnings.push_back("timeline hop coarsened to " + fmt2(hopS) +
                                       " s to cap the series at 400 windows");
                }
                const auto ws = meter::analyzeAmbisonicTimeline(tr.buf, winS, hopS, orderOverride,
                                                                isN3D, reSpeakers);
                Json pts = Json::array();
                for (const auto& w : ws) {
                    if (!w.field.valid) continue;
                    pts.push_back(Json{
                        {"t", r2(w.timeSec)},
                        {"azimuthDeg", r2(w.field.doaAzDeg)},
                        {"elevationDeg", r2(w.field.doaElDeg)},
                        {"diffuseness", r2(w.field.diffuseness)},
                        {"rV", r2(w.field.rVmag)}, {"rE", r2(w.field.rEmag)},
                        {"rEazimuthDeg", r2(w.field.rEazDeg)},
                        {"rEelevationDeg", r2(w.field.rEelDeg)}});
                }
                out["timeline"] = Json{{"windowSec", winS}, {"hopSec", r2(hopS)},
                                       {"windows", pts}};
            }
            out["warnings"] = warnings;
            return out;
#else
            (void)boundsFlag; (void)isN3D; (void)reSpeakers; (void)orderOverride;
            (void)targetIsTrack; (void)targetTrack; (void)channels;
            if (a.contains("target") && a["target"].is_string()) targetLabel = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                targetLabel = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", targetLabel}, {"normalization", norm},
                        {"measuredSource", "none (host build)"}, {"dryRun", dryRun},
                        {"note", "host build cannot render; run under REAPER for a live field analysis. "
                                 "The DirAC/rE/rV DSP is unit-tested host-side (unit.meter)."}};
#endif
        }});

    // ================== Stem / dialog / downmix / timeline loudness meters ==================

    // ---- analysis.stem_loudness — per-track gated loudness + true peak in ONE stem pass ----------
    reg.add(Tool{
        "analysis.stem_loudness",
        "Per-stem loudness for a LIST of tracks in ONE bounded render pass (READ-ONLY) — the object/"
        "stem balance readout for immersive sessions. Renders the given tracks as BIT-EXACT "
        "multichannel stems (selected-track stems, no normalize/limit; snapshots + restores every "
        "RENDER_* field + selection; temp files deleted after reading), then computes per stem in C++: "
        "ITU-R BS.1770-4 GATED integrated loudness (-70 LKFS absolute + -10 LU relative gate, "
        "channel weights from the SMPTE bed labels, LFE excluded), ungated level, activity fraction "
        "(how much of the range the stem is audibly active), oversampled true peak (dBTP) and sample "
        "peak, plus each stem's offset from the LOUDEST stem (deltaFromLoudestLu). Program loudness of "
        "the MIX belongs to analysis.meter; this tool ranks the parts. Bound the range with boundsFlag "
        "(0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection) — one "
        "synchronous render covers all stems, so keep the range short.",
        jparse(R"({"type":"object","properties":{
            "tracks":{"type":"array","items":{"type":["integer","string"]},"minItems":1,"maxItems":32},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "required":["tracks"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "stems":{"type":"array"},"loudest":{"type":"object"},"count":{"type":"integer"},
            "measuredSource":{"type":"string"},"boundsFlag":{"type":"integer"},
            "plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            if (!a.contains("tracks") || !a["tracks"].is_array() || a["tracks"].empty())
                throw std::runtime_error("missing/invalid array arg: tracks");
            if (a["tracks"].size() > 32)
                return makeError("too_many_stems", "more than 32 stems requested",
                                 "meter at most 32 stems per call");
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
#ifdef REAPER_MCP_HAVE_SDK
            struct Stem { int idx; int channels; std::string label; };
            std::vector<Stem> stems;
            const int ntr = CountTracks(kCur);
            for (const auto& e : a["tracks"]) {
                MediaTrack* t = nullptr; int idx = -1;
                if (e.is_number()) { idx = e.get<int>(); t = GetTrack(kCur, idx); }
                else if (e.is_string()) {
                    const std::string want = e.get<std::string>();
                    for (int i = 0; i < ntr; ++i) {
                        MediaTrack* c = GetTrack(kCur, i);
                        if (c && trackNameStr(c) == want) { t = c; idx = i; break; }
                    }
                }
                if (!t)
                    return makeError("target_not_found", "stem target not found: " + e.dump(),
                                     "pass valid track indices or names");
                Stem s; s.idx = idx; s.channels = trackChannels(t);
                const std::string nm = trackNameStr(t);
                s.label = nm.empty() ? ("track " + std::to_string(idx)) : nm;
                stems.push_back(s);
            }
            // Stems render top-to-bottom, so sort by index (files match by order) + reject duplicates.
            std::sort(stems.begin(), stems.end(),
                      [](const Stem& x, const Stem& y) { return x.idx < y.idx; });
            for (size_t i = 1; i < stems.size(); ++i)
                if (stems[i].idx == stems[i - 1].idx)
                    return makeError("duplicate_stem", "track " + std::to_string(stems[i].idx) +
                                     " listed twice", "list each stem once");
            int maxCh = 2;
            for (const auto& s : stems) if (s.channels > maxCh) maxCh = s.channels;

            if (dryRun) {
                Json list = Json::array();
                for (const auto& s : stems)
                    list.push_back(Json{{"track", s.idx}, {"label", s.label},
                                        {"channels", s.channels}});
                return Json{{"dryRun", true}, {"stems", list}, {"count", (int)stems.size()},
                            {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → render the " + std::to_string(stems.size()) +
                                     " track(s) as bit-exact multichannel stems in one pass → read each "
                                     "temp WAV → C++ BS.1770-4 gated loudness + dBTP per stem → delete "
                                     "temps → restore"}};
            }

            std::vector<int> sel;
            std::vector<std::string> lbls;
            for (const auto& s : stems) { sel.push_back(s.idx); lbls.push_back(s.label); }
            MultiStemRender mr = renderTracksToTempWavs(sel, lbls, maxCh, /*measureLoudness*/ false,
                                                        boundsFlag, a);
            if (!mr.ok) return makeError(mr.error, "stem render failed", mr.remediation);

            Json warnings = Json::array();
            if (mr.fileCount != stems.size())
                warnings.push_back("render emitted " + std::to_string(mr.fileCount) +
                                   " stem file(s) for " + std::to_string(stems.size()) +
                                   " track(s) — matched by track name; check bounds/selection for any "
                                   "unmeasured stem.");
            Json stemJson = Json::array();
            double bestLufs = meter::kMinDb(); int bestIdx = -1;
            std::vector<double> lufsPer(stems.size(), meter::kMinDb());
            for (size_t i = 0; i < stems.size(); ++i) {
                if (!mr.haveBuf[i]) {
                    warnings.push_back("no stem file rendered for track " +
                                       std::to_string(stems[i].idx) +
                                       " — check bounds/selection");
                    continue;
                }
                const meter::AudioBuffer& buf = mr.bufs[i];
                std::vector<double> w = bedChannelWeights(stems[i].channels);
                w.resize((size_t)buf.channels, 1.0);   // padded channels are silent — weight harmless
                meter::GatedLoudness g = meter::gatedLoudness(buf, w);
                double tp = meter::kMinDb(), sp = meter::kMinDb();
                const int usech = std::min(stems[i].channels, buf.channels);
                for (int c = 0; c < usech; ++c) {
                    meter::ChannelMetrics cm = meter::analyzeChannel(buf, c);
                    if (cm.truePeakDb > tp) tp = cm.truePeakDb;
                    if (cm.peakDb > sp) sp = cm.peakDb;
                }
                lufsPer[i] = g.integratedLufs;
                if (g.integratedLufs > bestLufs) { bestLufs = g.integratedLufs; bestIdx = (int)i; }
                stemJson.push_back(Json{
                    {"track", stems[i].idx}, {"label", stems[i].label},
                    {"channels", stems[i].channels}, {"layout", bedLayoutName(stems[i].channels)},
                    {"lufsIntegrated", r2(g.integratedLufs)}, {"ungatedLufs", r2(g.ungatedLufs)},
                    {"activityFraction", r2(g.activityFraction)},
                    {"truePeakDb", r2(tp)}, {"samplePeakDb", r2(sp)}});
            }
            // Balance: each stem vs the loudest.
            for (auto& sj : stemJson) {
                const double l = sj.value("lufsIntegrated", meter::kMinDb());
                sj["deltaFromLoudestLu"] = r2(l - bestLufs);
            }
            Json loudest = Json::object();
            if (bestIdx >= 0)
                loudest = Json{{"track", stems[(size_t)bestIdx].idx},
                               {"label", stems[(size_t)bestIdx].label},
                               {"lufsIntegrated", r2(bestLufs)}};
            return Json{{"stems", stemJson}, {"loudest", loudest}, {"count", (int)stemJson.size()},
                        {"boundsFlag", boundsFlag},
                        {"measuredSource", "render+samples (C++ BS.1770-4 gated)"},
                        {"warnings", warnings}};
#else
            (void)boundsFlag;
            return Json{{"stems", Json::array()}, {"count", (int)a["tracks"].size()},
                        {"dryRun", dryRun}, {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for live stem loudness. "
                                 "The gated-loudness DSP is unit-tested host-side (unit.meter)."}};
#endif
        }});

    // ---- analysis.dialog_loudness — dialog-gated loudness + program-to-dialog offset -------------
    reg.add(Tool{
        "analysis.dialog_loudness",
        "Dialog loudness QC (READ-ONLY): designate the DIALOG track/bus and get (1) program loudness "
        "of the mix from REAPER's native RENDER_STATS, (2) the dialog stem's SPEECH-ACTIVE loudness — "
        "ITU-R BS.1770-4 gated loudness computed in C++ on a bit-exact render of the ISOLATED dialog "
        "stem, where the -70 LKFS absolute + -10 LU relative gates drop the silence between phrases "
        "(an honest approximation of dialogue-gated measurement — valid when the stem carries dialog "
        "only; NOT Dolby Dialogue Intelligence VAD), (3) the dialog activity fraction, and (4) the "
        "program-to-dialog offset (dialnorm-style; e.g. Netflix specifies dialogue-gated -27 LKFS "
        "±2 LU). Runs TWO bounded renders (program + dialog stem), each snapshotting + restoring every "
        "RENDER_* field + selection, temp files deleted. Bound the range with boundsFlag (0 = custom "
        "startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection) and keep it short — "
        "renders are synchronous.",
        jparse(R"({"type":"object","properties":{
            "dialogTrack":{"type":["integer","string"]},
            "target":{"type":["integer","string"]},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "required":["dialogTrack"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "dialogTrack":{"type":"integer"},"dialogLabel":{"type":"string"},
            "program":{"type":"object"},"dialog":{"type":"object"},
            "programToDialogLu":{"type":["number","null"]},"interpretation":{"type":"string"},
            "method":{"type":"string"},"measuredSource":{"type":"string"},
            "boundsFlag":{"type":"integer"},"plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            if (!a.contains("dialogTrack") ||
                (!a["dialogTrack"].is_number() && !a["dialogTrack"].is_string()))
                throw std::runtime_error("missing/invalid arg: dialogTrack (track index or name)");
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
#ifdef REAPER_MCP_HAVE_SDK
            // Resolve the dialog stem.
            Json da = Json{{"target", a["dialogTrack"]}};
            TargetInfo dlg = resolveAnalysisTarget(da);
            if (!dlg.ok) return dlg.err;
            if (!dlg.isTrack)
                return makeError("invalid_dialog_target",
                                 "dialogTrack must be a TRACK (index or name), not the master",
                                 "point at the dialog bus/track");
            // Resolve the program target (default master).
            TargetInfo prog = resolveAnalysisTarget(a);
            if (!prog.ok) return prog.err;

            if (dryRun)
                return Json{{"dryRun", true}, {"dialogTrack", dlg.track},
                            {"dialogLabel", dlg.label}, {"boundsFlag", boundsFlag},
                            {"plan", "two bounded renders: (1) measure-don't-limit render of " +
                                     prog.label + " → RENDER_STATS program loudness; (2) bit-exact "
                                     "stem render of the dialog track → C++ BS.1770-4 gated loudness "
                                     "(speech-active level) + activity → offset = program − dialog"}};

            Json warnings = Json::array();

            // (1) Program loudness (REAPER-native).
            TempRender pr = renderTargetToTempWav(prog.isTrack ? prog.track : -1, prog.channels,
                                                  /*measureLoudness*/ true, boundsFlag, a);
            if (!pr.ok) return makeError(pr.error, "program analysis render failed", pr.remediation);
            Json program = Json{{"target", prog.label}, {"channels", pr.buf.channels}};
            bool haveProgLufs = false; double progLufs = 0.0;
            if (pr.statsFile.contains("lufsIntegrated")) {
                progLufs = pr.statsFile["lufsIntegrated"].get<double>();
                haveProgLufs = true;
                program["lufsIntegrated"] = progLufs;
                program["source"] = "render-stats";
            } else {
                // Fall back to the C++ gated path on the program buffer.
                std::vector<double> w = bedChannelWeights(pr.buf.channels);
                meter::GatedLoudness g = meter::gatedLoudness(pr.buf, w);
                if (g.valid && g.integratedLufs > meter::kMinDb() + 1.0) {
                    progLufs = g.integratedLufs; haveProgLufs = true;
                    program["lufsIntegrated"] = r2(progLufs);
                    program["source"] = "c++ bs1770 (RENDER_STATS had no loudness tokens)";
                    warnings.push_back("RENDER_STATS returned no program loudness — using the C++ "
                                       "gated measurement of the program render instead.");
                } else {
                    warnings.push_back("program render carried no measurable loudness — is the range "
                                       "bounded to audio?");
                }
            }
            if (pr.statsFile.contains("truePeak")) program["truePeak"] = pr.statsFile["truePeak"];

            // (2) Dialog stem, bit-exact, C++ gated.
            TempRender dr = renderTargetToTempWav(dlg.track, dlg.channels,
                                                  /*measureLoudness*/ false, boundsFlag, a);
            if (!dr.ok) return makeError(dr.error, "dialog stem render failed", dr.remediation);
            std::vector<double> dw = bedChannelWeights(dlg.channels);
            dw.resize((size_t)dr.buf.channels, 1.0);
            meter::GatedLoudness dg = meter::gatedLoudness(dr.buf, dw);
            double dtp = meter::kMinDb();
            const int usech = std::min(dlg.channels, dr.buf.channels);
            for (int c = 0; c < usech; ++c) {
                meter::ChannelMetrics cm = meter::analyzeChannel(dr.buf, c);
                if (cm.truePeakDb > dtp) dtp = cm.truePeakDb;
            }
            const bool dialogSilent = !dg.valid || dg.integratedLufs <= meter::kMinDb() + 1.0;
            if (dialogSilent)
                warnings.push_back("dialog stem is silent (or below the -70 LKFS gate) in the "
                                   "analyzed range — no dialog loudness to report.");
            Json dialog = Json{{"track", dlg.track}, {"label", dlg.label},
                               {"channels", dlg.channels},
                               {"lufsIntegrated", r2(dg.integratedLufs)},
                               {"ungatedLufs", r2(dg.ungatedLufs)},
                               {"activityFraction", r2(dg.activityFraction)},
                               {"truePeakDb", r2(dtp)},
                               {"source", "c++ bs1770 gated on the isolated stem"}};

            Json offset = Json(nullptr);
            std::string interp = "no interpretation (missing program or dialog loudness)";
            if (haveProgLufs && !dialogSilent) {
                const double off = progLufs - dg.integratedLufs;
                offset = r2(off);
                interp = "dialog sits " + fmt2(std::fabs(off)) +
                         (off <= 0 ? " LU above" : " LU below") + " program loudness; dialog-gated "
                         "level " + fmt2(dg.integratedLufs) + " LKFS (for reference: "
                         "Netflix dialogue-gated spec is -27 LKFS ±2 LU)";
            }
            return Json{{"dialogTrack", dlg.track}, {"dialogLabel", dlg.label},
                        {"program", program}, {"dialog", dialog},
                        {"programToDialogLu", offset}, {"interpretation", interp},
                        {"method", "dialog level = BS.1770-4 gated loudness of the ISOLATED dialog "
                                   "stem (gates drop inter-phrase silence). Approximation is honest "
                                   "only if the stem carries dialog exclusively."},
                        {"boundsFlag", boundsFlag},
                        {"measuredSource", "render-stats + render+samples"},
                        {"warnings", warnings}};
#else
            (void)boundsFlag;
            Json dlgEcho = a["dialogTrack"];
            return Json{{"dialogTrack", dlgEcho.is_number() ? dlgEcho : Json(-1)},
                        {"dialogLabel", dlgEcho.is_string() ? dlgEcho.get<std::string>()
                                                            : std::string()},
                        {"program", Json::object()}, {"dialog", Json::object()},
                        {"programToDialogLu", Json(nullptr)}, {"dryRun", dryRun},
                        {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for live dialog "
                                 "loudness. The gated-loudness DSP is unit-tested host-side."}};
#endif
        }});

    // ---- analysis.downmix_check — ITU BS.775 stereo/mono fold-down QC ----------------------------
    reg.add(Tool{
        "analysis.downmix_check",
        "Downmix compatibility QC for a multichannel master/bus (READ-ONLY). Runs ONE bounded, "
        "BIT-EXACT render (snapshot + restore every RENDER_* + selection; temp deleted), then folds "
        "the channels in C++ per ITU-R BS.775 practice — L/R at 0 dB, C at -3 dB to both, surrounds/"
        "heights/wides to their side at -3 dB, LFE omitted (includeLfe:true folds it at -3 dB) — and "
        "measures what the fold does: stereo downmix BS.1770-4 gated loudness + true peak + Lo/Ro "
        "correlation, MONO fold ((Lo+Ro)/2) loudness + true peak, and the level deltas (multichannel→"
        "stereo, stereo→mono). A mono delta well below -3 LU beyond the decorrelation baseline flags "
        "phase cancellation. Supported widths: 2 (stereo mono-check), 5.1, 7.1, 7.1.4, 9.1.6; the fold "
        "matrix is echoed per channel. Loudness here is the C++ BS.1770-4 gated engine (unit-tested "
        "host-side) because the folds exist only in memory — program loudness of the ORIGINAL mix "
        "still belongs to analysis.meter/RENDER_STATS.",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "includeLfe":{"type":"boolean","default":false},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"channels":{"type":"integer"},"layout":{"type":"string"},
            "includeLfe":{"type":"boolean"},"coefficients":{"type":"array"},
            "multichannel":{"type":"object"},"stereo":{"type":"object"},"mono":{"type":"object"},
            "deltas":{"type":"object"},"interpretation":{"type":"string"},
            "measuredSource":{"type":"string"},"boundsFlag":{"type":"integer"},
            "plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const bool includeLfe = optBool(a, "includeLfe", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
#ifdef REAPER_MCP_HAVE_SDK
            TargetInfo ti = resolveAnalysisTarget(a);
            if (!ti.ok) return ti.err;

            std::vector<std::vector<double>> matrix;
            Json coefficients;
            std::string merr;
            if (!buildStereoDownmixMatrix(ti.channels, includeLfe, matrix, coefficients, merr))
                return makeError("downmix_unsupported", merr,
                                 "meter a labeled bed width (2/6/8/12/16 channels)");

            if (dryRun)
                return Json{{"dryRun", true}, {"target", ti.label}, {"channels", ti.channels},
                            {"layout", bedLayoutName(ti.channels)}, {"includeLfe", includeLfe},
                            {"coefficients", coefficients}, {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → bounded BIT-EXACT render of the target → "
                                     "fold to stereo (BS.775 coefficients) + mono in C++ → gated "
                                     "loudness/true-peak/correlation of each → delete temp → restore"}};

            TempRender tr = renderTargetToTempWav(ti.isTrack ? ti.track : -1, ti.channels,
                                                  /*measureLoudness*/ false, boundsFlag, a);
            if (!tr.ok) return makeError(tr.error, "analysis render/read failed", tr.remediation);

            Json warnings = Json::array();
            const int nc = std::min(ti.channels, tr.buf.channels);
            if (tr.buf.channels != ti.channels)
                warnings.push_back("rendered width " + std::to_string(tr.buf.channels) +
                                   " != target width " + std::to_string(ti.channels) +
                                   " — folding the first " + std::to_string(nc) + " channel(s).");

            // Multichannel reference: C++ gated loudness with bed weights + max true peak.
            std::vector<double> w = bedChannelWeights(ti.channels);
            w.resize((size_t)tr.buf.channels, 1.0);
            meter::GatedLoudness gm = meter::gatedLoudness(tr.buf, w);
            double mtp = meter::kMinDb();
            for (int c = 0; c < nc; ++c) {
                meter::ChannelMetrics cm = meter::analyzeChannel(tr.buf, c);
                if (cm.truePeakDb > mtp) mtp = cm.truePeakDb;
            }

            // Stereo fold.
            meter::AudioBuffer st = meter::applyDownmixMatrix(tr.buf, matrix);
            meter::GatedLoudness gs = meter::gatedLoudness(st, {1.0, 1.0});
            meter::ChannelMetrics lo = meter::analyzeChannel(st, 0);
            meter::ChannelMetrics ro = meter::analyzeChannel(st, 1);
            const double corr = meter::interChannelCorrelation(st, 0, 1);

            // Mono fold of the stereo fold.
            meter::AudioBuffer mono = meter::applyDownmixMatrix(st, {{0.5, 0.5}});
            meter::GatedLoudness gmo = meter::gatedLoudness(mono, {1.0});
            meter::ChannelMetrics mm = meter::analyzeChannel(mono, 0);

            const double stereoVsMulti = gs.integratedLufs - gm.integratedLufs;
            const double monoVsStereo = gmo.integratedLufs - gs.integratedLufs;
            std::string interp;
            if (monoVsStereo > -3.5)
                interp = "mono-compatible: the mono fold holds level (delta " +
                         fmt2(monoVsStereo) + " LU vs stereo)";
            else if (monoVsStereo > -6.5)
                interp = "wide/decorrelated content: expected mono level loss (delta " +
                         fmt2(monoVsStereo) + " LU vs stereo) — verify intent";
            else
                interp = "PHASE CANCELLATION in the mono fold (delta " +
                         fmt2(monoVsStereo) + " LU vs stereo) — check channel phase "
                         "and out-of-phase width tricks";

            return Json{{"target", ti.label}, {"channels", ti.channels},
                        {"layout", bedLayoutName(ti.channels)}, {"includeLfe", includeLfe},
                        {"coefficients", coefficients},
                        {"multichannel", Json{{"lufsIntegrated", r2(gm.integratedLufs)},
                                              {"truePeakDb", r2(mtp)}}},
                        {"stereo", Json{{"lufsIntegrated", r2(gs.integratedLufs)},
                                        {"truePeakDbLo", r2(lo.truePeakDb)},
                                        {"truePeakDbRo", r2(ro.truePeakDb)},
                                        {"correlation", r2(corr)}}},
                        {"mono", Json{{"lufsIntegrated", r2(gmo.integratedLufs)},
                                      {"truePeakDb", r2(mm.truePeakDb)}}},
                        {"deltas", Json{{"stereoVsMultichannelLu", r2(stereoVsMulti)},
                                        {"monoVsStereoLu", r2(monoVsStereo)}}},
                        {"interpretation", interp}, {"boundsFlag", boundsFlag},
                        {"measuredSource", "render+samples (C++ BS.1770-4 gated)"},
                        {"warnings", warnings}};
#else
            (void)includeLfe; (void)boundsFlag;
            std::string targetLabel = "master";
            if (a.contains("target") && a["target"].is_string())
                targetLabel = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                targetLabel = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", targetLabel}, {"dryRun", dryRun},
                        {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for a live downmix "
                                 "check. The fold + loudness DSP is unit-tested host-side."}};
#endif
        }});

    // ---- analysis.loudness_timeline — momentary/short-term series + LRA detail -------------------
    reg.add(Tool{
        "analysis.loudness_timeline",
        "Loudness-over-time for a master/bus (READ-ONLY) — the data behind a loudness graph. Runs ONE "
        "bounded measure-don't-limit render (RENDER_STATS program block included for reference; "
        "snapshot + restore; temp deleted), then computes in C++ from the samples: MOMENTARY (400 ms) "
        "and SHORT-TERM (3 s) BS.1770-4 K-weighted series on a common hop (default 0.1 s; channel "
        "weights from the bed labels, LFE excluded), the maxima WITH their positions in time, and the "
        "EBU Tech 3342 loudness-range detail (gated 10th/95th percentiles). The series is capped at "
        "maxPoints (default 1000) by auto-coarsening the hop — a long selection still returns a "
        "readable curve. Times are seconds from the START of the analyzed range. Bound the range with "
        "boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time "
        "selection).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "hopSec":{"type":"number","minimum":0.02,"maximum":2,"default":0.1},
            "maxPoints":{"type":"integer","minimum":50,"maximum":4000,"default":1000},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"channels":{"type":"integer"},"layout":{"type":"string"},
            "hopSec":{"type":"number"},"points":{"type":"integer"},
            "times":{"type":"array"},"momentary":{"type":"array"},"shortTerm":{"type":"array"},
            "maxMomentary":{"type":"object"},"maxShortTerm":{"type":"object"},
            "lra":{"type":"object"},"program":{"type":"object"},
            "measuredSource":{"type":"string"},"boundsFlag":{"type":"integer"},
            "plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
            double hopSec = meter::clampd(optNum(a, "hopSec", 0.1), 0.02, 2.0);
            const int maxPoints = std::max(50, std::min(4000, optInt(a, "maxPoints", 1000)));
#ifdef REAPER_MCP_HAVE_SDK
            TargetInfo ti = resolveAnalysisTarget(a);
            if (!ti.ok) return ti.err;

            if (dryRun)
                return Json{{"dryRun", true}, {"target", ti.label}, {"channels", ti.channels},
                            {"layout", bedLayoutName(ti.channels)}, {"hopSec", hopSec},
                            {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → bounded measure-don't-limit render of the "
                                     "target → C++ momentary/short-term series + LRA percentiles from "
                                     "the samples (RENDER_STATS program block for reference) → delete "
                                     "temp → restore"}};

            TempRender tr = renderTargetToTempWav(ti.isTrack ? ti.track : -1, ti.channels,
                                                  /*measureLoudness*/ true, boundsFlag, a);
            if (!tr.ok) return makeError(tr.error, "analysis render/read failed", tr.remediation);

            Json warnings = Json::array();
            const double dur = tr.buf.sampleRate > 0.0
                                   ? (double)tr.buf.frames / tr.buf.sampleRate : 0.0;
            if (dur / hopSec > (double)maxPoints) {
                hopSec = dur / (double)maxPoints;
                warnings.push_back("hop coarsened to " + fmt2(hopSec) +
                                   " s to cap the series at " + std::to_string(maxPoints) +
                                   " points");
            }
            std::vector<double> w = bedChannelWeights(tr.buf.channels);
            meter::LoudnessTimeline tl = meter::loudnessTimeline(tr.buf, w, hopSec);
            if (!tl.valid)
                return makeError("range_too_short",
                                 "analyzed range is shorter than one 400 ms loudness block",
                                 "widen the bounds (startPos/endPos or the time selection)");
            if (dur < 3.0)
                warnings.push_back("range shorter than one full 3 s short-term window — shortTerm "
                                   "values use a growing window and the LRA detail is indicative "
                                   "only.");

            Json times = Json::array(), mom = Json::array(), st = Json::array();
            for (double v : tl.times) times.push_back(r2(v));
            for (double v : tl.momentary) mom.push_back(r2(v));
            for (double v : tl.shortTerm) st.push_back(r2(v));

            // REAPER program block for reference (LUFS-I / LRA / TP as the render engine saw them).
            Json program = Json::object();
            const Json& f0 = tr.statsFile;
            if (f0.contains("lufsIntegrated"))   program["lufsIntegrated"]   = f0["lufsIntegrated"];
            if (f0.contains("lufsShortTermMax")) program["lufsShortTermMax"] = f0["lufsShortTermMax"];
            if (f0.contains("lufsMomentaryMax")) program["lufsMomentaryMax"] = f0["lufsMomentaryMax"];
            if (f0.contains("loudnessRange"))    program["loudnessRange"]    = f0["loudnessRange"];
            if (f0.contains("truePeak"))         program["truePeak"]         = f0["truePeak"];

            return Json{{"target", ti.label}, {"channels", tr.buf.channels},
                        {"layout", bedLayoutName(tr.buf.channels)},
                        {"hopSec", r2(hopSec)}, {"points", (int)tl.times.size()},
                        {"times", times}, {"momentary", mom}, {"shortTerm", st},
                        {"maxMomentary", Json{{"lufs", r2(tl.maxMomentaryLufs)},
                                              {"timeSec", r2(tl.maxMomentaryTime)}}},
                        {"maxShortTerm", Json{{"lufs", r2(tl.maxShortTermLufs)},
                                              {"timeSec", r2(tl.maxShortTermTime)}}},
                        {"lra", Json{{"lowLufs", r2(tl.lraLowLufs)},
                                     {"highLufs", r2(tl.lraHighLufs)},
                                     {"rangeLu", r2(tl.lraLu)}}},
                        {"program", program}, {"boundsFlag", boundsFlag},
                        {"measuredSource", "render+samples (C++ BS.1770-4 windows)"},
                        {"warnings", warnings}};
#else
            (void)boundsFlag; (void)hopSec; (void)maxPoints;
            std::string targetLabel = "master";
            if (a.contains("target") && a["target"].is_string())
                targetLabel = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                targetLabel = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", targetLabel}, {"dryRun", dryRun},
                        {"times", Json::array()}, {"momentary", Json::array()},
                        {"shortTerm", Json::array()},
                        {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for a live timeline. "
                                 "The windowing/LRA DSP is unit-tested host-side (unit.meter)."}};
#endif
        }});

    // ================== Object loudness / binaural / decode-coverage meters ==================

    // ---- analysis.object_loudness — per-OBJECT gated loudness + position (Atmos object model) -----
    reg.add(Tool{
        "analysis.object_loudness",
        "Per-object loudness + position for a LIST of Atmos/immersive OBJECT tracks in ONE bounded "
        "render pass (READ-ONLY) — the object-model balance readout (objects are usually mono streams "
        "with a panner, unlike the layout beds analysis.stem_loudness targets). Renders the given "
        "object tracks as BIT-EXACT stems (no normalize/limit; snapshots + restores every RENDER_* + "
        "selection; temps deleted), then per object computes in C++: ITU-R BS.1770-4 GATED integrated "
        "loudness, ungated level, activity fraction, oversampled true peak (dBTP) and sample peak, each "
        "object's SHARE of the total object energy, and its offset from the LOUDEST object "
        "(deltaFromLoudestLu). Each object's live spatial POSITION is read best-effort from its "
        "panner/encoder FX (azimuth/elevation/distance) — who is loud AND where they are. Optionally "
        "pass bed:<track> to also meter the bed bus for a bed-vs-objects level reference. Bound the "
        "range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, "
        "2 = time selection); one synchronous render covers all objects, so keep the range short.",
        jparse(R"({"type":"object","properties":{
            "objects":{"type":"array","items":{"type":["integer","string"]},"minItems":1,"maxItems":128},
            "bed":{"type":["integer","string"]},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "required":["objects"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "objects":{"type":"array"},"loudest":{"type":"object"},"count":{"type":"integer"},
            "bed":{"type":["object","null"]},"bedVsObjects":{"type":["number","null"]},
            "measuredSource":{"type":"string"},"boundsFlag":{"type":"integer"},
            "interpretation":{"type":"string"},"plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            if (!a.contains("objects") || !a["objects"].is_array() || a["objects"].empty())
                throw std::runtime_error("missing/invalid array arg: objects");
            if (a["objects"].size() > 128)
                return makeError("too_many_objects", "more than 128 objects requested",
                                 "meter at most 128 objects per call");
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
#ifdef REAPER_MCP_HAVE_SDK
            struct Obj { int idx; int channels; std::string label; };
            std::vector<Obj> objs;
            const int ntr = CountTracks(kCur);
            for (const auto& e : a["objects"]) {
                MediaTrack* t = nullptr; int idx = -1;
                if (e.is_number()) { idx = e.get<int>(); t = GetTrack(kCur, idx); }
                else if (e.is_string()) {
                    const std::string want = e.get<std::string>();
                    for (int i = 0; i < ntr; ++i) {
                        MediaTrack* c = GetTrack(kCur, i);
                        if (c && trackNameStr(c) == want) { t = c; idx = i; break; }
                    }
                }
                if (!t)
                    return makeError("target_not_found", "object target not found: " + e.dump(),
                                     "pass valid track indices or names");
                Obj o; o.idx = idx; o.channels = trackChannels(t);
                const std::string nm = trackNameStr(t);
                o.label = nm.empty() ? ("track " + std::to_string(idx)) : nm;
                objs.push_back(o);
            }
            std::sort(objs.begin(), objs.end(),
                      [](const Obj& x, const Obj& y) { return x.idx < y.idx; });
            for (size_t i = 1; i < objs.size(); ++i)
                if (objs[i].idx == objs[i - 1].idx)
                    return makeError("duplicate_object", "track " + std::to_string(objs[i].idx) +
                                     " listed twice", "list each object once");
            int maxCh = 2;
            for (const auto& o : objs) if (o.channels > maxCh) maxCh = o.channels;

            bool haveBed = false; int bedTrack = -1, bedCh = 2; std::string bedLabel;
            if (a.contains("bed")) {
                Json ba = Json{{"target", a["bed"]}};
                TargetInfo bt = resolveAnalysisTarget(ba);
                if (!bt.ok) return bt.err;
                if (!bt.isTrack)
                    return makeError("invalid_bed_target",
                                     "bed must be a TRACK (index or name), not the master",
                                     "point at the bed bus track");
                haveBed = true; bedTrack = bt.track; bedCh = bt.channels; bedLabel = bt.label;
            }

            if (dryRun) {
                Json list = Json::array();
                for (const auto& o : objs)
                    list.push_back(Json{{"track", o.idx}, {"label", o.label},
                                        {"channels", o.channels}});
                return Json{{"dryRun", true}, {"objects", list}, {"count", (int)objs.size()},
                            {"bed", haveBed ? Json{{"track", bedTrack}, {"label", bedLabel}}
                                            : Json(nullptr)},
                            {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → render the " + std::to_string(objs.size()) +
                                     " object(s)" + (haveBed ? " + the bed" : "") + " as bit-exact "
                                     "stems → C++ BS.1770-4 gated loudness + dBTP + energy share per "
                                     "object, read each object's panner position → delete temps → "
                                     "restore"}};
            }

            std::vector<int> sel; std::vector<std::string> lbls;
            for (const auto& o : objs) { sel.push_back(o.idx); lbls.push_back(o.label); }
            MultiStemRender mr = renderTracksToTempWavs(sel, lbls, maxCh, /*measureLoudness*/ false,
                                                        boundsFlag, a);
            if (!mr.ok) return makeError(mr.error, "object render failed", mr.remediation);

            Json warnings = Json::array();
            if (mr.fileCount != objs.size())
                warnings.push_back("render emitted " + std::to_string(mr.fileCount) +
                                   " stem file(s) for " + std::to_string(objs.size()) +
                                   " object(s) — matched by track name; check bounds/selection.");

            std::vector<double> ungatedPow(objs.size(), 0.0);
            std::vector<meter::GatedLoudness> gl(objs.size());
            std::vector<double> tpv(objs.size(), meter::kMinDb()), spv(objs.size(), meter::kMinDb());
            double bestLufs = meter::kMinDb(); int bestIdx = -1; double totPow = 0.0;
            for (size_t i = 0; i < objs.size(); ++i) {
                if (!mr.haveBuf[i]) {
                    warnings.push_back("no stem rendered for object track " +
                                       std::to_string(objs[i].idx) + " — check bounds/selection");
                    continue;
                }
                const meter::AudioBuffer& buf = mr.bufs[i];
                std::vector<double> w = bedChannelWeights(objs[i].channels);
                w.resize((size_t)buf.channels, 1.0);
                gl[i] = meter::gatedLoudness(buf, w);
                const int usech = std::min(objs[i].channels, buf.channels);
                for (int c = 0; c < usech; ++c) {
                    meter::ChannelMetrics cm = meter::analyzeChannel(buf, c);
                    if (cm.truePeakDb > tpv[i]) tpv[i] = cm.truePeakDb;
                    if (cm.peakDb > spv[i]) spv[i] = cm.peakDb;
                }
                if (gl[i].ungatedLufs > meter::kMinDb() + 1.0)
                    ungatedPow[i] = std::pow(10.0, (gl[i].ungatedLufs + 0.691) / 10.0);
                totPow += ungatedPow[i];
                if (gl[i].integratedLufs > bestLufs) {
                    bestLufs = gl[i].integratedLufs; bestIdx = (int)i;
                }
            }

            Json objJson = Json::array();
            for (size_t i = 0; i < objs.size(); ++i) {
                if (!mr.haveBuf[i]) continue;
                const double share = (totPow > 1e-30) ? ungatedPow[i] / totPow : 0.0;
                Json pos = readObjectPosition(GetTrack(kCur, objs[i].idx));
                objJson.push_back(Json{
                    {"track", objs[i].idx}, {"label", objs[i].label}, {"channels", objs[i].channels},
                    {"lufsIntegrated", r2(gl[i].integratedLufs)},
                    {"ungatedLufs", r2(gl[i].ungatedLufs)},
                    {"activityFraction", r2(gl[i].activityFraction)},
                    {"truePeakDb", r2(tpv[i])}, {"samplePeakDb", r2(spv[i])},
                    {"energyShare", r2(share)},
                    {"deltaFromLoudestLu", r2(gl[i].integratedLufs - bestLufs)},
                    {"position", pos}});
            }

            Json bedJson = Json(nullptr); Json bedVsObjects = Json(nullptr);
            if (haveBed) {
                TempRender br = renderTargetToTempWav(bedTrack, bedCh, /*measureLoudness*/ false,
                                                      boundsFlag, a);
                if (br.ok) {
                    std::vector<double> bw = bedChannelWeights(bedCh);
                    bw.resize((size_t)br.buf.channels, 1.0);
                    meter::GatedLoudness bg = meter::gatedLoudness(br.buf, bw);
                    bedJson = Json{{"track", bedTrack}, {"label", bedLabel}, {"channels", bedCh},
                                   {"layout", bedLayoutName(bedCh)},
                                   {"lufsIntegrated", r2(bg.integratedLufs)}};
                    if (bestIdx >= 0 && bg.integratedLufs > meter::kMinDb() + 1.0)
                        bedVsObjects = r2(bg.integratedLufs - bestLufs);
                } else {
                    warnings.push_back("bed render failed (" + br.error + ") — bed context omitted");
                }
            }

            Json loudest = Json::object();
            if (bestIdx >= 0)
                loudest = Json{{"track", objs[(size_t)bestIdx].idx},
                               {"label", objs[(size_t)bestIdx].label},
                               {"lufsIntegrated", r2(bestLufs)}};
            std::string interp = bestIdx < 0
                ? std::string("no audible objects in the analyzed range")
                : ("loudest object '" + objs[(size_t)bestIdx].label + "' at " + fmt2(bestLufs) +
                   " LKFS; " + std::to_string((int)objJson.size()) + " object(s) metered" +
                   (haveBed && !bedVsObjects.is_null()
                        ? "; bed sits " + fmt2(bedVsObjects.get<double>()) +
                          " LU vs the loudest object"
                        : ""));

            return Json{{"objects", objJson}, {"loudest", loudest}, {"count", (int)objJson.size()},
                        {"bed", bedJson}, {"bedVsObjects", bedVsObjects},
                        {"boundsFlag", boundsFlag},
                        {"measuredSource", "render+samples (C++ BS.1770-4 gated)"},
                        {"interpretation", interp}, {"warnings", warnings}};
#else
            (void)boundsFlag;
            return Json{{"objects", Json::array()}, {"count", (int)a["objects"].size()},
                        {"dryRun", dryRun}, {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for live object loudness. "
                                 "The gated-loudness DSP is unit-tested host-side (unit.meter)."}};
#endif
        }});

    // ---- analysis.binaural_check — headphone/binaural deliverable QC (2-ch fold) -----------------
    reg.add(Tool{
        "analysis.binaural_check",
        "Binaural / headphone-deliverable QC for a 2-channel binaural monitor bus or stereo binaural "
        "render (READ-ONLY) — the headphone counterpart to analysis.downmix_check's loudspeaker fold. "
        "Point it at the spatial.add_binaural_monitor bus (or any 2-ch binaural render). Runs ONE "
        "bounded measure-don't-limit render (RENDER_STATS program loudness + true peak; snapshots + "
        "restores every RENDER_* + selection; temp deleted), then computes from the samples the "
        "binaural-specific metrics: L/R phase CORRELATION (image integrity / mono compatibility), the "
        "INTER-AURAL LEVEL BALANCE (L vs R K-weighted level — a lopsided or collapsed HRTF image), and "
        "the MID/SIDE ratio (near-mono collapse vs healthy width), plus per-ear RMS/peak/true-peak "
        "(headphone clip risk). Accepts a 2-ch stereo bus OR a wider binaural monitor bus (the "
        "add_binaural_monitor bus is sized to receive the ambisonic send with the decoded binaural on "
        "channels 1/2 — those two channels are measured); fails closed on a labeled loudspeaker BED "
        "(use analysis.downmix_check) or a mono target. Bound the range with boundsFlag (0 = custom "
        "startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"channels":{"type":"integer"},
            "program":{"type":"object"},"left":{"type":"object"},"right":{"type":"object"},
            "interAuralBalanceDb":{"type":"number"},"correlation":{"type":"number"},
            "midSide":{"type":"object"},"interpretation":{"type":"string"},
            "measuredSource":{"type":"string"},"boundsFlag":{"type":"integer"},
            "plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
#ifdef REAPER_MCP_HAVE_SDK
            TargetInfo ti = resolveAnalysisTarget(a);
            if (!ti.ok) return ti.err;
            // A binaural deliverable is a 2-ch L/R pair. The spatial.add_binaural_monitor bus is often
            // WIDER than 2 (sized to receive the ambisonic send) with the decoded binaural on channels
            // 1/2 — so accept any non-bed width >= 2 and measure channels 1/2; reject a labeled
            // loudspeaker BED (use downmix_check) and anything narrower than 2.
            const std::string blay = bedLayoutName(ti.channels);
            if (ti.channels < 2)
                return makeError("not_binaural",
                    "target has " + std::to_string(ti.channels) +
                        " channel(s) — binaural check needs a 2-channel L/R pair",
                    "point at the spatial.add_binaural_monitor bus or a 2-ch binaural render");
            if (blay != "stereo" && blay != "multichannel")
                return makeError("not_binaural",
                    "target is a " + std::to_string(ti.channels) + "-ch " + blay +
                        " loudspeaker bed, not a binaural pair",
                    "for a loudspeaker fold use analysis.downmix_check; for headphones point at the "
                    "spatial.add_binaural_monitor bus or a 2-ch binaural render");

            if (dryRun)
                return Json{{"dryRun", true}, {"target", ti.label}, {"channels", ti.channels},
                            {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → bounded measure-don't-limit render of the "
                                     "2-ch bus → RENDER_STATS program loudness/true-peak + C++ L/R "
                                     "correlation, inter-aural balance, mid/side ratio → delete temp "
                                     "→ restore"}};

            TempRender tr = renderTargetToTempWav(ti.isTrack ? ti.track : -1, 2,
                                                  /*measureLoudness*/ true, boundsFlag, a);
            if (!tr.ok) return makeError(tr.error, "analysis render/read failed", tr.remediation);

            Json warnings = Json::array();
            const int nc = std::min(2, tr.buf.channels);
            if (tr.buf.channels < 2)
                warnings.push_back("rendered buffer has " + std::to_string(tr.buf.channels) +
                                   " channel(s) — binaural metrics need 2; results may be partial.");
            if (ti.channels > 2)
                warnings.push_back("target bus is " + std::to_string(ti.channels) +
                                   "-ch — measuring channels 1/2 as the binaural L/R pair (the "
                                   "add_binaural_monitor convention puts the decoded binaural on ch 1/2).");

            Json program = Json::object();
            bool haveProg = false;
            if (tr.statsFile.contains("lufsIntegrated")) {
                program["lufsIntegrated"] = tr.statsFile["lufsIntegrated"];
                program["source"] = "render-stats"; haveProg = true;
            } else {
                meter::GatedLoudness g = meter::gatedLoudness(tr.buf, {1.0, 1.0});
                if (g.valid && g.integratedLufs > meter::kMinDb() + 1.0) {
                    program["lufsIntegrated"] = r2(g.integratedLufs);
                    program["source"] = "c++ bs1770 (RENDER_STATS had no loudness tokens)";
                    haveProg = true;
                }
            }
            if (tr.statsFile.contains("truePeak")) program["truePeak"] = tr.statsFile["truePeak"];
            if (tr.statsFile.contains("samplePeak")) program["samplePeak"] = tr.statsFile["samplePeak"];
            if (!haveProg)
                warnings.push_back("no program loudness measured — is the range bounded to audio?");

            meter::ChannelMetrics lm = meter::analyzeChannel(tr.buf, 0);
            meter::ChannelMetrics rm = (nc > 1) ? meter::analyzeChannel(tr.buf, 1) : lm;
            const double corr = (nc > 1) ? meter::interChannelCorrelation(tr.buf, 0, 1) : 1.0;
            const double balance = lm.kLevelLkfs - rm.kLevelLkfs;   // +ve = left louder

            const double q = 1.0 / std::sqrt(2.0);
            meter::AudioBuffer mid = meter::applyDownmixMatrix(tr.buf, {{q, q}});
            meter::AudioBuffer side = meter::applyDownmixMatrix(tr.buf, {{q, -q}});
            meter::ChannelMetrics mm = meter::analyzeChannel(mid, 0);
            meter::ChannelMetrics sm = meter::analyzeChannel(side, 0);
            const double sideToMid = sm.rmsDb - mm.rmsDb;

            std::string interp;
            if (corr < -0.2)
                interp = "OUT-OF-PHASE: L/R correlation " + fmt2(corr) + " — likely a decode/polarity "
                         "error (the headphone image will be unstable)";
            else if (sideToMid < -20.0)
                interp = "near-MONO image (side " + fmt2(sideToMid) + " dB below mid) — the binaural "
                         "decode is barely widening; confirm the HRTF/decoder is engaged";
            else if (std::fabs(balance) > 3.0)
                interp = "LOPSIDED image: " + fmt2(std::fabs(balance)) + " dB louder on the " +
                         (balance > 0 ? std::string("left") : std::string("right")) +
                         " — check the decode/panning symmetry";
            else
                interp = "healthy binaural image (correlation " + fmt2(corr) + ", balance " +
                         fmt2(balance) + " dB, side/mid " + fmt2(sideToMid) + " dB)";

            return Json{{"target", ti.label}, {"channels", ti.channels}, {"program", program},
                        {"left", Json{{"rmsDb", r2(lm.rmsDb)}, {"peakDb", r2(lm.peakDb)},
                                      {"truePeakDb", r2(lm.truePeakDb)},
                                      {"kLevelLkfs", r2(lm.kLevelLkfs)}}},
                        {"right", Json{{"rmsDb", r2(rm.rmsDb)}, {"peakDb", r2(rm.peakDb)},
                                       {"truePeakDb", r2(rm.truePeakDb)},
                                       {"kLevelLkfs", r2(rm.kLevelLkfs)}}},
                        {"interAuralBalanceDb", r2(balance)}, {"correlation", r2(corr)},
                        {"midSide", Json{{"midRmsDb", r2(mm.rmsDb)}, {"sideRmsDb", r2(sm.rmsDb)},
                                         {"sideToMidDb", r2(sideToMid)}}},
                        {"interpretation", interp}, {"boundsFlag", boundsFlag},
                        {"measuredSource", "render-stats + render+samples"},
                        {"warnings", warnings}};
#else
            (void)boundsFlag;
            std::string targetLabel = "master";
            if (a.contains("target") && a["target"].is_string())
                targetLabel = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                targetLabel = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", targetLabel}, {"dryRun", dryRun},
                        {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for a live binaural "
                                 "check. The correlation/mid-side DSP is unit-tested host-side."}};
#endif
        }});

    // ---- analysis.decode_coverage — ambisonic spatial coverage / balance map ---------------------
    reg.add(Tool{
        "analysis.decode_coverage",
        "Decode-coverage metering of an ACN/SN3D ambisonic scene bus (READ-ONLY) — the spatial "
        "complement of analysis.spatial_field's single DoA: instead of 'where is the dominant source' "
        "it answers 'how EVENLY is the whole sphere covered — are there directional holes, is the mix "
        "front-heavy or missing height'. Runs ONE bounded BIT-EXACT render (snapshots + restores every "
        "RENDER_* + selection; temp deleted), decodes the scene onto a virtual-speaker set with the "
        "real SN3D spherical harmonics (higher order => sharper), and reports the per-direction ENERGY "
        "distribution: a UNIFORMITY index (normalized entropy, 1 = perfectly even), coefficient of "
        "variation, the energy CONCENTRATION in the loudest ~10% of directions, the count of DEAD "
        "ZONES (directions below deadZoneFrac of the mean), the front/back · left/right · upper/lower · "
        "ear-level energy balance, the dominant direction, and the top energy hotspots. Default decode "
        "is a uniform Fibonacci sphere of `speakers` points; pass layout ('5.1'/'7.1'/'7.1.4'/'9.1.6') "
        "to decode to that delivery array's nominal directions and get per-speaker energy instead. "
        "Order inferred from channel count ((order+1)²); pass normalization='n3d' if the bus is N3D. "
        "Fails closed if the target is not a valid ambisonic width (≥4 ch). Bound with boundsFlag "
        "(0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "normalization":{"type":"string","enum":["sn3d","n3d"],"default":"sn3d"},
            "order":{"type":"integer","minimum":1},
            "speakers":{"type":"integer","minimum":12,"maximum":512,"default":64},
            "layout":{"type":"string","enum":["5.1","7.1","7.1.4","9.1.6"]},
            "deadZoneFrac":{"type":"number","minimum":0,"maximum":1,"default":0.1},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"channels":{"type":"integer"},"order":{"type":"integer"},
            "acnUsed":{"type":"integer"},"normalization":{"type":"string"},
            "mode":{"type":"string"},"speakers":{"type":"integer"},
            "uniformity":{"type":"number"},"coefficientOfVariation":{"type":"number"},
            "concentrationTop":{"type":"number"},"deadZones":{"type":"integer"},
            "balance":{"type":"object"},"dominant":{"type":"object"},
            "hotspots":{"type":"array"},"perSpeaker":{"type":["array","null"]},
            "interpretation":{"type":"string"},"measuredSource":{"type":"string"},
            "boundsFlag":{"type":"integer"},"plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
            const std::string norm = toLower(optStr(a, "normalization", "sn3d"));
            const bool isN3D = (norm == "n3d");
            const int orderOverride = optInt(a, "order", 0);
            const int nSpeakers = std::max(12, std::min(512, optInt(a, "speakers", 64)));
            const double deadZoneFrac = meter::clampd(optNum(a, "deadZoneFrac", 0.1), 0.0, 1.0);
            const std::string layout = (a.contains("layout") && a["layout"].is_string())
                                           ? a["layout"].get<std::string>() : std::string();

            std::vector<meter::Dir> dirs;
            std::vector<std::string> spkLabels;
            std::string mode;
            if (!layout.empty()) {
                std::vector<NamedSpeaker> ns; std::string lerr;
                if (!namedLayoutDirs(layout, ns, lerr))
                    return makeError("unknown_layout", lerr, "use 5.1, 7.1, 7.1.4, 9.1.6, or omit");
                for (const auto& s : ns) { dirs.push_back(s.dir); spkLabels.push_back(s.label); }
                mode = "layout:" + layout;
            } else {
                dirs = meter::fibonacciSphere(nSpeakers);
                mode = "uniform-sphere";
            }

            std::string targetLabel = "master";
            int targetTrack = -1; int channels = 2;
#ifdef REAPER_MCP_HAVE_SDK
            TargetInfo ti = resolveAnalysisTarget(a);
            if (!ti.ok) return ti.err;
            targetLabel = ti.label; targetTrack = ti.track; channels = ti.channels;

            const int inferredOrder = meter::ambiOrderFromChannels(channels);
            if (channels < 4 || inferredOrder < 1)
                return makeError("not_ambisonic",
                    "target has " + std::to_string(channels) + " channels — not a valid ambisonic bus",
                    "point at an ACN/SN3D scene bus with (order+1)² channels (4/9/16/25…)");

            if (dryRun)
                return Json{{"dryRun", true}, {"target", targetLabel}, {"channels", channels},
                            {"order", orderOverride > 0 ? orderOverride : inferredOrder},
                            {"normalization", norm}, {"mode", mode}, {"speakers", (int)dirs.size()},
                            {"boundsFlag", boundsFlag},
                            {"plan", "snapshot RENDER_* → bounded BIT-EXACT render of the scene bus → "
                                     "decode onto " + std::to_string(dirs.size()) + " virtual "
                                     "speaker(s) with real SN3D SH → per-direction energy → coverage "
                                     "uniformity/dead-zones/zone-balance → delete temp → restore"}};

            TempRender tr = renderTargetToTempWav(targetTrack, channels, /*measureLoudness*/ false,
                                                  boundsFlag, a);
            if (!tr.ok) return makeError(tr.error, "analysis render/read failed", tr.remediation);

            const int useOrder = orderOverride > 0 ? orderOverride : 0;   // 0 => infer in the decoder
            std::vector<double> E = meter::decodeSpeakerEnergies(tr.buf, 0, tr.buf.frames, dirs,
                                                                 useOrder, isN3D);
            meter::CoverageStats c = meter::computeCoverageStats(dirs, E, deadZoneFrac);
            if (!c.valid)
                return makeError("no_energy",
                    "the scene carried no measurable energy in the analyzed range",
                    "bound the range to audio (boundsFlag/startPos/endPos) and confirm the bus is an "
                    "encoded ambisonic scene");
            Json warnings = Json::array();
            const int usedOrder = orderOverride > 0 ? orderOverride
                                                    : meter::ambiOrderFromChannels(tr.buf.channels);
            const int acnUsed = std::min(tr.buf.channels, (usedOrder + 1) * (usedOrder + 1));

            double tot = 0.0; for (double e : E) tot += e;
            std::vector<size_t> idxByEnergy(E.size());
            for (size_t i = 0; i < E.size(); ++i) idxByEnergy[i] = i;
            std::sort(idxByEnergy.begin(), idxByEnergy.end(),
                      [&E](size_t x, size_t y) { return E[x] > E[y]; });
            Json hotspots = Json::array();
            for (size_t r = 0; r < idxByEnergy.size() && r < 5; ++r) {
                const size_t s = idxByEnergy[r];
                double az = 0, el = 0; meter::vecToAzEl(dirs[s], az, el);
                Json h{{"azimuthDeg", r2(az)}, {"elevationDeg", r2(el)},
                       {"energyFrac", r2(tot > 0 ? E[s] / tot : 0.0)}};
                if (s < spkLabels.size()) h["label"] = spkLabels[s];
                hotspots.push_back(h);
            }

            Json perSpeaker = Json(nullptr);
            if (!layout.empty()) {
                perSpeaker = Json::array();
                for (size_t s = 0; s < dirs.size(); ++s) {
                    double az = 0, el = 0; meter::vecToAzEl(dirs[s], az, el);
                    perSpeaker.push_back(Json{
                        {"label", s < spkLabels.size() ? spkLabels[s] : std::to_string(s)},
                        {"azimuthDeg", r2(az)}, {"elevationDeg", r2(el)},
                        {"energyFrac", r2(tot > 0 ? E[s] / tot : 0.0)}});
                }
            }

            // Interpretation keys off the coefficient of variation + dead zones (a first-order decode
            // is broad, so the normalized-entropy uniformity compresses near 1 and discriminates
            // poorly — CV separates a point source from a diffuse field cleanly at every order).
            std::string interp;
            const double deadFrac = c.speakers > 0 ? (double)c.deadZones / (double)c.speakers : 0.0;
            if (c.cv < 0.35 && deadFrac < 0.02)
                interp = "very even coverage (CV " + fmt2(c.cv) + ", uniformity " +
                         fmt2(c.uniformity) + ") — diffuse/enveloping field";
            else if (c.cv < 0.8)
                interp = "balanced coverage (CV " + fmt2(c.cv) + ", uniformity " +
                         fmt2(c.uniformity) + ") with " + std::to_string(c.deadZones) +
                         " weak direction(s)";
            else
                interp = "directional/uneven coverage (CV " + fmt2(c.cv) + "): energy concentrates "
                         "toward az " + fmt2(c.dominantAzDeg) + "°, el " + fmt2(c.dominantElDeg) +
                         "°, with " + std::to_string(c.deadZones) + " dead zone(s)";
            const bool hasHeights = layout.empty() || layout == "7.1.4" || layout == "9.1.6";
            if (hasHeights && c.upperFrac < 0.05)
                warnings.push_back("almost no upper-hemisphere energy (fraction " +
                                   fmt2(c.upperFrac) + ") — the scene is essentially planar");

            return Json{{"target", targetLabel}, {"channels", channels}, {"order", usedOrder},
                        {"acnUsed", acnUsed}, {"normalization", norm}, {"mode", mode},
                        {"speakers", (int)dirs.size()},
                        {"uniformity", r2(c.uniformity)}, {"coefficientOfVariation", r2(c.cv)},
                        {"concentrationTop", r2(c.concentrationTop)}, {"deadZones", c.deadZones},
                        {"balance", Json{{"frontFrac", r2(c.frontFrac)}, {"backFrac", r2(c.backFrac)},
                                         {"leftFrac", r2(c.leftFrac)}, {"rightFrac", r2(c.rightFrac)},
                                         {"upperFrac", r2(c.upperFrac)}, {"lowerFrac", r2(c.lowerFrac)},
                                         {"earLevelFrac", r2(c.horizFrac)}}},
                        {"dominant", Json{{"azimuthDeg", r2(c.dominantAzDeg)},
                                          {"elevationDeg", r2(c.dominantElDeg)}}},
                        {"hotspots", hotspots}, {"perSpeaker", perSpeaker},
                        {"interpretation", interp}, {"boundsFlag", boundsFlag},
                        {"measuredSource", "render+samples (C++ SN3D decode)"},
                        {"warnings", warnings}};
#else
            (void)boundsFlag; (void)isN3D; (void)orderOverride; (void)deadZoneFrac;
            (void)targetTrack; (void)channels;
            if (a.contains("target") && a["target"].is_string())
                targetLabel = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                targetLabel = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", targetLabel}, {"normalization", norm}, {"mode", mode},
                        {"speakers", (int)dirs.size()}, {"dryRun", dryRun},
                        {"measuredSource", "none (host build)"},
                        {"note", "host build cannot render; run under REAPER for live decode coverage. "
                                 "The decode + coverage DSP is unit-tested host-side (unit.meter)."}};
#endif
        }});

    // ---- analysis.adm_inspect — parse + summarize an ADM (BS.2076) BWF deliverable (read-only) ------
    reg.add(Tool{
        "analysis.adm_inspect",
        "Parse + summarize an ADM (ITU-R BS.2076) Broadcast-Wave deliverable (READ-ONLY). Read the file "
        "at 'path', decode its chna (channel-allocation) + axml (ADM XML) chunks, and report: the "
        "container (RIFF/WAVE vs BW64/RF64), the PCM format (channels / sampleRate / bitDepth / frames / "
        "duration), the chna track table (audioTrackUID + track/pack refs per data channel), and an ADM "
        "structure summary (audioObject / audioPackFormat / audioChannelFormat / audioBlockFormat counts, "
        "coordinate mode spherical|cartesian, DirectSpeakers/Objects presence, object names, programme "
        "name, BS.2076 version, and the per-object metadata footprint — extent / objectDivergence / "
        "importance presence + block counts + the importance values). Round-trips spatial.export_adm and QCs ANY third-party ADM BWF (Dolby / "
        "Nuendo / EBU). This is a structural summarizer, not an XSD validator — it flags missing or "
        "inconsistent chunks (e.g. chna numTracks != fmt channels). 'path' may be absolute or relative "
        "to the project directory. SDK-free — the parser is host-unit-tested (unit.adm).",
        jparse(R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "container":{"type":"string"},"isAdm":{"type":"boolean"},"format":{"type":"object"},
            "chunks":{"type":"array"},"chna":{"type":["object","null"]},"adm":{"type":["object","null"]},
            "warnings":{"type":"array","items":{"type":"string"}},"path":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const std::string path = reqStr(a, "path");
            std::string resolved = path;
#ifdef REAPER_MCP_HAVE_SDK
            if (!path.empty() && path[0] != '/') {  // relative -> project directory
                char pbuf[4096] = {0};
                GetProjectPath(pbuf, (int)sizeof(pbuf));
                if (pbuf[0]) resolved = std::string(pbuf) + "/" + path;
            }
#endif
            std::ifstream is(resolved, std::ios::binary);
            if (!is)
                return makeError("file_not_found", "could not open ADM file: " + resolved,
                                 "pass a readable path (absolute, or relative to the project directory)");
            std::string bytes((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            is.close();
            adm::ParseResult pr = adm::parseAdmImage(bytes);
            if (!pr.ok)
                return makeError(pr.error.empty() ? "parse_failed" : pr.error,
                                 "not a readable WAV / BW64 file", "check the path points at a WAV/BWF");
            Json out = pr.summary;
            out["path"] = resolved;
            return out;
        }});

    // ---- analysis.adm_profile_check — Dolby Atmos Master ADM Profile conformance (read-only) ---------
    reg.add(Tool{
        "analysis.adm_profile_check",
        "Check an ADM (ITU-R BS.2076) Broadcast-Wave against the Dolby Atmos Master ADM Profile v1.0 "
        "(READ-ONLY) — the constraints that make an ADM BWF ingestable by a certified Dolby renderer AS "
        "an Atmos master (Dolby's Conversion Tool round-trips a conformant file to/from a DAMF). Reads "
        "the file at 'path', parses its chna+axml (like analysis.adm_inspect), and reports 'conformant' "
        "plus a list of per-rule 'violations' (each a code + detail): sample_rate (must be 48000); "
        "bed_layout / channel_cap (<=128) / object_cap (<=118); programme_id (must be APR_1001); "
        "coordinate (objects must be cartesian); extent (width=depth=height in [0,1]); divergence "
        "(objectDivergence is PROHIBITED); importance (only on inactive objects); interpolation (the 0 "
        "then 250-sample ramp). Point it at ANY ADM BWF — spatial.export_adm's profile:\"dolby-atmos\" "
        "output, or a third-party (Nuendo / Pro Tools / Dolby) file — to QC it before delivery. 'path' "
        "may be absolute or relative to the project directory. SDK-free — the validator is "
        "host-unit-tested (unit.adm_profile). Structural conformance to the published profile, NOT a "
        "guarantee a specific renderer ingests the file (that is the ingest check on your tools).",
        jparse(R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "profile":{"type":"string"},"conformant":{"type":"boolean"},
            "violations":{"type":"array"},"noteCount":{"type":"integer"},"notes":{"type":"array"},
            "adm":{"type":["object","null"]},"format":{"type":"object"},"isAdm":{"type":"boolean"},
            "path":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const std::string path = reqStr(a, "path");
            std::string resolved = path;
#ifdef REAPER_MCP_HAVE_SDK
            if (!path.empty() && path[0] != '/') {  // relative -> project directory
                char pbuf[4096] = {0};
                GetProjectPath(pbuf, (int)sizeof(pbuf));
                if (pbuf[0]) resolved = std::string(pbuf) + "/" + path;
            }
#endif
            std::ifstream is(resolved, std::ios::binary);
            if (!is)
                return makeError("file_not_found", "could not open ADM file: " + resolved,
                                 "pass a readable path (absolute, or relative to the project directory)");
            std::string bytes((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            is.close();
            adm::ParseResult pr = adm::parseAdmImage(bytes);
            if (!pr.ok)
                return makeError(pr.error.empty() ? "parse_failed" : pr.error,
                                 "not a readable WAV / BW64 file", "check the path points at a WAV/BWF");
            adm::profile::Report rep = adm::profile::validateParsed(pr);
            Json out = rep.toJson();
            out["path"] = resolved;
            out["isAdm"] = pr.summary["isAdm"];
            out["format"] = pr.summary["format"];
            out["adm"] = pr.summary["adm"];
            return out;
        }});

    // ---- analysis.damf_inspect — parse + summarize a Dolby Atmos Master File (DAMF) triad (read-only) -
    reg.add(Tool{
        "analysis.damf_inspect",
        "Parse + summarize a Dolby Atmos Master File (DAMF) triad (READ-ONLY) — the Dolby Atmos Renderer's "
        "native master, the sibling of ADM BWF (see spatial.export_damf). Point 'path' at the '.atmos' "
        "manifest (or either sibling — the base is derived); the tool reads all three files and reports: "
        "the manifest roster (bedInstances / bed channel count / object count), the metadata (sampleRate + "
        "position-event count), and the CAF essence (format / channels / bitDepth / sampleRate / frames / "
        "duration / endianness). It warns when the CAF channel count disagrees with the bed+object roster "
        "or the essence is little-endian (Dolby masters are big-endian PCM). Round-trips spatial.export_damf "
        "and QCs a third-party triad (Pro Tools / Nuendo / DaVinci / Dolby Conversion Tool). 'path' may be "
        "absolute or relative to the project directory. SDK-free — host-unit-tested (unit.damf). A "
        "structural summarizer, NOT a certified-renderer ingest guarantee.",
        jparse(R"({"type":"object","properties":{"path":{"type":"string"}},
            "required":["path"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "isDamf":{"type":"boolean"},"manifest":{"type":"object"},"metadata":{"type":"object"},
            "audio":{"type":"object"},"warnings":{"type":"array","items":{"type":"string"}},
            "path":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const std::string path = reqStr(a, "path");
            std::string resolved = path;
#ifdef REAPER_MCP_HAVE_SDK
            if (!path.empty() && path[0] != '/') {  // relative -> project directory
                char pbuf[4096] = {0};
                GetProjectPath(pbuf, (int)sizeof(pbuf));
                if (pbuf[0]) resolved = std::string(pbuf) + "/" + path;
            }
#endif
            auto hasSuf = [](const std::string& s, const std::string& suf) {
                return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
            };
            std::string base = resolved;  // normalize to the '.atmos' manifest path
            if (hasSuf(base, ".atmos.audio")) base = base.substr(0, base.size() - 6);
            else if (hasSuf(base, ".atmos.metadata")) base = base.substr(0, base.size() - 9);
            if (!hasSuf(base, ".atmos")) base += ".atmos";
            const std::string metadataPath = base + ".metadata";
            const std::string audioPath = base + ".audio";
            auto readAll = [](const std::string& p, std::string& out) -> bool {
                std::ifstream is(p, std::ios::binary);
                if (!is) return false;
                out.assign((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
                return true;
            };
            std::string atmos, meta, caf;
            if (!readAll(base, atmos))
                return makeError("file_not_found", "could not open the .atmos manifest: " + base,
                                 "pass the path to the '.atmos' file (absolute or project-relative)");
            readAll(metadataPath, meta);  // best-effort; inspect flags absence
            readAll(audioPath, caf);
            Json out = damf::inspectTriad(atmos, meta, caf);
            out["path"] = base;
            if (meta.empty()) out["warnings"].push_back("missing sibling: " + metadataPath);
            if (caf.empty()) out["warnings"].push_back("missing sibling: " + audioPath);
            return out;
        }});

    // ---- analysis.read_samples — render-free direct PCM read + per-channel stats -----------------
    reg.add(Tool{
        "analysis.read_samples",
        "Direct, render-free PCM read of a track's item content or a take's source via a REAPER audio "
        "accessor (READ-ONLY). Reads a bounded window with NO render round-trip and reports per-channel "
        "sample peak / RMS / oversampled true-peak (dBTP) / K-weighted level, DC offset and full-scale "
        "clip count, plus an optional decimated min/max waveform overview. A TRACK target reads the "
        "summed media-item/take content BEFORE track FX/volume/pan; add itemIndex (and optional "
        "takeIndex) to read one take's source through its take FX. Window defaults to the whole source "
        "extent; bound it with start/duration (seconds) and set sampleRate to resample the read. "
        "Frame-accurate and fast — for post-FX bus/master metering use analysis.meter (which renders).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "itemIndex":{"type":"integer"},"takeIndex":{"type":"integer"},
            "start":{"type":"number","default":0},"duration":{"type":"number"},
            "sampleRate":{"type":"integer"},
            "overview":{"type":"boolean","default":false},
            "overviewBuckets":{"type":"integer","default":512,"minimum":1,"maximum":8192},
            "clipThreshold":{"type":"number","default":0.999},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"source":{"type":"string"},
            "channels":{"type":"integer"},"sampleRate":{"type":"integer"},
            "frames":{"type":"number"},"duration":{"type":"number"},
            "window":{"type":"object"},"sourceExtent":{"type":"object"},
            "clamped":{"type":"boolean"},"silent":{"type":"boolean"},
            "channelsDetail":{"type":"array"},"overview":{"type":"object"},
            "measuredSource":{"type":"string"},"plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "warnings":{"type":"array"},"error":{"type":"string"},"detail":{"type":"string"},
            "remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
#ifdef REAPER_MCP_HAVE_SDK
            AccTarget t = resolveAccessorTarget(a);
            if (!t.ok) return t.err;
            if (dryRun)
                return Json{{"dryRun", true}, {"target", t.label}, {"source", t.source},
                            {"plan", "create a " + t.source + " audio accessor -> read the bounded "
                                     "window (no render) -> per-channel peak/rms/true-peak/K-level + DC "
                                     "+ clip count (+ optional waveform overview) -> destroy accessor"}};
            AccessorRead r = readAccTarget(t, a);
            if (!r.ok) return makeError(r.error, "accessor read failed", r.remediation);

            const int nc = r.buf.channels;
            const std::vector<std::string> labels = bedChannelLabels(nc);
            const double clipThr = optNum(a, "clipThreshold", 0.999);
            Json chDetail = Json::array();
            for (int c = 0; c < nc; ++c) {
                meter::ChannelMetrics m = meter::analyzeChannel(r.buf, c);
                chDetail.push_back(Json{
                    {"index", c},
                    {"label", c < (int)labels.size() ? labels[c] : ("ch" + std::to_string(c))},
                    {"isLFE", bedChannelIsLFE(nc, c)},
                    {"peakDb", r2(m.peakDb)}, {"rmsDb", r2(m.rmsDb)},
                    {"truePeakDb", r2(m.truePeakDb)}, {"kLevelLkfs", r2(m.kLevelLkfs)},
                    {"dcOffset", r4(meter::channelDcOffset(r.buf, c))},
                    {"clipCount", (double)meter::channelClipCount(r.buf, c, clipThr)}});
            }
            Json out = accWindowBlock(r);
            out["target"] = t.label; out["source"] = t.source;
            out["channelsDetail"] = chDetail;
            out["measuredSource"] = "accessor";
            if (optBool(a, "overview", false)) {
                const int buckets = optInt(a, "overviewBuckets", 512);
                Json ovCh = Json::array();
                for (int c = 0; c < nc; ++c) {
                    auto bk = meter::channelOverview(r.buf, c, buckets);
                    Json arr = Json::array();
                    for (const auto& b : bk) arr.push_back(Json{{"min", r4(b.min)}, {"max", r4(b.max)}});
                    ovCh.push_back(arr);
                }
                out["overview"] = Json{{"buckets", ovCh.empty() ? 0 : (int)ovCh[0].size()},
                                       {"channels", ovCh}};
            }
            Json warnings = Json::array();
            if (r.clamped) warnings.push_back("window was clamped to the source extent (or the 60 s cap)");
            if (r.silent) warnings.push_back("accessor returned silence across the whole window");
            out["warnings"] = warnings;
            return out;
#else
            std::string label = "track";
            if (a.contains("target") && a["target"].is_string()) label = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                label = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", label}, {"source", a.contains("itemIndex") ? "take" : "track"},
                        {"measuredSource", "none (host build)"}, {"dryRun", dryRun},
                        {"channelsDetail", Json::array()},
                        {"note", "host build has no REAPER audio accessor; run under REAPER for a live "
                                 "read. The per-channel/overview/DC/clip DSP is unit-tested (unit.meter)."}};
#endif
        }});

    // ---- analysis.accessor_meter — render-free per-channel meter of item/take content ------------
    reg.add(Tool{
        "analysis.accessor_meter",
        "Full per-channel meter of a track's item content or a take's source, render-free, via a "
        "REAPER audio accessor (READ-ONLY). Reports per-channel RMS / sample peak / oversampled "
        "true-peak (dBTP) / K-weighted level with SMPTE bed-layout labels (LFE flagged), L/R phase "
        "correlation, and BS.1770-4 GATED loudness (integrated + ungated + activity fraction) computed "
        "in-box from the samples — no RENDER_STATS, no render round-trip. A TRACK target measures the "
        "summed item/take content BEFORE track FX/volume/pan; add itemIndex (+ optional takeIndex) for "
        "one take. The fast, frame-accurate accessor sibling of analysis.meter — use analysis.meter for "
        "the post-FX bus/master (it renders and adds REAPER-native program loudness).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "itemIndex":{"type":"integer"},"takeIndex":{"type":"integer"},
            "start":{"type":"number","default":0},"duration":{"type":"number"},
            "sampleRate":{"type":"integer"},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"source":{"type":"string"},"layout":{"type":"string"},
            "channels":{"type":"integer"},"sampleRate":{"type":"integer"},
            "frames":{"type":"number"},"duration":{"type":"number"},
            "window":{"type":"object"},"sourceExtent":{"type":"object"},
            "clamped":{"type":"boolean"},"silent":{"type":"boolean"},
            "loudness":{"type":"object"},"channelsDetail":{"type":"array"},"downmix":{"type":"object"},
            "measuredSource":{"type":"string"},"plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "warnings":{"type":"array"},"error":{"type":"string"},"detail":{"type":"string"},
            "remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
#ifdef REAPER_MCP_HAVE_SDK
            AccTarget t = resolveAccessorTarget(a);
            if (!t.ok) return t.err;
            if (dryRun)
                return Json{{"dryRun", true}, {"target", t.label}, {"source", t.source},
                            {"plan", "create a " + t.source + " audio accessor -> read the window (no "
                                     "render) -> per-channel level/peak/true-peak/K-level + L/R "
                                     "correlation + BS.1770-4 gated loudness -> destroy accessor"}};
            AccessorRead r = readAccTarget(t, a);
            if (!r.ok) return makeError(r.error, "accessor read failed", r.remediation);

            const int nc = r.buf.channels;
            const std::vector<std::string> labels = bedChannelLabels(nc);
            Json chDetail = Json::array();
            for (int c = 0; c < nc; ++c) {
                meter::ChannelMetrics m = meter::analyzeChannel(r.buf, c);
                chDetail.push_back(Json{
                    {"index", c},
                    {"label", c < (int)labels.size() ? labels[c] : ("ch" + std::to_string(c))},
                    {"isLFE", bedChannelIsLFE(nc, c)},
                    {"rmsDb", r2(m.rmsDb)}, {"peakDb", r2(m.peakDb)},
                    {"truePeakDb", r2(m.truePeakDb)}, {"kLevelLkfs", r2(m.kLevelLkfs)}});
            }
            Json downmix = Json::object();
            if (nc >= 2) {
                const double corr = meter::interChannelCorrelation(r.buf, 0, 1);
                downmix["lrCorrelation"] = r2(corr);
                downmix["note"] = corr < -0.2
                    ? std::string("L/R strongly out of phase — mono fold-down will partially cancel")
                    : (corr < 0.4 ? std::string("wide/decorrelated stereo — check mono compatibility")
                                  : std::string("mono-compatible"));
            }
            meter::GatedLoudness g = meter::gatedLoudness(r.buf, bedChannelWeights(nc));
            Json loudness = Json::object();
            if (g.valid)
                loudness = Json{{"integratedLufs", r2(g.integratedLufs)},
                                {"ungatedLufs", r2(g.ungatedLufs)},
                                {"activityFraction", r2(g.activityFraction)},
                                {"gatedBlocks", g.gatedBlocks}, {"totalBlocks", g.totalBlocks}};

            Json out = accWindowBlock(r);
            out["target"] = t.label; out["source"] = t.source; out["layout"] = bedLayoutName(nc);
            out["loudness"] = loudness; out["channelsDetail"] = chDetail; out["downmix"] = downmix;
            out["measuredSource"] = "accessor";
            Json warnings = Json::array();
            if (!t.isTake)
                warnings.push_back("track accessor measures item content PRE track-FX/volume/pan — use "
                                   "analysis.meter for the post-FX signal");
            if (r.clamped) warnings.push_back("window was clamped to the source extent (or the 60 s cap)");
            if (r.silent) warnings.push_back("accessor returned silence across the whole window");
            out["warnings"] = warnings;
            return out;
#else
            std::string label = "track";
            if (a.contains("target") && a["target"].is_string()) label = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                label = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", label}, {"source", a.contains("itemIndex") ? "take" : "track"},
                        {"measuredSource", "none (host build)"}, {"dryRun", dryRun},
                        {"channelsDetail", Json::array()},
                        {"note", "host build cannot read a live accessor; run under REAPER. The "
                                 "per-channel + correlation + gated-loudness DSP is unit-tested "
                                 "(unit.meter)."}};
#endif
        }});

    // ---- analysis.detect_silence — frame-accurate dead-air / gap detection (accessor) ------------
    reg.add(Tool{
        "analysis.detect_silence",
        "Frame-accurate silence detection on a track's item content or a take's source, render-free, "
        "via a REAPER audio accessor (READ-ONLY). Reads a bounded window (no render) and reports "
        "leading and trailing silence, internal silent gaps, and the content-bearing span for trimming "
        "— useful for finding dead air, top/tail points, and gaps between phrases. A region is silent "
        "where the trailing-window cross-channel RMS stays below thresholdDb for at least minSilenceSec; "
        "windowMs smooths the detector. A TRACK target reads item content pre track-FX; add itemIndex "
        "(+ optional takeIndex) for one take. Times are in the accessor's time base (project time for a "
        "track, source time for a take).",
        jparse(R"({"type":"object","properties":{
            "target":{"type":["integer","string"]},
            "itemIndex":{"type":"integer"},"takeIndex":{"type":"integer"},
            "start":{"type":"number","default":0},"duration":{"type":"number"},
            "sampleRate":{"type":"integer"},
            "thresholdDb":{"type":"number","default":-60},
            "minSilenceSec":{"type":"number","default":0.3},
            "windowMs":{"type":"number","default":10},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "target":{"type":"string"},"source":{"type":"string"},
            "sampleRate":{"type":"integer"},"frames":{"type":"number"},"duration":{"type":"number"},
            "window":{"type":"object"},"sourceExtent":{"type":"object"},
            "clamped":{"type":"boolean"},"silent":{"type":"boolean"},
            "thresholdDb":{"type":"number"},"minSilenceSec":{"type":"number"},
            "fullySilent":{"type":"boolean"},
            "leadingSilenceSec":{"type":"number"},"trailingSilenceSec":{"type":"number"},
            "internalGaps":{"type":"array"},"contentSpan":{"type":"object"},
            "measuredSource":{"type":"string"},"plan":{"type":"string"},"dryRun":{"type":"boolean"},
            "warnings":{"type":"array"},"error":{"type":"string"},"detail":{"type":"string"},
            "remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const double threshDb = optNum(a, "thresholdDb", -60.0);
            const double minSil = optNum(a, "minSilenceSec", 0.3);
#ifdef REAPER_MCP_HAVE_SDK
            const double winMs = optNum(a, "windowMs", 10.0);
            AccTarget t = resolveAccessorTarget(a);
            if (!t.ok) return t.err;
            if (dryRun)
                return Json{{"dryRun", true}, {"target", t.label}, {"source", t.source},
                            {"thresholdDb", threshDb}, {"minSilenceSec", minSil},
                            {"plan", "create a " + t.source + " audio accessor -> read the window (no "
                                     "render) -> windowed cross-channel RMS silence scan -> leading/"
                                     "trailing/internal gaps + content span -> destroy accessor"}};
            AccessorRead r = readAccTarget(t, a);
            if (!r.ok) return makeError(r.error, "accessor read failed", r.remediation);

            const double sr = r.buf.sampleRate > 0.0 ? r.buf.sampleRate : 48000.0;
            const size_t winFrames = (size_t)std::max((long long)1, std::llround(winMs / 1000.0 * sr));
            const size_t minFrames = (size_t)std::max((long long)1, std::llround(minSil * sr));
            const auto regions = meter::scanSilence(r.buf, threshDb, winFrames, minFrames);
            const size_t N = r.buf.frames;
            auto f2t = [&](size_t f) { return r.readStart + (double)f / sr; };

            double leading = 0.0, trailing = 0.0;
            bool fullySilent = false;
            double trimStart = r.readStart, trimEnd = r.readStart + r.readDur;
            Json internal = Json::array();
            for (const auto& reg : regions) {
                const bool atStart = (reg.startFrame == 0);
                const bool atEnd = (reg.endFrame >= N);
                if (atStart && atEnd) { fullySilent = true; leading = r.readDur; }
                else if (atStart) { leading = (double)reg.endFrame / sr; trimStart = f2t(reg.endFrame); }
                else if (atEnd) { trailing = (double)(N - reg.startFrame) / sr; trimEnd = f2t(reg.startFrame); }
                else internal.push_back(Json{{"startSec", r2(f2t(reg.startFrame))},
                                             {"endSec", r2(f2t(reg.endFrame))},
                                             {"durationSec", r2((double)(reg.endFrame - reg.startFrame) / sr)}});
            }
            Json out = accWindowBlock(r);
            out["target"] = t.label; out["source"] = t.source;
            out["thresholdDb"] = threshDb; out["minSilenceSec"] = minSil;
            out["fullySilent"] = fullySilent;
            out["leadingSilenceSec"] = r2(leading); out["trailingSilenceSec"] = r2(trailing);
            out["internalGaps"] = internal;
            out["contentSpan"] = fullySilent ? Json(nullptr)
                                             : Json{{"start", r2(trimStart)}, {"end", r2(trimEnd)}};
            out["measuredSource"] = "accessor";
            Json warnings = Json::array();
            if (r.clamped) warnings.push_back("window was clamped to the source extent (or the 60 s cap)");
            out["warnings"] = warnings;
            return out;
#else
            std::string label = "track";
            if (a.contains("target") && a["target"].is_string()) label = a["target"].get<std::string>();
            else if (a.contains("target") && a["target"].is_number())
                label = "track " + std::to_string(a["target"].get<int>());
            return Json{{"target", label}, {"source", a.contains("itemIndex") ? "take" : "track"},
                        {"thresholdDb", threshDb}, {"minSilenceSec", minSil},
                        {"measuredSource", "none (host build)"}, {"dryRun", dryRun},
                        {"internalGaps", Json::array()},
                        {"note", "host build has no REAPER audio accessor; run under REAPER for a live "
                                 "scan. The windowed silence scan is unit-tested host-side (unit.meter)."}};
#endif
        }});

    // ---- analysis.send_layout_inspect — read-only QC of a renderer bus's object send layout ----
    reg.add(Tool{
        "analysis.send_layout_inspect",
        "Read back the object + bed send layout feeding an external Dolby Atmos Renderer / DAPS input "
        "bus and QC it (the read-only counterpart to spatial.orchestrate_sends; the sibling of "
        "analysis.damf_inspect for routing). Decodes every incoming send on rendererTrack, reconstructs "
        "the 1-based renderer input roster, and flags problems: channel collisions (two sources on one "
        "channel), gaps (an unused channel below the top), objects over 118 / channels over 128, a bed "
        "of the wrong width or absent, non-mono objects, and — when expectedObjects is given — unrouted "
        "objects. Pass bedLayout (or bedChannels) to validate the bed; ok=true means the layout is a "
        "clean, spec-conformant master bus.",
        jparse(R"({"type":"object","properties":{"rendererTrack":{"type":"integer","minimum":0},
            "bedLayout":{"type":"string","enum":["5.1","7.1","7.1.2","7.1.4","9.1.6","22.2"]},
            "bedChannels":{"type":"integer","minimum":0},
            "expectedObjects":{"type":"integer","minimum":0}},
            "required":["rendererTrack"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"rendererTrack":{"type":"integer"},
            "busChannels":{"type":"integer"},"bedDeclared":{"type":["integer","null"]},
            "bedPresent":{"type":"boolean"},"coveredChannels":{"type":"integer"},
            "objectChannels":{"type":"integer"},"receiveCount":{"type":"integer"},
            "receives":{"type":"array"},"issues":{"type":"array"},"summary":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true}, Profile::Analysis,
        [](const Json& a) -> Json {
            namespace sl = send_layout;
            // Bed width: explicit bedChannels wins; else map bedLayout (mirrors admBedChannels); else 0.
            auto layoutWidth = [](const std::string& L) -> int {
                if (L == "5.1")   return 6;
                if (L == "7.1")   return 8;
                if (L == "7.1.2") return 10;
                if (L == "7.1.4") return 12;
                if (L == "9.1.6") return 16;
                if (L == "22.2")  return 24;
                return 0;
            };
            int bedW = 0;
            if (a.contains("bedChannels") && a["bedChannels"].is_number()) bedW = a["bedChannels"].get<int>();
            else if (a.contains("bedLayout") && a["bedLayout"].is_string()) bedW = layoutWidth(a["bedLayout"].get<std::string>());
            const int expectedObjects = optInt(a, "expectedObjects", -1);
#ifdef REAPER_MCP_HAVE_SDK
            const int rendererTrack = reqInt(a, "rendererTrack");
            MediaTrack* rt = GetTrack(kCur, rendererTrack);
            if (!rt) return makeError("bad_renderer_track",
                                      "rendererTrack out of range: " + std::to_string(rendererTrack), "");
            const int busChannels = trackChannels(rt);
            std::vector<sl::ExistingSend> existing;
            Json receives = Json::array();
            const int nr = GetTrackNumSends(rt, -1);
            for (int i = 0; i < nr; ++i) {
                MediaTrack* src = (MediaTrack*)GetSetTrackSendInfo(rt, -1, i, "P_SRCTRACK", nullptr);
                const int srcIdx = src ? (CSurf_TrackToID(src, false) - 1) : -1;
                sl::DecodedSend d = sl::decodeSend((int)GetTrackSendInfo_Value(rt, -1, i, "I_SRCCHAN"),
                                                   (int)GetTrackSendInfo_Value(rt, -1, i, "I_DSTCHAN"));
                sl::ExistingSend es;
                es.sendIndex = i; es.sourceTrack = srcIdx; es.srcOff = d.srcOff; es.width = d.width;
                es.dstOff = d.dstOff; es.monoMix = d.monoMix; es.audio = d.audio;
                existing.push_back(es);
                receives.push_back(Json{{"sendIndex", i}, {"sourceTrack", srcIdx}, {"srcChannel", d.srcOff},
                                        {"width", d.width}, {"rendererChannel", d.dstOff + 1},
                                        {"monoMix", d.monoMix}, {"audio", d.audio}});
            }
            sl::InspectResult r = sl::inspect(existing, bedW, expectedObjects, busChannels);
            Json issues = Json::array();
            for (const auto& is : r.issues) issues.push_back(Json{{"code", is.code}, {"message", is.message}});
            const std::string summary = r.ok
                ? ("clean: bed " + std::string(r.bedPresent ? "present" : (bedW > 0 ? "declared-absent" : "n/a")) +
                   ", " + std::to_string(r.objectChannels) + " object(s), " +
                   std::to_string(r.coveredChannels) + " renderer channels used")
                : (std::to_string((int)r.issues.size()) + " issue(s) in the send layout");
            return Json{{"ok", r.ok}, {"rendererTrack", rendererTrack}, {"busChannels", busChannels},
                        {"bedDeclared", bedW > 0 ? Json(bedW) : Json(nullptr)}, {"bedPresent", r.bedPresent},
                        {"coveredChannels", r.coveredChannels}, {"objectChannels", r.objectChannels},
                        {"receiveCount", nr}, {"receives", receives}, {"issues", issues},
                        {"summary", summary}};
#else
            (void)expectedObjects; (void)bedW;
            return Json{{"ok", true}, {"rendererTrack", reqInt(a, "rendererTrack")}, {"busChannels", 0},
                        {"bedDeclared", bedW > 0 ? Json(bedW) : Json(nullptr)}, {"bedPresent", false},
                        {"coveredChannels", 0}, {"objectChannels", 0}, {"receiveCount", 0},
                        {"receives", Json::array()}, {"issues", Json::array()},
                        {"summary", "host build: no REAPER routing to inspect"}};
#endif
        }});

    // ---- analysis.object_decode_timeline — per-object decode-coverage over time (read-only) ---------
    reg.add(Tool{
        "analysis.object_decode_timeline",
        "Per-object decode-coverage TIMELINE (READ-ONLY) — the object-model complement of "
        "analysis.decode_coverage (which decodes a whole ambisonic SCENE bus): for each immersive OBJECT "
        "it answers, over time, 'which delivery loudspeaker(s) does this object energize, and how does "
        "that migrate as it moves?'. PURELY GEOMETRIC — it reads each object's position TRAJECTORY (no "
        "render, no audio): either from a live object track's panner automation (azimuth/elevation/"
        "distance, sampled over the window exactly like spatial.export_adm) via `objects`, or from "
        "explicit `trajectories` keyframes. At each time slice it ENCODES the object direction to real "
        "SN3D spherical harmonics at `order` and PROJECTION-DECODES onto the chosen layout's nominal "
        "speaker directions (the same basis analysis.decode_coverage uses — sharper at higher order), "
        "picking the dominant speaker. Per object it reports the timeline (dominant speaker + position "
        "per slice), the dominant-speaker MIGRATION path, per-speaker DWELL fractions, the great-circle "
        "angular TRAVEL + elevation reach, and a time-weighted coverage FOOTPRINT (hemisphere balance, "
        "uniformity, dead zones, speakers touched). Azimuth is +LEFT/CCW (the ambisonic/IEM convention, "
        "matching ambisonic_encode/export_adm); distance is carried for reporting only (direction is "
        "distance-invariant). Give `objects` (live tracks — omit both to auto-discover 'Object N' tracks) "
        "and/or `trajectories` (explicit). Bound live sampling with boundsFlag (0=custom startPos/endPos, "
        "1=ENTIRE PROJECT — the default, 2=time selection), blockMs, and maxBlocks.",
        jparse(R"({"type":"object","properties":{
            "objects":{"type":"array","items":{"type":["integer","string"]},"maxItems":128},
            "trajectories":{"type":"array","items":{"type":"object","properties":{
                "name":{"type":"string"},"gain":{"type":"number"},
                "keyframes":{"type":"array","minItems":1,"items":{"type":"object","properties":{
                    "t":{"type":"number"},"azimuthDeg":{"type":"number"},"elevationDeg":{"type":"number"},
                    "distance":{"type":"number"},"gain":{"type":"number"}},"additionalProperties":false}}},
                "required":["keyframes"],"additionalProperties":false}},
            "layout":{"type":"string","enum":["5.1","7.1","7.1.4","9.1.6"],"default":"7.1.4"},
            "order":{"type":"integer","minimum":1,"maximum":7,"default":3},
            "maxTimelinePoints":{"type":"integer","minimum":2,"maximum":512,"default":48},
            "includePerSpeaker":{"type":"boolean","default":false},
            "deadZoneFrac":{"type":"number","minimum":0,"maximum":1,"default":0.1},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "blockMs":{"type":"number","minimum":1,"default":100},
            "maxBlocks":{"type":"integer","minimum":1,"maximum":20000,"default":1000},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "layout":{"type":"string"},"order":{"type":"integer"},"speakers":{"type":"array"},
            "count":{"type":"integer"},"boundsFlag":{"type":"integer"},"window":{"type":["object","null"]},
            "objects":{"type":"array"},"measuredSource":{"type":"string"},
            "interpretation":{"type":"string"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "warnings":{"type":"array"}}})"),
        ToolAnnotations{/*readOnly*/ true, /*destructive*/ false, /*idempotent*/ true},
        Profile::Analysis,
        [](const Json& a) -> Json {
            namespace dtl = decode_timeline;
            const std::string layout = optStr(a, "layout", "7.1.4");
            int order = optInt(a, "order", 3); if (order < 1) order = 1; if (order > 7) order = 7;
            const int maxPoints = std::max(2, std::min(512, optInt(a, "maxTimelinePoints", 48)));
            const bool includePerSpeaker = optBool(a, "includePerSpeaker", false);
            const double deadZoneFrac = meter::clampd(optNum(a, "deadZoneFrac", 0.1), 0.0, 1.0);
            const bool dryRun = optBool(a, "dryRun", false);
            const int boundsFlag = optInt(a, "boundsFlag", 1);

            std::vector<NamedSpeaker> ns; std::string lerr;
            if (!namedLayoutDirs(layout, ns, lerr))
                return makeError("unknown_layout", lerr, "use 5.1, 7.1, 7.1.4, or 9.1.6");
            std::vector<meter::Dir> spkDirs; std::vector<std::string> spkLabels;
            Json speakerJson = Json::array();
            for (const auto& s : ns) {
                spkDirs.push_back(s.dir); spkLabels.push_back(s.label);
                double az = 0, el = 0; meter::vecToAzEl(s.dir, az, el);
                speakerJson.push_back(Json{{"label", s.label}, {"azimuthDeg", r2(az)}, {"elevationDeg", r2(el)}});
            }
            auto lbl = [&](int i) -> std::string {
                return (i >= 0 && i < (int)spkLabels.size()) ? spkLabels[(size_t)i] : std::string();
            };

            struct ObjIn { std::string label; std::string source; std::string positionSource; int track;
                           std::vector<dtl::Sample> samples; };
            std::vector<ObjIn> objsIn;
            Json warnings = Json::array();
            bool anyTrack = false, anyTraj = false;

            // (A) explicit trajectories — build in any build (pure geometry).
            if (a.contains("trajectories") && a["trajectories"].is_array()) {
                int autoN = 0;
                for (const auto& tj : a["trajectories"]) {
                    if (!tj.is_object() || !tj.contains("keyframes") || !tj["keyframes"].is_array() ||
                        tj["keyframes"].empty())
                        return makeError("bad_trajectory",
                                         "each trajectories[] entry needs a non-empty keyframes[]",
                                         "e.g. {\"keyframes\":[{\"t\":0,\"azimuthDeg\":90},"
                                         "{\"t\":4,\"azimuthDeg\":-90}]}");
                    ObjIn o; o.source = "trajectory"; o.track = -1; o.positionSource = "keyframes";
                    ++autoN;
                    o.label = (tj.contains("name") && tj["name"].is_string())
                                  ? tj["name"].get<std::string>()
                                  : ("trajectory " + std::to_string(autoN));
                    const double gGain = (tj.contains("gain") && tj["gain"].is_number())
                                             ? tj["gain"].get<double>() : 1.0;
                    for (const auto& kf : tj["keyframes"]) {
                        if (!kf.is_object()) continue;
                        dtl::Sample sm;
                        sm.t = (kf.contains("t") && kf["t"].is_number()) ? kf["t"].get<double>()
                                                                         : (double)o.samples.size();
                        sm.azDeg = (kf.contains("azimuthDeg") && kf["azimuthDeg"].is_number())
                                       ? kf["azimuthDeg"].get<double>() : 0.0;
                        sm.elDeg = (kf.contains("elevationDeg") && kf["elevationDeg"].is_number())
                                       ? kf["elevationDeg"].get<double>() : 0.0;
                        sm.dist = (kf.contains("distance") && kf["distance"].is_number())
                                      ? kf["distance"].get<double>() : 1.0;
                        sm.gain = (kf.contains("gain") && kf["gain"].is_number())
                                      ? kf["gain"].get<double>() : gGain;
                        o.samples.push_back(sm);
                    }
                    std::sort(o.samples.begin(), o.samples.end(),
                              [](const dtl::Sample& x, const dtl::Sample& y) { return x.t < y.t; });
                    if (o.samples.empty()) continue;
                    anyTraj = true;
                    objsIn.push_back(std::move(o));
                }
            }

            double startWin = 0.0, endWin = 0.0; bool haveWindow = false;
#ifdef REAPER_MCP_HAVE_SDK
            // Window for live sampling (mirrors export_adm).
            if (boundsFlag == 0) { startWin = optNum(a, "startPos", 0.0); endWin = optNum(a, "endPos", 0.0); }
            else { startWin = 0.0; endWin = GetProjectLength(nullptr); }
            haveWindow = true;
            const double blockMs = optNum(a, "blockMs", 100.0);
            const int maxBlocks = std::max(1, std::min(20000, optInt(a, "maxBlocks", 1000)));

            // Locate an object track's positional panner + az/el/dist param indices (mirrors
            // tools_spatial.cpp findPositionalFx / analysis.readObjectPosition's scan).
            struct PFx { int fx = -1, azP = -1, elP = -1, distP = -1; };
            auto findPanner = [](MediaTrack* t) -> PFx {
                PFx r; if (!t) return r;
                const int nfx = TrackFX_GetCount(t);
                for (int fx = 0; fx < nfx; ++fx) {
                    const int np = TrackFX_GetNumParams(t, fx);
                    int az = -1, el = -1, di = -1;
                    for (int p = 0; p < np; ++p) {
                        char pn[128] = {0};
                        if (!TrackFX_GetParamName(t, fx, p, pn, (int)sizeof(pn))) continue;
                        const std::string ln = toLower(pn);
                        if (az < 0 && ln.find("azim") != std::string::npos) az = p;
                        else if (el < 0 && ln.find("elev") != std::string::npos) el = p;
                        else if (di < 0 && ln.find("dist") != std::string::npos) di = p;
                    }
                    if (az >= 0 || el >= 0) { r.fx = fx; r.azP = az; r.elP = el; r.distP = di; return r; }
                }
                return r;
            };
            // Sample one FX param over time by linear interpolation of its automation envelope; falls
            // back to the current normalized value with no envelope (mirrors tools_spatial ParamSampler).
            struct Samp {
                MediaTrack* t = nullptr; int fx = -1, param = -1;
                std::vector<double> times, norms; double staticNorm = 0.0; bool hasEnv = false;
                void init(MediaTrack* tr, int f, int p) {
                    t = tr; fx = f; param = p; staticNorm = TrackFX_GetParamNormalized(t, fx, param);
                    TrackEnvelope* env = GetFXEnvelope(t, fx, param, false);
                    if (env) {
                        const int n = CountEnvelopePoints(env);
                        for (int i = 0; i < n; ++i) {
                            double tm = 0, val = 0, tens = 0; int shape = 0; bool sel = false;
                            if (GetEnvelopePoint(env, i, &tm, &val, &shape, &tens, &sel)) {
                                times.push_back(tm); norms.push_back(val);
                            }
                        }
                        if (!times.empty()) hasEnv = true;
                    }
                }
                double normAt(double at) const {
                    if (!hasEnv) return staticNorm;
                    if (at <= times.front()) return norms.front();
                    if (at >= times.back()) return norms.back();
                    for (size_t i = 1; i < times.size(); ++i)
                        if (at <= times[i]) {
                            const double t0 = times[i - 1], t1 = times[i];
                            const double f = (t1 > t0) ? (at - t0) / (t1 - t0) : 0.0;
                            return norms[i - 1] + f * (norms[i] - norms[i - 1]);
                        }
                    return norms.back();
                }
                bool degAt(double at, double& deg) const {
                    if (param < 0) return false;
                    const double nv = normAt(at);
                    char buf[128] = {0};
                    if (!TrackFX_FormatParamValueNormalized(t, fx, param, nv, buf, (int)sizeof(buf)) || !buf[0])
                        return false;
                    // parse the leading number out of the formatted degree string
                    std::string s(buf); size_t i = 0;
                    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                    const size_t st = i;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                    bool dig = false;
                    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
                        if (std::isdigit((unsigned char)s[i])) dig = true;
                        ++i;
                    }
                    if (!dig) return false;
                    try { deg = std::stod(s.substr(st, i - st)); } catch (...) { return false; }
                    return true;
                }
            };
            // Build an object's trajectory samples over [startWin,endWin] (mirrors objectTrajectory).
            auto buildSamples = [&](MediaTrack* t, std::string& psrc) -> std::vector<dtl::Sample> {
                std::vector<dtl::Sample> out;
                const double dur = (endWin > startWin) ? (endWin - startWin) : 0.0;
                const PFx pf = findPanner(t);
                if (pf.fx < 0) {
                    psrc = "none";
                    out.push_back(dtl::Sample{0.0, 0.0, 0.0, 1.0, 1.0});
                    return out;
                }
                Samp az, el, di;
                if (pf.azP >= 0) az.init(t, pf.fx, pf.azP);
                if (pf.elP >= 0) el.init(t, pf.fx, pf.elP);
                if (pf.distP >= 0) di.init(t, pf.fx, pf.distP);
                auto samp = [&](double at) -> dtl::Sample {
                    dtl::Sample b; double d;
                    b.azDeg = (pf.azP >= 0 && az.degAt(at, d)) ? d : 0.0;
                    b.elDeg = (pf.elP >= 0 && el.degAt(at, d)) ? d : 0.0;
                    b.dist = (pf.distP >= 0) ? di.normAt(at) : 1.0;
                    if (b.dist < 0) b.dist = 0; if (b.dist > 1.5) b.dist = 1.5;
                    b.gain = 1.0;
                    return b;
                };
                const bool anyEnv = az.hasEnv || el.hasEnv || di.hasEnv;
                if (!anyEnv) {
                    psrc = "static";
                    dtl::Sample b = samp(startWin); b.t = 0.0;
                    out.push_back(b);
                    return out;
                }
                psrc = "envelope";
                double step = blockMs / 1000.0; if (step <= 0) step = 0.1;
                int nblk = (dur > 0) ? (int)std::ceil(dur / step) + 1 : 1;
                if (nblk > maxBlocks) { nblk = maxBlocks; step = (nblk > 1) ? dur / (double)(nblk - 1) : dur; }
                if (nblk < 1) nblk = 1;
                for (int i = 0; i < nblk; ++i) {
                    double rt = i * step; if (rt > dur) rt = dur;
                    dtl::Sample b = samp(startWin + rt); b.t = rt;
                    out.push_back(b);
                }
                return out;
            };

            // Resolve the live object track list: explicit `objects` (indices/names) OR, when neither
            // objects nor trajectories were given, auto-discover 'Object N' tracks in N order.
            const int ntr = CountTracks(kCur);
            std::vector<int> trackIdxs;
            if (a.contains("objects") && a["objects"].is_array() && !a["objects"].empty()) {
                if (a["objects"].size() > 128)
                    return makeError("too_many_objects", "more than 128 objects requested",
                                     "analyze at most 128 objects per call");
                for (const auto& e : a["objects"]) {
                    int idx = -1; MediaTrack* t = nullptr;
                    if (e.is_number()) { idx = e.get<int>(); t = GetTrack(kCur, idx); }
                    else if (e.is_string()) {
                        const std::string want = e.get<std::string>();
                        for (int i = 0; i < ntr; ++i) {
                            MediaTrack* c = GetTrack(kCur, i);
                            if (c && trackNameStr(c) == want) { t = c; idx = i; break; }
                        }
                    }
                    if (!t) return makeError("target_not_found", "object target not found: " + e.dump(),
                                             "pass valid track indices or names");
                    trackIdxs.push_back(idx);
                }
            } else if (!anyTraj) {
                // auto-discover 'Object <k>' tracks
                std::vector<std::pair<int,int>> found;  // (k, trackIdx)
                for (int i = 0; i < ntr; ++i) {
                    MediaTrack* c = GetTrack(kCur, i);
                    if (!c) continue;
                    const std::string nm = trackNameStr(c);
                    if (nm.rfind("Object ", 0) != 0) continue;
                    const std::string tail = nm.substr(7);
                    if (tail.empty()) continue;
                    bool allDig = true; for (char ch : tail) if (!std::isdigit((unsigned char)ch)) { allDig = false; break; }
                    if (!allDig) continue;
                    found.push_back({std::stoi(tail), i});
                }
                std::sort(found.begin(), found.end(),
                          [](const std::pair<int,int>& x, const std::pair<int,int>& y) { return x.first < y.first; });
                for (const auto& f : found) trackIdxs.push_back(f.second);
            }
            std::sort(trackIdxs.begin(), trackIdxs.end());
            trackIdxs.erase(std::unique(trackIdxs.begin(), trackIdxs.end()), trackIdxs.end());
            for (int idx : trackIdxs) {
                MediaTrack* t = GetTrack(kCur, idx);
                if (!t) continue;
                ObjIn o; o.source = "track"; o.track = idx;
                const std::string nm = trackNameStr(t);
                o.label = nm.empty() ? ("track " + std::to_string(idx)) : nm;
                o.samples = buildSamples(t, o.positionSource);
                if (o.positionSource == "none")
                    warnings.push_back("object '" + o.label + "' has no positional panner — treated as "
                                       "front-centre (az 0, el 0)");
                anyTrack = true;
                objsIn.push_back(std::move(o));
            }
#else
            if (a.contains("objects"))
                warnings.push_back("host build cannot read live object tracks — pass explicit "
                                   "trajectories[]; the decode geometry is unit-tested (unit.decode_timeline)");
#endif

            if (objsIn.empty())
                return makeError("no_objects",
                                 "no objects to analyze",
                                 "pass `objects` (track indices/names) and/or `trajectories`, or name your "
                                 "object tracks 'Object 1', 'Object 2', … for auto-discovery");

            const std::string measuredSource =
                (anyTrack && anyTraj) ? "mixed: panner automation + explicit keyframes (geometric SN3D decode)"
                : anyTrack            ? "panner automation (geometric SN3D encode->decode; no render)"
                                      : "explicit keyframes (geometric SN3D encode->decode)";
            Json window = haveWindow ? Json{{"startSec", r2(startWin)}, {"endSec", r2(endWin)}} : Json(nullptr);

            if (dryRun) {
                Json list = Json::array();
                for (const auto& o : objsIn)
                    list.push_back(Json{{"source", o.source},
                                        {"track", o.track >= 0 ? Json(o.track) : Json(nullptr)},
                                        {"label", o.label}, {"positionSource", o.positionSource},
                                        {"samples", (int)o.samples.size()}});
                return Json{{"dryRun", true}, {"layout", layout}, {"order", order},
                            {"speakers", speakerJson}, {"count", (int)objsIn.size()},
                            {"boundsFlag", boundsFlag}, {"window", window}, {"objects", list},
                            {"measuredSource", measuredSource},
                            {"plan", "read each object's position trajectory (no render), encode->decode "
                                     "onto the " + layout + " layout at order " + std::to_string(order) +
                                     ", and report the per-object dominant-speaker timeline + coverage "
                                     "footprint"},
                            {"warnings", warnings}};
            }

            Json objJson = Json::array();
            int movingCount = 0, staticCount = 0;
            for (const auto& oi : objsIn) {
                dtl::ObjectTimeline tl = dtl::computeTimeline(oi.samples, spkDirs, order, deadZoneFrac);
                const int total = (int)tl.slices.size();

                Json timeline = Json::array();
                const int rep = std::max(1, std::min(total, maxPoints));
                for (int r = 0; r < rep; ++r) {
                    const int idx = (rep <= 1) ? 0
                                   : (int)std::llround((double)r * (double)(total - 1) / (double)(rep - 1));
                    const dtl::Slice& sl = tl.slices[(size_t)idx];
                    Json pt{{"t", r2(sl.t)}, {"azimuthDeg", r2(sl.azDeg)}, {"elevationDeg", r2(sl.elDeg)},
                            {"dominant", Json{{"label", lbl(sl.dominantIdx)},
                                              {"energyFrac", r2(sl.dominantFrac)}}}};
                    if (includePerSpeaker) {
                        Json ps = Json::array();
                        for (size_t s = 0; s < spkLabels.size() && s < sl.frac.size(); ++s)
                            ps.push_back(Json{{"label", spkLabels[s]}, {"energyFrac", r2(sl.frac[s])}});
                        pt["perSpeaker"] = ps;
                    }
                    timeline.push_back(pt);
                }

                Json migration = Json::array(); std::string migStr;
                for (const dtl::MigrationRun& mr : tl.migration) {
                    const std::string L = lbl(mr.speakerIdx);
                    migration.push_back(L);
                    if (!migStr.empty()) migStr += " -> ";
                    migStr += L;
                }

                std::vector<size_t> di(tl.dwellFrac.size());
                for (size_t s = 0; s < di.size(); ++s) di[s] = s;
                std::sort(di.begin(), di.end(),
                          [&](size_t x, size_t y) { return tl.dwellFrac[x] > tl.dwellFrac[y]; });
                Json dwell = Json::array();
                for (size_t r = 0; r < di.size() && r < 6; ++r) {
                    if (tl.dwellFrac[di[r]] <= 0.0) break;
                    dwell.push_back(Json{{"label", lbl((int)di[r])}, {"frac", r2(tl.dwellFrac[di[r]])}});
                }

                const meter::CoverageStats& c = tl.footprint;
                Json coverage = c.valid
                    ? Json{{"dominant", Json{{"label", lbl(c.dominantIdx)},
                                             {"azimuthDeg", r2(c.dominantAzDeg)},
                                             {"elevationDeg", r2(c.dominantElDeg)}}},
                           {"frontFrac", r2(c.frontFrac)}, {"backFrac", r2(c.backFrac)},
                           {"leftFrac", r2(c.leftFrac)}, {"rightFrac", r2(c.rightFrac)},
                           {"upperFrac", r2(c.upperFrac)}, {"lowerFrac", r2(c.lowerFrac)},
                           {"earLevelFrac", r2(c.horizFrac)}, {"uniformity", r2(c.uniformity)},
                           {"concentrationTop", r2(c.concentrationTop)}, {"deadZones", c.deadZones},
                           {"speakersTouched", tl.speakersTouched}}
                    : Json(nullptr);

                Json travel{{"angularTravelDeg", r2(tl.angularTravelDeg)},
                            {"maxElevationDeg", r2(tl.maxElevationDeg)},
                            {"minElevationDeg", r2(tl.minElevationDeg)}, {"static", tl.isStatic}};

                std::string interp;
                if (tl.isStatic) {
                    ++staticCount;
                    const dtl::Slice& s0 = tl.slices.empty() ? dtl::Slice() : tl.slices.front();
                    interp = "static at az " + fmt2(s0.azDeg) + " deg, el " + fmt2(s0.elDeg) +
                             " deg -> dominant " + lbl(s0.dominantIdx);
                } else {
                    ++movingCount;
                    interp = "moves " + migStr + " over " + fmt2(tl.angularTravelDeg) + " deg of travel";
                    if (c.valid)
                        interp += "; " + fmt2(c.frontFrac * 100.0) + "% front, " +
                                  fmt2(c.upperFrac * 100.0) + "% upper";
                }

                objJson.push_back(Json{{"source", oi.source},
                                       {"track", oi.track >= 0 ? Json(oi.track) : Json(nullptr)},
                                       {"label", oi.label}, {"positionSource", oi.positionSource},
                                       {"blocks", total}, {"reportedPoints", rep},
                                       {"timeline", timeline}, {"migration", migration},
                                       {"dwell", dwell}, {"coverage", coverage}, {"travel", travel},
                                       {"interpretation", interp}});
            }

            const std::string topInterp =
                std::to_string((int)objJson.size()) + " object(s) on " + layout + " (order " +
                std::to_string(order) + "): " + std::to_string(movingCount) + " moving, " +
                std::to_string(staticCount) + " static";

            return Json{{"layout", layout}, {"order", order}, {"speakers", speakerJson},
                        {"count", (int)objJson.size()}, {"boundsFlag", boundsFlag}, {"window", window},
                        {"objects", objJson}, {"measuredSource", measuredSource},
                        {"interpretation", topInterp}, {"warnings", warnings}};
        }});
}

}  // namespace reaper_mcp
