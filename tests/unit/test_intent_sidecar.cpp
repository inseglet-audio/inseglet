// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_intent_sidecar.cpp — pure unit test for the intent-sidecar emitter core
// (src/intent_sidecar.h, B1 R1/R2): the judged-layout
// second-witness pin (E-124.1, the E-116.1 idiom), the byte-golden emissions ×2 (E-124.2),
// the decode-prediction identity with decode_timeline (E-124.3 — the R1 acceptance),
// the block→trajectory reach-time convention (E-124.4), the forbidden-key schema scan
// (E-124.5 — "never a deliverable-loudness key"), the measurement mirrors + path/layout
// helpers (E-124.6), and JSON validity of every emission (E-124.7). No REAPER, no SDK —
// every number is deterministic. Expectations were pre-registered before implementation.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "intent_sidecar.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        std::fprintf(stderr, "  FAIL: %s\n---- got ----\n%s\n---- want ----\n%s\n----\n",
                     what.c_str(), got.c_str(), want.c_str());
        ++g_failures;
    } else {
        std::fprintf(stderr, "  ok:   %s\n", what.c_str());
    }
}
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Build the R1-shaped model (5.1 bed + 2 objects, one static @ az +90 + one 3-point sweep,
// decode 7.1.4) used by the golden + scan + validity sections. Level values are hand-set —
// the emitter never measures; measurement mirrors are tested separately.
static intent::Sidecar makeAdmModel() {
    intent::Sidecar sc;
    sc.producerVersion = "1.6.0";
    sc.producerExport = "spatial.export_adm";
    sc.sampleRate = 48000;
    sc.start = 0.0;
    sc.end = 1.0;
    sc.sliceSec = 0.1;
    sc.haveBed = true;
    sc.bedLayout = "5.1";
    sc.haveBedLufs = true;
    sc.bedLufs = -20.125;
    static const char* kCh[6] = {"M+030", "M-030", "M+000", "LFE1", "M+110", "M-110"};
    for (int c = 0; c < 6; ++c) {
        intent::RosterEntry r;
        r.channel = kCh[c];
        r.track = c + 1;
        r.haveRms = true;    r.rmsDb = -18.0 - c;
        r.haveLufs = true;   r.lufs = -17.5 - c;
        r.haveActive = true; r.active = 0.9;
        sc.roster.push_back(r);
    }
    {   // static object exactly at M+090
        intent::ObjectEntry o;
        o.id = 1; o.label = "Obj static"; o.track = 7;
        o.trajectory.push_back(intent::TrajPoint{1.0, 90.0, 0.0, 1.0, 1.0});
        o.decodeLayout = "7.1.4";
        o.haveDecode = intent::computeDecode(o, sc.start, sc.end, sc.sliceSec);
        o.haveRms = true;    o.rmsDb = -18.0;
        o.haveLufs = true;   o.lufs = -17.25;
        o.haveActive = true; o.active = 1.0;
        sc.objects.push_back(o);
    }
    {   // front -> hard-left sweep (reach times 0.25 / 0.5 / 1.0)
        intent::ObjectEntry o;
        o.id = 2; o.label = "Obj sweep"; o.track = 8;
        o.trajectory.push_back(intent::TrajPoint{0.25, 0.0, 0.0, 1.0, 1.0});
        o.trajectory.push_back(intent::TrajPoint{0.5, 30.0, 0.0, 1.0, 1.0});
        o.trajectory.push_back(intent::TrajPoint{1.0, 90.0, 0.0, 1.0, 1.0});
        o.decodeLayout = "7.1.4";
        o.haveDecode = intent::computeDecode(o, sc.start, sc.end, sc.sliceSec);
        o.haveRms = true;    o.rmsDb = -21.0;
        o.haveLufs = true;   o.lufs = -20.5;
        o.haveActive = true; o.active = 0.75;
        sc.objects.push_back(o);
    }
    return sc;
}

// The R2-shaped model (7.1.4 bed + order-2 scene, decode-free — the toolchain-free consumer
// path). Exact-string golden: every byte is hand-derivable.
static intent::Sidecar makeLoomModel() {
    intent::Sidecar sc;
    sc.producerVersion = "1.6.0";
    sc.producerExport = "spatial.export_loom_manifest";
    sc.sampleRate = 48000;
    sc.start = 0.0;
    sc.end = 2.5;
    sc.sliceSec = 0.1;
    sc.haveBed = true;
    sc.bedLayout = "7.1.4";
    sc.haveBedLufs = true;
    sc.bedLufs = -23.0;
    const intent::IntentLayout* lay = intent::findIntentLayout("7.1.4");
    for (int c = 0; c < (int)lay->speakers.size(); ++c) {
        intent::RosterEntry r;
        r.channel = lay->speakers[(size_t)c].label;
        r.track = c + 1;
        r.haveRms = true;    r.rmsDb = -18.0 - 1.5 * c;
        r.haveLufs = true;   r.lufs = -17.0 - 1.5 * c;
        r.haveActive = true; r.active = 1.0;
        sc.roster.push_back(r);
    }
    sc.haveScene = true;
    sc.scene.order = 2;
    for (int k = 0; k < 9; ++k) sc.scene.rmsDb.push_back(-18.0 - k);
    return sc;
}

// ==== Byte-goldens (E-124.2) — authored from the first gated emission run, verified
// against hand-derived semantic pins, then frozen as regression pins (the E-116.3
// idiom). Every subsequent run must reproduce these bytes.
static const char kAdmGolden[] = R"json({
  "intent": 0,
  "producer": {"tool": "inseglet", "version": "1.6.0", "export": "spatial.export_adm"},
  "program": {"sampleRate": 48000, "start": 0, "end": 1, "sliceSec": 0.1},
  "bed": {
    "layout": "5.1",
    "expectLufs": -20.125,
    "roster": [
      {"channel": "M+030", "track": 1, "expectRmsDb": -18, "expectLufs": -17.5, "expectActive": 0.9},
      {"channel": "M-030", "track": 2, "expectRmsDb": -19, "expectLufs": -18.5, "expectActive": 0.9},
      {"channel": "M+000", "track": 3, "expectRmsDb": -20, "expectLufs": -19.5, "expectActive": 0.9},
      {"channel": "LFE1", "track": 4, "expectRmsDb": -21, "expectLufs": -20.5, "expectActive": 0.9},
      {"channel": "M+110", "track": 5, "expectRmsDb": -22, "expectLufs": -21.5, "expectActive": 0.9},
      {"channel": "M-110", "track": 6, "expectRmsDb": -23, "expectLufs": -22.5, "expectActive": 0.9}
    ]
  },
  "objects": [
    {
      "id": 1, "label": "Obj static", "track": 7,
      "trajectory": [
        {"t": 1, "az": 90, "el": 0, "dist": 1, "gain": 1}
      ],
      "decode": {"layout": "7.1.4",
        "dominant": ["M+090", "M+090", "M+090", "M+090", "M+090", "M+090", "M+090", "M+090", "M+090", "M+090"],
        "coverage": {"M+030": 0.0358, "M-030": 0.0269, "M+000": 0.0102, "M+090": 0.6516, "M-090": 0, "M+135": 0.1291, "M-135": 0.0211, "U+045": 0.0358, "U-045": 0.0269, "U+135": 0.0358, "U-135": 0.0269}},
      "expectRmsDb": -18, "expectLufs": -17.25, "expectActive": 1
    },
    {
      "id": 2, "label": "Obj sweep", "track": 8,
      "trajectory": [
        {"t": 0.25, "az": 0, "el": 0, "dist": 1, "gain": 1},
        {"t": 0.5, "az": 30, "el": 0, "dist": 1, "gain": 1},
        {"t": 1, "az": 90, "el": 0, "dist": 1, "gain": 1}
      ],
      "decode": {"layout": "7.1.4",
        "dominant": ["M+000", "M+000", "M+000", "M+000", "M+030", "M+030", "M+030", "M+030", "M+090", "M+090"],
        "coverage": {"M+030": 0.3151, "M-030": 0.0896, "M+000": 0.2417, "M+090": 0.1874, "M-090": 0.0103, "M+135": 0.0237, "M-135": 0.0076, "U+045": 0.0688, "U-045": 0.0182, "U+135": 0.0161, "U-135": 0.0213}},
      "expectRmsDb": -21, "expectLufs": -20.5, "expectActive": 0.75
    }
  ],
  "tolerances": {"posDeg": 5, "levelLu": 0.5, "dominantFrac": 0.9, "covSilent": 0.01, "silentDb": -40}
}
)json";

static const char kLoomGolden[] = R"json({
  "intent": 0,
  "producer": {"tool": "inseglet", "version": "1.6.0", "export": "spatial.export_loom_manifest"},
  "program": {"sampleRate": 48000, "start": 0, "end": 2.5, "sliceSec": 0.1},
  "bed": {
    "layout": "7.1.4",
    "expectLufs": -23,
    "roster": [
      {"channel": "M+030", "track": 1, "expectRmsDb": -18, "expectLufs": -17, "expectActive": 1},
      {"channel": "M-030", "track": 2, "expectRmsDb": -19.5, "expectLufs": -18.5, "expectActive": 1},
      {"channel": "M+000", "track": 3, "expectRmsDb": -21, "expectLufs": -20, "expectActive": 1},
      {"channel": "LFE1", "track": 4, "expectRmsDb": -22.5, "expectLufs": -21.5, "expectActive": 1},
      {"channel": "M+090", "track": 5, "expectRmsDb": -24, "expectLufs": -23, "expectActive": 1},
      {"channel": "M-090", "track": 6, "expectRmsDb": -25.5, "expectLufs": -24.5, "expectActive": 1},
      {"channel": "M+135", "track": 7, "expectRmsDb": -27, "expectLufs": -26, "expectActive": 1},
      {"channel": "M-135", "track": 8, "expectRmsDb": -28.5, "expectLufs": -27.5, "expectActive": 1},
      {"channel": "U+045", "track": 9, "expectRmsDb": -30, "expectLufs": -29, "expectActive": 1},
      {"channel": "U-045", "track": 10, "expectRmsDb": -31.5, "expectLufs": -30.5, "expectActive": 1},
      {"channel": "U+135", "track": 11, "expectRmsDb": -33, "expectLufs": -32, "expectActive": 1},
      {"channel": "U-135", "track": 12, "expectRmsDb": -34.5, "expectLufs": -33.5, "expectActive": 1}
    ]
  },
  "scene": {"order": 2, "norm": "SN3D", "expectRmsDb": [-18, -19, -20, -21, -22, -23, -24, -25, -26]},
  "tolerances": {"posDeg": 5, "levelLu": 0.5, "dominantFrac": 0.9, "covSilent": 0.01, "silentDb": -40}
}
)json";

int main() {
    // ==== E-124.1 — judged-layout pin (hard-coded second witness, NOT read from the header;
    //      mirror of intent_compare.py LAYOUT_LABELS + LABEL_POSITIONS @ cc6d8cf) ====
    {
        struct W { const char* lab; double az, el; bool lfe; };
        static const W k20[] = {{"M+030", 30, 0, false}, {"M-030", -30, 0, false}};
        static const W k51[] = {{"M+030", 30, 0, false}, {"M-030", -30, 0, false},
                                {"M+000", 0, 0, false}, {"LFE1", 0, 0, true},
                                {"M+110", 110, 0, false}, {"M-110", -110, 0, false}};
        static const W k71[] = {{"M+030", 30, 0, false}, {"M-030", -30, 0, false},
                                {"M+000", 0, 0, false}, {"LFE1", 0, 0, true},
                                {"M+090", 90, 0, false}, {"M-090", -90, 0, false},
                                {"M+135", 135, 0, false}, {"M-135", -135, 0, false}};
        static const W k714[] = {{"M+030", 30, 0, false}, {"M-030", -30, 0, false},
                                 {"M+000", 0, 0, false}, {"LFE1", 0, 0, true},
                                 {"M+090", 90, 0, false}, {"M-090", -90, 0, false},
                                 {"M+135", 135, 0, false}, {"M-135", -135, 0, false},
                                 {"U+045", 45, 45, false}, {"U-045", -45, 45, false},
                                 {"U+135", 135, 45, false}, {"U-135", -135, 45, false}};
        struct L { const char* name; const W* w; int n; };
        static const L kLay[] = {{"2.0", k20, 2}, {"5.1", k51, 6}, {"7.1", k71, 8},
                                 {"7.1.4", k714, 12}};
        check(intent::intentLayouts().size() == 4, "judged set: exactly 4 layouts");
        for (const L& l : kLay) {
            const intent::IntentLayout* got = intent::findIntentLayout(l.name);
            bool ok = got && (int)got->speakers.size() == l.n;
            if (ok)
                for (int i = 0; i < l.n; ++i) {
                    const intent::IntentSpeaker& s = got->speakers[(size_t)i];
                    if (std::string(s.label) != l.w[i].lab || s.az != l.w[i].az ||
                        s.el != l.w[i].el || s.lfe != l.w[i].lfe) { ok = false; break; }
                }
            check(ok, std::string("layout pin: ") + l.name);
        }
        check(intent::findIntentLayout("7.1.2") == nullptr &&
              intent::findIntentLayout("9.1.6") == nullptr &&
              intent::findIntentLayout("22.2") == nullptr,
              "7.1.2 / 9.1.6 / 22.2 are OUTSIDE the judged set (absent block, not a guess)");
    }

    // ==== E-124.6a — helper pins ====
    {
        check(intent::sidecarBedLayout("stereo") == "2.0", "sidecarBedLayout: stereo -> 2.0");
        check(intent::sidecarBedLayout("5.1") == "5.1" &&
              intent::sidecarBedLayout("7.1.4") == "7.1.4", "sidecarBedLayout: pass-through");
        check(intent::sidecarBedLayout("7.1.2").empty() &&
              intent::sidecarBedLayout("9.1.6").empty() &&
              intent::sidecarBedLayout("22.2").empty(),
              "sidecarBedLayout: out-of-set -> empty (no bed block)");
        check(intent::sidecarPathFor("dir/FOO.wav") == "dir/FOO.intent.json",
              "sidecarPathFor: extension swap");
        check(intent::sidecarPathFor("a.b/FOO") == "a.b/FOO.intent.json",
              "sidecarPathFor: dot in a directory name does not truncate");
        check(intent::sidecarPathFor("FOO") == "FOO.intent.json",
              "sidecarPathFor: extensionless append");
    }

    // ==== E-125.1 — LUFS silence floor (live cross-loop gate finding) ====
    // The consumer's BS.1770 gated measure returns exactly -70.0 for silence; Inseglet's
    // meter says -144 (kMinDb). A -144 claim on a silent channel is a false S-344 (+74 LU)
    // through the real consumer — every emitted expectLufs rides this clamp.
    {
        check(intent::lufsSilenceFloor() == -70.0, "lufsSilenceFloor == BS.1770 absolute gate");
        check(intent::clampLufs(-144.0) == -70.0, "clampLufs floors the meter silence sentinel");
        check(intent::clampLufs(-70.0) == -70.0, "clampLufs: floor value passes unchanged");
        check(intent::clampLufs(-23.5) == -23.5, "clampLufs: in-range loudness untouched");
    }

    // ==== E-124.6b — measurement mirrors (consumer formulas, bit-for-bit) ====
    {
        const int rate = 48000;
        std::vector<float> sine(48000);
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = (float)std::sin(2.0 * M_PI * 997.0 * (double)i / rate);
        const double r = intent::rmsDb(sine);
        check(std::fabs(r - (-3.0103)) < 0.01,
              "rmsDb: full-scale sine = -3.01 dBFS (got " + std::to_string(r) + ")");
        check(intent::rmsDb(std::vector<float>(1000, 0.0f)) == -120.0, "rmsDb: silence floor");
        // half tone / half silence -> activity 0.5 (100 ms windows, -60 dB floor)
        std::vector<float> half(96000, 0.0f);
        for (size_t i = 0; i < 48000; ++i) half[i] = sine[i];
        const double af = intent::activeFraction(half, rate);
        check(std::fabs(af - 0.5) < 1e-9,
              "activeFraction: half tone / half silence = 0.5 (got " + std::to_string(af) + ")");
        check(intent::activeFraction(std::vector<float>(10, 1.0f), rate) == 0.0,
              "activeFraction: shorter than one window = 0 (consumer truncation mirror)");
    }

    // ==== E-124.4 — block -> trajectory reach-time convention ====
    {
        std::vector<adm::Block> blocks;
        adm::Block b0; b0.rtime = 0.0; b0.duration = 0.0; b0.az = 10; blocks.push_back(b0);
        adm::Block b1; b1.rtime = 0.4; b1.duration = 0.0; b1.az = 20; blocks.push_back(b1);
        adm::Block b2; b2.rtime = 0.8; b2.duration = 0.0; b2.az = 30; blocks.push_back(b2);
        std::vector<intent::TrajPoint> tp = intent::trajectoryFromBlocks(blocks, 1.2);
        check(tp.size() == 3 && tp[0].t == 0.4 && tp[1].t == 0.8 && tp[2].t == 1.2,
              "open-ended durations fill to next rtime / programme end (reach times .4/.8/1.2)");
        check(tp[0].az == 10 && tp[2].az == 30, "block values carried through");
        std::vector<adm::Block> ex;
        adm::Block e0; e0.rtime = 0.0; e0.duration = 0.5; e0.az = 5; ex.push_back(e0);
        adm::Block e1; e1.rtime = 0.5; e1.duration = 0.25; e1.az = 15; e1.jump = true; ex.push_back(e1);
        std::vector<intent::TrajPoint> tq = intent::trajectoryFromBlocks(ex, 1.0);
        check(tq[0].t == 0.5, "explicit duration: reach = rtime + duration");
        check(tq[1].t == 0.5, "jump block applies AT rtime (consumer piecewise-constant mirror)");
        // static single block (objectTrajectory's shape): reach = programme end
        std::vector<adm::Block> st;
        adm::Block s0; s0.rtime = 0.0; s0.duration = 2.0; s0.az = 90; st.push_back(s0);
        std::vector<intent::TrajPoint> ts = intent::trajectoryFromBlocks(st, 2.0);
        check(ts.size() == 1 && ts[0].t == 2.0, "static single block: one point at programme end");
    }

    // ==== E-124.3 — decode identity with decode_timeline (the R1 acceptance) ====
    {
        // Trajectory points AT the grid midpoints: sampleTrajectory returns each point exactly,
        // so computeDecode must agree 1:1 with computeTimeline over the same samples/speakers.
        const double start = 0.0, end = 1.0, ss = 0.1;
        const int n = intent::sliceCount(start, end, ss);
        check(n == 10, "slice grid: 10 slices over 1 s @ 0.1 s");
        intent::ObjectEntry o;
        o.id = 1; o.label = "ident"; o.track = 1; o.decodeLayout = "7.1.4";
        std::vector<decode_timeline::Sample> samples;
        for (int i = 0; i < n; ++i) {
            const double t = start + (i + 0.5) * ss;
            const double az = -90.0 + 180.0 * (double)i / (double)(n - 1);  // R -> L sweep
            o.trajectory.push_back(intent::TrajPoint{t, az, 15.0, 1.0, 1.0});
            samples.push_back(decode_timeline::Sample{t, az, 15.0, 1.0, 1.0});
        }
        check(intent::computeDecode(o, start, end, ss), "computeDecode runs");
        const intent::IntentLayout* lay = intent::findIntentLayout("7.1.4");
        std::vector<meter::Dir> dirs;
        std::vector<std::string> labels;
        for (const auto& s : lay->speakers) {
            if (s.lfe) continue;
            dirs.push_back(decode_timeline::dirFromAzEl(s.az, s.el));
            labels.push_back(s.label);
        }
        decode_timeline::ObjectTimeline tl =
            decode_timeline::computeTimeline(samples, dirs, intent::decodeOrder());
        bool identical = tl.slices.size() == o.dominant.size();
        if (identical)
            for (size_t i = 0; i < tl.slices.size(); ++i)
                if (labels[(size_t)tl.slices[i].dominantIdx] != o.dominant[i]) {
                    identical = false;
                    std::fprintf(stderr, "    slice %zu: timeline %s vs sidecar %s\n", i,
                                 labels[(size_t)tl.slices[i].dominantIdx].c_str(),
                                 o.dominant[i].c_str());
                    break;
                }
        check(identical, "per-slice dominant identical to object_decode_timeline's core (10/10)");
        check(o.dominant.front() == "M-090" && o.dominant.back() == "M+090",
              "sweep endpoints dominate the side speakers (R start, L end)");
        double covSum = 0.0;
        for (const auto& c : o.coverage) covSum += c.second;
        check(std::fabs(covSum - 1.0) < 1e-6, "coverage fractions sum to 1");
        check(o.coverage.size() == 11, "coverage keys = 11 non-LFE 7.1.4 speakers");
    }

    // ==== E-124.2 — byte-golden emissions x2 ====
    const intent::Sidecar admModel = makeAdmModel();
    const intent::Sidecar loomModel = makeLoomModel();
    const std::string admJson = intent::writeSidecarJson(admModel);
    const std::string loomJson = intent::writeSidecarJson(loomModel);
    {
        checkEq(intent::writeSidecarJson(admModel), admJson, "ADM emission byte-stable x2");
        checkEq(intent::writeSidecarJson(loomModel), loomJson, "Loom emission byte-stable x2");
        // The decode-free Loom golden is pinned exactly (every byte hand-derivable).
        checkEq(loomJson, std::string(kLoomGolden), "Loom golden (exact bytes)");
        // The ADM golden's decode block carries SH-computed floats; its bytes are pinned by
        // the x2 identity above plus these semantic pins (pre-registered):
        check(has(admJson, "\"producer\": {\"tool\": \"inseglet\", \"version\": \"1.6.0\", "
                           "\"export\": \"spatial.export_adm\"}"), "ADM producer line");
        check(has(admJson, "\"program\": {\"sampleRate\": 48000, \"start\": 0, \"end\": 1, "
                           "\"sliceSec\": 0.1}"), "ADM program line");
        std::string tenStatic;
        for (int i = 0; i < 10; ++i) tenStatic += std::string(i ? ", " : "") + "\"M+090\"";
        check(has(admJson, "\"dominant\": [" + tenStatic + "]"),
              "static object @ az +90: all 10 slices dominant M+090");
        check(has(admJson, "\"tolerances\": {\"posDeg\": 5, \"levelLu\": 0.5, "
                           "\"dominantFrac\": 0.9, \"covSilent\": 0.01, \"silentDb\": -40}"),
              "tolerance block == consumer defaults");
        checkEq(admJson, std::string(kAdmGolden), "ADM golden (exact bytes)");
    }

    // ==== E-124.5 — forbidden-key schema scan (never a deliverable-loudness key) ====
    {
        static const char* kForbidden[] = {"\"loudness\":", "\"integratedLufs\":", "\"lufs\":",
                                           "\"normalize\":", "\"dialnorm\":", "\"lkfs\":"};
        for (const std::string& j : {admJson, loomJson})
            for (const char* f : kForbidden)
                check(!has(j, f), std::string("no forbidden key ") + f);
        check(has(admJson, "\"expectLufs\":") && has(admJson, "\"expectRmsDb\":"),
              "levels are expect* claims only");
    }

    // ==== E-124.7 — every emission is valid JSON with the v0 envelope ====
    {
        for (const std::string& j : {admJson, loomJson}) {
            nlohmann::json p = nlohmann::json::parse(j, nullptr, false);
            check(!p.is_discarded(), "emission parses as JSON");
            if (!p.is_discarded()) {
                check(p.value("intent", -1) == 0, "envelope intent: 0");
                check(p.contains("program") && p.contains("tolerances"), "required blocks");
            }
        }
        nlohmann::json pa = nlohmann::json::parse(admJson);
        check(pa["objects"].size() == 2 && pa["objects"][0]["track"] == 7 &&
              pa["objects"][1]["trajectory"].size() == 3, "ADM objects round-trip");
        nlohmann::json pl = nlohmann::json::parse(loomJson);
        check(pl["bed"]["roster"].size() == 12 && pl["scene"]["expectRmsDb"].size() == 9 &&
              !pl.contains("objects"), "Loom bed/scene round-trip, no objects block");
        // escaping: a hostile label survives the round trip
        intent::Sidecar hostile = makeAdmModel();
        hostile.objects[0].label = "he said \"hi\"\\path\n";
        nlohmann::json ph = nlohmann::json::parse(intent::writeSidecarJson(hostile),
                                                  nullptr, false);
        check(!ph.is_discarded() &&
                  ph["objects"][0]["label"] == "he said \"hi\"\\path\n",
              "jsonEscape round-trips quotes/backslash/newline");
    }

    if (g_failures) {
        std::fprintf(stderr, "%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all intent-sidecar unit tests passed\n");
    return 0;
}
