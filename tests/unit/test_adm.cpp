// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_adm.cpp — pure unit test for the Batch O1 ADM (Audio Definition Model) BWF authoring +
// inspection (src/adm_bwf.h): model -> chna/axml/RIFF/BW64 serialization, the spherical->cartesian
// mapping, BS.2051 speaker positions, and the round-trip parser. No REAPER, no SDK — synthetic models
// only, so every assertion is deterministic.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "adm_bwf.h"

using namespace reaper_mcp;
using namespace reaper_mcp::adm;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// Build a canonical 7.1.2 bed (10 ch) from the label list, positions via speakerPosFor.
static void addBed(Model& m, const std::string& layout, const std::vector<std::string>& labels) {
    m.bedLayoutName = layout;
    for (const auto& l : labels) {
        BedSpeaker s; s.label = l;
        SpeakerPos p = speakerPosFor(layout, l);
        s.az = p.az; s.el = p.el; s.speakerLabel = p.speakerLabel; s.lfe = (l == "LFE" || l == "LFE1" || l == "LFE2");
        m.bed.push_back(s);
    }
}
static Object movingObject(const std::string& name, int num, Coord c) {
    Object o; o.name = name; o.objectNumber = num; o.coord = c; o.positionSource = "envelope";
    Block b0; b0.rtime = 0.0; b0.duration = 0.0; b0.az = 90; b0.el = 0;  b0.dist = 1.0; b0.gain = 1.0;
    Block b1; b1.rtime = 0.5; b1.duration = 0.0; b1.az = -90; b1.el = 30; b1.dist = 0.8; b1.gain = 0.9;
    o.blocks = {b0, b1};
    return o;
}
static std::vector<std::vector<float>> silentChannels(int nch, size_t frames) {
    return std::vector<std::vector<float>>((size_t)nch, std::vector<float>(frames, 0.05f));
}

int main() {
    const std::vector<std::string> bed712 = {"L","R","C","LFE","Lss","Rss","Lrs","Rrs","Ltf","Rtf"};

    // ---- (1) full round-trip: 7.1.2 bed + 2 objects ----
    {
        Model m; m.sampleRate = 48000; m.bitDepth = 24; m.durationSec = 1.0;
        addBed(m, "7.1.2", bed712);
        m.objects.push_back(movingObject("Object 1", 1, Coord::Spherical));
        m.objects.push_back(movingObject("Object 2", 2, Coord::Cartesian));
        check(m.channelCount() == 12, "channelCount = 10 bed + 2 objects");

        size_t frames = 48000;
        WriteResult w = writeAdmImage(m, silentChannels(m.channelCount(), frames), frames);
        check(w.ok, "write ok");
        check(!w.bw64, "sub-4GiB payload uses classic RIFF");
        check(w.bytes.substr(0, 4) == "RIFF" && w.bytes.substr(8, 4) == "WAVE", "RIFF/WAVE magic");

        ParseResult pr = parseAdmImage(w.bytes);
        check(pr.ok, "parse ok");
        check(pr.summary["isAdm"].get<bool>(), "parsed as ADM (chna+axml present)");
        check(pr.summary["container"].get<std::string>() == "RIFF/WAVE", "container RIFF/WAVE");
        check(pr.summary["format"]["channels"].get<int>() == 12, "fmt channels 12");
        check(pr.summary["format"]["bitDepth"].get<int>() == 24, "fmt bitDepth 24");
        check(near(pr.summary["format"]["durationSec"].get<double>(), 1.0, 1e-6), "duration 1s");
        check(pr.summary["chna"]["numTracks"].get<int>() == 12, "chna numTracks 12");
        check(pr.summary["chna"]["numUIDs"].get<int>() == 12, "chna numUIDs 12");
        check(pr.summary["adm"]["audioObjects"].get<int>() == 3, "3 audioObjects (bed + 2)");
        check(pr.summary["adm"]["audioBlockFormats"].get<int>() == 14, "14 blocks (10 bed static + 2+2)");
        check(pr.summary["adm"]["audioTrackUIDs"].get<int>() == 12, "12 audioTrackUIDs");
        check(pr.summary["adm"]["hasDirectSpeakers"].get<bool>(), "axml has DirectSpeakers");
        check(pr.summary["adm"]["hasObjects"].get<bool>(), "axml has Objects");
        check(pr.summary["adm"]["coordinateMode"].get<std::string>() == "cartesian",
              "cartesian object flips coordinate mode to cartesian");
        check(pr.summary["warnings"].empty(), "no structural warnings");

        // chna field-format sanity: first UID/refs match the ID scheme + column widths.
        const Json& t0 = pr.summary["chna"]["tracks"][0];
        check(t0["trackIndex"].get<int>() == 1, "first chna trackIndex = 1");
        check(t0["uid"].get<std::string>() == "ATU_00000001", "first UID ATU_00000001 (12 char)");
        check(t0["trackRef"].get<std::string>() == "AT_00011001_01", "first trackRef AT_..._01 (14 char)");
        check(t0["packRef"].get<std::string>() == "AP_00011001", "bed packRef AP_00011001 (11 char)");
        const Json& tObj = pr.summary["chna"]["tracks"][10];
        check(tObj["packRef"].get<std::string>() == "AP_00031001", "first object packRef AP_00031001 (Objects type)");
    }

    // ---- (2) objects-only (no bed) ----
    {
        Model m; m.sampleRate = 48000; m.durationSec = 2.0;
        m.objects.push_back(movingObject("Dialog", 1, Coord::Spherical));
        size_t frames = 96000;
        WriteResult w = writeAdmImage(m, silentChannels(1, frames), frames);
        check(w.ok && w.channels == 1, "objects-only writes 1 channel");
        ParseResult pr = parseAdmImage(w.bytes);
        check(pr.summary["adm"]["audioObjects"].get<int>() == 1, "objects-only: 1 audioObject");
        check(!pr.summary["adm"]["hasDirectSpeakers"].get<bool>(), "objects-only: no DirectSpeakers");
        check(pr.summary["adm"]["coordinateMode"].get<std::string>() == "spherical", "spherical mode");
    }

    // ---- (3) BW64 path via threshold override (no multi-GiB payload needed) ----
    {
        Model m; m.sampleRate = 48000; m.durationSec = 0.1;
        addBed(m, "7.1.2", bed712);
        size_t frames = 4800;
        WriteResult w = writeAdmImage(m, silentChannels(10, frames), frames, /*bw64Threshold*/ 100);
        check(w.ok && w.bw64, "threshold override forces BW64 header");
        check(w.bytes.substr(0, 4) == "BW64", "BW64 magic");
        ParseResult pr = parseAdmImage(w.bytes);
        check(pr.ok, "BW64 parse ok");
        check(pr.summary["container"].get<std::string>() == "BW64", "container BW64");
        check(pr.summary["format"]["channels"].get<int>() == 10, "BW64 fmt channels 10");
        check((uint64_t)pr.summary["format"]["frames"].get<double>() == 4800,
              "BW64 data size resolved from ds64 -> correct frame count");
        check(pr.summary["isAdm"].get<bool>(), "BW64 still ADM");
    }

    // ---- (4) speaker positions (BS.2051) ----
    {
        check(near(speakerPosFor("7.1.4", "L").az, 30, 1e-9), "L az +30");
        check(near(speakerPosFor("7.1.4", "R").az, -30, 1e-9), "R az -30");
        check(near(speakerPosFor("7.1.4", "C").az, 0, 1e-9) && near(speakerPosFor("7.1.4","C").el,0,1e-9), "C az/el 0");
        check(near(speakerPosFor("5.1", "Ls").az, 110, 1e-9), "5.1 Ls az +110 (rear)");
        check(near(speakerPosFor("7.1", "Ls").az, 90, 1e-9), "7.1 Ls az +90 (side)");
        check(near(speakerPosFor("7.1.4", "Ltf").el, 30, 1e-9), "Ltf elevation +30 (upper)");
        check(std::string(speakerPosFor("7.1.4", "L").speakerLabel) == "M+030", "L speakerLabel M+030");
        check(std::string(speakerPosFor("7.1.4", "Ltf").speakerLabel) == "U+045", "Ltf speakerLabel U+045");
    }

    // ---- (5) spherical -> cartesian mapping ----
    {
        double X, Y, Z;
        sphToCart(0, 0, 1, X, Y, Z);
        check(near(X, 0, 1e-9) && near(Y, 1, 1e-9) && near(Z, 0, 1e-9), "front (az0) -> Y=+1");
        sphToCart(90, 0, 1, X, Y, Z);
        check(near(X, -1, 1e-9) && near(Y, 0, 1e-9), "az +90 (left) -> X=-1 (left is -right)");
        sphToCart(-90, 0, 1, X, Y, Z);
        check(near(X, 1, 1e-9), "az -90 (right) -> X=+1");
        sphToCart(0, 90, 1, X, Y, Z);
        check(near(Z, 1, 1e-9), "el +90 -> Z=+1 (up)");
    }

    // ---- (6) ADM time formatting ----
    {
        check(admTime(0.0) == "00:00:00.00000", "t=0");
        check(admTime(1.5) == "00:00:01.50000", "t=1.5s");
        check(admTime(3661.25) == "01:01:01.25000", "t=1h1m1.25s");
    }

    // ---- (7) static (single-block) object + LFE frequency + bit-depth scaling ----
    {
        Model m; m.sampleRate = 48000; m.bitDepth = 16; m.durationSec = 1.0;
        addBed(m, "5.1", {"L","R","C","LFE","Ls","Rs"});
        Object o; o.name = "Static"; o.objectNumber = 1; o.coord = Coord::Spherical;
        Block b; b.rtime = 0; b.duration = 0; b.az = 0; b.el = 0; b.dist = 1; b.gain = 1;
        o.blocks = {b};
        m.objects.push_back(o);
        size_t frames = 48000;
        WriteResult w16 = writeAdmImage(m, silentChannels(7, frames), frames);
        check(w16.ok, "16-bit write ok");
        // LFE frequency element present in axml
        check(buildAxml(m).find("<frequency typeDefinition=\"lowPass\">") != std::string::npos,
              "LFE emits a lowPass frequency element");
        ParseResult pr = parseAdmImage(w16.bytes);
        check(pr.summary["adm"]["audioBlockFormats"].get<int>() == 7, "6 bed static + 1 object block");

        // 24-bit data is 1.5x the 16-bit data size for the same frames/channels.
        m.bitDepth = 24;
        WriteResult w24 = writeAdmImage(m, silentChannels(7, frames), frames);
        size_t d16 = 0, d24 = 0;
        for (const auto& c : parseAdmImage(w16.bytes).summary["chunks"]) if (c["id"] == "data") d16 = (size_t)c["size"].get<double>();
        for (const auto& c : parseAdmImage(w24.bytes).summary["chunks"]) if (c["id"] == "data") d24 = (size_t)c["size"].get<double>();
        check(d24 == d16 / 2 * 3, "24-bit data = 1.5x 16-bit data");
    }

    // ---- (8) non-ADM WAV: parser flags missing chunks, isAdm false ----
    {
        // Minimal RIFF/WAVE with fmt+data only (no chna/axml).
        std::string f = "RIFF";
        auto le32 = [&](std::string& o, uint32_t v){ for(int i=0;i<4;++i) o.push_back((char)((v>>(8*i))&0xFF)); };
        auto le16 = [&](std::string& o, uint16_t v){ o.push_back((char)(v&0xFF)); o.push_back((char)((v>>8)&0xFF)); };
        std::string fmt; le16(fmt,1); le16(fmt,2); le32(fmt,48000); le32(fmt,48000*2*2); le16(fmt,4); le16(fmt,16);
        std::string data(400, '\0');
        std::string body = "WAVE";
        body += "fmt "; le32(body,(uint32_t)fmt.size()); body += fmt;
        body += "data"; le32(body,(uint32_t)data.size()); body += data;
        le32(f, (uint32_t)body.size()); f += body;
        ParseResult pr = parseAdmImage(f);
        check(pr.ok, "plain WAV parses");
        check(!pr.summary["isAdm"].get<bool>(), "plain WAV is not ADM");
        bool warnedChna = false;
        for (const auto& wgn : pr.summary["warnings"]) if (wgn.get<std::string>().find("chna") != std::string::npos) warnedChna = true;
        check(warnedChna, "missing chna is warned");
    }

    // ---- (9) Batch O2: per-object extent / objectDivergence / importance metadata ----
    {
        // A spherical object carrying extent (width/height/depth) + divergence + importance.
        Model m; m.sampleRate = 48000; m.bitDepth = 24; m.durationSec = 1.0;
        Object o; o.name = "Wide"; o.objectNumber = 1; o.coord = Coord::Spherical; o.importance = 7;
        Block b; b.rtime = 0; b.duration = 0; b.az = 30; b.el = 10; b.dist = 1; b.gain = 1;
        b.width = 45; b.height = 20; b.depth = 0.2;
        b.hasDivergence = true; b.divergence = 0.5; b.divergenceRange = 30;
        o.blocks = {b};
        m.objects.push_back(o);

        const std::string x = buildAxml(m);
        check(x.find("importance=\"7\"") != std::string::npos, "audioObject carries importance=\"7\"");
        check(x.find("<width>45</width>") != std::string::npos, "extent width emitted (45 deg)");
        check(x.find("<height>20</height>") != std::string::npos, "extent height emitted");
        check(x.find("<depth>0.2</depth>") != std::string::npos, "extent depth emitted");
        check(x.find("<objectDivergence azimuthRange=\"30\">0.5</objectDivergence>") != std::string::npos,
              "spherical objectDivergence emits azimuthRange + value");

        WriteResult w = writeAdmImage(m, silentChannels(1, 48000), 48000);
        check(w.ok, "O2 metadata object writes");
        ParseResult pr = parseAdmImage(w.bytes);
        check(pr.summary["adm"]["hasImportance"].get<bool>(), "parser: hasImportance true");
        check(pr.summary["adm"]["hasExtent"].get<bool>(), "parser: hasExtent true");
        check(pr.summary["adm"]["hasObjectDivergence"].get<bool>(), "parser: hasObjectDivergence true");
        check(pr.summary["adm"]["divergenceBlocks"].get<int>() == 1, "parser: 1 divergence block");
        check(pr.summary["adm"]["objectImportance"].size() == 1 &&
              pr.summary["adm"]["objectImportance"][0].get<int>() == 7,
              "parser: objectImportance = [7]");
    }
    {
        // Cartesian divergence emits positionRange (not azimuthRange).
        Model m; m.sampleRate = 48000; m.bitDepth = 16; m.durationSec = 0.5;
        Object oc; oc.name = "Cart"; oc.objectNumber = 1; oc.coord = Coord::Cartesian;
        Block bc; bc.rtime = 0; bc.duration = 0; bc.az = 90; bc.el = 0; bc.dist = 1;
        bc.hasDivergence = true; bc.divergence = 0.25; bc.divergenceRange = 0.4;
        oc.blocks = {bc};
        m.objects.push_back(oc);
        const std::string xc = buildAxml(m);
        check(xc.find("<objectDivergence positionRange=\"0.4\">0.25</objectDivergence>") != std::string::npos,
              "cartesian objectDivergence emits positionRange");

        // Backward-compat: an object with NO O2 metadata emits none of the new elements/attrs, and the
        // parser reports every O2 flag false (an existing ADM BWF round-trips byte-identically).
        Model m2; m2.sampleRate = 48000; m2.durationSec = 0.5;
        m2.objects.push_back(movingObject("Plain", 1, Coord::Spherical));
        const std::string x2 = buildAxml(m2);
        check(x2.find("importance=") == std::string::npos, "no importance attr when unset");
        check(x2.find("<objectDivergence") == std::string::npos, "no objectDivergence when unset");
        check(x2.find("<width>") == std::string::npos, "no extent when unset");
        ParseResult pr2 = parseAdmImage(writeAdmImage(m2, silentChannels(1, 24000), 24000).bytes);
        check(!pr2.summary["adm"]["hasImportance"].get<bool>(), "parser: hasImportance false (plain)");
        check(!pr2.summary["adm"]["hasObjectDivergence"].get<bool>(),
              "parser: hasObjectDivergence false (plain)");
        check(!pr2.summary["adm"]["hasExtent"].get<bool>(), "parser: hasExtent false (plain)");
    }

    if (g_failures == 0) std::fprintf(stderr, "\nALL ADM BWF TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d ADM BWF TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
