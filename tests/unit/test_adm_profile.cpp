// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_adm_profile.cpp — pure unit test for Batch O3, the Dolby Atmos Master ADM Profile v1.0
// conformance layer (src/adm_profile.h): normalizeModel (writer path), validateModel (pre-serialize),
// validateParsed (analysis.adm_profile_check), and the serializer's profile output (interpolationLength
// ramp + cartesian clamp in adm_bwf.h). No REAPER, no SDK — synthetic models, deterministic assertions.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "adm_profile.h"

using namespace reaper_mcp;
using namespace reaper_mcp::adm;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Does the report carry a violation with this code?
static bool hasViolation(const profile::Report& r, const std::string& code) {
    for (const auto& v : r.violations) if (v.code == code) return true;
    return false;
}

static void addBed712(Model& m) {
    m.bedLayoutName = "7.1.2";
    const std::vector<std::string> labels = {"L","R","C","LFE","Lss","Rss","Lrs","Rrs","Ltf","Rtf"};
    for (const auto& l : labels) {
        BedSpeaker s; s.label = l;
        SpeakerPos p = speakerPosFor("7.1.2", l);
        s.az = p.az; s.el = p.el; s.speakerLabel = p.speakerLabel; s.lfe = (l == "LFE");
        m.bed.push_back(s);
    }
}

// An O2-rich object: spherical, unequal extent (degrees), divergence, importance — the profile's
// three deviations plus the wrong coordinate mode, all at once.
static Object o2RichObject() {
    Object o; o.name = "Wide"; o.objectNumber = 1; o.coord = Coord::Spherical; o.importance = 5;
    Block b0; b0.rtime = 0.0; b0.duration = 0.0; b0.az = 30; b0.el = 10; b0.dist = 1.0; b0.gain = 1.0;
    b0.width = 45; b0.height = 20; b0.depth = 0.2;
    b0.hasDivergence = true; b0.divergence = 0.5; b0.divergenceRange = 30;
    Block b1 = b0; b1.rtime = 0.5; b1.az = -30;
    o.blocks = {b0, b1};
    return o;
}

static std::vector<std::vector<float>> silent(int nch, size_t frames) {
    return std::vector<std::vector<float>>((size_t)nch, std::vector<float>(frames, 0.02f));
}

int main() {
    // ---- (1) bedLayoutAllowed: only the Atmos-legal beds (max 7.1.2), objects-only ok ----
    {
        check(profile::bedLayoutAllowed(""),      "objects-only bed allowed");
        check(profile::bedLayoutAllowed("5.1"),   "5.1 bed allowed");
        check(profile::bedLayoutAllowed("7.1"),   "7.1 bed allowed");
        check(profile::bedLayoutAllowed("7.1.2"), "7.1.2 bed allowed");
        check(!profile::bedLayoutAllowed("7.1.4"), "7.1.4 bed REJECTED (not an Atmos bed)");
        check(!profile::bedLayoutAllowed("9.1.6"), "9.1.6 bed REJECTED");
        check(!profile::bedLayoutAllowed("22.2"),  "22.2 bed REJECTED");
    }

    // ---- (2) normalizeModel: forces cartesian, strips divergence, collapses extent, drops importance ----
    {
        Model m; m.sampleRate = 48000; m.durationSec = 1.0;
        m.objects.push_back(o2RichObject());
        std::vector<std::string> warns; std::string err;
        bool ok = profile::normalizeModel(m, warns, err);
        check(ok && err.empty(), "normalize ok on a valid-bed 48k model");
        check(m.dolbyProfile, "dolbyProfile flag set");
        const Object& o = m.objects[0];
        check(o.coord == Coord::Cartesian, "object forced to cartesian");
        check(o.importance < 0, "importance dropped");
        for (const Block& b : o.blocks) {
            check(!b.hasDivergence, "divergence stripped from every block");
            check(b.width == b.height && b.height == b.depth, "extent collapsed to identical size");
            check(b.width >= 0.0 && b.width <= 1.0, "extent clamped to [0,1]");
        }
        check(!warns.empty(), "normalization emitted warnings");
    }

    // ---- (3) normalizeModel fails closed on unfixable invariants ----
    {
        // non-Atmos bed
        Model m1; m1.sampleRate = 48000; addBed712(m1); m1.bedLayoutName = "7.1.4";
        std::vector<std::string> w1; std::string e1;
        check(!profile::normalizeModel(m1, w1, e1) && e1 == "bed_not_profile_conformant",
              "7.1.4 bed fails closed");
        // non-48k
        Model m2; m2.sampleRate = 44100; m2.objects.push_back(o2RichObject());
        std::vector<std::string> w2; std::string e2;
        check(!profile::normalizeModel(m2, w2, e2) && e2 == "sample_rate_not_48k",
              "44.1k fails closed");
        // > 118 objects
        Model m3; m3.sampleRate = 48000;
        for (int i = 0; i < 119; ++i) { Object o; o.name = "o"; o.coord = Coord::Cartesian;
            Block b; o.blocks = {b}; m3.objects.push_back(o); }
        std::vector<std::string> w3; std::string e3;
        check(!profile::normalizeModel(m3, w3, e3) && e3 == "too_many_objects",
              "119 objects fails closed");
    }

    // ---- (4) validateModel: raw O2 model non-conformant, normalized model conformant ----
    {
        Model raw; raw.sampleRate = 48000; raw.objects.push_back(o2RichObject());
        profile::Report rr = profile::validateModel(raw);
        check(!rr.conformant, "raw O2 model non-conformant");
        check(hasViolation(rr, "coordinate"), "flags spherical coordinate");
        check(hasViolation(rr, "divergence"), "flags divergence");
        check(hasViolation(rr, "importance"), "flags importance");
        check(hasViolation(rr, "extent"),     "flags unequal extent");

        Model norm; norm.sampleRate = 48000; norm.objects.push_back(o2RichObject());
        std::vector<std::string> w; std::string e; profile::normalizeModel(norm, w, e);
        profile::Report nr = profile::validateModel(norm);
        check(nr.conformant, "normalized model conformant");
        check(nr.violations.empty(), "no violations after normalize");
    }

    // ---- (5) serializer profile output: interpolationLength ramp (0 then 250 samples) + cartesian ----
    {
        Model m; m.sampleRate = 48000; m.durationSec = 1.0;
        m.objects.push_back(o2RichObject());
        std::vector<std::string> w; std::string e; profile::normalizeModel(m, w, e);
        const std::string x = buildAxml(m);
        check(x.find("<jumpPosition interpolationLength=\"0\">1</jumpPosition>") != std::string::npos,
              "first block interpolationLength = 0");
        check(x.find("interpolationLength=\"0.005208\"") != std::string::npos,
              "subsequent block interpolationLength = 250/48000 s");
        check(x.find("<cartesian>1</cartesian>") != std::string::npos, "objects emitted cartesian");
        check(x.find("<objectDivergence") == std::string::npos, "no objectDivergence in profile output");
        check(x.find("importance=") == std::string::npos, "no importance attr in profile output");
    }

    // ---- (6) cartesian clamp: a dist>1 object clamps X/Y/Z to [-1,1] ----
    {
        Model m; m.sampleRate = 48000; m.durationSec = 0.5;
        Object o; o.name = "Far"; o.coord = Coord::Cartesian;
        Block b; b.rtime = 0; b.az = 90; b.el = 0; b.dist = 2.0;  // X would be -2
        o.blocks = {b};
        m.objects.push_back(o);
        std::vector<std::string> w; std::string e; profile::normalizeModel(m, w, e);
        const std::string x = buildAxml(m);
        check(x.find("coordinate=\"X\">-2") == std::string::npos, "unclamped X=-2 not emitted");
        check(x.find("coordinate=\"X\">-1") != std::string::npos, "X clamped to -1");
    }

    // ---- (7) validateParsed: a conformant profile file passes end-to-end ----
    {
        Model m; m.sampleRate = 48000; m.bitDepth = 24; m.durationSec = 1.0;
        addBed712(m);
        Object o; o.name = "Obj"; o.coord = Coord::Cartesian;
        Block b; b.rtime = 0; b.az = 45; b.el = 0; b.dist = 1; b.gain = 1; b.width = b.height = b.depth = 0.3;
        o.blocks = {b};
        m.objects.push_back(o);
        std::vector<std::string> w; std::string e;
        check(profile::normalizeModel(m, w, e), "normalize the bed+object model");
        size_t frames = 48000;
        WriteResult wr = writeAdmImage(m, silent(m.channelCount(), frames), frames);
        check(wr.ok, "profile file writes");
        ParseResult pr = parseAdmImage(wr.bytes);
        check(pr.ok && !pr.axml.empty(), "parse exposes axml");
        profile::Report rep = profile::validateParsed(pr);
        for (const auto& v : rep.violations) std::fprintf(stderr, "    (unexpected violation: %s — %s)\n",
                                                          v.code.c_str(), v.detail.c_str());
        check(rep.conformant, "conformant profile file validates clean");
    }

    // ---- (8) validateParsed: a raw O2 (non-profile) file is flagged with the right codes ----
    {
        Model m; m.sampleRate = 48000; m.durationSec = 1.0;
        m.objects.push_back(o2RichObject());   // spherical, divergence, importance, unequal extent
        WriteResult wr = writeAdmImage(m, silent(1, 48000), 48000);   // NOT dolbyProfile
        ParseResult pr = parseAdmImage(wr.bytes);
        profile::Report rep = profile::validateParsed(pr);
        check(!rep.conformant, "raw O2 file non-conformant");
        check(hasViolation(rep, "coordinate"),    "file: flags non-cartesian objects");
        check(hasViolation(rep, "divergence"),    "file: flags objectDivergence");
        check(hasViolation(rep, "importance"),    "file: flags importance");
        check(hasViolation(rep, "extent"),        "file: flags unequal extent");
        check(hasViolation(rep, "interpolation"), "file: flags missing interpolationLength");
    }

    // ---- (9) validateParsed: sample-rate violation on a 44.1k file ----
    {
        Model m; m.sampleRate = 44100; m.durationSec = 0.5;
        Object o; o.name = "Obj"; o.coord = Coord::Cartesian;
        Block b; b.rtime = 0; b.az = 0; b.el = 0; b.dist = 1; b.width = b.height = b.depth = 0.2;
        o.blocks = {b};
        m.dolbyProfile = true;  // emit interpolationLength so only sample_rate is wrong
        m.objects.push_back(o);
        WriteResult wr = writeAdmImage(m, silent(1, 22050), 22050);
        ParseResult pr = parseAdmImage(wr.bytes);
        profile::Report rep = profile::validateParsed(pr);
        check(hasViolation(rep, "sample_rate"), "file: flags 44.1k sample rate");
    }

    if (g_failures == 0) std::fprintf(stderr, "\nALL ADM PROFILE TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d ADM PROFILE TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
