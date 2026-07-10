// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_head_track.cpp — unit tests for the batch-e head-tracking OSC parser + orientation math.
//
// Exercises the SDK-free pieces of src/head_track_bridge.h: OSC 1.0 message + #bundle decoding
// (bounds-safe), quaternion->Euler(deg), and the OSC->orientation mapping (per-axis floats, a 3-float
// ypr, a 4-float quaternion, and carry-over of axes not present in a batch). No sockets, no REAPER.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "head_track_bridge.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}
static bool near(double a, double b, double eps = 0.05) { return std::fabs(a - b) <= eps; }

// --- tiny OSC packer (big-endian, 4-byte aligned), mirrors what a head-tracker sends ---
static void putStr(std::vector<char>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back('\0');
    while (b.size() % 4 != 0) b.push_back('\0');
}
static void putF32(std::vector<char>& b, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    b.push_back((char)((u >> 24) & 0xFF));
    b.push_back((char)((u >> 16) & 0xFF));
    b.push_back((char)((u >> 8) & 0xFF));
    b.push_back((char)(u & 0xFF));
}
static void putI32(std::vector<char>& b, int32_t v) {
    uint32_t u = (uint32_t)v;
    b.push_back((char)((u >> 24) & 0xFF));
    b.push_back((char)((u >> 16) & 0xFF));
    b.push_back((char)((u >> 8) & 0xFF));
    b.push_back((char)(u & 0xFF));
}
// address + type-tag string (leading ',') + already-appended args
static std::vector<char> oscMsg(const std::string& addr, const std::string& types) {
    std::vector<char> b;
    putStr(b, addr);
    putStr(b, "," + types);
    return b;
}

int main() {
    // --- 1. single float message: /yaw ,f 45.0 ---
    {
        std::vector<char> b = oscMsg("/yaw", "f");
        putF32(b, 45.0f);
        std::vector<OscMessage> msgs;
        bool ok = parseOscPacket(b.data(), b.size(), msgs);
        check(ok && msgs.size() == 1, "parse single /yaw message");
        check(!msgs.empty() && msgs[0].address == "/yaw", "address decoded");
        check(!msgs.empty() && msgs[0].floats.size() == 1 && near(msgs[0].floats[0], 45.0),
              "float arg decoded (45.0)");
    }

    // --- 2. int arg coerced to float: /pitch ,i -30 ---
    {
        std::vector<char> b = oscMsg("/pitch", "i");
        putI32(b, -30);
        std::vector<OscMessage> msgs;
        parseOscPacket(b.data(), b.size(), msgs);
        check(msgs.size() == 1 && msgs[0].floats.size() == 1 && near(msgs[0].floats[0], -30.0),
              "int32 arg coerced to float (-30)");
    }

    // --- 3. string+float mixed args are skipped correctly: /head ,sf "hi" 12.5 ---
    {
        std::vector<char> b = oscMsg("/head", "sf");
        putStr(b, "hi");
        putF32(b, 12.5f);
        std::vector<OscMessage> msgs;
        parseOscPacket(b.data(), b.size(), msgs);
        check(msgs.size() == 1 && msgs[0].floats.size() == 1 && near(msgs[0].floats[0], 12.5),
              "string arg skipped, trailing float kept");
    }

    // --- 4. #bundle carrying two messages ---
    {
        std::vector<char> y = oscMsg("/yaw", "f");   putF32(y, 10.0f);
        std::vector<char> p = oscMsg("/pitch", "f"); putF32(p, 20.0f);
        std::vector<char> b;
        putStr(b, "#bundle");
        for (int i = 0; i < 8; ++i) b.push_back('\0');  // timetag
        putI32(b, (int32_t)y.size()); b.insert(b.end(), y.begin(), y.end());
        putI32(b, (int32_t)p.size()); b.insert(b.end(), p.begin(), p.end());
        std::vector<OscMessage> msgs;
        parseOscPacket(b.data(), b.size(), msgs);
        check(msgs.size() == 2, "bundle decodes two elements");
        check(msgs.size() == 2 && msgs[0].address == "/yaw" && msgs[1].address == "/pitch",
              "bundle element addresses in order");
    }

    // --- 5. bounds safety: truncated datagrams must not crash / over-read ---
    {
        std::vector<char> b = oscMsg("/quaternion", "ffff");
        putF32(b, 1.0f);  // only 1 of 4 floats present
        std::vector<OscMessage> msgs;
        parseOscPacket(b.data(), b.size(), msgs);  // must return, not crash
        check(true, "truncated arg list handled without crash");
        std::vector<OscMessage> m2;
        parseOscPacket("/yaw", 4, m2);  // no null terminator inside range
        check(true, "unterminated address handled without crash");
        std::vector<OscMessage> m3;
        parseOscPacket("", 0, m3);
        check(m3.empty(), "empty datagram yields no messages");
    }

    // --- 6. quaternion -> Euler: identity is (0,0,0) ---
    {
        HeadOrientation o;
        quaternionToEulerDeg(1, 0, 0, 0, o);
        check(near(o.yaw, 0) && near(o.pitch, 0) && near(o.roll, 0), "identity quaternion -> 0,0,0");
    }

    // --- 7. quaternion -> Euler: 90 deg yaw about Z ---
    {
        // q = (cos(45),0,0,sin(45)) rotates +90 deg about Z (yaw).
        double h = std::sqrt(0.5);
        HeadOrientation o;
        quaternionToEulerDeg(h, 0, 0, h, o);
        check(near(o.yaw, 90.0, 0.1), "quaternion (cos45,0,0,sin45) -> yaw ~90");
        check(near(o.pitch, 0, 0.1) && near(o.roll, 0, 0.1), "…pitch/roll ~0");
    }

    // --- 8. quaternion -> Euler: 90 deg roll about X ---
    {
        double h = std::sqrt(0.5);
        HeadOrientation o;
        quaternionToEulerDeg(h, h, 0, 0, o);
        check(near(o.roll, 90.0, 0.1), "quaternion (cos45,sin45,0,0) -> roll ~90");
    }

    // --- 9. mapOscToOrientation: per-axis, with carry-over ---
    {
        std::vector<char> b = oscMsg("/yaw", "f"); putF32(b, 33.0f);
        std::vector<OscMessage> msgs; parseOscPacket(b.data(), b.size(), msgs);
        HeadOrientation prev{5, 6, 7};
        OrientationUpdate up = mapOscToOrientation(msgs, prev);
        check(up.yaw && !up.pitch && !up.roll, "only yaw flagged");
        check(near(up.o.yaw, 33.0) && near(up.o.pitch, 6.0) && near(up.o.roll, 7.0),
              "yaw updated; pitch/roll carried over from prev");
    }

    // --- 10. mapOscToOrientation: 3-float /ypr ---
    {
        std::vector<char> b = oscMsg("/ypr", "fff");
        putF32(b, 1.0f); putF32(b, 2.0f); putF32(b, 3.0f);
        std::vector<OscMessage> msgs; parseOscPacket(b.data(), b.size(), msgs);
        OrientationUpdate up = mapOscToOrientation(msgs, HeadOrientation{});
        check(up.yaw && up.pitch && up.roll, "ypr updates all three axes");
        check(near(up.o.yaw, 1) && near(up.o.pitch, 2) && near(up.o.roll, 3), "ypr values in order");
    }

    // --- 11. mapOscToOrientation: /SceneRotator/quaternions (4 floats) ---
    {
        double h = std::sqrt(0.5);
        std::vector<char> b = oscMsg("/SceneRotator/quaternions", "ffff");
        putF32(b, (float)h); putF32(b, 0); putF32(b, 0); putF32(b, (float)h);
        std::vector<OscMessage> msgs; parseOscPacket(b.data(), b.size(), msgs);
        OrientationUpdate up = mapOscToOrientation(msgs, HeadOrientation{});
        check(up.yaw && up.pitch && up.roll, "quaternion message updates all three axes");
        check(near(up.o.yaw, 90.0, 0.2), "quaternion address -> yaw ~90");
    }

    // --- 12. mapOscToOrientation: no recognized address -> no update ---
    {
        std::vector<char> b = oscMsg("/gain", "f"); putF32(b, 0.5f);
        std::vector<OscMessage> msgs; parseOscPacket(b.data(), b.size(), msgs);
        OrientationUpdate up = mapOscToOrientation(msgs, HeadOrientation{1, 2, 3});
        check(!up.any(), "unrecognized address yields no orientation update");
    }

    // --- 13. angleToParamValue: degrees -> normalized [0,1] param over a discovered degree range ---
    {
        // Normalized [0,1] param spanning -180..+180: 0 deg -> 0.5, +90 -> 0.75, +180 -> 1.0, -180 -> 0.
        check(near(angleToParamValue(0, 0, 1, -180, 180), 0.5), "0 deg -> 0.5 on a [0,1]/±180 param (center)");
        check(near(angleToParamValue(90, 0, 1, -180, 180), 0.75), "+90 deg -> 0.75 on a ±180 param");
        check(near(angleToParamValue(-90, 0, 1, -180, 180), 0.25), "-90 deg -> 0.25 on a ±180 param");
        check(near(angleToParamValue(180, 0, 1, -180, 180), 1.0), "+180 deg -> 1.0 (full)");
        check(near(angleToParamValue(-180, 0, 1, -180, 180), 0.0), "-180 deg -> 0.0 (full)");
        check(near(angleToParamValue(400, 0, 1, -180, 180), 1.0), "out-of-range degrees clamp to 1.0");
        // Elevation-style ±90 span: +45 deg -> 0.75 (per-axis range matters, not a fixed 360 span).
        check(near(angleToParamValue(45, 0, 1, -90, 90), 0.75), "+45 deg on a ±90 param -> 0.75");
        check(near(angleToParamValue(0, 0, 1, -90, 90), 0.5), "0 deg on a ±90 param -> 0.5 (center)");
        // Real-degree param (host range span > 2): written through, clamped (degLo/degHi ignored).
        check(near(angleToParamValue(90, -180, 180, -180, 180), 90.0), "+90 -> 90 on a real-degree param");
        check(near(angleToParamValue(999, -180, 180, -180, 180), 180.0), "degree param clamps to its max");
    }

    // --- 14. bridge lifecycle: stop() is safe when never started ---
    {
        HeadTrackBridge::instance().stop();
        check(!HeadTrackBridge::instance().running(), "bridge idle after stop()");
        check(HeadTrackBridge::instance().packetsReceived() == 0, "no packets when idle");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL HEAD-TRACK CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d HEAD-TRACK CHECK(S) FAILED\n", g_failures);
    return 1;
}
