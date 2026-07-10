// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_damf.cpp — pure unit test for the Dolby Atmos Master File (DAMF) triad serializer + parser
// (src/damf.h): the adm-spherical -> DAMF room coordinate mapping, the big-endian CAF essence writer,
// the .atmos / .atmos.metadata YAML emitters, and the inspect round-trip. No REAPER, no SDK — every
// number is deterministic.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "damf.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // ---- coordinate mapping: adm spherical (az +left, el +up, dist) -> DAMF room [x,y,z] ----
    {
        double x, y, z;
        damf::admToRoom(0, 0, 1, x, y, z);
        check(near(x, 0) && near(y, 1) && near(z, 0), "front (az0,el0) -> [0,1,0]");
        damf::admToRoom(90, 0, 1, x, y, z);
        check(near(x, -1) && near(y, 0) && near(z, 0), "hard-left (az+90) -> x=-1");
        damf::admToRoom(-90, 0, 1, x, y, z);
        check(near(x, 1) && near(y, 0) && near(z, 0), "hard-right (az-90) -> x=+1");
        damf::admToRoom(180, 0, 1, x, y, z);
        check(near(x, 0) && near(y, -1) && near(z, 0), "back (az180) -> y=-1");
        damf::admToRoom(0, 90, 1, x, y, z);
        check(near(x, 0) && near(y, 0) && near(z, 1), "overhead (el+90) -> z=1");
        damf::admToRoom(0, 45, 1, x, y, z);
        check(near(z, 0.70711), "el+45 -> z=sin45");
        damf::admToRoom(0, -45, 1, x, y, z);
        check(near(z, 0), "below-floor (el-45) folds to z=0 (no below-floor in Atmos)");
    }

    // ---- CAF essence: big-endian, integer PCM, correct header + interleave ----
    {
        // 2 channels, 3 frames, 24-bit.
        std::vector<std::vector<float>> ch = {{0.0f, 0.5f, -0.5f}, {1.0f, -1.0f, 0.0f}};
        std::string caf = damf::writeCaf(ch, 3, 48000, 24);
        damf::CafInfo ci = damf::parseCaf(caf);
        check(ci.ok, "parseCaf ok");
        check(ci.formatID == "lpcm", "CAF formatID lpcm");
        check(ci.channels == 2, "CAF channels 2");
        check(ci.bitDepth == 24, "CAF bitDepth 24");
        check(near(ci.sampleRate, 48000.0, 1.0), "CAF sampleRate 48000");
        check(!ci.isFloat, "CAF is integer PCM");
        check(!ci.littleEndian, "CAF is big-endian");
        check(ci.frames == 3, "CAF frames 3");

        // Byte-level big-endian proof: 1 ch, 1 frame, value 1.0 @16-bit -> 0x7F 0xFF.
        std::string caf16 = damf::writeCaf({{1.0f}}, 1, 48000, 16);
        size_t dpos = caf16.find("data");
        check(dpos != std::string::npos, "16-bit CAF has data chunk");
        // data: 'data'(4) + size(8) + editCount(4) => pcm at dpos+16
        check((uint8_t)caf16[dpos + 16] == 0x7F && (uint8_t)caf16[dpos + 17] == 0xFF,
              "16-bit full-scale is big-endian 0x7FFF");
    }

    // ---- build a Model (3-ch bed + 2 objects with trajectories) and serialize the triad ----
    adm::Model m;
    m.sampleRate = 48000;
    m.bitDepth = 24;
    m.durationSec = 2.0;
    m.bedLayoutName = "bed3";
    m.bed = { {"L","M+030",30,0,false}, {"R","M-030",-30,0,false}, {"C","M+000",0,0,false} };
    {
        adm::Object o0; o0.name = "Obj0"; o0.objectNumber = 1;
        adm::Block b0; b0.rtime = 0.0; b0.az = 90;  b0.el = 0; b0.dist = 1;   // hard left
        adm::Block b1; b1.rtime = 1.0; b1.az = -90; b1.el = 0; b1.dist = 1;   // hard right
        o0.blocks = {b0, b1};
        m.objects.push_back(o0);
        adm::Object o1; o1.name = "Obj1"; o1.objectNumber = 2;
        adm::Block c0; c0.rtime = 0.0; c0.az = 0; c0.el = 45; c0.dist = 1;    // front + up
        o1.blocks = {c0};
        m.objects.push_back(o1);
    }

    // essence: 5 channels (3 bed + 2 obj), 96000 frames of silence is wasteful — use a tiny frame count.
    const size_t frames = 480;
    std::vector<std::vector<float>> channels(5, std::vector<float>(frames, 0.0f));

    // ---- manifest ----
    std::string manifest = damf::writeManifest(m, "song.atmos.audio", "song.atmos.metadata");
    check(has(manifest, "metadata: song.atmos.metadata"), "manifest names the metadata file");
    check(has(manifest, "audio: song.atmos.audio"), "manifest names the audio file");
    check(has(manifest, "bedInstances:"), "manifest has bedInstances");
    check(has(manifest, "{channel: L, ID: 1}"), "bed channel L is input ID 1");
    check(has(manifest, "{channel: C, ID: 3}"), "bed channel C is input ID 3");
    check(has(manifest, "{ID: 4, name: \"Obj0\"}"), "object 0 is input ID 4 (after 3 bed channels)");
    check(has(manifest, "{ID: 5, name: \"Obj1\"}"), "object 1 is input ID 5");

    // ---- metadata ----
    std::string meta = damf::writeMetadata(m);
    check(has(meta, "sampleRate: 48000"), "metadata sampleRate 48000");
    check(has(meta, "{ID: 1, samplePos: 0, active: true}"), "bed channel event is static (no pos)");
    check(has(meta, "{ID: 4, samplePos: 0, active: true, pos: [-1, 0, 0]}"),
          "object0 block0 = hard left at t0");
    check(has(meta, "{ID: 4, samplePos: 48000, active: true, pos: [1, 0, 0]}"),
          "object0 block1 = hard right at t=1s (samplePos 48000)");
    check(has(meta, "{ID: 5, samplePos: 0, active: true, pos: [0, 0.707107, 0.707107]}"),
          "object1 = front + up");

    // ---- triad assembly + guards ----
    damf::Triad bad = damf::writeTriad(m, std::vector<std::vector<float>>(4), frames, "song");
    check(!bad.ok && bad.error == "channel_count_mismatch", "writeTriad rejects wrong channel count");
    damf::Triad t = damf::writeTriad(m, channels, frames, "song");
    check(t.ok, "writeTriad ok");
    check(t.channels == 5, "triad channel count 5");

    // ---- inspect round-trip ----
    Json ins = damf::inspectTriad(t.atmos, t.atmosMetadata, t.atmosAudio);
    check(ins["isDamf"].get<bool>(), "inspect: isDamf");
    check(ins["manifest"]["objects"].get<int>() == 2, "inspect: 2 objects");
    check(ins["manifest"]["bedChannels"].get<int>() == 3, "inspect: 3 bed channels");
    check(ins["audio"]["channels"].get<int>() == 5, "inspect: CAF 5 channels");
    check(ins["metadata"]["events"].get<int>() == 6,
          "inspect: 6 events (3 static bed + 3 object trajectory blocks)");
    check(ins["audio"]["frames"].get<double>() == (double)frames, "inspect: frame count round-trips");
    check(ins["warnings"].empty(), "inspect: no warnings on a well-formed triad");

    if (g_failures == 0) std::fprintf(stderr, "\nALL DAMF UNIT TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d DAMF UNIT TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
