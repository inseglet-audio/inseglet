// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// head_track_bridge.h
//
// Live head-tracking for the binaural monitor.
//
// A self-contained OSC-over-UDP receiver that drives an ambisonic scene rotator's (or a
// binauraliser's) yaw/pitch/roll parameters in real time, so a head-tracker (phone/IMU/Supperware/
// nvsonic/Unity) can steer the binaural monitor image.
//
// Design (mirrors the project's threading model):
//   - A dedicated worker thread blocks on recvfrom() and decodes OSC 1.0 datagrams (messages AND
//     #bundle), extracting a head orientation (yaw/pitch/roll in degrees). Trackers commonly send
//     /yaw //pitch //roll floats, a 3-float /ypr (or /orientation//euler), or a 4-float /quaternion
//     (w,x,y,z) — all supported. The latest orientation is stored under a mutex + a dirty flag
//     (last-writer-wins), so a 100 Hz tracker stream is COALESCED to one param write per UI tick
//     rather than flooding the API (the same coalescing idea as the SubscriptionHub).
//   - applyOnMainThread() is called from the control-surface Run() pump (MAIN THREAD). If running,
//     dirty, and the target track is still valid (ValidatePtr2), it writes the three rotator params
//     via TrackFX_SetParam, clamped to each param's own [min,max] captured at start(). The apply is
//     the ONLY place that touches the REAPER API, and it runs on the main thread — the invariant the
//     whole extension is built on.
//
// SDK independence: the OSC parser + quaternion->Euler math + orientation mapping are plain,
// dependency-free free functions (unit-tested in tests/unit/test_head_track.cpp). Only the
// TrackFX_SetParam apply is gated on REAPER_MCP_HAVE_SDK. The UDP listener is POSIX-only in this
// build; on Windows start() returns false with a clear message (avoids the winsock/windows.h include
// ordering trap — the primary immersive platform here is macOS/Linux). No new REAPERAPI WANTs:
// ValidatePtr2 + TrackFX_SetParam are already imported.

#pragma once

#include "reaper_api.h"  // FIRST: SDK headers + SWELL min/max #undef (no-op without REAPER_MCP_HAVE_SDK)

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

#ifndef REAPER_MCP_HAVE_SDK
class MediaTrack;  // forward decl so the target signature is well-formed without the SDK
#endif

namespace reaper_mcp {

// ---------------------------------------------------------------------------------------------
// SDK-free OSC 1.0 + orientation helpers (unit-tested — no sockets, no REAPER).
// ---------------------------------------------------------------------------------------------

struct HeadOrientation {
    double yaw = 0.0, pitch = 0.0, roll = 0.0;  // degrees
};

// One decoded OSC message: its address + every numeric arg coerced to float (f/i/d).
struct OscMessage {
    std::string address;
    std::vector<float> floats;
};

namespace osc_detail {

inline uint32_t rdU32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline float rdF32(const unsigned char* p) {
    uint32_t u = rdU32(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
inline double rdF64(const unsigned char* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | (uint64_t)p[i];
    double d;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}
// Read a null-terminated OSC string starting at `off`, padded to a 4-byte boundary. Advances `off`.
inline bool rdString(const unsigned char* d, size_t len, size_t& off, std::string& out) {
    size_t i = off;
    while (i < len && d[i] != 0) ++i;
    if (i >= len) return false;  // no terminator inside the datagram
    out.assign(reinterpret_cast<const char*>(d) + off, i - off);
    size_t adv = (i - off) + 1;             // include the null
    adv = (adv + 3) & ~static_cast<size_t>(3);
    off += adv;
    return true;
}

}  // namespace osc_detail

// Recursively decode an OSC packet (a message, or a #bundle of elements) into `out`. Bounds-safe:
// a malformed datagram yields whatever decoded cleanly and never reads out of range.
inline bool parseOscElement(const unsigned char* d, size_t len, std::vector<OscMessage>& out,
                            int depth = 0) {
    if (depth > 8 || len < 4) return false;

    if (len >= 8 && std::memcmp(d, "#bundle", 7) == 0 && d[7] == 0) {
        size_t off = 16;  // 8 bytes "#bundle\0" + 8 bytes OSC timetag
        while (off + 4 <= len) {
            uint32_t sz = osc_detail::rdU32(d + off);
            off += 4;
            if (sz == 0 || off + sz > len) break;
            parseOscElement(d + off, sz, out, depth + 1);
            off += sz;
        }
        return true;
    }

    if (d[0] != '/') return false;  // an OSC message address always starts with '/'
    size_t off = 0;
    OscMessage msg;
    if (!osc_detail::rdString(d, len, off, msg.address)) return false;

    std::string types;
    if (!osc_detail::rdString(d, len, off, types) || types.empty() || types[0] != ',') {
        out.push_back(std::move(msg));  // address-only (no type tags) — still a valid message
        return true;
    }
    for (size_t k = 1; k < types.size(); ++k) {
        switch (types[k]) {
            case 'f':
                if (off + 4 > len) { out.push_back(std::move(msg)); return true; }
                msg.floats.push_back(osc_detail::rdF32(d + off));
                off += 4;
                break;
            case 'i':
                if (off + 4 > len) { out.push_back(std::move(msg)); return true; }
                msg.floats.push_back(static_cast<float>(static_cast<int32_t>(osc_detail::rdU32(d + off))));
                off += 4;
                break;
            case 'd':
                if (off + 8 > len) { out.push_back(std::move(msg)); return true; }
                msg.floats.push_back(static_cast<float>(osc_detail::rdF64(d + off)));
                off += 8;
                break;
            case 'h':  // int64 — skip
            case 't':  // timetag — skip
                if (off + 8 > len) { out.push_back(std::move(msg)); return true; }
                off += 8;
                break;
            case 's':
            case 'S': {
                std::string tmp;
                if (!osc_detail::rdString(d, len, off, tmp)) { out.push_back(std::move(msg)); return true; }
                break;
            }
            case 'b': {  // blob: int32 size + padded data
                if (off + 4 > len) { out.push_back(std::move(msg)); return true; }
                uint32_t bs = osc_detail::rdU32(d + off);
                off += 4;
                size_t padded = (static_cast<size_t>(bs) + 3) & ~static_cast<size_t>(3);
                if (off + padded > len) { out.push_back(std::move(msg)); return true; }
                off += padded;
                break;
            }
            case 'T': case 'F': case 'N': case 'I':
                break;  // no payload bytes
            default:
                // Unknown tag: we can't know its width, so stop reading args (keep what we have).
                out.push_back(std::move(msg));
                return true;
        }
    }
    out.push_back(std::move(msg));
    return true;
}

inline bool parseOscPacket(const char* data, size_t len, std::vector<OscMessage>& out) {
    return parseOscElement(reinterpret_cast<const unsigned char*>(data), len, out, 0);
}

// Standard ZYX (yaw-pitch-roll) Tait-Bryan quaternion-to-Euler conversion.
// Unit quaternion (w,x,y,z) -> yaw/pitch/roll degrees, ZYX Tait-Bryan (aerospace / IEM convention:
// yaw about Z, pitch about Y, roll about X). Matches SceneRotator's default yaw/pitch/roll axes.
inline void quaternionToEulerDeg(double w, double x, double y, double z, HeadOrientation& o) {
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n > 1e-9) { w /= n; x /= n; y /= n; z /= n; }
    const double R2D = 57.29577951308232;
    double sinr_cosp = 2.0 * (w * x + y * z);
    double cosr_cosp = 1.0 - 2.0 * (x * x + y * y);
    o.roll = std::atan2(sinr_cosp, cosr_cosp) * R2D;
    double sinp = 2.0 * (w * y - z * x);
    if (sinp > 1.0) sinp = 1.0;
    if (sinp < -1.0) sinp = -1.0;
    o.pitch = std::asin(sinp) * R2D;
    double siny_cosp = 2.0 * (w * z + x * y);
    double cosy_cosp = 1.0 - 2.0 * (y * y + z * z);
    o.yaw = std::atan2(siny_cosp, cosy_cosp) * R2D;
}

// Map a head-orientation angle (degrees) to a rotator PARAMETER value. IEM/JUCE ambisonic rotators
// expose yaw/pitch/roll as a NORMALIZED [0,1] parameter (default 0.5 = 0 deg), so writing a raw degree
// value would clamp to an extreme (e.g. +90 -> 1.0 -> +180). When the param range looks normalized
// (span <= 2) we map deg linearly onto the param's real degree range [degLo,degHi] (discovered from the
// plug-in, e.g. -180..180 for yaw/pitch/roll, -90..90 for elevation); otherwise the param is already in
// real degree units (some JSFX are), so we write it directly. Always clamped.
inline double angleToParamValue(double deg, double mn, double mx, double degLo, double degHi) {
    if (mx - mn <= 2.0 && degHi != degLo) {   // normalized [0,1]-style angle param over [degLo,degHi]
        double pos = (deg - degLo) / (degHi - degLo);
        if (pos < 0.0) pos = 0.0;
        if (pos > 1.0) pos = 1.0;
        return mn + pos * (mx - mn);
    }
    double v = deg;                            // real-degree param: write through, clamped
    if (mx > mn) { if (v < mn) v = mn; if (v > mx) v = mx; }
    return v;
}

// Which axes an OSC batch updated, plus the resulting orientation (carried over from `prev` for
// axes not present in this batch).
struct OrientationUpdate {
    bool yaw = false, pitch = false, roll = false;
    HeadOrientation o;
    bool any() const { return yaw || pitch || roll; }
};

inline OrientationUpdate mapOscToOrientation(const std::vector<OscMessage>& msgs,
                                             const HeadOrientation& prev) {
    OrientationUpdate up;
    up.o = prev;
    for (const auto& m : msgs) {
        std::string a = m.address;
        for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto has = [&](const char* s) { return a.find(s) != std::string::npos; };
        if (has("quat")) {
            if (m.floats.size() >= 4) {
                quaternionToEulerDeg(m.floats[0], m.floats[1], m.floats[2], m.floats[3], up.o);
                up.yaw = up.pitch = up.roll = true;
            }
        } else if (has("ypr") || has("euler") || has("orient")) {
            if (m.floats.size() >= 3) {
                up.o.yaw = m.floats[0];
                up.o.pitch = m.floats[1];
                up.o.roll = m.floats[2];
                up.yaw = up.pitch = up.roll = true;
            }
        } else if (has("yaw")) {
            if (!m.floats.empty()) { up.o.yaw = m.floats[0]; up.yaw = true; }
        } else if (has("pitch")) {
            if (!m.floats.empty()) { up.o.pitch = m.floats[0]; up.pitch = true; }
        } else if (has("roll")) {
            if (!m.floats.empty()) { up.o.roll = m.floats[0]; up.roll = true; }
        }
    }
    return up;
}

// ---------------------------------------------------------------------------------------------
// The live bridge (singleton). Socket/thread lifecycle + main-thread apply.
// ---------------------------------------------------------------------------------------------

struct HeadTrackTarget {
    MediaTrack* track = nullptr;  // validated via ValidatePtr2 on every apply
    int fx = -1;
    int yawParam = -1, pitchParam = -1, rollParam = -1;
    double yawMin = 0, yawMax = 0, pitchMin = 0, pitchMax = 0, rollMin = 0, rollMax = 0;
    // Real degree range each normalized param spans (discovered from the plug-in; ±180 default).
    double yawDegLo = -180, yawDegHi = 180, pitchDegLo = -180, pitchDegHi = 180,
           rollDegLo = -180, rollDegHi = 180;
    bool invertYaw = false, invertPitch = false, invertRoll = false;
};

struct HeadTrackConfig {
    int port = 9000;
    std::string bind = "127.0.0.1";  // "0.0.0.0" to accept LAN/phone trackers
};

class HeadTrackBridge {
public:
    static HeadTrackBridge& instance() {
        static HeadTrackBridge b;
        return b;
    }

    // MAIN THREAD. Start (replacing any existing) the UDP listener + set the rotator target.
    // Returns true on success; on failure sets `err` and leaves the bridge stopped.
    bool start(const HeadTrackTarget& tgt, const HeadTrackConfig& cfg, std::string& err) {
#if defined(_WIN32)
        (void)tgt; (void)cfg;
        err = "head-tracking OSC listener is not supported on Windows in this build (macOS/Linux only)";
        return false;
#else
        stop();  // replace any prior listener
        int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) { err = "socket() failed"; return false; }
        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 200 ms recv timeout so the loop can observe stop()
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
        if (cfg.bind.empty() || cfg.bind == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, cfg.bind.c_str(), &addr.sin_addr) != 1) {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        bool fellBack = false;
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            addr.sin_port = 0;  // requested port busy — let the OS assign one and report it back
            fellBack = true;
            if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(s);
                err = "bind() failed on port " + std::to_string(cfg.port) + " and on an OS-assigned port";
                return false;
            }
        }
        int actualPort = cfg.port;
        sockaddr_in bound;
        socklen_t bl = sizeof(bound);
        std::memset(&bound, 0, sizeof(bound));
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bl) == 0)
            actualPort = ntohs(bound.sin_port);

        {
            std::lock_guard<std::mutex> lk(m_);
            target_ = tgt;
            bind_ = cfg.bind;
            boundPort_ = actualPort;
            portFellBack_ = fellBack;
            orient_ = HeadOrientation{};
        }
        sock_.store(s);
        packets_.store(0);
        dirty_.store(false);
        running_.store(true);
        thread_ = std::thread([this] { recvLoop(); });
        return true;
#endif
    }

    // MAIN THREAD (or teardown). Idempotent. Closes the socket, joins the worker.
    void stop() {
        running_.store(false);
#if !defined(_WIN32)
        int s = sock_.exchange(-1);
        if (s >= 0) {
            ::shutdown(s, SHUT_RDWR);
            ::close(s);
        }
#endif
        if (thread_.joinable()) thread_.join();
    }

    // MAIN THREAD, every UI tick. Coalesced apply of the latest orientation to the rotator params.
    void applyOnMainThread() {
#ifdef REAPER_MCP_HAVE_SDK
        if (!running_.load()) return;
        if (!dirty_.exchange(false)) return;
        HeadTrackTarget t;
        HeadOrientation o;
        {
            std::lock_guard<std::mutex> lk(m_);
            t = target_;
            o = orient_;
        }
        if (!t.track) return;
        if (!ValidatePtr2(nullptr, t.track, "MediaTrack*")) {  // track deleted under us — stop cleanly
            stop();
            return;
        }
        auto setAxis = [&](int p, double deg, double mn, double mx, double dLo, double dHi, bool inv) {
            if (p < 0) return;
            // Convert the head-tracked angle (degrees) into the param's units (normalized or degrees),
            // so an IEM SceneRotator [0,1] param actually rotates to the tracked angle, not an extreme.
            TrackFX_SetParam(t.track, t.fx, p, angleToParamValue(inv ? -deg : deg, mn, mx, dLo, dHi));
        };
        setAxis(t.yawParam, o.yaw, t.yawMin, t.yawMax, t.yawDegLo, t.yawDegHi, t.invertYaw);
        setAxis(t.pitchParam, o.pitch, t.pitchMin, t.pitchMax, t.pitchDegLo, t.pitchDegHi, t.invertPitch);
        setAxis(t.rollParam, o.roll, t.rollMin, t.rollMax, t.rollDegLo, t.rollDegHi, t.invertRoll);
#endif
    }

    bool running() const { return running_.load(); }
    int boundPort() const {
        std::lock_guard<std::mutex> lk(m_);
        return boundPort_;
    }
    bool portFellBack() const {
        std::lock_guard<std::mutex> lk(m_);
        return portFellBack_;
    }
    std::string bindAddr() const {
        std::lock_guard<std::mutex> lk(m_);
        return bind_;
    }
    HeadOrientation lastOrientation() const {
        std::lock_guard<std::mutex> lk(m_);
        return orient_;
    }
    uint64_t packetsReceived() const { return packets_.load(); }

private:
    HeadTrackBridge() = default;
    ~HeadTrackBridge() { stop(); }
    HeadTrackBridge(const HeadTrackBridge&) = delete;
    HeadTrackBridge& operator=(const HeadTrackBridge&) = delete;

    void recvLoop() {
#if !defined(_WIN32)
        std::vector<char> buf(8192);
        while (running_.load()) {
            int s = sock_.load();
            if (s < 0) break;
            sockaddr_in src;
            socklen_t sl = sizeof(src);
            std::memset(&src, 0, sizeof(src));
            ssize_t n = ::recvfrom(s, buf.data(), buf.size(), 0,
                                   reinterpret_cast<sockaddr*>(&src), &sl);
            if (n <= 0) continue;  // timeout / interrupted / closed — re-check running_
            std::vector<OscMessage> msgs;
            if (!parseOscPacket(buf.data(), static_cast<size_t>(n), msgs) || msgs.empty()) continue;
            packets_.fetch_add(1);
            HeadOrientation prev;
            {
                std::lock_guard<std::mutex> lk(m_);
                prev = orient_;
            }
            OrientationUpdate up = mapOscToOrientation(msgs, prev);
            if (!up.any()) continue;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (up.yaw) orient_.yaw = up.o.yaw;
                if (up.pitch) orient_.pitch = up.o.pitch;
                if (up.roll) orient_.roll = up.o.roll;
            }
            dirty_.store(true);
        }
#endif
    }

    mutable std::mutex m_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{false};
    std::atomic<int> sock_{-1};
    std::atomic<uint64_t> packets_{0};
    int boundPort_ = 0;
    bool portFellBack_ = false;
    std::string bind_;
    HeadTrackTarget target_;
    HeadOrientation orient_;
};

}  // namespace reaper_mcp
