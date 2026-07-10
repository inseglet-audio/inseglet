// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// adm_bwf.h — native ADM (Audio Definition Model) Broadcast-Wave authoring + inspection.
//
// REAPER cannot author ADM BWF natively (see tools_spatial.cpp buildAtmosLayoutSDK — every Atmos path
// there stops at "wire the topology to the external Dolby Renderer"). This header closes that gap: it
// serializes an object-audio deliverable to an ITU-R BS.2076 ADM file — a Broadcast-Wave (RIFF/WAVE,
// or BW64/RF64 per ITU-R BS.2088 when the payload exceeds 4 GiB) carrying a `chna` (channel allocation)
// chunk and an `axml` (the ADM XML model) chunk alongside the interleaved PCM `data`.
//
// The deliverable model is: a DirectSpeakers BED (a fixed loudspeaker layout, e.g. Atmos 7.1.2) plus N
// mono OBJECTS, each object carrying a position/gain/size TRAJECTORY (a sequence of audioBlockFormat
// time-slices) sampled from its panner automation. This is exactly the essence + metadata split a
// certified renderer ingests.
//
// SDK-free + JSON-only (no REAPER) — like deliverable_specs.h / spatial_verbs.h, the whole authoring +
// parsing judgement is host-unit-tested with no DAW (tests/unit/test_adm.cpp). The tool bodies
// (tools_spatial.cpp / tools_analysis.cpp) only RENDER the bed + object stems, SAMPLE each object's
// panner position over the window, build a Model here, and hand it to writeAdmFile(); adm_inspect hands
// a file's bytes to parseAdmFile().
//
// Standards basis: ITU-R BS.2076-2 (ADM), EBU Tech 3285 Supplement 5 (chna/axml chunks in BWF),
// ITU-R BS.2088 (BW64 64-bit RIFF), ITU-R BS.2051-3 (loudspeaker nominal positions). Coordinate
// convention (BS.2076 spherical): azimuth positive ANTI-CLOCKWISE / to the LEFT, 0° = front; elevation
// positive UP; distance 0..1 (1 = reference room boundary) — this matches the ambisonic-standard frame
// analysis.spatial_field reports in, so an object's read-back position round-trips its panner.
//
// This header is SDK-free: it pulls composite_support.h (Json + makeError; reaper_api.h de-fangs SWELL
// min/max before <json>), so include it the same way. Everything is inline / header-only.

#pragma once

#include "composite_support.h"  // Json + makeError; reaper_api.h before <json> (SWELL min/max)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace adm {

// ============================================================================================
// Model — the pure description the writer serializes. Built by the SDK tool body from the render.
// ============================================================================================

enum class Coord { Spherical, Cartesian };

// One audioBlockFormat: an object's position/gain/size over a [rtime, rtime+duration) slice.
struct Block {
    double rtime = 0.0;     // seconds from programme start
    double duration = 0.0;  // seconds (0 => run to end; the writer fills the tail)
    // Spherical (authoring-native): az +left/CCW deg, el +up deg, distance 0..1+.
    double az = 0.0, el = 0.0, dist = 1.0;
    double gain = 1.0;      // linear
    double width = 0.0, height = 0.0, depth = 0.0;  // object size in degrees / normalized (0 = point)
    // objectDivergence (BS.2076 §5.4.3.9): split the object into two mirror copies diverging from the
    // main position. `divergence` 0..1 (0 = none); `divergenceRange` = azimuthRange (deg) in spherical
    // mode, positionRange (0..1) in cartesian. Emitted ONLY when hasDivergence — an object with no
    // divergence metadata serializes byte-identically to an export without it.
    double divergence = 0.0, divergenceRange = 0.0;
    bool   hasDivergence = false;
    bool   jump = false;    // jumpPosition: discontinuous move at rtime (no interpolation into it)
};

// One DirectSpeakers bed channel.
struct BedSpeaker {
    std::string label;          // "L","R","C","LFE","Lss",...  (from the bed-layout table)
    std::string speakerLabel;   // BS.2051 positional label, e.g. "M+030","M+000","LFE1","U+045"
    double az = 0.0, el = 0.0;  // nominal position (deg)
    bool   lfe = false;
};

// One object: mono essence (one data channel) + a position trajectory.
struct Object {
    std::string name;
    int         objectNumber = 0;   // stable 1-based number (Atmos object index)
    Coord       coord = Coord::Spherical;
    std::vector<Block> blocks;      // >= 1 (a static object = a single block)
    std::string positionSource;     // informational: "envelope" | "static" | "none"
    // audioObject importance (BS.2076): 0..10 renderer-priority hint — a renderer may drop
    // lower-importance objects under resource limits. -1 = unset => the attribute is omitted (so an
    // object with no importance metadata is byte-identical to an export without it).
    int         importance = -1;
};

struct Model {
    std::string programmeName = "REAPER MCP ADM Programme";
    std::string contentName   = "Main";
    int         sampleRate    = 48000;
    int         bitDepth      = 24;      // PCM 16/24/32
    double      durationSec   = 0.0;     // total programme length (fills open-ended block durations)
    std::string bedLayoutName;           // "7.1.2" etc.  ("" => objects only, no bed)
    std::vector<BedSpeaker> bed;         // DirectSpeakers channels, in data-channel order
    std::vector<Object>     objects;     // objects, in data-channel order (after the bed)
    // When set (via adm::profile::normalizeModel), the object serializer emits the Dolby Atmos
    // Master ADM Profile interpolationLength ramp (0 samples on the first block, 250 on every
    // subsequent one) and clamps cartesian X/Y/Z to [-1,1]. Off => byte-identical to an export without it.
    bool        dolbyProfile  = false;

    int channelCount() const { return (int)bed.size() + (int)objects.size(); }
};

// ============================================================================================
// Small formatting helpers (hex IDs, ADM timecode, XML escaping).
// ============================================================================================

inline std::string hex4(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%04x", v & 0xFFFFu);
    return b;
}
inline std::string hex8(unsigned v) {
    char b[12];
    std::snprintf(b, sizeof(b), "%08x", v);
    return b;
}

// ADM time string "hh:mm:ss.fffff" (5-decimal fractional seconds — 10 µs resolution, widely ingested).
inline std::string admTime(double sec) {
    if (sec < 0) sec = 0;
    long long whole = (long long)sec;
    double frac = sec - (double)whole;
    int hh = (int)(whole / 3600);
    int mm = (int)((whole % 3600) / 60);
    int ss = (int)(whole % 60);
    int f5 = (int)std::llround(frac * 100000.0);
    if (f5 >= 100000) { f5 -= 100000; ss += 1; if (ss >= 60) { ss -= 60; mm += 1; if (mm >= 60) { mm -= 60; hh += 1; } } }
    if (f5 < 0) f5 = 0;
    if (f5 > 99999) f5 = 99999;
    char b[64];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%05d", hh, mm, ss, f5);
    return b;
}

inline std::string xmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default:   o.push_back(c);
        }
    }
    return o;
}

inline std::string fmtNum(double v) {
    // Compact fixed formatting; trims trailing zeros. Deterministic across platforms.
    char b[32];
    std::snprintf(b, sizeof(b), "%.6f", v);
    std::string s(b);
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot) last = dot - 1;  // "1." -> "1"
        s.erase(last + 1);
    }
    return s;
}

// az/el/dist (spherical, az +left deg) -> ADM cartesian X(+right)/Y(+front)/Z(+up), each ~[-1,1].
inline void sphToCart(double azDeg, double elDeg, double dist, double& X, double& Y, double& Z) {
    const double a = azDeg * M_PI / 180.0, e = elDeg * M_PI / 180.0;
    X = -dist * std::cos(e) * std::sin(a);  // +right = -left
    Y =  dist * std::cos(e) * std::cos(a);  // +front
    Z =  dist * std::sin(e);                // +up
}

// ============================================================================================
// BS.2051 nominal speaker positions. Keyed by the bed-layout label; Ls/Rs depend on the layout
// (5.1 rear +110 vs 7.1 side +90), so the layout name is passed in. Returns (az,el,speakerLabel).
// ============================================================================================
struct SpeakerPos { double az; double el; const char* speakerLabel; };

inline SpeakerPos speakerPosFor(const std::string& layout, const std::string& lbl) {
    // Middle layer (el 0)
    if (lbl == "L")   return {  30, 0, "M+030" };
    if (lbl == "R")   return { -30, 0, "M-030" };
    if (lbl == "C")   return {   0, 0, "M+000" };
    if (lbl == "LFE" || lbl == "LFE1") return { 0, -30, "LFE1" };
    if (lbl == "LFE2") return { 0, -30, "LFE2" };
    if (lbl == "Ls")  return { (layout == "5.1") ? 110.0 : 90.0, 0,  "M+110" };
    if (lbl == "Rs")  return { (layout == "5.1") ? -110.0 : -90.0, 0, "M-110" };
    if (lbl == "Lss") return {  90, 0, "M+090" };
    if (lbl == "Rss") return { -90, 0, "M-090" };
    if (lbl == "Lrs") return { 135, 0, "M+135" };
    if (lbl == "Rrs") return { -135, 0, "M-135" };
    if (lbl == "Lw")  return {  60, 0, "M+060" };
    if (lbl == "Rw")  return { -60, 0, "M-060" };
    // Upper layer (el +30/+45)
    if (lbl == "Ltf") return {  45, 30, "U+045" };
    if (lbl == "Rtf") return { -45, 30, "U-045" };
    if (lbl == "Ltm") return {  90, 30, "U+090" };
    if (lbl == "Rtm") return { -90, 30, "U-090" };
    if (lbl == "Ltr") return { 135, 30, "U+135" };
    if (lbl == "Rtr") return { -135, 30, "U-135" };
    // 22.2 (BS.2051 System H) — best-effort; interleave is renderer-dependent (flagged upstream).
    if (lbl == "FL")  return {  30, 0, "M+030" };
    if (lbl == "FR")  return { -30, 0, "M-030" };
    if (lbl == "FC")  return {   0, 0, "M+000" };
    if (lbl == "BL")  return { 135, 0, "M+135" };
    if (lbl == "BR")  return { -135, 0, "M-135" };
    if (lbl == "FLc") return {  22, 0, "M+022" };
    if (lbl == "FRc") return { -22, 0, "M-022" };
    if (lbl == "BC")  return { 180, 0, "M+180" };
    if (lbl == "SiL") return {  90, 0, "M+090" };
    if (lbl == "SiR") return { -90, 0, "M-090" };
    if (lbl == "TpFL") return {  45, 30, "U+045" };
    if (lbl == "TpFR") return { -45, 30, "U-045" };
    if (lbl == "TpFC") return {   0, 30, "U+000" };
    if (lbl == "TpC")  return {   0, 90, "T+000" };
    if (lbl == "TpBL") return { 135, 30, "U+135" };
    if (lbl == "TpBR") return { -135, 30, "U-135" };
    if (lbl == "TpSiL") return {  90, 30, "U+090" };
    if (lbl == "TpSiR") return { -90, 30, "U-090" };
    if (lbl == "TpBC") return { 180, 30, "U+180" };
    if (lbl == "BtFC") return {   0, -30, "B+000" };
    if (lbl == "BtFL") return {  45, -30, "B+045" };
    if (lbl == "BtFR") return { -45, -30, "B-045" };
    return { 0, 0, "M+000" };
}

// ============================================================================================
// axml — the ADM XML document (payload of the `axml` chunk).
// ============================================================================================
//
// ID scheme (BS.2076 string forms; the field widths align with chna's fixed columns by design):
//   audioProgrammeID   APR_1001
//   audioContentID     ACO_1001
//   audioObjectID      AO_1001..            (bed = AO_1001, objects AO_1002..)
//   audioPackFormatID  AP_yyyyxxxx          yyyy = typeLabel (0001 DirectSpeakers, 0003 Objects)
//   audioChannelFormatID AC_yyyyxxxx
//   audioBlockFormatID AB_yyyyxxxx_zzzzzzzz
//   audioTrackUID      ATU_xxxxxxxx         (sequential over all data channels, 1-based)
//   audioTrackFormatID AT_yyyyxxxx_zz
//   audioStreamFormatID AS_yyyyxxxx
//
// Custom indices start at 0x1001 so they never collide with the ITU common-definitions (<=0x0FFF).

inline std::string buildAxml(const Model& m) {
    const double dur = (m.durationSec > 0) ? m.durationSec : 0.0;
    std::string x;
    x.reserve(4096 + m.objects.size() * 512);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<ebuCoreMain xmlns=\"urn:ebu:metadata-schema:ebuCore_2016\" "
         "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" schema=\"EBU_CORE_20161222.xsd\">\n";
    x += "  <coreMetadata>\n    <format>\n";
    x += "      <audioFormatExtended version=\"ITU-R_BS.2076-2\">\n";

    // ---- audioProgramme -> audioContent ----
    x += "        <audioProgramme audioProgrammeID=\"APR_1001\" audioProgrammeName=\"" +
         xmlEscape(m.programmeName) + "\" start=\"" + admTime(0) + "\" end=\"" + admTime(dur) + "\">\n";
    x += "          <audioContentIDRef>ACO_1001</audioContentIDRef>\n";
    x += "        </audioProgramme>\n";
    x += "        <audioContent audioContentID=\"ACO_1001\" audioContentName=\"" +
         xmlEscape(m.contentName) + "\">\n";
    if (!m.bed.empty()) x += "          <audioObjectIDRef>AO_1001</audioObjectIDRef>\n";
    for (size_t j = 0; j < m.objects.size(); ++j)
        x += "          <audioObjectIDRef>AO_" + std::to_string(1002 + (int)j) + "</audioObjectIDRef>\n";
    x += "        </audioContent>\n";

    int uid = 1;  // running audioTrackUID index (1-based, over all data channels)

    // ---- BED: one audioObject (DirectSpeakers) ----
    if (!m.bed.empty()) {
        const std::string apId = "AP_0001" + hex4(0x1001);
        x += "        <audioObject audioObjectID=\"AO_1001\" audioObjectName=\"" +
             xmlEscape(m.bedLayoutName.empty() ? std::string("Bed") : (m.bedLayoutName + " Bed")) + "\">\n";
        x += "          <audioPackFormatIDRef>" + apId + "</audioPackFormatIDRef>\n";
        for (size_t i = 0; i < m.bed.size(); ++i)
            x += "          <audioTrackUIDRef>ATU_" + hex8(uid + (int)i) + "</audioTrackUIDRef>\n";
        x += "        </audioObject>\n";
        // pack
        x += "        <audioPackFormat audioPackFormatID=\"" + apId +
             "\" audioPackFormatName=\"" + xmlEscape(m.bedLayoutName + "_bed") +
             "\" typeLabel=\"0001\" typeDefinition=\"DirectSpeakers\">\n";
        for (size_t i = 0; i < m.bed.size(); ++i)
            x += "          <audioChannelFormatIDRef>AC_0001" + hex4(0x1001 + (unsigned)i) +
                 "</audioChannelFormatIDRef>\n";
        x += "        </audioPackFormat>\n";
        // channels + a single static block each
        for (size_t i = 0; i < m.bed.size(); ++i) {
            const BedSpeaker& s = m.bed[i];
            const std::string acId = "AC_0001" + hex4(0x1001 + (unsigned)i);
            x += "        <audioChannelFormat audioChannelFormatID=\"" + acId +
                 "\" audioChannelFormatName=\"" + xmlEscape(s.label) +
                 "\" typeLabel=\"0001\" typeDefinition=\"DirectSpeakers\">\n";
            x += "          <audioBlockFormat audioBlockFormatID=\"AB_0001" + hex4(0x1001 + (unsigned)i) +
                 "_00000001\">\n";
            x += "            <speakerLabel>" + std::string(s.speakerLabel) + "</speakerLabel>\n";
            if (s.lfe) {
                x += "            <frequency typeDefinition=\"lowPass\">120</frequency>\n";
            }
            x += "            <position coordinate=\"azimuth\">" + fmtNum(s.az) + "</position>\n";
            x += "            <position coordinate=\"elevation\">" + fmtNum(s.el) + "</position>\n";
            x += "            <position coordinate=\"distance\">1</position>\n";
            x += "          </audioBlockFormat>\n";
            x += "        </audioChannelFormat>\n";
        }
        uid += (int)m.bed.size();
    }

    // ---- OBJECTS: one audioObject each (Objects), custom pack/channel + block trajectory ----
    for (size_t j = 0; j < m.objects.size(); ++j) {
        const Object& ob = m.objects[j];
        const std::string aoId = "AO_" + std::to_string(1002 + (int)j);
        const std::string apId = "AP_0003" + hex4(0x1001 + (unsigned)j);
        const std::string acId = "AC_0003" + hex4(0x1001 + (unsigned)j);
        x += "        <audioObject audioObjectID=\"" + aoId + "\" audioObjectName=\"" +
             xmlEscape(ob.name) + "\"" +
             (ob.importance >= 0 ? (" importance=\"" + std::to_string(ob.importance) + "\"")
                                 : std::string()) + ">\n";
        x += "          <audioPackFormatIDRef>" + apId + "</audioPackFormatIDRef>\n";
        x += "          <audioTrackUIDRef>ATU_" + hex8(uid) + "</audioTrackUIDRef>\n";
        x += "        </audioObject>\n";
        x += "        <audioPackFormat audioPackFormatID=\"" + apId + "\" audioPackFormatName=\"" +
             xmlEscape(ob.name) + "\" typeLabel=\"0003\" typeDefinition=\"Objects\">\n";
        x += "          <audioChannelFormatIDRef>" + acId + "</audioChannelFormatIDRef>\n";
        x += "        </audioPackFormat>\n";
        x += "        <audioChannelFormat audioChannelFormatID=\"" + acId + "\" audioChannelFormatName=\"" +
             xmlEscape(ob.name) + "\" typeLabel=\"0003\" typeDefinition=\"Objects\">\n";
        for (size_t k = 0; k < ob.blocks.size(); ++k) {
            const Block& b = ob.blocks[k];
            double bdur = b.duration;
            if (bdur <= 0) {  // open-ended: run to the next block's rtime, or to programme end
                double next = (k + 1 < ob.blocks.size()) ? ob.blocks[k + 1].rtime : dur;
                bdur = (next > b.rtime) ? (next - b.rtime) : 0.0;
            }
            char abz[12];
            std::snprintf(abz, sizeof(abz), "%08x", (unsigned)(k + 1));
            x += "          <audioBlockFormat audioBlockFormatID=\"AB_0003" + hex4(0x1001 + (unsigned)j) +
                 "_" + abz + "\" rtime=\"" + admTime(b.rtime) + "\" duration=\"" + admTime(bdur) + "\">\n";
            if (m.dolbyProfile) {
                // Dolby Atmos Master ADM Profile: interpolationLength = 0 samples on the FIRST block,
                // 250 samples on every SUBSEQUENT block, carried on jumpPosition (profile-shaped).
                const int sr = m.sampleRate > 0 ? m.sampleRate : 48000;
                const double interp = (k == 0) ? 0.0 : 250.0 / (double)sr;
                x += "            <jumpPosition interpolationLength=\"" + fmtNum(interp) +
                     "\">1</jumpPosition>\n";
            } else if (b.jump) {
                x += "            <jumpPosition>1</jumpPosition>\n";
            }
            if (ob.coord == Coord::Cartesian) {
                double X, Y, Z; sphToCart(b.az, b.el, b.dist, X, Y, Z);
                if (m.dolbyProfile) {  // profile: X/Y/Z ∈ [-1,1]
                    auto cl = [](double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); };
                    X = cl(X); Y = cl(Y); Z = cl(Z);
                }
                x += "            <cartesian>1</cartesian>\n";
                x += "            <position coordinate=\"X\">" + fmtNum(X) + "</position>\n";
                x += "            <position coordinate=\"Y\">" + fmtNum(Y) + "</position>\n";
                x += "            <position coordinate=\"Z\">" + fmtNum(Z) + "</position>\n";
            } else {
                x += "            <position coordinate=\"azimuth\">" + fmtNum(b.az) + "</position>\n";
                x += "            <position coordinate=\"elevation\">" + fmtNum(b.el) + "</position>\n";
                x += "            <position coordinate=\"distance\">" + fmtNum(b.dist) + "</position>\n";
            }
            if (b.width > 0 || b.height > 0 || b.depth > 0) {
                x += "            <width>" + fmtNum(b.width) + "</width>\n";
                x += "            <height>" + fmtNum(b.height) + "</height>\n";
                x += "            <depth>" + fmtNum(b.depth) + "</depth>\n";
            }
            if (b.hasDivergence) {
                // spherical => azimuthRange (deg); cartesian => positionRange (0..1). Value 0..1.
                const char* rangeAttr = (ob.coord == Coord::Cartesian) ? "positionRange" : "azimuthRange";
                x += "            <objectDivergence " + std::string(rangeAttr) + "=\"" +
                     fmtNum(b.divergenceRange) + "\">" + fmtNum(b.divergence) + "</objectDivergence>\n";
            }
            x += "            <gain>" + fmtNum(b.gain) + "</gain>\n";
            x += "          </audioBlockFormat>\n";
        }
        x += "        </audioChannelFormat>\n";
        ++uid;
    }

    // ---- audioTrackUID + audioTrackFormat + audioStreamFormat for every data channel ----
    uid = 1;
    auto emitTrackFormats = [&](const std::string& typeHex, unsigned idx, const std::string& nm) {
        const std::string atId = "AT_" + typeHex + hex4(idx) + "_01";
        const std::string asId = "AS_" + typeHex + hex4(idx);
        const std::string acId = "AC_" + typeHex + hex4(idx);
        const std::string apId = "AP_" + typeHex + hex4(idx);
        x += "        <audioTrackUID UID=\"ATU_" + hex8(uid) + "\" sampleRate=\"" +
             std::to_string(m.sampleRate) + "\" bitDepth=\"" + std::to_string(m.bitDepth) + "\">\n";
        x += "          <audioTrackFormatIDRef>" + atId + "</audioTrackFormatIDRef>\n";
        x += "          <audioPackFormatIDRef>" + (typeHex == "0001" ? ("AP_0001" + hex4(0x1001))
                                                                     : apId) + "</audioPackFormatIDRef>\n";
        x += "        </audioTrackUID>\n";
        x += "        <audioStreamFormat audioStreamFormatID=\"" + asId + "\" audioStreamFormatName=\"" +
             xmlEscape(nm) + "\" formatLabel=\"0001\" formatDefinition=\"PCM\">\n";
        x += "          <audioChannelFormatIDRef>" + acId + "</audioChannelFormatIDRef>\n";
        x += "          <audioTrackFormatIDRef>" + atId + "</audioTrackFormatIDRef>\n";
        x += "        </audioStreamFormat>\n";
        x += "        <audioTrackFormat audioTrackFormatID=\"" + atId + "\" audioTrackFormatName=\"" +
             xmlEscape(nm) + "\" typeLabel=\"" + typeHex + "\" typeDefinition=\"" +
             (typeHex == "0001" ? "DirectSpeakers" : "Objects") + "\" formatLabel=\"0001\" "
             "formatDefinition=\"PCM\">\n";
        x += "          <audioStreamFormatIDRef>" + asId + "</audioStreamFormatIDRef>\n";
        x += "        </audioTrackFormat>\n";
        ++uid;
    };
    for (size_t i = 0; i < m.bed.size(); ++i) emitTrackFormats("0001", 0x1001 + (unsigned)i, m.bed[i].label);
    for (size_t j = 0; j < m.objects.size(); ++j) emitTrackFormats("0003", 0x1001 + (unsigned)j, m.objects[j].name);

    x += "      </audioFormatExtended>\n    </format>\n  </coreMetadata>\n</ebuCoreMain>\n";
    return x;
}

// ============================================================================================
// chna — binary channel-allocation chunk (EBU Tech 3285 Supp 5): uint16 numTracks, uint16 numUIDs,
// then one 40-byte record per data channel: uint16 trackIndex(1-based) + char UID[12] +
// char trackRef[14] + char packRef[11] + pad[1].
// ============================================================================================
inline void putField(std::string& out, const std::string& s, size_t width) {
    for (size_t i = 0; i < width; ++i) out.push_back(i < s.size() ? s[i] : '\0');
}
inline std::string buildChna(const Model& m) {
    const int n = m.channelCount();
    std::string out;
    out.reserve(4 + (size_t)n * 40);
    auto putU16 = [&](uint16_t v) { out.push_back((char)(v & 0xFF)); out.push_back((char)((v >> 8) & 0xFF)); };
    putU16((uint16_t)n);   // numTracks
    putU16((uint16_t)n);   // numUIDs (one UID per track here)
    int uid = 1;
    for (size_t i = 0; i < m.bed.size(); ++i, ++uid) {
        putU16((uint16_t)uid);
        putField(out, "ATU_" + hex8(uid), 12);
        putField(out, "AT_0001" + hex4(0x1001 + (unsigned)i) + "_01", 14);
        putField(out, "AP_0001" + hex4(0x1001), 11);
        out.push_back('\0');
    }
    for (size_t j = 0; j < m.objects.size(); ++j, ++uid) {
        putU16((uint16_t)uid);
        putField(out, "ATU_" + hex8(uid), 12);
        putField(out, "AT_0003" + hex4(0x1001 + (unsigned)j) + "_01", 14);
        putField(out, "AP_0003" + hex4(0x1001 + (unsigned)j), 11);
        out.push_back('\0');
    }
    return out;
}

// ============================================================================================
// RIFF / BW64 assembly. `channels` holds one float vector per data channel (all the same length);
// it is interleaved + quantized to PCM (16/24/32-bit) and wrapped with fmt + chna + axml + data.
// data is placed LAST so the BW64 (>4 GiB) path can carry its 64-bit size in ds64.
// ============================================================================================
inline void putLE32(std::string& o, uint32_t v) {
    o.push_back((char)(v & 0xFF)); o.push_back((char)((v >> 8) & 0xFF));
    o.push_back((char)((v >> 16) & 0xFF)); o.push_back((char)((v >> 24) & 0xFF));
}
inline void putLE16(std::string& o, uint16_t v) {
    o.push_back((char)(v & 0xFF)); o.push_back((char)((v >> 8) & 0xFF));
}
inline void putLE64(std::string& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back((char)((v >> (8 * i)) & 0xFF));
}
inline void padTo2(std::string& o) { if (o.size() & 1) o.push_back('\0'); }

// Quantize interleaved float -> PCM bytes at the given bit depth (little-endian).
inline std::string quantizePcm(const std::vector<std::vector<float>>& channels, size_t frames,
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
                if (q > 32767) q = 32767;
                if (q < -32768) q = -32768;
                putLE16(data, (uint16_t)(int16_t)q);
            } else if (bitDepth == 24) {
                int32_t q = (int32_t)std::lround(v * 8388607.0);
                if (q > 8388607) q = 8388607;
                if (q < -8388608) q = -8388608;
                uint32_t u = (uint32_t)q;
                data.push_back((char)(u & 0xFF));
                data.push_back((char)((u >> 8) & 0xFF));
                data.push_back((char)((u >> 16) & 0xFF));
            } else {  // 32-bit int
                int64_t q = (int64_t)std::llround((double)v * 2147483647.0);
                if (q > 2147483647LL) q = 2147483647LL;
                if (q < -2147483648LL) q = -2147483648LL;
                putLE32(data, (uint32_t)(int32_t)q);
            }
        }
    }
    return data;
}

struct WriteResult {
    bool ok = false;
    std::string error;
    std::string bytes;     // the complete file image
    bool bw64 = false;     // true if the RF64/BW64 (>4 GiB) header form was used
    int channels = 0;
    size_t frames = 0;
};

// Build the full ADM BWF image. `channels` = per-data-channel float (bed channels first, then objects),
// each length == frames. Returns the file bytes (caller fwrites). Fails closed on shape mismatch.
// `bw64Threshold` is the projected-file-size (bytes) above which the BW64/RF64 64-bit header form is
// used instead of classic RIFF; it defaults to the 4 GiB RIFF limit and is lowered only by the unit
// test to exercise the BW64 path without a multi-gigabyte payload.
inline WriteResult writeAdmImage(const Model& m, const std::vector<std::vector<float>>& channels,
                                 size_t frames, uint64_t bw64Threshold = 0xFFFFFFFFull) {
    WriteResult r;
    const int nch = m.channelCount();
    if (nch <= 0) { r.error = "no_channels"; return r; }
    if ((int)channels.size() != nch) { r.error = "channel_count_mismatch"; return r; }
    if (nch > 0xFFFF) { r.error = "too_many_channels"; return r; }
    r.channels = nch; r.frames = frames;

    const int bd = (m.bitDepth == 16 || m.bitDepth == 24 || m.bitDepth == 32) ? m.bitDepth : 24;
    const int bytesPerSample = bd / 8;
    const uint32_t byteRate = (uint32_t)m.sampleRate * (uint32_t)nch * (uint32_t)bytesPerSample;
    const uint16_t blockAlign = (uint16_t)(nch * bytesPerSample);

    std::string data = quantizePcm(channels, frames, bd);
    std::string chna = buildChna(m);
    std::string axml = buildAxml(m);

    // fmt chunk (PCM). 16-byte PCM fmt is fine for ADM (channel meaning comes from chna, not a mask).
    std::string fmt;
    putLE16(fmt, 1);                       // wFormatTag = PCM
    putLE16(fmt, (uint16_t)nch);
    putLE32(fmt, (uint32_t)m.sampleRate);
    putLE32(fmt, byteRate);
    putLE16(fmt, blockAlign);
    putLE16(fmt, (uint16_t)bd);

    // Decide RIFF vs BW64 by the projected total size.
    const uint64_t dataSize = data.size();
    uint64_t projected = 4 /*WAVE*/ + (8 + fmt.size()) + (8 + chna.size() + (chna.size() & 1)) +
                         (8 + axml.size() + (axml.size() & 1)) + (8 + dataSize + (dataSize & 1));
    const bool bw64 = projected + 8 > bw64Threshold;
    r.bw64 = bw64;

    std::string f;
    if (!bw64) {
        f += "RIFF";
        putLE32(f, 0);  // patched below
        f += "WAVE";
    } else {
        f += "BW64";
        putLE32(f, 0xFFFFFFFFu);  // RF64 sentinel
        f += "WAVE";
        // ds64: riffSize, dataSize, sampleCount (64-bit each) + tableLength(0).
        std::string ds64;
        putLE64(ds64, 0);          // riffSize (patched conceptually; readers use dataSize here)
        putLE64(ds64, dataSize);   // dataSize
        putLE64(ds64, (uint64_t)frames);  // sampleCount
        putLE32(ds64, 0);          // table length
        f += "ds64"; putLE32(f, (uint32_t)ds64.size()); f += ds64; padTo2(f);
    }
    auto addChunk = [&](const char* id, const std::string& payload, bool sentinelSize) {
        f += id;
        putLE32(f, sentinelSize ? 0xFFFFFFFFu : (uint32_t)payload.size());
        f += payload;
        padTo2(f);
    };
    addChunk("fmt ", fmt, false);
    addChunk("chna", chna, false);
    addChunk("axml", axml, false);
    addChunk("data", data, bw64);  // data size sentinel under BW64

    if (!bw64) {
        uint32_t riffSize = (uint32_t)(f.size() - 8);
        f[4] = (char)(riffSize & 0xFF); f[5] = (char)((riffSize >> 8) & 0xFF);
        f[6] = (char)((riffSize >> 16) & 0xFF); f[7] = (char)((riffSize >> 24) & 0xFF);
    }
    r.bytes.swap(f);
    r.ok = true;
    return r;
}

// ============================================================================================
// Parser — walk a RIFF/BW64 image, decode chna, and lightly summarize axml. Powers analysis.adm_inspect
// and the host round-trip test. Not an XSD validator: it reports structure + flags obvious breakage.
// ============================================================================================
inline uint32_t rdLE32(const std::string& s, size_t off) {
    if (off + 4 > s.size()) return 0;
    return (uint32_t)(uint8_t)s[off] | ((uint32_t)(uint8_t)s[off + 1] << 8) |
           ((uint32_t)(uint8_t)s[off + 2] << 16) | ((uint32_t)(uint8_t)s[off + 3] << 24);
}
inline uint16_t rdLE16(const std::string& s, size_t off) {
    if (off + 2 > s.size()) return 0;
    return (uint16_t)((uint8_t)s[off] | ((uint16_t)(uint8_t)s[off + 1] << 8));
}
inline uint64_t rdLE64(const std::string& s, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8 && off + (size_t)i < s.size(); ++i) v |= (uint64_t)(uint8_t)s[off + i] << (8 * i);
    return v;
}

// Count non-overlapping occurrences of `needle` in `hay`.
inline int countOccur(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int c = 0; size_t p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) { ++c; p += needle.size(); }
    return c;
}
// Pull every value of a simple <tag ...>value</tag> or attribute="value" occurrence (best-effort).
inline std::vector<std::string> extractAttr(const std::string& hay, const std::string& attr) {
    std::vector<std::string> out;
    const std::string key = attr + "=\"";
    size_t p = 0;
    while ((p = hay.find(key, p)) != std::string::npos) {
        size_t s = p + key.size();
        size_t e = hay.find('"', s);
        if (e == std::string::npos) break;
        out.push_back(hay.substr(s, e - s));
        p = e + 1;
    }
    return out;
}

struct ParseResult {
    bool ok = false;
    std::string error;
    Json summary;
    std::string axml;  // the raw ADM XML (axml chunk) — profile validation scans it directly.
};

inline ParseResult parseAdmImage(const std::string& s) {
    ParseResult r;
    if (s.size() < 12) { r.error = "too_small"; return r; }
    const std::string magic = s.substr(0, 4);
    const bool isRf64 = (magic == "RF64" || magic == "BW64");
    if (magic != "RIFF" && !isRf64) { r.error = "not_riff"; return r; }
    if (s.substr(8, 4) != "WAVE") { r.error = "not_wave"; return r; }

    Json chunks = Json::array();
    std::string chnaBytes, axml;
    int fmtChannels = 0, fmtBits = 0; uint32_t fmtRate = 0;
    uint64_t dataBytes = 0, ds64DataSize = 0;
    bool haveFmt = false, haveData = false, haveChna = false, haveAxml = false;

    size_t off = 12;
    while (off + 8 <= s.size()) {
        std::string id = s.substr(off, 4);
        uint32_t sz = rdLE32(s, off + 4);
        uint64_t realSz = sz;
        if (isRf64 && sz == 0xFFFFFFFFu) {
            if (id == "data") realSz = ds64DataSize;  // resolved from ds64
        }
        size_t body = off + 8;
        chunks.push_back(Json{{"id", id}, {"size", (double)realSz}});
        if (id == "ds64") {
            ds64DataSize = rdLE64(s, body + 8);  // second 64-bit field = dataSize
        } else if (id == "fmt ") {
            haveFmt = true;
            fmtChannels = rdLE16(s, body + 2);
            fmtRate = rdLE32(s, body + 4);
            fmtBits = rdLE16(s, body + 14);
        } else if (id == "chna") {
            haveChna = true;
            chnaBytes = s.substr(body, (size_t)realSz <= s.size() - body ? (size_t)realSz : s.size() - body);
        } else if (id == "axml") {
            haveAxml = true;
            axml = s.substr(body, (size_t)realSz <= s.size() - body ? (size_t)realSz : s.size() - body);
        } else if (id == "data") {
            haveData = true;
            dataBytes = realSz;
        }
        size_t adv = (size_t)realSz + ((realSz & 1) ? 1 : 0);
        off = body + adv;
    }

    // chna decode
    Json chnaJson = Json(nullptr);
    if (haveChna && chnaBytes.size() >= 4) {
        int numTracks = rdLE16(chnaBytes, 0);
        int numUIDs = rdLE16(chnaBytes, 2);
        Json uids = Json::array();
        size_t p = 4;
        for (int i = 0; i < numUIDs && p + 40 <= chnaBytes.size(); ++i, p += 40) {
            int ti = rdLE16(chnaBytes, p);
            auto field = [&](size_t o, size_t w) {
                std::string f = chnaBytes.substr(p + o, w);
                size_t z = f.find('\0'); if (z != std::string::npos) f.erase(z);
                return f;
            };
            uids.push_back(Json{{"trackIndex", ti}, {"uid", field(2, 12)},
                                {"trackRef", field(14, 14)}, {"packRef", field(28, 11)}});
        }
        chnaJson = Json{{"numTracks", numTracks}, {"numUIDs", numUIDs}, {"tracks", uids}};
    }

    // axml summary (best-effort structural scan)
    Json admJson = Json(nullptr);
    if (haveAxml && !axml.empty()) {
        const int nObj = countOccur(axml, "<audioObject ");
        const int nPack = countOccur(axml, "<audioPackFormat ");
        const int nChan = countOccur(axml, "<audioChannelFormat ");
        const int nBlock = countOccur(axml, "<audioBlockFormat ");
        const int nTrackUID = countOccur(axml, "<audioTrackUID ");
        const bool cartesian = axml.find("<cartesian>1</cartesian>") != std::string::npos;
        const bool directSpeakers = axml.find("DirectSpeakers") != std::string::npos;
        const bool objects = axml.find("typeDefinition=\"Objects\"") != std::string::npos;
        std::vector<std::string> objNames = extractAttr(axml, "audioObjectName");
        std::vector<std::string> prog = extractAttr(axml, "audioProgrammeName");
        Json names = Json::array();
        for (const auto& n : objNames) names.push_back(n);
        // Per-object metadata readout (extent / objectDivergence / importance).
        const int nDivergence = countOccur(axml, "<objectDivergence");
        const int nExtent = countOccur(axml, "<width>");  // only objects with extent emit <width>
        const bool hasImportance = axml.find(" importance=\"") != std::string::npos;
        Json importanceVals = Json::array();
        for (const auto& v : extractAttr(axml, "importance")) {
            try { importanceVals.push_back(std::stoi(v)); } catch (...) { /* skip non-numeric */ }
        }
        admJson = Json{{"audioObjects", nObj}, {"audioPackFormats", nPack},
                       {"audioChannelFormats", nChan}, {"audioBlockFormats", nBlock},
                       {"audioTrackUIDs", nTrackUID},
                       {"coordinateMode", cartesian ? "cartesian" : "spherical"},
                       {"hasDirectSpeakers", directSpeakers}, {"hasObjects", objects},
                       {"hasExtent", nExtent > 0}, {"extentBlocks", nExtent},
                       {"hasObjectDivergence", nDivergence > 0}, {"divergenceBlocks", nDivergence},
                       {"hasImportance", hasImportance}, {"objectImportance", importanceVals},
                       {"objectNames", names},
                       {"programme", prog.empty() ? std::string() : prog[0]},
                       {"version", extractAttr(axml, "version").empty()
                                       ? std::string() : extractAttr(axml, "version")[0]}};
    }

    Json warnings = Json::array();
    if (!haveFmt) warnings.push_back("missing fmt chunk");
    if (!haveData) warnings.push_back("missing data chunk");
    if (!haveChna) warnings.push_back("missing chna chunk — not an ADM BWF");
    if (!haveAxml) warnings.push_back("missing axml chunk — not an ADM BWF");
    if (haveChna && haveFmt && chnaJson.is_object() &&
        chnaJson["numTracks"].get<int>() != fmtChannels)
        warnings.push_back("chna numTracks != fmt channel count");

    uint64_t frames = (haveFmt && fmtChannels > 0 && fmtBits > 0)
                          ? dataBytes / ((uint64_t)fmtChannels * (uint64_t)(fmtBits / 8)) : 0;
    r.summary = Json{
        {"container", isRf64 ? std::string("BW64") : std::string("RIFF/WAVE")},
        {"isAdm", haveChna && haveAxml},
        {"format", Json{{"channels", fmtChannels}, {"sampleRate", (double)fmtRate},
                        {"bitDepth", fmtBits}, {"frames", (double)frames},
                        {"durationSec", fmtRate ? (double)frames / (double)fmtRate : 0.0}}},
        {"chunks", chunks}, {"chna", chnaJson}, {"adm", admJson}, {"warnings", warnings}};
    r.axml = axml;  // expose the raw ADM XML for profile conformance scanning.
    r.ok = true;
    return r;
}

}  // namespace adm
}  // namespace reaper_mcp
