// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_loom_manifest.cpp — pure unit test for the Inseglet -> iamf-loom bridge core
// (src/loom_manifest.h): the Inseglet<->Loom channel-order mapping pin (E-116.1), the
// hand-rolled identity-tone verification of the WAV writer (E-116.2 — the B4 method: one
// identifier sine per channel, 313 + 139·k Hz, read back through an INDEPENDENT in-test RIFF
// reader and detected per channel by Goertzel), the golden manifest emissions (E-116.3), and
// the emission rules (E-116.4: never a loudness value; policy only when non-default; M-207
// slugging; the S-208 default-name trap; season.yaml shape). No REAPER, no SDK — every number
// is deterministic. Expectations were pre-registered before implementation.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "loom_manifest.h"

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

// ---------------------------------------------------------------------------------------------
// Independent RIFF reader (deliberately NOT the production reader — the test is the second
// witness). Minimal: fmt + data chunks, 16/24-bit integer PCM, little-endian.
// ---------------------------------------------------------------------------------------------
struct WavRead {
    bool ok = false;
    int channels = 0, bits = 0, rate = 0;
    std::vector<std::vector<double>> ch;  // per-channel samples in [-1,1]
};
static uint32_t rdLE32(const std::string& s, size_t o) {
    return (uint32_t)(uint8_t)s[o] | ((uint32_t)(uint8_t)s[o + 1] << 8) |
           ((uint32_t)(uint8_t)s[o + 2] << 16) | ((uint32_t)(uint8_t)s[o + 3] << 24);
}
static uint16_t rdLE16(const std::string& s, size_t o) {
    return (uint16_t)((uint8_t)s[o] | ((uint8_t)s[o + 1] << 8));
}
static WavRead readWav(const std::string& f) {
    WavRead r;
    if (f.size() < 12 || f.substr(0, 4) != "RIFF" || f.substr(8, 4) != "WAVE") return r;
    size_t off = 12, dataOff = 0, dataLen = 0;
    while (off + 8 <= f.size()) {
        const std::string id = f.substr(off, 4);
        const uint32_t sz = rdLE32(f, off + 4);
        if (id == "fmt " && sz >= 16) {
            r.channels = rdLE16(f, off + 8 + 2);
            r.rate = (int)rdLE32(f, off + 8 + 4);
            r.bits = rdLE16(f, off + 8 + 14);
        } else if (id == "data") {
            dataOff = off + 8;
            dataLen = sz;
        }
        off += 8 + sz + (sz & 1);
    }
    if (!r.channels || !r.bits || !dataOff) return r;
    const int bps = r.bits / 8;
    const size_t frames = dataLen / (size_t)(r.channels * bps);
    r.ch.assign((size_t)r.channels, std::vector<double>(frames, 0.0));
    for (size_t fr = 0; fr < frames; ++fr) {
        for (int c = 0; c < r.channels; ++c) {
            const size_t o = dataOff + (fr * (size_t)r.channels + (size_t)c) * (size_t)bps;
            if (r.bits == 16) {
                int16_t v = (int16_t)rdLE16(f, o);
                r.ch[(size_t)c][fr] = v / 32768.0;
            } else {  // 24
                int32_t v = (int32_t)((uint32_t)(uint8_t)f[o] | ((uint32_t)(uint8_t)f[o + 1] << 8) |
                                      ((uint32_t)(uint8_t)f[o + 2] << 16));
                if (v & 0x800000) v |= (int32_t)0xFF000000;
                r.ch[(size_t)c][fr] = v / 8388608.0;
            }
        }
    }
    r.ok = true;
    return r;
}

// Goertzel power of frequency f (Hz) over the samples.
static double goertzel(const std::vector<double>& x, double f, double sr) {
    const double w = 2.0 * 3.14159265358979323846 * f / sr;
    const double c = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (double v : x) { s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

int main() {
    // ==== E-116.1 — the mapping pin (hard-coded second witness, NOT read from the header) ====
    {
        static const char* kInseglet714[12] = {"L", "R", "C", "LFE", "Lss", "Rss",
                                               "Lsr", "Rsr", "Ltf", "Rtf", "Ltr", "Rtr"};
        static const char* kLoom714[12] = {"L", "R", "C", "LFE", "Lss", "Rss",
                                           "Lrs", "Rrs", "Ltf", "Rtf", "Ltb", "Rtb"};
        const loomb::LoomBed* b = loomb::findLoomBed("7.1.4");
        check(b && b->channels == 12, "7.1.4 pin: present, 12 ch");
        if (b) {
            bool all = true;
            for (int i = 0; i < 12; ++i) {
                if (std::string(b->map[(size_t)i].inseglet) != kInseglet714[i] ||
                    std::string(b->map[(size_t)i].loom) != kLoom714[i]) {
                    all = false;
                    std::fprintf(stderr, "    idx %d: header says %s->%s, pin says %s->%s\n", i,
                                 b->map[(size_t)i].inseglet, b->map[(size_t)i].loom,
                                 kInseglet714[i], kLoom714[i]);
                }
                // side agreement (paired channels): same leading L/R on both spellings
                const char a = b->map[(size_t)i].inseglet[0], l = b->map[(size_t)i].loom[0];
                if ((a == 'L' || a == 'R') && a != l) all = false;
            }
            check(all, "7.1.4 pin: all 12 label pairs + sides match the pinned table");
            // spelling deltas are EXACTLY {6,7,10,11}
            bool deltasOk = true;
            for (int i = 0; i < 12; ++i) {
                const bool differs = std::string(b->map[(size_t)i].inseglet) !=
                                     std::string(b->map[(size_t)i].loom);
                const bool expected = (i == 6 || i == 7 || i == 10 || i == 11);
                if (differs != expected) deltasOk = false;
            }
            check(deltasOk, "7.1.4 pin: spelling deltas exactly at rear-surrounds + top-rears");
        }
        const loomb::LoomBed* s = loomb::findLoomBed("stereo");
        const loomb::LoomBed* f = loomb::findLoomBed("5.1");
        check(s && s->channels == 2 && f && f->channels == 6, "stereo + 5.1 pins present");
        bool ident = true;
        if (s) for (const auto& p : s->map)
            if (std::string(p.inseglet) != p.loom) ident = false;
        if (f) for (const auto& p : f->map)
            if (std::string(p.inseglet) != p.loom) ident = false;
        check(ident, "stereo + 5.1: spelling-identical at every index");
        check(loomb::findLoomBed("7.1") == nullptr && loomb::findLoomBed("9.1.6") == nullptr &&
              loomb::findLoomBed("7.1.2") == nullptr,
              "non-Loom beds (7.1 / 7.1.2 / 9.1.6) are not in the bridge's table");
        check(loomb::loomMaxAmbiOrder() == 4, "ambisonics cap pinned at Loom's order 4");
    }

    // ==== E-116.1(c) + E-116.2 — identity tones through the WAV writer (B4 method) ==========
    for (int nch : {12, 6, 2}) {
        const double sr = 48000.0;
        const size_t N = 48000;  // 1 s — every 313+139k is an integer cycle count
        std::vector<std::vector<float>> chans((size_t)nch);
        for (int k = 0; k < nch; ++k) {
            const double fk = 313.0 + 139.0 * k;
            chans[(size_t)k].resize(N);
            for (size_t i = 0; i < N; ++i)
                chans[(size_t)k][i] =
                    (float)(0.5 * std::sin(2.0 * 3.14159265358979323846 * fk * (double)i / sr));
        }
        const std::string wav = loomb::writeWavPcm(chans, N, 48000, 24);
        WavRead r = readWav(wav);
        check(r.ok && r.channels == nch && r.rate == 48000 && r.bits == 24,
              "tones " + std::to_string(nch) + "ch: WAV reads back 48k/24-bit/" +
                  std::to_string(nch) + "ch (independent reader)");
        bool identity = true;
        double worstMarginDb = 1e9;
        for (int k = 0; r.ok && k < nch; ++k) {
            double own = 0, best = -1;
            int bestIdx = -1;
            for (int j = 0; j < nch; ++j) {
                const double p = goertzel(r.ch[(size_t)k], 313.0 + 139.0 * j, sr);
                if (j == k) own = p;
                if (p > best) { best = p; bestIdx = j; }
            }
            if (bestIdx != k) identity = false;
            double worstOther = 0;
            for (int j = 0; j < nch; ++j) {
                if (j == k) continue;
                const double p = goertzel(r.ch[(size_t)k], 313.0 + 139.0 * j, sr);
                if (p > worstOther) worstOther = p;
            }
            const double margin =
                10.0 * std::log10(own / (worstOther > 0 ? worstOther : 1e-300));
            if (margin < worstMarginDb) worstMarginDb = margin;
        }
        check(identity, "tones " + std::to_string(nch) +
                            "ch: every channel k carries ITS OWN tone (identity mapping)");
        check(worstMarginDb >= 60.0,
              "tones " + std::to_string(nch) + "ch: off-diagonal rejection >= 60 dB (worst " +
                  std::to_string((int)worstMarginDb) + " dB)");
    }

    // ==== E-116.3(1) — golden: 7.1.4 bed -> iamf + mp4/youtube ==============================
    {
        loomb::BridgeModel m;
        m.title = "Feature Mix";
        m.sources = {{"main", "wavs/main.wav", "bed", "7.1.4"}};
        m.targets = {{"iamf", "dist/feature.iamf", "", ""},
                     {"mp4", "dist/feature.mp4", "v.mp4", "youtube"}};
        const std::string want =
            "loom: 0\n"
            "title: \"Feature Mix\"\n"
            "sources:\n"
            "  main: { path: wavs/main.wav, kind: bed, layout: \"7.1.4\" }\n"
            "elements:\n"
            "  main: { from: main }\n"
            "presentations:\n"
            "  - id: main\n"
            "    annotations: { en-us: \"Feature Mix\" }\n"
            "    elements:\n"
            "      - { ref: main }\n"
            "targets:\n"
            "  - { format: iamf, out: dist/feature.iamf }\n"
            "  - { format: mp4, out: dist/feature.mp4, video: v.mp4, preset: youtube }\n";
        checkEq(loomb::writeManifestYaml(m), want, "golden 1: 7.1.4 bed, iamf + youtube mp4");
    }

    // ==== E-116.3(2) — golden: stereo bed -> binaural preview ================================
    {
        loomb::BridgeModel m;
        m.title = "Review Copy";
        m.sources = {{"main", "wavs/main.wav", "bed", "stereo"}};
        m.targets = {{"preview", "dist/review.wav", "", ""}};
        m.binaural = true;
        const std::string want =
            "loom: 0\n"
            "title: \"Review Copy\"\n"
            "sources:\n"
            "  main: { path: wavs/main.wav, kind: bed, layout: \"stereo\" }\n"
            "elements:\n"
            "  main: { from: main }\n"
            "presentations:\n"
            "  - id: main\n"
            "    annotations: { en-us: \"Review Copy\" }\n"
            "    elements:\n"
            "      - { ref: main, headphones: binaural }\n"
            "targets:\n"
            "  - { format: preview, out: dist/review.wav }\n";
        checkEq(loomb::writeManifestYaml(m), want, "golden 2: stereo bed, binaural preview");
    }

    // ==== E-116.3(3) — golden: 3rd-order ambisonics -> archive flac mezzanine ===============
    {
        loomb::BridgeModel m;
        m.title = "Scene Master";
        m.sources = {{"scene", "wavs/scene.wav", "ambisonics", ""}};
        m.targets = {{"iamf", "dist/scene.iamf", "", "archive"}};
        m.codec = "flac";
        const std::string want =
            "loom: 0\n"
            "title: \"Scene Master\"\n"
            "sources:\n"
            "  scene: { path: wavs/scene.wav, kind: ambisonics }\n"
            "elements:\n"
            "  scene: { from: scene }\n"
            "presentations:\n"
            "  - id: main\n"
            "    annotations: { en-us: \"Scene Master\" }\n"
            "    elements:\n"
            "      - { ref: scene }\n"
            "policy:\n"
            "  codec: { name: flac }\n"
            "targets:\n"
            "  - { format: iamf, out: dist/scene.iamf, preset: archive }\n";
        checkEq(loomb::writeManifestYaml(m), want, "golden 3: 3OA scene, archive flac");
    }

    // ==== E-116.3(4) — golden: languages rows (bed + 2 VO stereo beds) ======================
    {
        loomb::BridgeModel m;
        m.title = "Multilang Feature";
        m.sources = {{"main", "wavs/main.wav", "bed", "7.1.4"},
                     {"vo_en", "wavs/vo_en.wav", "bed", "stereo"},
                     {"vo_de", "wavs/vo_de.wav", "bed", "stereo"}};
        m.languages = {{"en-us", "vo_en", "English"}, {"de-de", "vo_de", "Deutsch"}};
        m.targets = {{"iamf", "dist/multilang.iamf", "", ""}};
        const std::string want =
            "loom: 0\n"
            "title: \"Multilang Feature\"\n"
            "sources:\n"
            "  main: { path: wavs/main.wav, kind: bed, layout: \"7.1.4\" }\n"
            "  vo_en: { path: wavs/vo_en.wav, kind: bed, layout: \"stereo\" }\n"
            "  vo_de: { path: wavs/vo_de.wav, kind: bed, layout: \"stereo\" }\n"
            "elements:\n"
            "  main: { from: main }\n"
            "  vo_en: { from: vo_en }\n"
            "  vo_de: { from: vo_de }\n"
            "presentations:\n"
            "  - id: \"main-{lang}\"\n"
            "    languages:\n"
            "      - { lang: en-us, vo: vo_en, label: \"English\" }\n"
            "      - { lang: de-de, vo: vo_de, label: \"Deutsch\" }\n"
            "    elements:\n"
            "      - { ref: main }\n"
            "      - { ref: \"{vo}\" }\n"
            "targets:\n"
            "  - { format: iamf, out: dist/multilang.iamf }\n";
        checkEq(loomb::writeManifestYaml(m), want, "golden 4: languages expansion rows");
    }

    // ==== E-116.3(5) — golden: explicit normalize ===========================================
    {
        loomb::BridgeModel m;
        m.title = "YT Norm";
        m.sources = {{"main", "wavs/main.wav", "bed", "5.1"}};
        m.targets = {{"iamf", "dist/yt.iamf", "", ""}};
        m.haveNormalize = true;
        m.normalize = -14.0;
        const std::string y = loomb::writeManifestYaml(m);
        check(has(y, "policy:\n  loudness: { mode: measure, normalize: -14 }\n"),
              "golden 5: normalize emits mode: measure + the caller's LUFS");
    }

    // ==== E-116.3(6) — golden: bed + scene, two elements, one presentation ==================
    {
        loomb::BridgeModel m;
        m.title = "Hybrid";
        m.sources = {{"main", "wavs/main.wav", "bed", "7.1.4"},
                     {"scene", "wavs/scene.wav", "ambisonics", ""}};
        m.targets = {{"iamf", "dist/hybrid.iamf", "", ""}};
        const std::string y = loomb::writeManifestYaml(m);
        check(has(y, "  main: { path: wavs/main.wav, kind: bed, layout: \"7.1.4\" }\n") &&
                  has(y, "  scene: { path: wavs/scene.wav, kind: ambisonics }\n"),
              "golden 6: both sources declared");
        check(has(y, "      - { ref: main }\n      - { ref: scene }\n"),
              "golden 6: one presentation references both elements");
    }

    // ==== E-116.4 — emission rules ===========================================================
    {
        // Never a loudness VALUE: no measured/lufs/integrated key in any emission; no
        // `loudness` at all unless the caller passed normalize.
        loomb::BridgeModel m;
        m.title = "Plain";
        m.sources = {{"main", "wavs/main.wav", "bed", "stereo"}};
        m.targets = {{"iamf", "dist/plain.iamf", "", ""}};
        const std::string y = loomb::writeManifestYaml(m);
        check(!has(y, "loudness") && !has(y, "lufs") && !has(y, "measured") &&
                  !has(y, "integrated") && !has(y, "policy"),
              "rules: default emission carries NO policy and NO loudness anything");
        check(has(y, "loom: 0\n") && has(y, "title: "), "rules: loom: 0 + title always present");

        // M-207 slugging.
        check(loomb::slugIdent("Mix Bus (final)", "main") == "Mix_Bus__final_",
              "rules: slugIdent maps non-identifier chars to _");
        check(loomb::slugIdent("___", "main") == "main" &&
                  loomb::slugIdent("", "main") == "main",
              "rules: degenerate names fall back");

        // S-208 default-shaped names.
        check(loomb::defaultShapedName("Track 3") && loomb::defaultShapedName("object 12") &&
                  loomb::defaultShapedName("Audio 7") && loomb::defaultShapedName(""),
              "rules: default-shaped names detected");
        check(!loomb::defaultShapedName("Dialog Stem") && !loomb::defaultShapedName("Track One") &&
                  !loomb::defaultShapedName("Tracker 5"),
              "rules: real names not flagged");

        // Flow-mapping path quoting: a {episode}-templated out is NOT valid YAML unquoted inside
        // { ... } (found live at the acceptance gate — M-102 parse error); the emitter must quote it.
        loomb::BridgeModel t;
        t.title = "Season One";
        t.sources = {{"main", "wavs/main.wav", "bed", "7.1.4"}};
        t.targets = {{"iamf", "dist/{episode}.iamf", "", ""}};
        const std::string ty = loomb::writeManifestYaml(t);
        check(has(ty, "  - { format: iamf, out: \"dist/{episode}.iamf\" }\n"),
              "rules: {episode}-templated out is double-quoted in the flow mapping");
        check(has(loomb::writeManifestYaml(m), "out: dist/plain.iamf }"),
              "rules: flow-safe out paths stay unquoted (goldens unchanged)");

        // season.yaml shape.
        const std::string s = loomb::writeSeasonYaml("manifest.yaml", {"ep01", "ep02"});
        const std::string wantSeason =
            "loom_batch: 0\n"
            "manifest: manifest.yaml\n"
            "jobs:\n"
            "  - { id: ep01, vars: { episode: ep01 } }\n"
            "  - { id: ep02, vars: { episode: ep02 } }\n";
        checkEq(s, wantSeason, "rules: season.yaml batch spec shape");

        // WAV writer: float never written; 16/24 only (bad depth coerces to 24).
        std::vector<std::vector<float>> one = {{0.25f, -0.25f}};
        WavRead w32 = readWav(loomb::writeWavPcm(one, 2, 48000, 32));
        check(w32.ok && w32.bits == 24, "rules: unsupported bit depth coerces to 24-bit int");
        // Deterministic interleave spot-check: ch0 +0.5, ch1 -0.5 at frame 0.
        std::vector<std::vector<float>> two = {{0.5f}, {-0.5f}};
        WavRead w = readWav(loomb::writeWavPcm(two, 1, 48000, 24));
        check(w.ok && w.ch[0][0] > 0.49 && w.ch[1][0] < -0.49,
              "rules: interleave keeps channel order (identity)");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d LOOM-MANIFEST UNIT TEST FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nALL LOOM-MANIFEST UNIT TESTS PASSED\n");
    return 0;
}
