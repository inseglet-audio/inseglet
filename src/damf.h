// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// damf.h — native Dolby Atmos Master File (DAMF) authoring + inspection.
//
// adm_bwf.h authors an ITU-R BS.2076 ADM Broadcast-Wave (a single .wav carrying chna+axml). The Dolby
// Atmos Renderer / Conversion Tool interchange also accepts a second native master shape — the DAMF
// "triad": three same-basename files that split the same object+bed deliverable across a text manifest,
// a text position track, and a Core Audio essence file:
//
//   <base>.atmos           YAML manifest — version, presentation, the file references, and the element
//                          roster (bed channels + objects) each with a 1-based input ID.
//   <base>.atmos.metadata  YAML — sampleRate + a flat list of position `events` keyed by element ID:
//                          bed channels are static (no `pos`), objects carry one event per trajectory
//                          block ({ID, samplePos, active, pos:[x,y,z]}).
//   <base>.atmos.audio     Core Audio Format (CAF) PCM — the interleaved essence, BED CHANNELS FIRST
//                          then OBJECTS in roster order (the same channel order writeAdmImage() uses).
//
// This is the SAME adm::Model the ADM BWF writer consumes — a DirectSpeakers bed (<= 7.1.2) plus N mono
// objects with a position/gain trajectory — re-serialized to the triad instead of a BWF. So the tool
// bodies build ONE Model + render ONE set of stems (render_stems.h) and can emit either master.
//
// Coordinate frame (DAMF room, from the Dolby Atmos Master ADM Profile + the Dolby Renderer readout):
//   x  -1 = left   .. +1 = right
//   y  -1 = back   .. +1 = front
//   z   0 = floor  ..  1 = ceiling   (NOTE: z is [0,1] here, unlike ADM BWF cartesian's [-1,1] — the
//                                     Atmos floor plane is the listener/ear plane; there is no below-floor)
// admToRoom() reuses adm::sphToCart() (which already yields x=+right, y=+front in [-1,1]) and folds the
// up axis to [0,1].
//
// Constraints are the Dolby Atmos Master ADM Profile v1.0 (adm_profile.h already encodes + enforces them
// upstream on the Model: 48 kHz, <=128 channels, <=118 objects, bed <= 7.1.2). This header does NOT
// re-validate; it serializes a Model the tool has already normalized.
//
// SDK-free (no REAPER). Pulls adm_bwf.h for adm::Model / adm::sphToCart / adm::fmtNum / Json (which in
// turn pulls composite_support.h with the SWELL-safe include order). Host-unit-tested (tests/unit/
// test_damf.cpp) with no DAW.
//
// HONESTY CAVEAT (same footing as adm_bwf.h): the output is DAMF-SHAPED and self-validated / round-trips
// this header's own parser; whether a SPECIFIC certified Dolby renderer ingests the exact manifest
// spelling is your ingest check.

#pragma once

#include "adm_bwf.h"  // adm::Model / adm::Object / adm::Block / adm::BedSpeaker / adm::sphToCart / fmtNum / Json

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace damf {

// ============================================================================================
// Coordinate mapping — adm spherical (az +left/CCW deg, el +up deg, dist 0..1) -> DAMF room [x,y,z].
// ============================================================================================
inline void admToRoom(double azDeg, double elDeg, double dist, double& x, double& y, double& z) {
    double X, Y, Z;
    adm::sphToCart(azDeg, elDeg, dist, X, Y, Z);  // X=+right, Y=+front, Z=+up, each ~[-1,1]
    auto cl1 = [](double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); };
    x = cl1(X);
    y = cl1(Y);
    z = Z < 0.0 ? 0.0 : (Z > 1.0 ? 1.0 : Z);  // floor plane (ear level) = 0; ceiling = 1; no below-floor
}

// ============================================================================================
// Element ID assignment — 1-based INPUT channel numbers, bed channels first then objects, matching the
// CAF interleave order. (The Dolby Renderer identifies elements by their 1..128 input channel.)
// ============================================================================================
inline int bedChannelID(const adm::Model& m, size_t bedIdx) { (void)m; return (int)bedIdx + 1; }
inline int objectID(const adm::Model& m, size_t objIdx)    { return (int)m.bed.size() + (int)objIdx + 1; }

// ============================================================================================
// .atmos — the YAML manifest.
// ============================================================================================
inline std::string writeManifest(const adm::Model& m, const std::string& audioName,
                                  const std::string& metadataName) {
    std::string y;
    y.reserve(1024 + m.objects.size() * 48);
    y += "version:\n  major: 1\n  minor: 0\n  revision: 0\n";
    y += "presentations:\n";
    y += "- type: home\n";
    y += "  simplified: false\n";
    y += "metadata: " + metadataName + "\n";
    y += "audio: " + audioName + "\n";

    // Bed roster: one DirectSpeakers instance whose channels carry their speaker label + input ID.
    if (!m.bed.empty()) {
        y += "bedInstances:\n";
        y += "- channels:\n";
        for (size_t i = 0; i < m.bed.size(); ++i)
            y += "  - {channel: " + m.bed[i].label + ", ID: " + std::to_string(bedChannelID(m, i)) + "}\n";
    }

    // Object roster: one entry per object, ID = its input channel, name echoed for readability.
    if (!m.objects.empty()) {
        y += "objects:\n";
        for (size_t j = 0; j < m.objects.size(); ++j)
            y += "- {ID: " + std::to_string(objectID(m, j)) + ", name: \"" +
                 adm::xmlEscape(m.objects[j].name) + "\"}\n";
    }
    return y;
}

// ============================================================================================
// .atmos.metadata — sampleRate + the flat position-event list.
//   Bed channels: one static event each (ID, samplePos 0, active) — no `pos` (fixed speaker feed).
//   Objects: one event per trajectory block (ID, samplePos = round(rtime*sr), active, pos:[x,y,z]).
// ============================================================================================
// Normalize IEEE negative zero (e.g. -sin(0)) to +0 so a front-centre object serializes "0" not "-0".
inline std::string fmtCoord(double v) { return adm::fmtNum(v == 0.0 ? 0.0 : v); }

inline std::string writeMetadata(const adm::Model& m) {
    const int sr = m.sampleRate > 0 ? m.sampleRate : 48000;
    std::string y;
    y.reserve(1024 + m.objects.size() * 96);
    y += "version:\n  major: 1\n  minor: 0\n  revision: 0\n";
    y += "sampleRate: " + std::to_string(sr) + "\n";
    y += "events:\n";

    // Static bed channel events (no position — DirectSpeakers feeds).
    for (size_t i = 0; i < m.bed.size(); ++i)
        y += "- {ID: " + std::to_string(bedChannelID(m, i)) + ", samplePos: 0, active: true}\n";

    // Object trajectory events.
    for (size_t j = 0; j < m.objects.size(); ++j) {
        const adm::Object& o = m.objects[j];
        const int id = objectID(m, j);
        if (o.blocks.empty()) {
            // No trajectory: emit a single front-centre static event so the object still exists.
            y += "- {ID: " + std::to_string(id) + ", samplePos: 0, active: true, pos: [0, 1, 0]}\n";
            continue;
        }
        for (const adm::Block& b : o.blocks) {
            double x, yy, z;
            admToRoom(b.az, b.el, b.dist, x, yy, z);
            const long long samplePos = (long long)std::llround(b.rtime * (double)sr);
            y += "- {ID: " + std::to_string(id) + ", samplePos: " + std::to_string(samplePos) +
                 ", active: true, pos: [" + fmtCoord(x) + ", " + fmtCoord(yy) + ", " + fmtCoord(z) + "]}\n";
        }
    }
    return y;
}

// ============================================================================================
// .atmos.audio — big-endian Core Audio Format (CAF) PCM.
//   'caff' header + 'desc' (CAFAudioFormat) + 'data' (UInt32 mEditCount + interleaved PCM).
//   Integer PCM, big-endian (mFormatFlags = 0), at the requested bit depth. Interleave order = the
//   `channels` vector order (bed channels first, then objects) — identical to writeAdmImage().
// ============================================================================================
inline void putBE16(std::string& o, uint16_t v) {
    o.push_back((char)((v >> 8) & 0xFF)); o.push_back((char)(v & 0xFF));
}
inline void putBE32(std::string& o, uint32_t v) {
    o.push_back((char)((v >> 24) & 0xFF)); o.push_back((char)((v >> 16) & 0xFF));
    o.push_back((char)((v >> 8) & 0xFF));  o.push_back((char)(v & 0xFF));
}
inline void putBE64(std::string& o, uint64_t v) {
    for (int i = 7; i >= 0; --i) o.push_back((char)((v >> (8 * i)) & 0xFF));
}
inline void putF64BE(std::string& o, double d) {
    uint64_t bits; std::memcpy(&bits, &d, sizeof(bits));
    putBE64(o, bits);
}

// Interleave + quantize float channels to big-endian integer PCM at `bitDepth` (16/24/32).
inline std::string quantizePcmBE(const std::vector<std::vector<float>>& channels, size_t frames,
                                 int bitDepth) {
    const int nch = (int)channels.size();
    std::string data;
    data.reserve(frames * (size_t)nch * (size_t)(bitDepth / 8));
    auto clampf = [](float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); };
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < nch; ++c) {
            float v = (f < channels[(size_t)c].size()) ? clampf(channels[(size_t)c][f]) : 0.0f;
            if (bitDepth == 16) {
                int32_t q = (int32_t)std::lround(v * 32767.0f);
                if (q > 32767) q = 32767; if (q < -32768) q = -32768;
                uint16_t u = (uint16_t)(int16_t)q;
                data.push_back((char)((u >> 8) & 0xFF)); data.push_back((char)(u & 0xFF));
            } else if (bitDepth == 24) {
                int32_t q = (int32_t)std::lround(v * 8388607.0);
                if (q > 8388607) q = 8388607; if (q < -8388608) q = -8388608;
                uint32_t u = (uint32_t)q;
                data.push_back((char)((u >> 16) & 0xFF));
                data.push_back((char)((u >> 8) & 0xFF));
                data.push_back((char)(u & 0xFF));
            } else {  // 32-bit int
                int64_t q = (int64_t)std::llround((double)v * 2147483647.0);
                if (q > 2147483647LL) q = 2147483647LL; if (q < -2147483648LL) q = -2147483648LL;
                putBE32(data, (uint32_t)(int32_t)q);
            }
        }
    }
    return data;
}

inline std::string writeCaf(const std::vector<std::vector<float>>& channels, size_t frames,
                            int sampleRate, int bitDepth) {
    if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) bitDepth = 24;
    const int nch = (int)channels.size();
    const int bytesPerSample = bitDepth / 8;

    std::string f;
    // ---- File header: 'caff' + mFileVersion(1) + mFileFlags(0) ----
    f += "caff";
    putBE16(f, 1);
    putBE16(f, 0);

    // ---- 'desc' chunk (CAFAudioFormat, 32 bytes) ----
    f += "desc";
    putBE64(f, 32);
    putF64BE(f, (double)sampleRate);       // mSampleRate
    f += "lpcm";                            // mFormatID
    putBE32(f, 0);                          // mFormatFlags: 0 = integer, big-endian
    putBE32(f, (uint32_t)(nch * bytesPerSample));  // mBytesPerPacket
    putBE32(f, 1);                          // mFramesPerPacket
    putBE32(f, (uint32_t)nch);              // mChannelsPerFrame
    putBE32(f, (uint32_t)bitDepth);         // mBitsPerChannel

    // ---- 'data' chunk: mEditCount(UInt32=0) + interleaved PCM ----
    std::string pcm = quantizePcmBE(channels, frames, bitDepth);
    f += "data";
    putBE64(f, (uint64_t)(4 + pcm.size()));  // chunk size = editCount(4) + audio bytes
    putBE32(f, 0);                            // mEditCount
    f += pcm;
    return f;
}

// ============================================================================================
// Triad assembly — build all three files from the Model + rendered essence.
// ============================================================================================
struct Triad {
    bool ok = false;
    std::string error;
    std::string atmos;          // .atmos manifest bytes
    std::string atmosMetadata;  // .atmos.metadata bytes
    std::string atmosAudio;     // .atmos.audio (CAF) bytes
    int channels = 0;
    size_t frames = 0;
};

// `channels` = per-data-channel float, bed channels first then objects (== m.channelCount()).
inline Triad writeTriad(const adm::Model& m, const std::vector<std::vector<float>>& channels,
                        size_t frames, const std::string& baseName) {
    Triad t;
    const int nch = m.channelCount();
    if (nch <= 0) { t.error = "no_channels"; return t; }
    if ((int)channels.size() != nch) { t.error = "channel_count_mismatch"; return t; }
    if (nch > 128) { t.error = "too_many_channels"; return t; }
    t.channels = nch;
    t.frames = frames;

    const std::string audioName = baseName + ".atmos.audio";
    const std::string metadataName = baseName + ".atmos.metadata";
    t.atmos = writeManifest(m, audioName, metadataName);
    t.atmosMetadata = writeMetadata(m);
    t.atmosAudio = writeCaf(channels, frames, m.sampleRate > 0 ? m.sampleRate : 48000, m.bitDepth);
    t.ok = true;
    return t;
}

// ============================================================================================
// Parser — inspect a CAF essence + best-effort YAML scans. Powers analysis.damf_inspect + the round-trip
// test. Not a full YAML validator: reports structure + flags obvious breakage (mirrors adm_bwf.h's
// parseAdmImage philosophy).
// ============================================================================================
inline uint16_t rdBE16(const std::string& s, size_t off) {
    if (off + 2 > s.size()) return 0;
    return (uint16_t)(((uint8_t)s[off] << 8) | (uint8_t)s[off + 1]);
}
inline uint32_t rdBE32(const std::string& s, size_t off) {
    if (off + 4 > s.size()) return 0;
    return ((uint32_t)(uint8_t)s[off] << 24) | ((uint32_t)(uint8_t)s[off + 1] << 16) |
           ((uint32_t)(uint8_t)s[off + 2] << 8) | (uint32_t)(uint8_t)s[off + 3];
}
inline uint64_t rdBE64(const std::string& s, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | (off + (size_t)i < s.size() ? (uint8_t)s[off + i] : 0);
    return v;
}

struct CafInfo {
    bool ok = false;
    std::string error;
    int channels = 0;
    int bitDepth = 0;
    double sampleRate = 0.0;
    bool isFloat = false;
    bool littleEndian = false;
    std::string formatID;
    uint64_t frames = 0;
};

inline CafInfo parseCaf(const std::string& s) {
    CafInfo r;
    if (s.size() < 8 || s.substr(0, 4) != "caff") { r.error = "not_caf"; return r; }
    size_t off = 8;  // past file header (caff + version + flags)
    uint64_t dataAudioBytes = 0;
    bool haveDesc = false, haveData = false;
    int bytesPerPacket = 0, framesPerPacket = 1;
    while (off + 12 <= s.size()) {
        const std::string id = s.substr(off, 4);
        const uint64_t sz = rdBE64(s, off + 4);
        const size_t body = off + 12;
        if (id == "desc" && body + 32 <= s.size()) {
            haveDesc = true;
            uint64_t srBits = rdBE64(s, body);
            double sr; std::memcpy(&sr, &srBits, sizeof(sr));
            r.sampleRate = sr;
            r.formatID = s.substr(body + 8, 4);
            const uint32_t flags = rdBE32(s, body + 12);
            r.isFloat = (flags & 0x1) != 0;
            r.littleEndian = (flags & 0x2) != 0;
            bytesPerPacket = (int)rdBE32(s, body + 16);
            framesPerPacket = (int)rdBE32(s, body + 20);
            r.channels = (int)rdBE32(s, body + 24);
            r.bitDepth = (int)rdBE32(s, body + 28);
        } else if (id == "data") {
            haveData = true;
            dataAudioBytes = (sz >= 4) ? (sz - 4) : 0;  // minus mEditCount
        }
        // CAF chunk size can be -1 (unknown, to EOF); guard against runaway.
        if ((int64_t)sz < 0) break;
        off = body + (size_t)sz;
    }
    if (!haveDesc) { r.error = "no_desc_chunk"; return r; }
    if (haveData && bytesPerPacket > 0)
        r.frames = (dataAudioBytes / (uint64_t)bytesPerPacket) * (uint64_t)(framesPerPacket > 0 ? framesPerPacket : 1);
    r.ok = true;
    return r;
}

// Best-effort scan helpers over the YAML text.
inline int countLinePrefix(const std::string& hay, const std::string& prefix) {
    int c = 0; size_t p = 0;
    while ((p = hay.find(prefix, p)) != std::string::npos) { ++c; p += prefix.size(); }
    return c;
}
inline std::string scanValue(const std::string& hay, const std::string& key) {
    size_t p = hay.find(key);
    if (p == std::string::npos) return "";
    size_t s = p + key.size();
    size_t e = hay.find('\n', s);
    std::string v = hay.substr(s, (e == std::string::npos ? hay.size() : e) - s);
    size_t a = v.find_first_not_of(" \t");
    size_t b = v.find_last_not_of(" \t\r");
    if (a == std::string::npos) return "";
    return v.substr(a, b - a + 1);
}

// Summarize a triad (manifest + metadata + CAF) into a Json readout for analysis.damf_inspect.
inline Json inspectTriad(const std::string& atmos, const std::string& metadata,
                         const std::string& caf) {
    CafInfo ci = parseCaf(caf);
    const int nObjects = countLinePrefix(atmos, "\n- {ID:");  // object roster entries
    const int nBedInst = countLinePrefix(atmos, "\nbedInstances:");
    const int nBedCh = countLinePrefix(atmos, "\n  - {channel:");
    const int nEvents = countLinePrefix(metadata, "\n- {ID:");
    const std::string srStr = scanValue(metadata, "sampleRate:");

    Json warnings = Json::array();
    if (!ci.ok) warnings.push_back("CAF: " + ci.error);
    if (ci.ok && (int)ci.channels != nBedCh + nObjects && (nBedCh + nObjects) > 0)
        warnings.push_back("CAF channel count (" + std::to_string(ci.channels) +
                           ") != bed+object roster (" + std::to_string(nBedCh + nObjects) + ")");
    if (ci.ok && ci.littleEndian)
        warnings.push_back("CAF essence is little-endian (Dolby masters are big-endian PCM)");

    return Json{
        {"isDamf", ci.ok && (nBedCh + nObjects) > 0},
        {"manifest", Json{{"bedInstances", nBedInst}, {"bedChannels", nBedCh}, {"objects", nObjects}}},
        {"metadata", Json{{"sampleRate", srStr.empty() ? Json(nullptr) : Json(std::atoi(srStr.c_str()))},
                          {"events", nEvents}}},
        {"audio", Json{{"format", ci.formatID}, {"channels", ci.channels}, {"bitDepth", ci.bitDepth},
                       {"sampleRate", ci.sampleRate}, {"isFloat", ci.isFloat},
                       {"littleEndian", ci.littleEndian}, {"frames", (double)ci.frames},
                       {"durationSec", ci.sampleRate > 0 ? (double)ci.frames / ci.sampleRate : 0.0}}},
        {"warnings", warnings}};
}

}  // namespace damf
}  // namespace reaper_mcp
