// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_spatial.cpp — the immersive / spatial audio tool surface.
//
// REAPER carries the multichannel busses; these tools encode the immersive *conventions* (channel
// counts, pin/connector routing, ambisonic order & normalization, bed layouts, external-renderer
// send topology) as high-level, intent-level tools. Registered here:
//
//   Multichannel foundation + standard bed builders + per-channel bed routing (in-box):
//         spatial.set_track_channels, spatial.build_bed, spatial.assign_to_bed
//   REAPER-native surround panner (ReaSurroundPan) control:
//         spatial.add_surround_panner, spatial.get_surround_state, spatial.set_source_position
//   Scene-based ambisonics (no native REAPER support — we orchestrate the installed third-party
//   suites, IEM > SPARTA > ATK per priority; NOT ambiX):
//         spatial.detect_spatial_suites  (read-only: probe installed encoders/decoders/rotators)
//         spatial.ambisonic_encode       (order->channel count, insert encoder, az/el, pin-wire)
//         spatial.rotate_scene           (SceneRotator yaw/pitch/roll; identity HOA pin-wire)
//         spatial.ambisonic_decode       (HOA->loudspeaker decoder; input HOA pin-wire)
//   Render deliverables (in-box masters + Atmos layout):
//         spatial.render_deliverables    (RENDER_* config + no-dialog render; multichannel bed /
//                                         ambiX B-format / binaural masters; native RENDER_STATS
//                                         LUFS+true-peak; Dolby Atmos Renderer / DAPS send matrix)
//   Binaural monitor + LIVE head-tracking:
//         spatial.add_binaural_monitor   (non-destructive 2ch monitor bus fed by a send off the HOA
//                                         scene / object tracks; IEM BinauralDecoder or SPARTA
//                                         Binauraliser; optional SceneRotator; extension-hosted OSC/UDP
//                                         head-track listener drives yaw/pitch/roll on the main thread)
//         spatial.stop_head_tracking     (release the OSC listener)
//   Outcome-level composite verbs (thin, single-undo compositions over the primitives above):
//         spatial.spatialize_stems, spatial.stereo_to_ambisonic, spatial.setup_immersive_session
//   Spatial / ambisonic depth tools:
//         spatial.convert_order, spatial.convert_format, spatial.get_scene_info,
//         spatial.mirror_scene, spatial.beamform, spatial.set_distance
//   Native object-audio authoring (ITU-R BS.2076 ADM BWF):
//         spatial.author_object_bed, spatial.export_adm
//   (head_track_bridge.h is the SDK-free OSC parser + apply.)
//
// Channel-encoding facts are taken verbatim from the pinned reaper_plugin_functions.h:
//   I_NCHAN      : number of track channels, 2..128, even only.
//   I_SRCCHAN    : -1 = no audio. low 10 bits = channel offset; (v>>10) = count code
//                  (0=stereo, 1=mono, 2=4ch, 3=6ch, ...).
//   I_DSTCHAN    : low 10 bits = destination index; &1024 = mix to mono.
//   I_FOLDERDEPTH: 1 = track is a folder parent.
//   ReaSurroundPan exposes NUMCHANNELS / NUMSPEAKERS / RESETCHANNELS via
//     TrackFX_Get/SetNamedConfigParm (per-source X/Y positions are regular FX params, so we
//     discover them by name at runtime and also accept explicit param indices — robust to drift).
//
// Ambisonics conventions:
//   * Full 3D HOA of order N has (N+1)^2 channels. REAPER track channel counts must be EVEN, so the
//     odd-count orders (2nd=9, 4th=25, 6th=49) ride on an even-padded bus (10, 26, 50) with the
//     surplus channel left silent. We report hoaChannels (the real ACN count) AND trackChannels.
//   * ACN channel ordering + SN3D normalization is the modern default (IEM, SPARTA, ambiX). FuMa is
//     legacy and only defined up to 3rd order (we warn above that). ATK Reaper is FOA (1st order).
//   * Order Setting / Normalization are CHOICE params whose enum index differs per plug-in build, so
//     we discover + REPORT them rather than blind-writing a wrong enum; azimuth/elevation/yaw/pitch/
//     roll are continuous (degrees) and ARE written, clamped to each param's own [min,max].
//   * Pin/connector wiring — the fiddliest manual step in immersive REAPER, and API-only — is done
//     explicitly: encoder ACN output pin k -> track channel k (identity), via TrackFX_SetPinMappings.
//
// Dual-path: real API bodies under REAPER_MCP_HAVE_SDK; SDK-free host build returns representative,
// schema-valid structured output (the channel math is SDK-independent, so it is identical on both).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "tool_helpers.h"      // pulls reaper_api.h before any STL/JSON header; arg + track helpers
#include "../head_track_bridge.h"  // live OSC head-tracking bridge (SDK-free parser + apply)
#include "../render_stats.h"   // render-config + RENDER_STATS parsing (shared with analysis)
#include "../render_stems.h"   // shared bounded stem-render helpers (bed + object stems)
#include "../adm_bwf.h"        // native ADM (BS.2076) BWF authoring + parsing
#include "../adm_profile.h"    // Dolby Atmos Master ADM Profile normalize + validate
#include "../damf.h"           // native Dolby Atmos Master File (DAMF) triad serializer (spatial.export_damf)
#include "../send_layout.h"    // SDK-free object send-layout roster/encode/reconcile/inspect (Batch L1)
#include "../tool_registry.h"
#include "../spatial_verbs.h"  // SDK-free planning core (target/placement/plan) + composite_support

namespace reaper_mcp {

namespace {

// ---------------------------------------------------------------------------------------------
// Standard immersive bed layouts. Channel labels are 1-based-position -> speaker; `lfe` lists the
// 0-based LFE channel indices. Orders for 5.1..9.1.6 follow the SMPTE / Dolby Atmos bed convention
// (film order: L R C LFE then surrounds then heights). 22.2 uses the SMPTE ST 2036-2 grouping;
// its exact interleave is renderer-dependent, so we flag that in `note`.
// ---------------------------------------------------------------------------------------------
struct BedLayout {
    const char* name;
    int channels;
    std::vector<const char*> labels;  // size == channels
    std::vector<int> lfe;             // 0-based indices that are LFE
    const char* note;
};

const std::vector<BedLayout>& bedLayouts() {
    static const std::vector<BedLayout> kLayouts = {
        {"5.1", 6,
         {"L", "R", "C", "LFE", "Ls", "Rs"},
         {3},
         "SMPTE/ITU 5.1 film order (L R C LFE Ls Rs)."},
        {"7.1", 8,
         {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs"},
         {3},
         "7.1 with side (Lss/Rss) then rear (Lrs/Rrs) surrounds."},
        {"7.1.4", 12,
         {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf", "Ltr", "Rtr"},
         {3},
         "Dolby Atmos 7.1.4 bed: 7.1 + 4 tops (front Ltf/Rtf, rear Ltr/Rtr)."},
        {"9.1.6", 16,
         {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Lw", "Rw",
          "Ltf", "Rtf", "Ltm", "Rtm", "Ltr", "Rtr"},
         {3},
         "Atmos 9.1.6 bed: 7.1 + wides (Lw/Rw) + 6 tops (front/middle/rear)."},
        {"22.2", 24,
         {"FL", "FR", "FC", "LFE1", "BL", "BR", "FLc", "FRc", "BC", "LFE2", "SiL", "SiR",
          "TpFL", "TpFR", "TpFC", "TpC", "TpBL", "TpBR", "TpSiL", "TpSiR", "TpBC",
          "BtFC", "BtFL", "BtFR"},
         {3, 9},
         "NHK 22.2 (SMPTE ST 2036-2 grouping: 10-ch middle w/ 2 LFE, 9-ch upper, 3-ch lower). "
         "Interleave order is renderer-dependent — confirm against your target."},
    };
    return kLayouts;
}

const BedLayout* findBedLayout(const std::string& name) {
    for (const auto& b : bedLayouts())
        if (name == b.name) return &b;
    return nullptr;
}

Json bedChannelMap(const BedLayout& b) {
    Json arr = Json::array();
    for (int i = 0; i < b.channels; ++i) {
        bool isLfe = false;
        for (int l : b.lfe)
            if (l == i) { isLfe = true; break; }
        arr.push_back(Json{{"channel", i + 1}, {"label", b.labels[(size_t)i]}, {"lfe", isLfe}});
    }
    return arr;
}

// Even-clamp a requested channel count into REAPER's legal I_NCHAN range (2..128, even only).
int clampChannels(int ch) {
    if (ch < 2) ch = 2;
    if (ch > 128) ch = 128;
    if (ch & 1) ++ch;
    return ch;
}

// Describe what a channel count means in immersive terms: matching bed layout and/or ambisonic
// order (a perfect square (order+1)^2). Sets *ambiOrder to the HOA order or -1 if not square.
std::string interpretChannels(int ch, int* ambiOrder) {
    int order = -1;
    for (int o = 1; o <= 7; ++o)
        if ((o + 1) * (o + 1) == ch) { order = o; break; }
    if (ambiOrder) *ambiOrder = order;

    std::vector<std::string> parts;
    if (ch == 4) parts.push_back("quad / 4.0");
    for (const auto& b : bedLayouts())
        if (b.channels == ch) parts.push_back(std::string(b.name) + " bed");
    if (order >= 1) {
        static const char* kOrd[] = {"", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th"};
        parts.push_back(std::string(kOrd[order]) + "-order ambisonics (ACN, " +
                        std::to_string(ch) + "ch)");
    }
    if (parts.empty()) return std::to_string(ch) + "-channel (no standard immersive layout)";
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) s += " or "; s += parts[i]; }
    return s;
}

// Encode a REAPER multichannel send's source/dest channel fields (see header facts at top).
struct SendChanEnc { int src; int dst; int countCode; };
SendChanEnc encodeSend(int srcChannel, int channels, int destChannel, bool monoMix) {
    int code;
    if (channels == 1)      code = 1;                 // mono
    else if (channels == 2) code = 0;                 // stereo
    else if (channels >= 4 && (channels % 2) == 0)
        code = channels / 2;                          // 4ch->2, 6ch->3, 8ch->4, ...
    else
        throw std::runtime_error("channels must be 1, 2, or an even number >= 4");
    if (srcChannel < 0 || destChannel < 0)
        throw std::runtime_error("channel offsets must be >= 0");
    SendChanEnc e;
    e.countCode = code;
    e.src = (srcChannel & 0x3FF) | (code << 10);
    e.dst = (destChannel & 0x3FF) | (monoMix ? 1024 : 0);
    return e;
}

// ---------------------------------------------------------------------------------------------
// Ambisonics — SDK-independent helpers: channel math + installed-suite plug-in tables.
// ---------------------------------------------------------------------------------------------

// Full 3D HOA channel count for order N: (N+1)^2. (1st=4, 2nd=9, 3rd=16, 4th=25, 5th=36, 6th=49,
// 7th=64.) REAPER track channel counts are even, so clampChannels() rounds the odd ones up.
int hoaChannels(int order) { return (order + 1) * (order + 1); }

const char* ordinal(int order) {
    static const char* kOrd[] = {"0th", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th"};
    return (order >= 0 && order <= 7) ? kOrd[order] : "?";
}

// Candidate FX names for a role, in the suite priority order (IEM > SPARTA > ATK; NOT ambiX by
// default — ambiX only when explicitly asked). TrackFX_AddByName does substring matching, so these
// are match seeds; the first that resolves on the user's machine wins. An explicit `plugin`/`fx`
// override is layered on by the caller. suite == "auto" means "try all, in priority order".
std::vector<std::string> ambiCandidates(const std::string& role, const std::string& suite) {
    auto want = [&](const char* s) { return suite == "auto" || suite == s; };
    std::vector<std::string> v;
    if (role == "encoder") {
        if (want("IEM"))    { v.push_back("StereoEncoder"); v.push_back("MultiEncoder"); }
        if (want("SPARTA")) { v.push_back("SPARTA AmbiENC"); v.push_back("AmbiENC"); }
        if (want("ATK"))    { v.push_back("ATK FOA Encode Mono"); v.push_back("FOA Encode"); }
        if (want("ambiX"))  { v.push_back("ambix_encoder_o1"); v.push_back("ambix_encoder"); }
    } else if (role == "rotator") {
        if (want("IEM"))    { v.push_back("SceneRotator"); }
        if (want("SPARTA")) { v.push_back("SPARTA Rotator"); v.push_back("Rotator"); }
        if (want("ambiX"))  { v.push_back("ambix_rotator"); }
        if (want("ATK"))    { v.push_back("FOA Transform Rotate"); }
    } else if (role == "decoder") {
        if (want("IEM"))    { v.push_back("SimpleDecoder"); v.push_back("AllRADecoder"); }
        if (want("SPARTA")) { v.push_back("SPARTA AmbiDEC"); v.push_back("AmbiDEC"); }
        if (want("ambiX"))  { v.push_back("ambix_decoder"); }
        if (want("ATK"))    { v.push_back("ATK FOA Decode"); v.push_back("FOA Decode"); }
    } else if (role == "binaural") {
        if (want("IEM"))    { v.push_back("BinauralDecoder"); }
        if (want("SPARTA")) { v.push_back("SPARTA Binauraliser"); v.push_back("Binauraliser"); }
        if (want("ambiX"))  { v.push_back("ambix_binaural"); }
        if (want("ATK"))    { v.push_back("FOA Decode Binaural"); }
    } else if (role == "beamformer") {
        // Steerable directional pickup / virtual-microphone from a HOA scene. SPARTA Beamformer is
        // the practical HOA choice; IEM has no beamformer plug-in.
        if (want("SPARTA")) { v.push_back("SPARTA Beamformer"); v.push_back("Beamformer"); }
    } else if (role == "room") {
        // Shoebox room / distance / early-reflection encoder. IEM RoomEncoder is the reference HOA one.
        if (want("IEM"))    { v.push_back("RoomEncoder"); }
    }
    return v;
}

// Classify an installed-FX name into a suite ("IEM"/"SPARTA"/"ATK"/"ambiX"/"BlueRipple"/"") and a
// role ("encoder"/"decoder"/"rotator"/"binaural"/"other"/""). Used by detect_spatial_suites.
std::string classifySuite(const std::string& lname) {
    if (lname.find("(iem)") != std::string::npos) return "IEM";
    if (lname.find("sparta") != std::string::npos) return "SPARTA";
    if (lname.find("ambix") != std::string::npos) return "ambiX";
    if (lname.find("o3a") != std::string::npos || lname.find("blue ripple") != std::string::npos ||
        lname.find("toa ") != std::string::npos) return "BlueRipple";
    // ATK Reaper JSFX are named "ATK FOA ...", "ATK HOA ...", "ATK Matrix ...".
    if (lname.find("atk foa") != std::string::npos || lname.find("atk hoa") != std::string::npos ||
        lname.find("atk matrix") != std::string::npos || lname.find("atk ") != std::string::npos)
        return "ATK";
    return "";
}
std::string classifyRole(const std::string& lname) {
    if (lname.find("binaural") != std::string::npos) return "binaural";       // check before decoder
    if (lname.find("rotat") != std::string::npos) return "rotator";
    if (lname.find("decod") != std::string::npos || lname.find("ambidec") != std::string::npos ||
        lname.find("allrad") != std::string::npos) return "decoder";
    if (lname.find("encod") != std::string::npos || lname.find("ambienc") != std::string::npos)
        return "encoder";
    return "other";
}

// ---------------------------------------------------------------------------------------------
// Render deliverables — format ids, RENDER_STATS parsing, and (below, under the SDK) the RENDER_*
// accessors + snapshot/restore now live in ../render_stats.h so analysis.check_deliverable drives
// the exact same loudness engine. buildAtmosLayoutHost/SDK stay here (spatial).
// ---------------------------------------------------------------------------------------------

// Host-build representative Atmos bed+object send plan (no REAPER, so no sends are created). Mirrors
// the SDK version's channel-plan shape so structured output is schema-valid in both builds.
Json buildAtmosLayoutHost(const Json& a) {
    std::string bedLayout = optStr(a, "bedLayout", "7.1.4");
    const BedLayout* bl = findBedLayout(bedLayout);
    int bedW = bl ? bl->channels : 10;
    std::vector<int> objs;
    if (a.contains("objectTracks") && a["objectTracks"].is_array())
        for (const auto& o : a["objectTracks"]) if (o.is_number()) objs.push_back(o.get<int>());
    Json chanPlan = Json::array();
    chanPlan.push_back(Json{{"group", "bed"}, {"layout", bedLayout}, {"channels", bedW},
                            {"rendererChannels", "1.." + std::to_string(bedW)}});
    for (size_t i = 0; i < objs.size(); ++i)
        chanPlan.push_back(Json{{"group", "object"}, {"sourceTrack", objs[i]},
                                {"rendererChannel", bedW + (int)i + 1}});
    return Json{{"bedLayout", bedLayout}, {"bedChannels", bedW}, {"objectCount", (int)objs.size()},
                {"totalChannels", bedW + (int)objs.size()}, {"channelPlan", chanPlan},
                {"sends", Json::array()},
                {"note", "(host stub) external Dolby Atmos Renderer / DAPS send layout."}};
}

#ifdef REAPER_MCP_HAVE_SDK
// Read a plug-in named-config parm (e.g. ReaSurroundPan NUMCHANNELS) as a string ("" if absent).
std::string fxNamedCfg(MediaTrack* t, int fx, const char* name) {
    char buf[256] = {0};
    if (TrackFX_GetNamedConfigParm(t, fx, name, buf, sizeof(buf))) return buf;
    return "";
}
std::string fxNameStr(MediaTrack* t, int fx) {
    char buf[256] = {0};
    TrackFX_GetFXName(t, fx, buf, sizeof(buf));
    return buf;
}
std::string fxParamNameStr(MediaTrack* t, int fx, int p) {
    char buf[256] = {0};
    TrackFX_GetParamName(t, fx, p, buf, sizeof(buf));
    return buf;
}
// Find the first FX on a track whose name contains "surround" (case-insensitive); -1 if none.
int findSurroundFx(MediaTrack* t) {
    const int n = TrackFX_GetCount(t);
    for (int i = 0; i < n; ++i)
        if (toLower(fxNameStr(t, i)).find("surround") != std::string::npos) return i;
    return -1;
}
// Resolve the panner FX index: explicit `fx` arg if given, else auto-detect. Throws if none.
int resolveSurroundFx(const Json& a, MediaTrack* t) {
    if (a.contains("fx") && a["fx"].is_number()) return a["fx"].get<int>();
    int fx = findSurroundFx(t);
    if (fx < 0) throw std::runtime_error("no ReaSurroundPan-style FX on track; pass 'fx' explicitly");
    return fx;
}

// ---- ambisonics SDK helpers ----

// Find the first FX param whose (lowercased) name contains `key`. When source1 > 0 (a 1-based
// source index, e.g. MultiEncoder "Azimuth 3"), prefer the match that also contains that number.
// Returns the param index or -1.
int discoverParam(MediaTrack* t, int fx, int np, const std::string& key, int source1 = 0) {
    const std::string k = toLower(key);
    const std::string tok = source1 > 0 ? std::to_string(source1) : std::string();
    int fallback = -1;
    for (int p = 0; p < np; ++p) {
        std::string nm = toLower(fxParamNameStr(t, fx, p));
        if (nm.find(k) == std::string::npos) continue;
        if (fallback < 0) fallback = p;
        if (!tok.empty() && nm.find(tok) != std::string::npos) return p;
    }
    return fallback;
}

// Parse a leading signed decimal number out of a plug-in's formatted param string ("-180.0°",
// "90.0 deg", "+45"). Returns false if no number is found.
bool parseLeadingNumber(const char* s, double& out) {
    if (!s) return false;
    const char* p = s;
    while (*p && !((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.')) ++p;
    if (!*p) return false;
    char* end = nullptr;
    double v = std::strtod(p, &end);
    if (end == p) return false;
    out = v;
    return true;
}

// The full degree span an angle param covers, by axis name — used only as a FALLBACK when the plug-in's
// formatted range can't be read. IEM/SPARTA convention: elevation is ±90 (span 180); azimuth / yaw /
// pitch / roll / width are ±180 (span 360).
double angleSpanForKey(const std::string& key) {
    return toLower(key).find("elev") != std::string::npos ? 180.0 : 360.0;
}

// Discover the real degree range [loDeg,hiDeg] a NORMALIZED angle param spans, by reading the plug-in's
// own formatted display at the [0,1] endpoints (e.g. "-180.0°" .. "180.0°"). False if either endpoint
// can't be parsed or the span is implausible for a degree angle (guards radians / odd formats).
bool paramDegRange(MediaTrack* t, int fx, int p, double& loDeg, double& hiDeg) {
    char b0[128] = {0}, b1[128] = {0};
    if (!TrackFX_FormatParamValueNormalized(t, fx, p, 0.0, b0, sizeof(b0))) return false;
    if (!TrackFX_FormatParamValueNormalized(t, fx, p, 1.0, b1, sizeof(b1))) return false;
    if (!parseLeadingNumber(b0, loDeg) || !parseLeadingNumber(b1, hiDeg)) return false;
    double span = hiDeg - loDeg;
    return std::fabs(span) >= 30.0 && std::fabs(span) <= 1000.0;  // plausible degree range
}

// Resolve an angle param's degree range: the plug-in's own formatted range if readable, else the
// per-axis convention centered on 0.
void angleDegRange(MediaTrack* t, int fx, int p, const std::string& key, double& loDeg, double& hiDeg) {
    if (!paramDegRange(t, fx, p, loDeg, hiDeg)) {
        const double s = angleSpanForKey(key);
        loDeg = -s / 2.0;
        hiDeg = s / 2.0;
    }
}

// Set a CONTINUOUS angle param (found by name) to `valueDeg` DEGREES. IEM/JUCE plug-ins expose angle
// params as a NORMALIZED [0,1] value (0.5 = 0°), so we map the requested degrees onto the param's real
// degree range (read from the plug-in via angleDegRange, per-axis fallback) instead of writing raw
// degrees that would peg to an extreme. Real-degree params (JSFX) are written through, clamped. Returns
// a report (incl. requestedDeg + the degRange used), or JSON null if the param wasn't found. Reuses
// head_track_bridge.h::angleToParamValue so the head-track and encode/rotate paths map angles identically.
Json setNamedValueParam(MediaTrack* t, int fx, int np, const std::string& key, double valueDeg,
                        int source1 = 0) {
    int p = discoverParam(t, fx, np, key, source1);
    if (p < 0) return Json(nullptr);
    double mn = 0.0, mx = 0.0;
    TrackFX_GetParam(t, fx, p, &mn, &mx);
    Json out = Json{{"param", p}, {"name", fxParamNameStr(t, fx, p)}, {"requestedDeg", valueDeg},
                    {"min", mn}, {"max", mx}};
    double writeVal;
    if (mx - mn <= 2.0 + 1e-9) {                         // normalized angle param
        double loDeg = 0.0, hiDeg = 0.0;
        angleDegRange(t, fx, p, key, loDeg, hiDeg);
        writeVal = angleToParamValue(valueDeg, mn, mx, loDeg, hiDeg);
        out["degRange"] = Json{{"lo", loDeg}, {"hi", hiDeg}};
    } else {                                             // real-degree param: write through, clamped
        writeVal = valueDeg;
        if (mx > mn) { if (writeVal < mn) writeVal = mn; if (writeVal > mx) writeVal = mx; }
    }
    TrackFX_SetParam(t, fx, p, writeVal);
    out["value"] = writeVal;
    return out;
}

// Set a named CONTINUOUS param to a REAL-WORLD value (metres, a count, ...) — the non-angle sibling of
// setNamedValueParam. If the plug-in exposes the param normalized ([min,max] span <= 2, the JUCE/IEM
// convention) we discover its real span from the formatted endpoints (paramDegRange parses the leading
// number of TrackFX_FormatParamValueNormalized at 0 and 1) and map the requested real value onto
// [min,max]; otherwise the value is written through, clamped. Returns a report, or null if not found.
Json setNamedRealParam(MediaTrack* t, int fx, int np, const std::string& key, double realVal,
                       int source1 = 0) {
    int p = discoverParam(t, fx, np, key, source1);
    if (p < 0) return Json(nullptr);
    double mn = 0.0, mx = 0.0;
    TrackFX_GetParam(t, fx, p, &mn, &mx);
    Json out = Json{{"param", p}, {"name", fxParamNameStr(t, fx, p)}, {"requested", realVal},
                    {"min", mn}, {"max", mx}};
    double writeVal;
    if (mx - mn <= 2.0 + 1e-9) {                    // normalized param — map real value onto [min,max]
        // Parse the plug-in's own real range from its formatted endpoints (no degree guard — metres,
        // counts, etc. can span < 30 units, unlike angleDegRange which is angle-only).
        char b0[128] = {0}, b1[128] = {0};
        double lo = 0.0, hi = 0.0;
        bool haveRange =
            TrackFX_FormatParamValueNormalized(t, fx, p, 0.0, b0, sizeof(b0)) &&
            TrackFX_FormatParamValueNormalized(t, fx, p, 1.0, b1, sizeof(b1)) &&
            parseLeadingNumber(b0, lo) && parseLeadingNumber(b1, hi) && hi != lo;
        if (haveRange) {
            double nrm = (realVal - lo) / (hi - lo);
            if (nrm < 0.0) nrm = 0.0;
            if (nrm > 1.0) nrm = 1.0;
            writeVal = mn + nrm * (mx - mn);
            out["realRange"] = Json{{"lo", lo}, {"hi", hi}};
        } else {                                    // no parseable range — treat realVal as raw normalized
            writeVal = realVal;
            if (writeVal < mn) writeVal = mn;
            if (writeVal > mx) writeVal = mx;
        }
    } else {                                        // real-range param: write through, clamped
        writeVal = realVal;
        if (mx > mn) {
            if (writeVal < mn) writeVal = mn;
            if (writeVal > mx) writeVal = mx;
        }
    }
    TrackFX_SetParam(t, fx, p, writeVal);
    out["value"] = writeVal;
    return out;
}

// REPORT a param (index/name/value/range) WITHOUT changing it — for CHOICE params (Order Setting,
// Normalization) whose enum index differs per plug-in build and which we won't blind-write.
Json reportNamedParam(MediaTrack* t, int fx, int np, const std::string& key) {
    int p = discoverParam(t, fx, np, key);
    if (p < 0) return Json(nullptr);
    double mn = 0.0, mx = 0.0;
    double v = TrackFX_GetParam(t, fx, p, &mn, &mx);
    return Json{{"param", p}, {"name", fxParamNameStr(t, fx, p)}, {"value", v},
                {"min", mn}, {"max", mx}};
}

// Identity-wire pins 0..count-1 onto the same-indexed track channels (isoutput: 1 = output pins,
// 0 = input pins). Channel k's bitmask is bit k, split across the low-32 arg and the high-32 arg
// (pins 32..63 exist at 5th order and up). Returns the mapping as JSON for the caller's report.
Json pinIdentityWire(MediaTrack* t, int fx, int count, int isoutput) {
    Json arr = Json::array();
    for (int k = 0; k < count; ++k) {
        int low = (k < 32) ? (int)(1u << k) : 0;
        int high = (k >= 32) ? (int)(1u << (k - 32)) : 0;
        TrackFX_SetPinMappings(t, fx, isoutput, k, low, high);
        arr.push_back(Json{{"pin", k}, {"channel", k}});
    }
    return arr;
}

// Resolve an FX to drive. Priority: (1) an explicit `fx` index always wins; (2) reuse an instance
// already on the track — first via REAPER's own resolver (TrackFX_AddByName instantiate = 0, query
// only), which also folds a bundled JSFX by its file alias, then via a case-insensitive substring
// scan of on-track FX names against the candidate seeds — so the shared encode/rotate/decode/
// beamform/room substrate re-drives its instance instead of stacking a duplicate on every call;
// (3) otherwise add the first candidate that resolves on this machine (instantiate = -1 → fresh
// instance). Returns the fx index, or -1 if none found. Pass an explicit `fx` to target a second
// instance deliberately.
int resolveOrAddByCandidates(MediaTrack* t, const Json& a,
                             const std::vector<std::string>& cands, std::string& usedName) {
    if (a.contains("fx") && a["fx"].is_number()) {
        int fx = a["fx"].get<int>();
        usedName = fxNameStr(t, fx);
        return fx;
    }
    // Find-existing pass (a): REAPER's own resolver, query-only (instantiate = 0) — matches an
    // existing instance the same way the add path resolves the candidate, so it also finds a bundled
    // JSFX by its file alias ("mcp_ambi_convert") even though its on-track name is the @desc string
    // ("JS: MCP Ambisonic Format Converter"). Scanned in candidate priority order; no side effects.
    for (const auto& c : cands) {
        int fx = TrackFX_AddByName(t, c.c_str(), false, 0);
        if (fx >= 0) { usedName = fxNameStr(t, fx); return fx; }
    }
    // Find-existing pass (b): fall back to a case-insensitive substring scan of on-track FX names
    // against the candidate seeds (same idiom as findSurroundFx) — a VST instance shows up as e.g.
    // "VST3: RoomEncoder (IEM)" containing the seed "RoomEncoder".
    const int nfx = TrackFX_GetCount(t);
    for (const auto& c : cands) {
        const std::string needle = toLower(c);
        for (int i = 0; i < nfx; ++i)
            if (toLower(fxNameStr(t, i)).find(needle) != std::string::npos) {
                usedName = fxNameStr(t, i);
                return i;
            }
    }
    // None present — add the first candidate that resolves on this machine (fresh instance).
    for (const auto& c : cands) {
        int fx = TrackFX_AddByName(t, c.c_str(), false, -1);
        if (fx >= 0) { usedName = c; return fx; }
    }
    return -1;
}

// The RENDER_* accessors, track-selection save/apply, and the RenderSnapshot
// snapshot/restore now live in ../render_stats.h (shared with analysis.check_deliverable). They are
// inline in namespace reaper_mcp, so the call sites below resolve unqualified exactly as before.

// Build the external Dolby Atmos Renderer / DAPS send layout: the bed bus feeds renderer channels
// 1..bedW (one multichannel send); each object track feeds a subsequent mono renderer channel.
// REAPER cannot author ADM BWF, so this wires the topology the certified renderer consumes and
// reports it — the ADM/BWF authoring happens in the external tool. Creates real sends only when a
// rendererTrack is given and this isn't a dry run; otherwise returns the channel plan for review.
Json buildAtmosLayoutSDK(const Json& a, bool dryRun, Json& warnings) {
    const int bedTrack = (a.contains("bedTrack") && a["bedTrack"].is_number())
                             ? a["bedTrack"].get<int>() : -1;
    const int rendererTrack = (a.contains("rendererTrack") && a["rendererTrack"].is_number())
                                  ? a["rendererTrack"].get<int>() : -1;
    std::vector<int> objs;
    if (a.contains("objectTracks") && a["objectTracks"].is_array())
        for (const auto& o : a["objectTracks"]) if (o.is_number()) objs.push_back(o.get<int>());
    const std::string bedLayout = optStr(a, "bedLayout", "7.1.4");
    const BedLayout* bl = findBedLayout(bedLayout);
    int bedW = 0;
    if (bedTrack >= 0)      bedW = trackChannels(requireTrack(bedTrack));
    else if (bl)           bedW = bl->channels;
    else                   bedW = 10;  // 7.1.2 Dolby bed default

    Json chanPlan = Json::array();
    chanPlan.push_back(Json{{"group", "bed"}, {"layout", bedLayout}, {"channels", bedW},
                            {"rendererChannels", "1.." + std::to_string(bedW)}});
    for (size_t i = 0; i < objs.size(); ++i)
        chanPlan.push_back(Json{{"group", "object"}, {"sourceTrack", objs[i]},
                                {"rendererChannel", bedW + (int)i + 1}});

    Json sends = Json::array();
    const int totalCh = bedW + (int)objs.size();
    if (rendererTrack >= 0 && !dryRun) {
        MediaTrack* rt = requireTrack(rendererTrack);
        if (trackChannels(rt) < totalCh)
            SetMediaTrackInfo_Value(rt, "I_NCHAN", (double)clampChannels(totalCh));
        Undo_BeginBlock2(kCur);
        if (bedTrack >= 0) {
            MediaTrack* bt = requireTrack(bedTrack);
            const SendChanEnc enc = encodeSend(0, bedW, 0, false);
            int si = CreateTrackSend(bt, rt);
            if (si >= 0) {
                SetTrackSendInfo_Value(bt, 0, si, "I_SRCCHAN", (double)enc.src);
                SetTrackSendInfo_Value(bt, 0, si, "I_DSTCHAN", (double)enc.dst);
                sends.push_back(Json{{"from", bedTrack}, {"to", rendererTrack}, {"sendIndex", si},
                                     {"kind", "bed"}, {"destChannel", 0}, {"channels", bedW}});
            }
        } else {
            warnings.push_back("atmos-send-layout: no bedTrack given — objects wired, bed send skipped.");
        }
        for (size_t i = 0; i < objs.size(); ++i) {
            MediaTrack* ot = requireTrack(objs[i]);
            const int dst = bedW + (int)i;
            const SendChanEnc enc = encodeSend(0, 1, dst, false);  // mono object -> one renderer channel
            int si = CreateTrackSend(ot, rt);
            if (si >= 0) {
                SetTrackSendInfo_Value(ot, 0, si, "I_SRCCHAN", (double)enc.src);
                SetTrackSendInfo_Value(ot, 0, si, "I_DSTCHAN", (double)enc.dst);
                sends.push_back(Json{{"from", objs[i]}, {"to", rendererTrack}, {"sendIndex", si},
                                     {"kind", "object"}, {"destChannel", dst}, {"channels", 1}});
            }
        }
        Undo_EndBlock2(kCur, "MCP: Atmos bed+object send layout", -1);
    } else if (rendererTrack < 0) {
        warnings.push_back("atmos-send-layout: no rendererTrack given — returning the channel plan "
                           "only (no sends created).");
    }

    return Json{
        {"bedLayout", bedLayout}, {"bedChannels", bedW}, {"objectCount", (int)objs.size()},
        {"totalChannels", totalCh}, {"rendererTrack", rendererTrack},
        {"channelPlan", chanPlan}, {"sends", sends},
        {"note", "REAPER cannot author ADM BWF natively. Route this wide bus to the external Dolby "
                 "Atmos Renderer / DAPS input (e.g. via ReaRoute / BlackHole / Dante) with a matching "
                 "input config. The standard Dolby bed is 7.1.2 (10 ch); heights beyond that are "
                 "usually carried as objects — set bedLayout to match your renderer's bed config."}};
}

// ---------------------------------------------------------------------------------------------
// SDK execution helpers for the spatial composite verbs (spatialize_stems, stereo_to_ambisonic,
// setup_immersive_session). Each mirrors the dryRun plan (spatial_verbs.h) step-for-step, reusing
// the primitives above so a composite is a thin, single-undo orchestration. All REAPER APIs used
// here are already WANTed by the primitive handlers — the composites introduce no new WANT. The
// SDK-free planning / geometry lives in spatial_verbs.h.
// ---------------------------------------------------------------------------------------------

// Find a track by exact P_NAME; -1 if none. Resolves `sources` given as names and powers the
// idempotent "does this bed already exist?" check (spatialize_stems).
int findTrackByName(const std::string& nm) {
    const int n = CountTracks(kCur);
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(kCur, i);
        if (t && trackNameStr(t) == nm) return i;
    }
    return -1;
}

// Resolve a `sources[]` entry (integer index or track name) to a SourceRef (index=-1 if unresolved).
spatial_verbs::SourceRef resolveSourceEntry(const Json& e) {
    spatial_verbs::SourceRef s;
    if (e.is_number())      { s.index = e.get<int>(); }
    else if (e.is_string()) { s.index = findTrackByName(e.get<std::string>()); s.name = e.get<std::string>(); }
    if (s.index >= 0) {
        MediaTrack* t = GetTrack(kCur, s.index);
        if (t) { s.name = trackNameStr(t); s.channels = trackChannels(t); }
    }
    return s;
}

// Is at least one ambisonic ENCODER for `suite` installed? A non-mutating pre-flight (enumerates
// installed FX) so a "no suite" composite fails cleanly with a structured error before any edit.
bool ambisonicEncoderAvailable(const std::string& suite) {
    const char* name = nullptr;
    const char* ident = nullptr;
    int i = 0;
    while (EnumInstalledFX(i, &name, &ident)) {
        const std::string ln = toLower(name ? name : "");
        if (classifyRole(ln) == "encoder") {
            const std::string s = classifySuite(ln);
            if (suite == "auto" || suite == s) return true;
        }
        ++i;
    }
    return false;
}

// Insert a new track at the end; set channel count (>=2) + name; optional folder parent. -1 on fail.
int insertNamedTrack(const std::string& name, int channels, bool folderParent) {
    const int idx = CountTracks(kCur);
    InsertTrackAtIndex(idx, true);
    TrackList_AdjustWindows(false);
    MediaTrack* t = GetTrack(kCur, idx);
    if (!t) return -1;
    if (channels >= 2) SetMediaTrackInfo_Value(t, "I_NCHAN", (double)clampChannels(channels));
    setTrackString(t, "P_NAME", name);
    if (folderParent) SetMediaTrackInfo_Value(t, "I_FOLDERDEPTH", 1.0);
    return idx;
}

// Encode a source track into an ambisonic scene: set its channel count, insert an encoder, point it
// at (az,el)[+width], identity-wire the ACN outputs. Returns a per-source report (null if no encoder).
Json encodeSourceToScene(int srcIdx, const spatial_verbs::Target& tgt,
                         const spatial_verbs::ResolvedPlacement& p, const std::string& suite) {
    MediaTrack* t = GetTrack(kCur, srcIdx);
    if (!t) return Json(nullptr);
    SetMediaTrackInfo_Value(t, "I_NCHAN", (double)tgt.channels);
    std::vector<std::string> cands = ambiCandidates("encoder", suite);
    std::string used;
    Json noargs = Json::object();
    int fx = resolveOrAddByCandidates(t, noargs, cands, used);
    if (fx < 0) return Json(nullptr);
    const int np = TrackFX_GetNumParams(t, fx);
    Json positions = Json::array();
    Json rAz = setNamedValueParam(t, fx, np, "azimuth", p.az, 0);
    if (!rAz.is_null()) { rAz["axis"] = "azimuth"; positions.push_back(rAz); }
    Json rEl = setNamedValueParam(t, fx, np, "elevation", p.el, 0);
    if (!rEl.is_null()) { rEl["axis"] = "elevation"; positions.push_back(rEl); }
    const double spread = placement::widthSpreadDeg(p.width);
    if (spread > 0.0) {
        Json rW = setNamedValueParam(t, fx, np, "width", spread, 0);
        if (!rW.is_null()) { rW["axis"] = "width"; positions.push_back(rW); }
    }
    Json pinMap = pinIdentityWire(t, fx, hoaChannels(tgt.order), 1 /*output*/);
    return Json{{"source", srcIdx}, {"encoder", used}, {"fxIndex", fx},
                {"placement", p.label}, {"positions", positions}, {"pinMap", pinMap}};
}

// Pan a source into a channel bed via ReaSurroundPan (or, for an LFE source, a mono send to the bed's
// LFE channel). Creates the send into the bed. Returns a per-source report.
Json panSourceToBed(int srcIdx, int bedIdx, int bedChannels, const spatial_verbs::ResolvedPlacement& p) {
    MediaTrack* src = GetTrack(kCur, srcIdx);
    MediaTrack* bed = GetTrack(kCur, bedIdx);
    if (!src || !bed) return Json(nullptr);
    if (p.lfe) {
        const int lfeChan = 3;  // LFE = channel index 3 (0-based) in the SMPTE 5.1/7.1/7.1.4/9.1.6 order
        const SendChanEnc enc = encodeSend(0, 1, lfeChan, true);
        int si = CreateTrackSend(src, bed);
        if (si >= 0) {
            SetTrackSendInfo_Value(src, 0, si, "I_SRCCHAN", (double)enc.src);
            SetTrackSendInfo_Value(src, 0, si, "I_DSTCHAN", (double)enc.dst);
        }
        return Json{{"source", srcIdx}, {"placement", "lfe"}, {"sendIndex", si},
                    {"destChannel", lfeChan}, {"lfe", true}};
    }
    int fx = TrackFX_AddByName(src, "ReaSurroundPan", false, -1);
    Json posSet = Json::array();
    if (fx >= 0) {
        TrackFX_SetNamedConfigParm(src, fx, "NUMCHANNELS", "1");
        TrackFX_SetNamedConfigParm(src, fx, "NUMSPEAKERS", std::to_string(bedChannels).c_str());
        const double az = p.az * M_PI / 180.0, el = p.el * M_PI / 180.0;
        const double xu = -std::sin(az) * std::cos(el);  // +az = left (matches the encoder/descriptor convention)
        const double yu = std::cos(az) * std::cos(el);   // front
        const double zu = std::sin(el);                  // up
        const int np = TrackFX_GetNumParams(src, fx);
        auto setAxis = [&](char axis, double v) {
            const std::string want(1, axis);
            for (int q = 0; q < np; ++q)
                if (toLower(fxParamNameStr(src, fx, q)).find(want) != std::string::npos) {
                    double n = (v + 1.0) * 0.5;
                    if (n < 0.0) n = 0.0;
                    if (n > 1.0) n = 1.0;
                    TrackFX_SetParamNormalized(src, fx, q, n);
                    posSet.push_back(Json{{"axis", std::string(1, axis)}, {"param", q}, {"normalized", n}});
                    return;
                }
        };
        setAxis('x', -xu);  // ReaSurroundPan in1X 0=right..1=left (verified 2026-07-10): negate x for the panner
        setAxis('y', yu);
        if (p.el != 0.0) setAxis('z', zu);
    }
    const SendChanEnc enc = encodeSend(0, bedChannels, 0, false);
    int si = CreateTrackSend(src, bed);
    if (si >= 0) {
        SetTrackSendInfo_Value(src, 0, si, "I_SRCCHAN", (double)enc.src);
        SetTrackSendInfo_Value(src, 0, si, "I_DSTCHAN", (double)enc.dst);
    }
    return Json{{"source", srcIdx}, {"placement", p.label}, {"fxIndex", fx},
                {"positionSet", posSet}, {"sendIndex", si}, {"channels", bedChannels}};
}

// Build a binaural monitor bus. ambisonic => a multichannel send off `busIdx` (the scene) feeds a
// binaural decoder (input pins identity, output on ch 1/2). object => a 2-ch bus fed by per-object
// mono sends through a binauraliser. Returns the report (monitorTrack = -1 on failure).
Json buildBinauralMonitor(int busIdx, bool ambisonic, int busChannels,
                          const std::vector<int>& objectTracks, const std::string& suite) {
    const int monCh = ambisonic ? clampChannels(busChannels) : 2;
    const int monIdx = insertNamedTrack("Binaural Monitor", monCh, false);
    if (monIdx < 0) return Json{{"monitorTrack", -1}};
    MediaTrack* mon = GetTrack(kCur, monIdx);
    Json rep = Json{{"monitorTrack", monIdx}, {"mode", ambisonic ? "ambisonic" : "object"},
                    {"busChannels", monCh}};
    Json sends = Json::array();
    if (ambisonic && busIdx >= 0) {
        MediaTrack* bus = GetTrack(kCur, busIdx);
        const SendChanEnc enc = encodeSend(0, clampChannels(busChannels), 0, false);
        int si = CreateTrackSend(bus, mon);
        if (si >= 0) {
            SetTrackSendInfo_Value(bus, 0, si, "I_SRCCHAN", (double)enc.src);
            SetTrackSendInfo_Value(bus, 0, si, "I_DSTCHAN", (double)enc.dst);
            sends.push_back(Json{{"from", busIdx}, {"to", monIdx}, {"kind", "scene"}});
        }
    } else if (!ambisonic) {
        for (size_t i = 0; i < objectTracks.size(); ++i) {
            MediaTrack* ot = GetTrack(kCur, objectTracks[i]);
            if (!ot) continue;
            const SendChanEnc enc = encodeSend(0, 1, 0, true);  // mono object -> stereo monitor
            int si = CreateTrackSend(ot, mon);
            if (si >= 0) {
                SetTrackSendInfo_Value(ot, 0, si, "I_SRCCHAN", (double)enc.src);
                SetTrackSendInfo_Value(ot, 0, si, "I_DSTCHAN", (double)enc.dst);
                sends.push_back(Json{{"from", objectTracks[i]}, {"to", monIdx}, {"kind", "object"}});
            }
        }
    }
    std::vector<std::string> cands = ambiCandidates("binaural", suite);
    std::string used;
    Json noargs = Json::object();
    int fx = resolveOrAddByCandidates(mon, noargs, cands, used);
    if (fx >= 0) {
        if (ambisonic) pinIdentityWire(mon, fx, clampChannels(busChannels), 0 /*input*/);
        TrackFX_SetPinMappings(mon, fx, 1, 0, 1, 0);  // output pin 0 -> ch 1
        TrackFX_SetPinMappings(mon, fx, 1, 1, 2, 0);  // output pin 1 -> ch 2
        rep["decoder"] = used;
        rep["decoderFx"] = fx;
    }
    rep["sends"] = sends;
    return rep;
}

// ---------------------------------------------------------------------------------------------
// Native ADM object-audio authoring (spatial.export_adm) SDK helpers. The pure ADM model ->
// chna/axml/RIFF/BW64 serialization + parsing lives in ../adm_bwf.h (host-unit-tested); here we
// resolve the bed/object tracks, sample each object's panner position over the render window, and
// marshal the rendered stems into an adm::Model.
// ---------------------------------------------------------------------------------------------

// Bed speakers for an ADM layout: the project bed layouts PLUS the Atmos-canonical 7.1.2 (10 ch).
// Positions + BS.2051 speaker labels come from adm::speakerPosFor.
std::vector<adm::BedSpeaker> admBedSpeakers(const std::string& layout) {
    std::vector<std::string> labels;
    if (layout == "7.1.2")
        labels = {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf"};
    else if (const BedLayout* bl = findBedLayout(layout))
        for (const char* l : bl->labels) labels.push_back(l);
    std::vector<adm::BedSpeaker> out;
    for (const std::string& l : labels) {
        adm::BedSpeaker s;
        s.label = l;
        const adm::SpeakerPos p = adm::speakerPosFor(layout, l);
        s.az = p.az; s.el = p.el; s.speakerLabel = p.speakerLabel;
        s.lfe = (l == "LFE" || l == "LFE1" || l == "LFE2");
        out.push_back(s);
    }
    return out;
}
inline int admBedChannels(const std::string& layout) {
    if (layout == "7.1.2") return 10;
    const BedLayout* bl = findBedLayout(layout);
    return bl ? bl->channels : -1;
}

// Parse the leading signed number out of a formatted param string ("45.0 °", "-90.00 deg", "3.5 m").
bool parseLeadingNumber(const std::string& s, double& out) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    bool dig = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
        if (std::isdigit((unsigned char)s[i])) dig = true;
        ++i;
    }
    if (!dig) return false;
    try { out = std::stod(s.substr(start, i - start)); } catch (...) { return false; }
    return true;
}

// Locate the positional panner FX + its az/el/dist param indices on an object track (fx>=0 on success).
// Mirrors analysis.readObjectPosition's scan but yields indices, for per-parameter envelope sampling.
struct PosFx { int fx = -1; int azP = -1; int elP = -1; int distP = -1; };
PosFx findPositionalFx(MediaTrack* t) {
    PosFx r;
    if (!t) return r;
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
}

// Sample one FX param over time by linear interpolation of its automation envelope; falls back to the
// current normalized value when there is no envelope. degAt() formats the sampled normalized value to
// its degree reading (azimuth/elevation); normAt() returns the raw [0,1] (distance -> ADM 0..1).
struct ParamSampler {
    MediaTrack* t = nullptr; int fx = -1; int param = -1;
    std::vector<double> times, norms;
    double staticNorm = 0.0; bool hasEnv = false;
    void init(MediaTrack* tr, int f, int p) {
        t = tr; fx = f; param = p;
        staticNorm = TrackFX_GetParamNormalized(t, fx, param);
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
        return TrackFX_FormatParamValueNormalized(t, fx, param, nv, buf, (int)sizeof(buf)) && buf[0] &&
               parseLeadingNumber(buf, deg);
    }
};

// Build an object's block trajectory over [startWin,endWin] (project seconds): a uniform time grid at
// `blockMs` spacing (capped at maxBlocks), each block sampling az/el/dist. A static (un-automated)
// object collapses to a single block; a track with no positional panner reports positionSource="none".
std::vector<adm::Block> objectTrajectory(MediaTrack* t, double startWin, double endWin, double blockMs,
                                         int maxBlocks, std::string& positionSource) {
    std::vector<adm::Block> blocks;
    const double dur = (endWin > startWin) ? (endWin - startWin) : 0.0;
    const PosFx pf = findPositionalFx(t);
    if (pf.fx < 0) {
        positionSource = "none";
        adm::Block b; b.rtime = 0; b.duration = dur; b.az = 0; b.el = 0; b.dist = 1.0; b.gain = 1.0;
        blocks.push_back(b);
        return blocks;
    }
    ParamSampler az, el, di;
    if (pf.azP >= 0) az.init(t, pf.fx, pf.azP);
    if (pf.elP >= 0) el.init(t, pf.fx, pf.elP);
    if (pf.distP >= 0) di.init(t, pf.fx, pf.distP);
    auto sample = [&](double at, adm::Block& b) {
        double d;
        b.az = (pf.azP >= 0 && az.degAt(at, d)) ? d : 0.0;
        b.el = (pf.elP >= 0 && el.degAt(at, d)) ? d : 0.0;
        b.dist = (pf.distP >= 0) ? di.normAt(at) : 1.0;   // normalized [0,1] -> ADM distance
        if (b.dist < 0) b.dist = 0;
        if (b.dist > 1.5) b.dist = 1.5;
        b.gain = 1.0;
    };
    const bool anyEnv = az.hasEnv || el.hasEnv || di.hasEnv;
    if (!anyEnv) {
        positionSource = "static";
        adm::Block b; b.rtime = 0; b.duration = dur;
        sample(startWin, b);
        blocks.push_back(b);
        return blocks;
    }
    positionSource = "envelope";
    double step = blockMs / 1000.0;
    if (step <= 0) step = 0.1;
    int nblk = (dur > 0) ? (int)std::ceil(dur / step) + 1 : 1;
    if (nblk > maxBlocks) { nblk = maxBlocks; step = (nblk > 1) ? dur / (double)(nblk - 1) : dur; }
    if (nblk < 1) nblk = 1;
    for (int i = 0; i < nblk; ++i) {
        double rt = i * step;
        if (rt > dur) rt = dur;
        adm::Block b; b.rtime = rt; b.duration = 0.0;  // writer fills duration to the next rtime
        sample(startWin + rt, b);
        blocks.push_back(b);
    }
    return blocks;
}
#endif  // REAPER_MCP_HAVE_SDK

}  // namespace

void registerSpatialTools(ToolRegistry& reg) {
    // =============================================================================================
    // (a) MULTICHANNEL FOUNDATION + BED BUILDERS
    // =============================================================================================

    // ---- spatial.set_track_channels — the multichannel foundation ----
    reg.add(Tool{
        "spatial.set_track_channels",
        "Set a track's channel count (2..128, even) and report what that count means in immersive "
        "terms (bed layout and/or ambisonic order). 16 = 9.1.6 or 3rd-order ambisonics, 12 = 7.1.4.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "channels":{"type":"integer","minimum":2,"maximum":128}},
            "required":["track","channels"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"channels":{"type":"integer"},
            "interpretation":{"type":"string"},"ambisonicOrder":{"type":"integer"}},
            "required":["ok","channels"]})"),
        ToolAnnotations{false, false, true}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            int ch = clampChannels(reqInt(a, "channels"));
            int order = -1;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_NCHAN", (double)ch);
            Undo_EndBlock2(kCur, "MCP: set spatial track channels", -1);
            ch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
#else
            (void)idx;
#endif
            std::string interp = interpretChannels(ch, &order);
            Json out{{"ok", true}, {"channels", ch}, {"interpretation", interp}};
            if (order >= 1) out["ambisonicOrder"] = order;
            return out;
        }});

    // ---- spatial.build_bed — standard immersive bed layouts with a labeled multichannel bus ----
    reg.add(Tool{
        "spatial.build_bed",
        "Create a channel-based bed bus (5.1 | 7.1 | 7.1.4 | 9.1.6 | 22.2): inserts a multichannel "
        "track with the right channel count, names it, and returns the speaker channel map (ready "
        "for stem assignment via spatial.assign_to_bed). Optionally make it a folder parent.",
        jparse(R"({"type":"object","properties":{"layout":{"type":"string",
            "enum":["5.1","7.1","7.1.4","9.1.6","22.2"]},"busName":{"type":"string"},
            "index":{"type":"integer","minimum":0},"folder":{"type":"boolean","default":false}},
            "required":["layout"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"busTrack":{"type":"integer"},
            "channels":{"type":"integer"},"layout":{"type":"string"},"name":{"type":"string"},
            "note":{"type":"string"},"channelMap":{"type":"array","items":{"type":"object",
                "properties":{"channel":{"type":"integer"},"label":{"type":"string"},
                "lfe":{"type":"boolean"}}}}},
            "required":["busTrack","channels","layout","channelMap"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const std::string layout = reqStr(a, "layout");
            const BedLayout* b = findBedLayout(layout);
            if (!b) throw std::runtime_error("unknown bed layout: " + layout);
            const std::string name = optStr(a, "busName", std::string(b->name) + " Bed");
            const bool folder = optBool(a, "folder", false);
            int busTrack = 0;
#ifdef REAPER_MCP_HAVE_SDK
            int idx = (a.contains("index") && a["index"].is_number()) ? a["index"].get<int>()
                                                                      : CountTracks(kCur);
            Undo_BeginBlock2(kCur);
            InsertTrackAtIndex(idx, true);
            TrackList_AdjustWindows(false);
            MediaTrack* t = GetTrack(kCur, idx);
            if (!t) { Undo_EndBlock2(kCur, "MCP: build bed", -1);
                      throw std::runtime_error("could not insert bed bus track"); }
            SetMediaTrackInfo_Value(t, "I_NCHAN", (double)b->channels);
            setTrackString(t, "P_NAME", name);
            if (folder) SetMediaTrackInfo_Value(t, "I_FOLDERDEPTH", 1.0);
            Undo_EndBlock2(kCur, ("MCP: build " + layout + " bed").c_str(), -1);
            busTrack = idx;
#else
            busTrack = (a.contains("index") && a["index"].is_number()) ? a["index"].get<int>() : 0;
            (void)folder;
#endif
            return Json{{"busTrack", busTrack}, {"channels", b->channels}, {"layout", b->name},
                        {"name", name}, {"note", b->note}, {"channelMap", bedChannelMap(*b)}};
        }});

    // ---- spatial.assign_to_bed — per-channel multichannel send routing (stem -> bed) ----
    reg.add(Tool{
        "spatial.assign_to_bed",
        "Route a source track into a bed bus with a multichannel send landing at a destination "
        "channel offset. channels=1 mono, 2 stereo, 4/6/8/... wider. Use destChannel to place a "
        "stem at a specific speaker (0-based); monoToChannel sums a stereo source into one channel.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "bed":{"type":"integer","minimum":0},
            "destChannel":{"type":"integer","minimum":0,"default":0},
            "channels":{"type":"integer","minimum":1,"maximum":64,"default":2},
            "srcChannel":{"type":"integer","minimum":0,"default":0},
            "db":{"type":"number"},"monoToChannel":{"type":"boolean","default":false}},
            "required":["track","bed"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"sendIndex":{"type":"integer"},
            "track":{"type":"integer"},"bed":{"type":"integer"},"destChannel":{"type":"integer"},
            "channels":{"type":"integer"},"srcChanEnc":{"type":"integer"},
            "dstChanEnc":{"type":"integer"}},"required":["sendIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int src = reqInt(a, "track");
            const int bed = reqInt(a, "bed");
            const int destChannel = optInt(a, "destChannel", 0);
            const int channels = optInt(a, "channels", 2);
            const int srcChannel = optInt(a, "srcChannel", 0);
            const bool monoMix = optBool(a, "monoToChannel", false);
            const SendChanEnc enc = encodeSend(srcChannel, channels, destChannel, monoMix);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* s = requireTrack(src);
            MediaTrack* d = requireTrack(bed);
            Undo_BeginBlock2(kCur);
            int sendIdx = CreateTrackSend(s, d);
            if (sendIdx >= 0) {
                SetTrackSendInfo_Value(s, 0, sendIdx, "I_SRCCHAN", (double)enc.src);
                SetTrackSendInfo_Value(s, 0, sendIdx, "I_DSTCHAN", (double)enc.dst);
                if (a.contains("db") && a["db"].is_number())
                    SetTrackSendInfo_Value(s, 0, sendIdx, "D_VOL", dbToGain(a["db"].get<double>()));
            }
            Undo_EndBlock2(kCur, "MCP: assign stem to bed", -1);
            if (sendIdx < 0) throw std::runtime_error("could not create send to bed");
            return Json{{"sendIndex", sendIdx}, {"track", src}, {"bed", bed},
                        {"destChannel", destChannel}, {"channels", channels},
                        {"srcChanEnc", enc.src}, {"dstChanEnc", enc.dst}};
#else
            return Json{{"sendIndex", 0}, {"track", src}, {"bed", bed},
                        {"destChannel", destChannel}, {"channels", channels},
                        {"srcChanEnc", enc.src}, {"dstChanEnc", enc.dst}};
#endif
        }});

    // =============================================================================================
    // (b) REAPER-NATIVE SURROUND PANNER (ReaSurroundPan)
    // =============================================================================================

    // ---- spatial.add_surround_panner — insert & configure ReaSurroundPan ----
    reg.add(Tool{
        "spatial.add_surround_panner",
        "Add REAPER's native surround panner (ReaSurroundPan) to a track and optionally set its "
        "NUMCHANNELS (sources it pans) and NUMSPEAKERS (speaker array). Returns the FX index plus "
        "the discovered parameter names so a caller can drive per-source positions.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "plugin":{"type":"string","default":"ReaSurroundPan"},
            "channels":{"type":"integer","minimum":1,"maximum":128},
            "speakers":{"type":"integer","minimum":1,"maximum":128},
            "setTrackChannels":{"type":"integer","minimum":2,"maximum":128}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"fxIndex":{"type":"integer"},"name":{"type":"string"},
            "paramCount":{"type":"integer"},"numChannels":{"type":"string"},
            "numSpeakers":{"type":"string"},"params":{"type":"array","items":{"type":"object",
                "properties":{"index":{"type":"integer"},"name":{"type":"string"}}}}},
            "required":["fxIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string plugin = optStr(a, "plugin", "ReaSurroundPan");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (a.contains("setTrackChannels") && a["setTrackChannels"].is_number())
                SetMediaTrackInfo_Value(t, "I_NCHAN",
                                        (double)clampChannels(a["setTrackChannels"].get<int>()));
            int fx = TrackFX_AddByName(t, plugin.c_str(), false, -1);
            if (fx < 0) { Undo_EndBlock2(kCur, "MCP: add surround panner", -1);
                          throw std::runtime_error("surround panner not found / could not add: " + plugin); }
            if (a.contains("channels") && a["channels"].is_number())
                TrackFX_SetNamedConfigParm(t, fx, "NUMCHANNELS",
                                           std::to_string(a["channels"].get<int>()).c_str());
            if (a.contains("speakers") && a["speakers"].is_number())
                TrackFX_SetNamedConfigParm(t, fx, "NUMSPEAKERS",
                                           std::to_string(a["speakers"].get<int>()).c_str());
            Undo_EndBlock2(kCur, "MCP: add surround panner", -1);
            const int np = TrackFX_GetNumParams(t, fx);
            Json params = Json::array();
            for (int p = 0; p < np; ++p)
                params.push_back(Json{{"index", p}, {"name", fxParamNameStr(t, fx, p)}});
            return Json{{"fxIndex", fx}, {"name", fxNameStr(t, fx)}, {"paramCount", np},
                        {"numChannels", fxNamedCfg(t, fx, "NUMCHANNELS")},
                        {"numSpeakers", fxNamedCfg(t, fx, "NUMSPEAKERS")},
                        {"params", std::move(params)}};
#else
            (void)idx;
            return Json{{"fxIndex", 0}, {"name", plugin}, {"paramCount", 0},
                        {"numChannels", ""}, {"numSpeakers", ""}, {"params", Json::array()}};
#endif
        }});

    // ---- spatial.get_surround_state — read the panner's config + all params (read-only) ----
    reg.add(Tool{
        "spatial.get_surround_state",
        "Read a ReaSurroundPan instance's NUMCHANNELS/NUMSPEAKERS and every parameter (index, name, "
        "value, normalized, min, max). This is the source of truth for the panner's parameter "
        "layout — use it to discover which params control each source's position.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0}},"required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"fxIndex":{"type":"integer"},"name":{"type":"string"},
            "numChannels":{"type":"string"},"numSpeakers":{"type":"string"},
            "paramCount":{"type":"integer"},"params":{"type":"array","items":{"type":"object",
                "properties":{"index":{"type":"integer"},"name":{"type":"string"},
                "value":{"type":"number"},"normalized":{"type":"number"},
                "min":{"type":"number"},"max":{"type":"number"}}}}},
            "required":["fxIndex","paramCount","params"]})"),
        ToolAnnotations{true, false, true}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int fx = resolveSurroundFx(a, t);
            const int np = TrackFX_GetNumParams(t, fx);
            Json params = Json::array();
            for (int p = 0; p < np; ++p) {
                double mn = 0.0, mx = 0.0;
                double v = TrackFX_GetParam(t, fx, p, &mn, &mx);
                params.push_back(Json{{"index", p}, {"name", fxParamNameStr(t, fx, p)},
                                      {"value", v}, {"normalized", TrackFX_GetParamNormalized(t, fx, p)},
                                      {"min", mn}, {"max", mx}});
            }
            return Json{{"fxIndex", fx}, {"name", fxNameStr(t, fx)},
                        {"numChannels", fxNamedCfg(t, fx, "NUMCHANNELS")},
                        {"numSpeakers", fxNamedCfg(t, fx, "NUMSPEAKERS")},
                        {"paramCount", np}, {"params", std::move(params)}};
#else
            (void)idx;
            return Json{{"fxIndex", 0}, {"name", "ReaSurroundPan"}, {"numChannels", ""},
                        {"numSpeakers", ""}, {"paramCount", 0}, {"params", Json::array()}};
#endif
        }});

    // ---- spatial.set_source_position — place a panned source (x/y[/z] or azimuth/elevation) ----
    reg.add(Tool{
        "spatial.set_source_position",
        "Position a source in the surround panner. Give x/y in [-1,1] (or azimuthDeg[/elevationDeg]) "
        "and optional z height; values are written normalized ([0,1], centre 0.5) so any param range "
        "is safe. Param indices are discovered by name; pass paramX/paramY/paramZ (from "
        "spatial.get_surround_state) to drive them deterministically.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "fx":{"type":"integer","minimum":0},"source":{"type":"integer","minimum":0,"default":0},
            "x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},
            "azimuthDeg":{"type":"number"},"elevationDeg":{"type":"number"},
            "paramX":{"type":"integer","minimum":0},"paramY":{"type":"integer","minimum":0},
            "paramZ":{"type":"integer","minimum":0},"normalizedInput":{"type":"boolean","default":false}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "set":{"type":"array","items":{"type":"object","properties":{"axis":{"type":"string"},
                "param":{"type":"integer"},"normalized":{"type":"number"}}}},
            "message":{"type":"string"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const bool normIn = optBool(a, "normalizedInput", false);

            // Resolve axis values in user space [-1,1] (front/right/up positive).
            bool hasX = a.contains("x") && a["x"].is_number();
            bool hasY = a.contains("y") && a["y"].is_number();
            bool hasZ = a.contains("z") && a["z"].is_number();
            double xu = hasX ? a["x"].get<double>() : 0.0;
            double yu = hasY ? a["y"].get<double>() : 0.0;
            double zu = hasZ ? a["z"].get<double>() : 0.0;
            if (a.contains("azimuthDeg") && a["azimuthDeg"].is_number()) {
                const double az = a["azimuthDeg"].get<double>() * M_PI / 180.0;
                double el = 0.0;
                if (a.contains("elevationDeg") && a["elevationDeg"].is_number())
                    el = a["elevationDeg"].get<double>() * M_PI / 180.0;
                xu = -std::sin(az) * std::cos(el);  // +az = left (matches the suite convention)
                yu = std::cos(az) * std::cos(el);   // front
                zu = std::sin(el);                  // up
                hasX = hasY = true;
                if (a.contains("elevationDeg")) hasZ = true;
            }
            auto toNorm = [normIn](double v) -> double {
                double n = normIn ? v : (v + 1.0) * 0.5;   // [-1,1] -> [0,1]
                return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
            };

#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int fx = resolveSurroundFx(a, t);
            const int np = TrackFX_GetNumParams(t, fx);
            const int source = optInt(a, "source", 0);

            // Discover a param index for an axis: explicit override wins; else match a param whose
            // name contains the axis letter (and, when source>0, the source number) as a token.
            auto discover = [&](const char* explicitKey, char axis) -> int {
                if (a.contains(explicitKey) && a[explicitKey].is_number())
                    return a[explicitKey].get<int>();
                const std::string want(1, axis);
                const std::string srcTok = std::to_string(source + 1);  // ReaSurroundPan is 1-based
                int fallback = -1;
                for (int p = 0; p < np; ++p) {
                    std::string nm = toLower(fxParamNameStr(t, fx, p));
                    if (nm.find(want) == std::string::npos) continue;
                    if (source == 0 && fallback < 0) fallback = p;
                    if (nm.find(srcTok) != std::string::npos) return p;
                }
                return fallback;
            };

            Json set = Json::array();
            if (hasX) { int p = discover("paramX", 'x');
                        if (p >= 0) { double n = toNorm(normIn ? xu : -xu); TrackFX_SetParamNormalized(t, fx, p, n);  // ReaSurroundPan in1X 0=right..1=left (verified 2026-07-10): negate x for the panner (normIn is already panner-normalized)
                                      set.push_back(Json{{"axis","x"},{"param",p},{"normalized",n}}); } }
            if (hasY) { int p = discover("paramY", 'y');
                        if (p >= 0) { double n = toNorm(yu); TrackFX_SetParamNormalized(t, fx, p, n);
                                      set.push_back(Json{{"axis","y"},{"param",p},{"normalized",n}}); } }
            if (hasZ) { int p = discover("paramZ", 'z');
                        if (p >= 0) { double n = toNorm(zu); TrackFX_SetParamNormalized(t, fx, p, n);
                                      set.push_back(Json{{"axis","z"},{"param",p},{"normalized",n}}); } }

            if (set.empty())
                return Json{{"ok", false}, {"fxIndex", fx},
                            {"message", "no position params matched by name; call "
                                        "spatial.get_surround_state and pass paramX/paramY/paramZ"}};
            return Json{{"ok", true}, {"fxIndex", fx}, {"set", std::move(set)}};
#else
            (void)idx;
            Json set = Json::array();
            if (hasX) set.push_back(Json{{"axis","x"},{"param",0},{"normalized",toNorm(normIn ? xu : -xu)}});
            if (hasY) set.push_back(Json{{"axis","y"},{"param",1},{"normalized",toNorm(yu)}});
            if (hasZ) set.push_back(Json{{"axis","z"},{"param",2},{"normalized",toNorm(zu)}});
            return Json{{"ok", true}, {"fxIndex", 0}, {"set", std::move(set)}};
#endif
        }});

    // =============================================================================================
    // (c) SCENE-BASED AMBISONICS — orchestrate the installed third-party suites (REAPER has none
    //     native). IEM > SPARTA > ATK priority; ambiX only on explicit request. Order/normalization
    //     are explicit; encoder/rotator/decoder pins are wired identity onto the HOA channels.
    // =============================================================================================

    // ---- spatial.detect_spatial_suites — read-only probe of installed ambisonic FX ----
    reg.add(Tool{
        "spatial.detect_spatial_suites",
        "Enumerate the ambisonic/spatial FX actually installed on this machine (IEM, SPARTA, ATK, "
        "ambiX, Blue Ripple) and classify each into a role (encoder/decoder/rotator/binaural). "
        "Returns the preferred encoder/decoder/rotator/binaural per the IEM>SPARTA>ATK priority so "
        "the other spatial tools know what they can drive. Instantiates nothing.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"suites":{"type":"array","items":{"type":"object",
            "properties":{"suite":{"type":"string"},"installed":{"type":"boolean"},
            "plugins":{"type":"array","items":{"type":"object","properties":{
                "name":{"type":"string"},"ident":{"type":"string"},"role":{"type":"string"}}}}}}},
            "preferred":{"type":"object"},"totalInstalledFx":{"type":"integer"}},
            "required":["suites"]})"),
        ToolAnnotations{true, false, true}, Profile::Spatial,
        [](const Json&) -> Json {
            const char* kSuites[] = {"IEM", "SPARTA", "ATK", "ambiX", "BlueRipple"};
            Json bySuite = Json::object();
            for (const char* s : kSuites)
                bySuite[s] = Json{{"suite", s}, {"installed", false}, {"plugins", Json::array()}};
            int total = 0;
#ifdef REAPER_MCP_HAVE_SDK
            const char* name = nullptr;
            const char* ident = nullptr;
            for (int i = 0; EnumInstalledFX(i, &name, &ident); ++i) {
                ++total;
                if (!name) continue;
                std::string nm = name;
                std::string suite = classifySuite(toLower(nm));
                if (suite.empty()) continue;
                bySuite[suite]["installed"] = true;
                bySuite[suite]["plugins"].push_back(Json{{"name", nm},
                    {"ident", ident ? std::string(ident) : std::string()},
                    {"role", classifyRole(toLower(nm))}});
            }
#endif
            // Preferred picks, honoring suite priority (IEM>SPARTA>ATK>ambiX>BlueRipple) THEN a
            // canonical-name preference within a suite. Several plug-ins share a role keyword — e.g.
            // IEM ships StereoEncoder/MultiEncoder/RoomEncoder/GranularEncoder, all "encod" — so
            // without a name preference the pick is enumeration order (alphabetical) and a niche
            // encoder like Granular can win. `prefer` ranks the general-purpose ones first so
            // `preferred.encoder` matches what the encode/decode/rotate tools actually insert; if
            // none of the preferred names match we fall back to the first role-classified plug-in.
            auto pick = [&](const char* role, const std::vector<std::string>& prefer) -> Json {
                for (const char* s : kSuites)
                    for (const auto& seed : prefer)
                        for (const auto& p : bySuite[s]["plugins"]) {
                            if (p.value("role", std::string()) != role) continue;
                            if (toLower(p.value("name", std::string())).find(seed) != std::string::npos)
                                return Json{{"suite", s}, {"name", p.value("name", std::string())}};
                        }
                for (const char* s : kSuites)
                    for (const auto& p : bySuite[s]["plugins"])
                        if (p.value("role", std::string()) == role)
                            return Json{{"suite", s}, {"name", p.value("name", std::string())}};
                return Json(nullptr);
            };
            Json suites = Json::array();
            for (const char* s : kSuites) suites.push_back(bySuite[s]);
            Json preferred{
                {"encoder", pick("encoder",
                    {"stereoencoder", "multiencoder", "ambienc", "ambix_encoder", "foa encode"})},
                {"decoder", pick("decoder",
                    {"simpledecoder", "allradecoder", "ambidec", "ambix_decoder", "foa decode"})},
                {"rotator", pick("rotator",
                    {"scenerotator", "rotator", "ambix_rotator", "foa transform rotate"})},
                {"binaural", pick("binaural",
                    {"binauraldecoder", "binauralis", "binaural", "ambix_binaural"})}};
            return Json{{"suites", std::move(suites)}, {"preferred", std::move(preferred)},
                        {"totalInstalledFx", total}};
        }});

    // ---- spatial.ambisonic_encode — order/normalization-aware, pin-mapped encoder ----
    reg.add(Tool{
        "spatial.ambisonic_encode",
        "Encode a track to ambisonics: set the track channel count for the order ((order+1)^2, "
        "even-padded), insert an installed encoder (IEM StereoEncoder by default; SPARTA/ATK/ambiX on "
        "request), point the source at (azimuthDeg, elevationDeg), and pin-wire the encoder's ACN "
        "outputs onto channels 0..N-1. Order Setting & Normalization (SN3D default; N3D/FuMa) are "
        "REPORTED for exact control rather than blind-set (their enum indices vary per plug-in). Pass "
        "source (1-based) to target one input of a multi-source encoder (e.g. MultiEncoder Azimuth 3).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "order":{"type":"integer","minimum":1,"maximum":7},
            "azimuthDeg":{"type":"number"},"elevationDeg":{"type":"number"},
            "widthDeg":{"type":"number"},"rollDeg":{"type":"number"},
            "source":{"type":"integer","minimum":1},
            "normalization":{"type":"string","enum":["SN3D","N3D","FuMa"],"default":"SN3D"},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ATK","ambiX"],"default":"auto"},
            "plugin":{"type":"string"},"fx":{"type":"integer","minimum":0},
            "setChannels":{"type":"boolean","default":true},
            "wirePins":{"type":"boolean","default":true}},
            "required":["track","order"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"order":{"type":"integer"},
            "hoaChannels":{"type":"integer"},"trackChannels":{"type":"integer"},
            "padded":{"type":"boolean"},"encoder":{"type":"string"},"fxIndex":{"type":"integer"},
            "requestedNormalization":{"type":"string"},"orderParam":{"type":["object","null"]},
            "normalizationParam":{"type":["object","null"]},"positions":{"type":"array"},
            "outputPins":{"type":"integer"},"pinMap":{"type":"array"},"warnings":{"type":"array"}},
            "required":["ok","order","hoaChannels","trackChannels"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            int order = reqInt(a, "order");
            if (order < 1) order = 1;
            if (order > 7) order = 7;
            const int hoa = hoaChannels(order);
            const int trackCh = clampChannels(hoa);
            const bool padded = trackCh != hoa;
            const std::string norm = optStr(a, "normalization", "SN3D");
            const std::string suite = optStr(a, "suite", "auto");
            const int source = optInt(a, "source", 0);
            const bool wirePins = optBool(a, "wirePins", true);
            const bool setCh = optBool(a, "setChannels", true);
            const int idx = reqInt(a, "track");

            std::vector<std::string> warnings;
            if (norm == "FuMa" && order > 3)
                warnings.push_back("FuMa is only defined up to 3rd order; use ACN/SN3D above 3rd.");
            if (padded)
                warnings.push_back(std::string(ordinal(order)) + "-order HOA has " +
                    std::to_string(hoa) + " channels; track padded to " + std::to_string(trackCh) +
                    " (REAPER channel counts are even) — the surplus channel stays silent.");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (setCh) SetMediaTrackInfo_Value(t, "I_NCHAN", (double)trackCh);

            std::vector<std::string> cands;
            if (a.contains("plugin") && a["plugin"].is_string())
                cands.push_back(a["plugin"].get<std::string>());
            for (const auto& c : ambiCandidates("encoder", suite)) cands.push_back(c);

            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: ambisonic encode", -1);
                throw std::runtime_error("no ambisonic encoder found (suite=" + suite +
                    "); install IEM/SPARTA/ATK or pass 'plugin'. Run spatial.detect_spatial_suites.");
            }
            const int np = TrackFX_GetNumParams(t, fx);

            // Continuous position params (degrees) — safe to write, clamped to each param's range.
            Json positions = Json::array();
            auto pushPos = [&](const char* axis, const char* key, double deg) {
                Json r = setNamedValueParam(t, fx, np, key, deg, source);
                if (!r.is_null()) { r["axis"] = axis; positions.push_back(r); }
            };
            if (a.contains("azimuthDeg") && a["azimuthDeg"].is_number())
                pushPos("azimuth", "azimuth", a["azimuthDeg"].get<double>());
            if (a.contains("elevationDeg") && a["elevationDeg"].is_number())
                pushPos("elevation", "elevation", a["elevationDeg"].get<double>());
            if (a.contains("widthDeg") && a["widthDeg"].is_number())
                pushPos("width", "width", a["widthDeg"].get<double>());
            if (a.contains("rollDeg") && a["rollDeg"].is_number())
                pushPos("roll", "roll", a["rollDeg"].get<double>());

            // Choice params — REPORT, don't blind-write (enum index differs per plug-in build).
            Json orderParam = reportNamedParam(t, fx, np, "order");
            Json normParam = reportNamedParam(t, fx, np, "normali");
            if (normParam.is_null())
                warnings.push_back("no Normalization param found by name; the encoder's own default "
                                   "applies (IEM ships SN3D).");
            else if (norm != "SN3D")
                warnings.push_back("Normalization '" + norm + "' requested: set the reported "
                    "normalizationParam by hand — its enum index is plug-in-specific.");

            // Pin-wire the encoder's ACN outputs identity onto channels 0..hoa-1 (the fiddly step).
            int inPins = 0, outPins = 0;
            TrackFX_GetIOSize(t, fx, &inPins, &outPins);
            Json pinMap = Json::array();
            if (wirePins) pinMap = pinIdentityWire(t, fx, hoa, 1 /*output*/);

            Undo_EndBlock2(kCur,
                ("MCP: ambisonic encode (" + std::string(ordinal(order)) + " order)").c_str(), -1);

            return Json{{"ok", true}, {"order", order}, {"hoaChannels", hoa},
                {"trackChannels", setCh ? trackCh : (int)GetMediaTrackInfo_Value(t, "I_NCHAN")},
                {"padded", padded}, {"encoder", fxNameStr(t, fx)}, {"fxIndex", fx},
                {"requestedNormalization", norm}, {"orderParam", orderParam},
                {"normalizationParam", normParam}, {"positions", std::move(positions)},
                {"outputPins", outPins}, {"pinMap", std::move(pinMap)}, {"warnings", warnings}};
#else
            (void)idx; (void)suite; (void)source; (void)wirePins; (void)setCh;
            Json positions = Json::array();
            if (a.contains("azimuthDeg") && a["azimuthDeg"].is_number())
                positions.push_back(Json{{"axis", "azimuth"}, {"value", a["azimuthDeg"].get<double>()}});
            if (a.contains("elevationDeg") && a["elevationDeg"].is_number())
                positions.push_back(Json{{"axis", "elevation"}, {"value", a["elevationDeg"].get<double>()}});
            Json pinMap = Json::array();
            for (int k = 0; k < hoa; ++k) pinMap.push_back(Json{{"pin", k}, {"channel", k}});
            return Json{{"ok", true}, {"order", order}, {"hoaChannels", hoa},
                {"trackChannels", trackCh}, {"padded", padded}, {"encoder", "IEM StereoEncoder"},
                {"fxIndex", 0}, {"requestedNormalization", norm}, {"orderParam", Json(nullptr)},
                {"normalizationParam", Json(nullptr)}, {"positions", std::move(positions)},
                {"outputPins", hoa}, {"pinMap", std::move(pinMap)}, {"warnings", warnings}};
#endif
        }});

    // ---- spatial.rotate_scene — SceneRotator yaw/pitch/roll on a HOA track ----
    reg.add(Tool{
        "spatial.rotate_scene",
        "Insert (or drive an existing) ambisonic scene rotator (IEM SceneRotator by default) on a HOA "
        "track and set yaw/pitch/roll in degrees. Identity-wires the rotator's HOA in/out pins onto "
        "the track channels. Returns the yaw/pitch/roll param indices so an agent can automate them "
        "(head-tracking / picture-locked yaw) with an envelope.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "yawDeg":{"type":"number"},"pitchDeg":{"type":"number"},"rollDeg":{"type":"number"},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ambiX","ATK"],"default":"auto"},
            "plugin":{"type":"string"},"fx":{"type":"integer","minimum":0},
            "wirePins":{"type":"boolean","default":true}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "rotator":{"type":"string"},"set":{"type":"array"},"automationParams":{"type":"object"},
            "pinMap":{"type":"array"},"warnings":{"type":"array"}},"required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string suite = optStr(a, "suite", "auto");
            const bool wirePins = optBool(a, "wirePins", true);
            std::vector<std::string> warnings;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            std::vector<std::string> cands;
            if (a.contains("plugin") && a["plugin"].is_string())
                cands.push_back(a["plugin"].get<std::string>());
            for (const auto& c : ambiCandidates("rotator", suite)) cands.push_back(c);
            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: rotate ambisonic scene", -1);
                throw std::runtime_error("no ambisonic rotator found (suite=" + suite +
                    "); install IEM SceneRotator / SPARTA Rotator or pass 'plugin'.");
            }
            const int np = TrackFX_GetNumParams(t, fx);
            Json set = Json::array();
            auto pushRot = [&](const char* axis, const char* key, const char* argKey) {
                if (!(a.contains(argKey) && a[argKey].is_number())) return;
                Json r = setNamedValueParam(t, fx, np, key, a[argKey].get<double>());
                if (!r.is_null()) { r["axis"] = axis; set.push_back(r); }
            };
            pushRot("yaw", "yaw", "yawDeg");
            pushRot("pitch", "pitch", "pitchDeg");
            pushRot("roll", "roll", "rollDeg");
            Json autom{{"yaw", discoverParam(t, fx, np, "yaw")},
                       {"pitch", discoverParam(t, fx, np, "pitch")},
                       {"roll", discoverParam(t, fx, np, "roll")}};
            const int nch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            Json pinMap = Json::array();
            if (wirePins) {
                pinIdentityWire(t, fx, nch, 0 /*input*/);
                pinMap = pinIdentityWire(t, fx, nch, 1 /*output*/);
            }
            Undo_EndBlock2(kCur, "MCP: rotate ambisonic scene", -1);
            if (set.empty())
                warnings.push_back("no yaw/pitch/roll params matched by name; inspect the rotator "
                                   "with the FX param tools and drive it by index.");
            return Json{{"ok", true}, {"fxIndex", fx}, {"rotator", fxNameStr(t, fx)},
                        {"set", std::move(set)}, {"automationParams", std::move(autom)},
                        {"pinMap", std::move(pinMap)}, {"warnings", warnings}};
#else
            (void)idx; (void)suite; (void)wirePins;
            Json set = Json::array();
            auto pushRot = [&](const char* axis, const char* argKey) {
                if (a.contains(argKey) && a[argKey].is_number())
                    set.push_back(Json{{"axis", axis}, {"value", a[argKey].get<double>()}});
            };
            pushRot("yaw", "yawDeg"); pushRot("pitch", "pitchDeg"); pushRot("roll", "rollDeg");
            return Json{{"ok", true}, {"fxIndex", 0}, {"rotator", "IEM SceneRotator"},
                        {"set", std::move(set)},
                        {"automationParams", Json{{"yaw", -1}, {"pitch", -1}, {"roll", -1}}},
                        {"pinMap", Json::array()}, {"warnings", warnings}};
#endif
        }});

    // ---- spatial.ambisonic_decode — HOA -> loudspeaker feeds ----
    reg.add(Tool{
        "spatial.ambisonic_decode",
        "Insert (or drive an existing) ambisonic decoder (IEM SimpleDecoder/AllRADecoder by default; "
        "SPARTA AmbiDEC on request) on a HOA track to produce loudspeaker feeds, and identity-wire the "
        "track's HOA channels 0..N-1 onto the decoder's input pins. Optionally set the output channel "
        "count. Order/Normalization are reported, not blind-set. NOTE: the loudspeaker layout for "
        "config-file decoders is chosen in the plug-in UI / a preset — this wires I/O and reports the "
        "params; it does not synthesize a decoder matrix.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "order":{"type":"integer","minimum":1,"maximum":7},
            "outputChannels":{"type":"integer","minimum":2,"maximum":128},
            "normalization":{"type":"string","enum":["SN3D","N3D","FuMa"],"default":"SN3D"},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ambiX","ATK"],"default":"auto"},
            "plugin":{"type":"string"},"fx":{"type":"integer","minimum":0},
            "wirePins":{"type":"boolean","default":true}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "decoder":{"type":"string"},"inputPins":{"type":"integer"},"outputPins":{"type":"integer"},
            "hoaChannels":{"type":["integer","null"]},"orderParam":{"type":["object","null"]},
            "normalizationParam":{"type":["object","null"]},"pinMap":{"type":"array"},
            "note":{"type":"string"},"warnings":{"type":"array"}},"required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string suite = optStr(a, "suite", "auto");
            const bool wirePins = optBool(a, "wirePins", true);
            const bool hasOrder = a.contains("order") && a["order"].is_number();
            int order = hasOrder ? a["order"].get<int>() : 0;
            if (order > 7) order = 7;
            std::vector<std::string> warnings;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (a.contains("outputChannels") && a["outputChannels"].is_number())
                SetMediaTrackInfo_Value(t, "I_NCHAN",
                                        (double)clampChannels(a["outputChannels"].get<int>()));
            std::vector<std::string> cands;
            if (a.contains("plugin") && a["plugin"].is_string())
                cands.push_back(a["plugin"].get<std::string>());
            for (const auto& c : ambiCandidates("decoder", suite)) cands.push_back(c);
            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: ambisonic decode", -1);
                throw std::runtime_error("no ambisonic decoder found (suite=" + suite +
                    "); install IEM SimpleDecoder/AllRADecoder or SPARTA AmbiDEC, or pass 'plugin'.");
            }
            const int np = TrackFX_GetNumParams(t, fx);
            Json orderParam = reportNamedParam(t, fx, np, "order");
            Json normParam = reportNamedParam(t, fx, np, "normali");
            int inPins = 0, outPins = 0;
            TrackFX_GetIOSize(t, fx, &inPins, &outPins);
            // Wire the HOA channels onto the decoder's INPUT pins (identity): prefer the requested
            // order's channel count, else the decoder's own reported input-pin count.
            int hoaN = hasOrder ? hoaChannels(order) : (inPins > 0 ? inPins : 0);
            Json pinMap = Json::array();
            if (wirePins && hoaN > 0) pinMap = pinIdentityWire(t, fx, hoaN, 0 /*input*/);
            Undo_EndBlock2(kCur, "MCP: ambisonic decode", -1);
            return Json{{"ok", true}, {"fxIndex", fx}, {"decoder", fxNameStr(t, fx)},
                {"inputPins", inPins}, {"outputPins", outPins},
                {"hoaChannels", hasOrder ? Json(hoaN) : Json(nullptr)},
                {"orderParam", orderParam}, {"normalizationParam", normParam},
                {"pinMap", std::move(pinMap)},
                {"note", "Loudspeaker layout for SimpleDecoder/AllRADecoder is set in the plug-in UI "
                         "or a decoder preset; this wires I/O and reports params."},
                {"warnings", warnings}};
#else
            (void)idx; (void)suite; (void)wirePins;
            int hoaN = hasOrder ? hoaChannels(order) : 0;
            Json pinMap = Json::array();
            for (int k = 0; k < hoaN; ++k) pinMap.push_back(Json{{"pin", k}, {"channel", k}});
            return Json{{"ok", true}, {"fxIndex", 0}, {"decoder", "IEM SimpleDecoder"},
                {"inputPins", hoaN}, {"outputPins", 0},
                {"hoaChannels", hasOrder ? Json(hoaN) : Json(nullptr)},
                {"orderParam", Json(nullptr)}, {"normalizationParam", Json(nullptr)},
                {"pinMap", std::move(pinMap)},
                {"note", "Loudspeaker layout for config-file decoders is UI/preset-driven."},
                {"warnings", warnings}};
#endif
        }});

    // =============================================================================================
    // BINAURAL MONITOR + LIVE HEAD-TRACKING
    // =============================================================================================

    // ---- spatial.add_binaural_monitor — audition immersive mixes on headphones ----
    reg.add(Tool{
        "spatial.add_binaural_monitor",
        "Build a non-destructive 2-channel binaural monitor bus so an agent can audition an immersive "
        "mix on headphones. mode 'ambisonic' (default): a labeled monitor bus is fed by a multichannel "
        "send off the given HOA scene track, and an ambisonic->binaural decoder (IEM BinauralDecoder by "
        "default; SPARTA/ATK fallback) is inserted with its HOA input pins identity-wired and its "
        "binaural output on channels 1/2. mode 'object': each objectTracks entry is sent to its own "
        "channel and an object binauraliser (SPARTA Binauraliser) is inserted. The source track is left "
        "untouched (solo the monitor, or mute the scene's master send, to audition in isolation). When "
        "headTracking is true a scene rotator (ambisonic) — or the binauraliser's own yaw/pitch/roll "
        "(object) — is driven live by an extension-hosted OSC/UDP listener: point a head-tracker "
        "(phone/IMU/Supperware/nvsonic/Unity) at the reported oscPort. Supported OSC: /yaw //pitch //roll "
        "floats, a 3-float /ypr (or /orientation//euler), or a 4-float /quaternion (w,x,y,z). The "
        "rotator's yaw/pitch/roll param indices are also returned so you can automate them with an "
        "envelope, and the OSC addresses are reported so you can instead target the plug-in's own OSC "
        "port. Call spatial.stop_head_tracking to release the port.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "mode":{"type":"string","enum":["ambisonic","object"],"default":"ambisonic"},
            "order":{"type":"integer","minimum":1,"maximum":7},
            "objectTracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "monitorName":{"type":"string"},"index":{"type":"integer","minimum":0},
            "createSend":{"type":"boolean","default":true},
            "headTracking":{"type":"boolean","default":false},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ambiX","ATK"],"default":"auto"},
            "decoderPlugin":{"type":"string"},"rotatorPlugin":{"type":"string"},
            "oscPort":{"type":"integer","minimum":0,"maximum":65535},
            "oscBind":{"type":"string","default":"127.0.0.1"},
            "invertYaw":{"type":"boolean","default":false},
            "invertPitch":{"type":"boolean","default":false},
            "invertRoll":{"type":"boolean","default":false},
            "wirePins":{"type":"boolean","default":true}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"mode":{"type":"string"},
            "monitorTrack":{"type":"integer"},"monitorName":{"type":"string"},
            "busChannels":{"type":"integer"},"decoder":{"type":"string"},"decoderFx":{"type":"integer"},
            "rotator":{"type":["string","null"]},"rotatorFx":{"type":"integer"},
            "inputPins":{"type":"integer"},"hoaChannels":{"type":["integer","null"]},
            "order":{"type":["integer","null"]},"sends":{"type":"array"},
            "automationParams":{"type":"object"},"headTracking":{"type":"object"},
            "warnings":{"type":"array","items":{"type":"string"}}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Spatial,
        [](const Json& a) -> Json {
            const int srcIdx = reqInt(a, "track");
            const std::string mode = optStr(a, "mode", "ambisonic");
            const std::string suite = optStr(a, "suite", "auto");
            const bool wirePins = optBool(a, "wirePins", true);
            const bool createSend = optBool(a, "createSend", true);
            const bool headTracking = optBool(a, "headTracking", false);
            std::vector<std::string> warnings;
            std::vector<int> objects;
            if (a.contains("objectTracks") && a["objectTracks"].is_array())
                for (const auto& o : a["objectTracks"]) if (o.is_number()) objects.push_back(o.get<int>());
            const bool hasOrder = a.contains("order") && a["order"].is_number();
            int order = hasOrder ? a["order"].get<int>() : 0;
            if (order > 7) order = 7;

            const Json oscAddresses = Json::array({"/yaw <deg>", "/pitch <deg>", "/roll <deg>",
                "/ypr <yaw> <pitch> <roll>", "/quaternion <w> <x> <y> <z>"});
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            // Resolve every source/object MediaTrack* BEFORE inserting the bus, so a custom insert
            // index can't shift the indices out from under us (we then work by pointer).
            MediaTrack* src = requireTrack(srcIdx);
            std::vector<MediaTrack*> objPtrs;
            for (int oi : objects) objPtrs.push_back(requireTrack(oi));

            // Input width + (ambisonic) order.
            int srcW = trackChannels(src);
            int hoaN = 0;
            if (mode == "ambisonic") {
                if (hasOrder) hoaN = hoaChannels(order);
                else {
                    for (int o = 1; o <= 7; ++o) { int c = (o + 1) * (o + 1); if (c <= srcW) { order = o; hoaN = c; } }
                    if (hoaN == 0) hoaN = srcW;
                }
                if (hoaN > srcW) {
                    warnings.push_back("source track has " + std::to_string(srcW) + " channels but order " +
                        std::to_string(order) + " needs " + std::to_string(hoaN) + " — using " +
                        std::to_string(srcW) + ".");
                    hoaN = srcW;
                }
            }
            const int busCh = (mode == "object")
                ? clampChannels((int)objects.size() >= 2 ? (int)objects.size() : 2)
                : clampChannels(hoaN >= 2 ? hoaN : 2);

            // Create the monitor bus.
            const int monIdx = (a.contains("index") && a["index"].is_number())
                ? a["index"].get<int>() : CountTracks(kCur);
            InsertTrackAtIndex(monIdx, true);
            TrackList_AdjustWindows(false);
            MediaTrack* mon = GetTrack(kCur, monIdx);
            if (!mon) { Undo_EndBlock2(kCur, "MCP: add binaural monitor", -1);
                        throw std::runtime_error("could not insert monitor bus track"); }
            const std::string monName = optStr(a, "monitorName",
                mode == "object" ? "Binaural Monitor (objects)" : "Binaural Monitor");
            SetMediaTrackInfo_Value(mon, "I_NCHAN", (double)busCh);
            setTrackString(mon, "P_NAME", monName);

            // Feed the monitor bus.
            Json sends = Json::array();
            if (createSend) {
                if (mode == "object") {
                    for (size_t i = 0; i < objPtrs.size(); ++i) {
                        const SendChanEnc enc = encodeSend(0, 1 /*mono*/, (int)i, true /*monoMix*/);
                        int si = CreateTrackSend(objPtrs[i], mon);
                        if (si >= 0) {
                            SetTrackSendInfo_Value(objPtrs[i], 0, si, "I_SRCCHAN", (double)enc.src);
                            SetTrackSendInfo_Value(objPtrs[i], 0, si, "I_DSTCHAN", (double)enc.dst);
                            sends.push_back(Json{{"from", objects[i]}, {"destChannel", (int)i},
                                                 {"sendIndex", si}});
                        }
                    }
                } else {
                    const int sendW = clampChannels(hoaN >= 2 ? hoaN : 2);
                    const SendChanEnc enc = encodeSend(0, sendW, 0, false);
                    int si = CreateTrackSend(src, mon);
                    if (si >= 0) {
                        SetTrackSendInfo_Value(src, 0, si, "I_SRCCHAN", (double)enc.src);
                        SetTrackSendInfo_Value(src, 0, si, "I_DSTCHAN", (double)enc.dst);
                        sends.push_back(Json{{"from", srcIdx}, {"channels", sendW}, {"sendIndex", si}});
                    }
                }
            }

            auto captureRanges = [&](int fx, HeadTrackTarget& tg) {
                auto rng = [&](int p, const char* key, double& mn, double& mx, double& dLo, double& dHi) {
                    if (p >= 0) {
                        double lo = 0, hi = 0; TrackFX_GetParam(mon, fx, p, &lo, &hi); mn = lo; mx = hi;
                        angleDegRange(mon, fx, p, key, dLo, dHi);  // real degree range for normalized params
                    }
                };
                rng(tg.yawParam, "yaw", tg.yawMin, tg.yawMax, tg.yawDegLo, tg.yawDegHi);
                rng(tg.pitchParam, "pitch", tg.pitchMin, tg.pitchMax, tg.pitchDegLo, tg.pitchDegHi);
                rng(tg.rollParam, "roll", tg.rollMin, tg.rollMax, tg.rollDegLo, tg.rollDegHi);
            };

            std::string decoderName, rotatorName;
            int decoderFx = -1, rotatorFx = -1, inputPins = 0;
            Json autom = Json::object();
            HeadTrackTarget htTarget;
            bool haveHtParams = false;

            // Head-track rotator (ambisonic mode only) goes BEFORE the decoder.
            if (mode == "ambisonic" && headTracking) {
                std::vector<std::string> rcands;
                if (a.contains("rotatorPlugin") && a["rotatorPlugin"].is_string())
                    rcands.push_back(a["rotatorPlugin"].get<std::string>());
                for (const auto& c : ambiCandidates("rotator", suite)) rcands.push_back(c);
                std::string rused;
                rotatorFx = resolveOrAddByCandidates(mon, a, rcands, rused);
                if (rotatorFx >= 0) {
                    rotatorName = fxNameStr(mon, rotatorFx);
                    const int rnp = TrackFX_GetNumParams(mon, rotatorFx);
                    if (wirePins) { pinIdentityWire(mon, rotatorFx, hoaN, 0); pinIdentityWire(mon, rotatorFx, hoaN, 1); }
                    const int yp = discoverParam(mon, rotatorFx, rnp, "yaw");
                    const int pp = discoverParam(mon, rotatorFx, rnp, "pitch");
                    const int rp = discoverParam(mon, rotatorFx, rnp, "roll");
                    autom = Json{{"yaw", yp}, {"pitch", pp}, {"roll", rp}};
                    if (yp >= 0 || pp >= 0 || rp >= 0) {
                        htTarget.fx = rotatorFx; htTarget.yawParam = yp; htTarget.pitchParam = pp;
                        htTarget.rollParam = rp; captureRanges(rotatorFx, htTarget); haveHtParams = true;
                    } else {
                        warnings.push_back("rotator inserted but no yaw/pitch/roll params matched by name.");
                    }
                } else {
                    warnings.push_back("head-tracking requested but no ambisonic rotator (IEM SceneRotator / "
                        "SPARTA Rotator) found; install one or pass rotatorPlugin. Monitor built without live rotation.");
                }
            }

            // Binaural decoder / object binauraliser.
            {
                std::vector<std::string> cands;
                if (a.contains("decoderPlugin") && a["decoderPlugin"].is_string())
                    cands.push_back(a["decoderPlugin"].get<std::string>());
                for (const auto& c : ambiCandidates("binaural", suite)) cands.push_back(c);
                std::string used;
                decoderFx = resolveOrAddByCandidates(mon, a, cands, used);
                if (decoderFx < 0) {
                    Undo_EndBlock2(kCur, "MCP: add binaural monitor", -1);
                    throw std::runtime_error(std::string("no ") + (mode == "object" ? "object binauraliser" :
                        "binaural decoder") + " found (IEM BinauralDecoder / SPARTA Binauraliser / ATK); "
                        "install the IEM Plug-in Suite or pass decoderPlugin.");
                }
                decoderName = fxNameStr(mon, decoderFx);
                inputPins = (mode == "object") ? (int)objects.size() : hoaN;
                if (wirePins) {
                    if (inputPins > 0) pinIdentityWire(mon, decoderFx, inputPins, 0 /*input*/);
                    TrackFX_SetPinMappings(mon, decoderFx, 1 /*output*/, 0, 1, 0);  // out pin 0 -> ch 1
                    TrackFX_SetPinMappings(mon, decoderFx, 1 /*output*/, 1, 2, 0);  // out pin 1 -> ch 2
                }
                // Object mode: the binauraliser's OWN yaw/pitch/roll is the head-track target.
                if (mode == "object" && headTracking) {
                    const int np = TrackFX_GetNumParams(mon, decoderFx);
                    const int yp = discoverParam(mon, decoderFx, np, "yaw");
                    const int pp = discoverParam(mon, decoderFx, np, "pitch");
                    const int rp = discoverParam(mon, decoderFx, np, "roll");
                    autom = Json{{"yaw", yp}, {"pitch", pp}, {"roll", rp}};
                    if (yp >= 0 || pp >= 0 || rp >= 0) {
                        htTarget.fx = decoderFx; htTarget.yawParam = yp; htTarget.pitchParam = pp;
                        htTarget.rollParam = rp; captureRanges(decoderFx, htTarget); haveHtParams = true;
                    } else {
                        warnings.push_back("head-tracking requested but the object binauraliser exposes no "
                            "yaw/pitch/roll params by name — use ambisonic mode for scene-rotated head-tracking.");
                    }
                }
            }

            // Start the extension-hosted OSC listener if we have params to drive.
            Json ht = Json{{"enabled", false}};
            if (headTracking) {
                if (haveHtParams) {
                    htTarget.track = mon;
                    htTarget.invertYaw = optBool(a, "invertYaw", false);
                    htTarget.invertPitch = optBool(a, "invertPitch", false);
                    htTarget.invertRoll = optBool(a, "invertRoll", false);
                    HeadTrackConfig cfg;
                    cfg.port = optInt(a, "oscPort", 9000);
                    if (cfg.port <= 0) cfg.port = 9000;
                    cfg.bind = optStr(a, "oscBind", "127.0.0.1");
                    std::string err;
                    if (HeadTrackBridge::instance().start(htTarget, cfg, err)) {
                        auto& b = HeadTrackBridge::instance();
                        ht = Json{{"enabled", true}, {"oscPort", b.boundPort()}, {"oscBind", cfg.bind},
                                  {"requestedPort", cfg.port}, {"portFellBack", b.portFellBack()},
                                  {"rotatorFx", (mode == "object" ? decoderFx : rotatorFx)},
                                  {"listenAddresses", oscAddresses},
                                  {"note", "Point your head-tracker's OSC output at " + cfg.bind + ":" +
                                           std::to_string(b.boundPort()) + " (the extension drives the "
                                           "rotator on the main thread, coalesced per UI tick). Or target "
                                           "the plug-in's own OSC port set in its UI. Call "
                                           "spatial.stop_head_tracking to release the port."}};
                        if (cfg.bind == "127.0.0.1")
                            warnings.push_back("OSC bound to loopback (127.0.0.1) — a LAN/phone tracker must "
                                "target this machine's IP; pass oscBind:\"0.0.0.0\" to accept it.");
                    } else {
                        ht = Json{{"enabled", false}, {"error", err}, {"listenAddresses", oscAddresses}};
                        warnings.push_back("head-tracking OSC listener did not start: " + err);
                    }
                } else {
                    ht = Json{{"enabled", false}, {"reason", "no yaw/pitch/roll params to drive"},
                              {"listenAddresses", oscAddresses}};
                }
            }

            warnings.push_back("Monitor bus '" + monName + "' outputs binaural on channels 1/2. Solo it "
                "(or disable the source's master send) to audition in isolation.");
            Undo_EndBlock2(kCur, "MCP: add binaural monitor", -1);

            return Json{{"ok", true}, {"mode", mode}, {"monitorTrack", monIdx}, {"monitorName", monName},
                {"busChannels", busCh}, {"decoder", decoderName}, {"decoderFx", decoderFx},
                {"rotator", rotatorName.empty() ? Json(nullptr) : Json(rotatorName)}, {"rotatorFx", rotatorFx},
                {"inputPins", inputPins}, {"hoaChannels", mode == "object" ? Json(nullptr) : Json(hoaN)},
                {"order", (mode == "ambisonic" && order > 0) ? Json(order) : Json(nullptr)},
                {"sends", sends}, {"automationParams", autom}, {"headTracking", ht}, {"warnings", warnings}};
#else
            // Host build: representative, schema-valid plan (channel math is SDK-independent; no bus,
            // no FX, and — importantly — NO socket is bound, so the protocol test stays hermetic).
            (void)wirePins;
            const std::string monName = optStr(a, "monitorName",
                mode == "object" ? "Binaural Monitor (objects)" : "Binaural Monitor");
            const int useOrder = hasOrder ? order : 3;
            const int hoaN = hoaChannels(useOrder);
            const int busCh = (mode == "object")
                ? clampChannels((int)objects.size() >= 2 ? (int)objects.size() : 2)
                : clampChannels(hoaN >= 2 ? hoaN : 2);
            Json sends = Json::array();
            if (createSend) {
                if (mode == "object")
                    for (size_t i = 0; i < objects.size(); ++i)
                        sends.push_back(Json{{"from", objects[i]}, {"destChannel", (int)i}, {"sendIndex", (int)i}});
                else
                    sends.push_back(Json{{"from", srcIdx}, {"channels", busCh}, {"sendIndex", 0}});
            }
            Json autom = Json::object();
            Json ht = Json{{"enabled", false}};
            std::string rotatorName;
            int rotatorFx = -1;
            if (headTracking) {
                if (mode == "object") { autom = Json{{"yaw", -1}, {"pitch", -1}, {"roll", -1}}; }
                else { rotatorName = "IEM SceneRotator"; rotatorFx = 0;
                       autom = Json{{"yaw", 2}, {"pitch", 3}, {"roll", 4}}; }
                int port = optInt(a, "oscPort", 9000); if (port <= 0) port = 9000;
                ht = Json{{"enabled", true}, {"oscPort", port}, {"oscBind", optStr(a, "oscBind", "127.0.0.1")},
                          {"requestedPort", port}, {"portFellBack", false},
                          {"rotatorFx", mode == "object" ? -1 : 0}, {"listenAddresses", oscAddresses},
                          {"note", "(host stub) extension-hosted OSC head-track bridge"}};
            }
            Json warns = Json::array();
            warns.push_back("(host stub) representative plan — no bus/FX created, no socket bound.");
            return Json{{"ok", true}, {"mode", mode}, {"monitorTrack", 0}, {"monitorName", monName},
                {"busChannels", busCh}, {"decoder", mode == "object" ? "SPARTA Binauraliser" : "IEM BinauralDecoder"},
                {"decoderFx", 0}, {"rotator", rotatorName.empty() ? Json(nullptr) : Json(rotatorName)},
                {"rotatorFx", rotatorFx}, {"inputPins", mode == "object" ? (int)objects.size() : hoaN},
                {"hoaChannels", mode == "object" ? Json(nullptr) : Json(hoaN)},
                {"order", mode == "ambisonic" ? Json(useOrder) : Json(nullptr)},
                {"sends", sends}, {"automationParams", autom}, {"headTracking", ht}, {"warnings", warns}};
#endif
        }});

    // ---- spatial.stop_head_tracking — release the live OSC head-track listener ----
    reg.add(Tool{
        "spatial.stop_head_tracking",
        "Stop the live head-tracking OSC listener started by spatial.add_binaural_monitor: closes the "
        "UDP port and stops driving the rotator. Safe to call when nothing is running (returns "
        "wasRunning=false). The binaural monitor bus itself is left in place.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"wasRunning":{"type":"boolean"},
            "packetsReceived":{"type":"integer"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true},
        Profile::Spatial,
        [](const Json&) -> Json {
            auto& b = HeadTrackBridge::instance();
            const bool was = b.running();
            const long long pkts = (long long)b.packetsReceived();
            b.stop();
            return Json{{"ok", true}, {"wasRunning", was}, {"packetsReceived", pkts}};
        }});

    // ---- spatial.render_deliverables — in-box masters + external-renderer orchestration ----
    reg.add(Tool{
        "spatial.render_deliverables",
        "Render immersive deliverables via REAPER's render engine (RENDER_* project settings + a "
        "no-dialog render action): in-box multichannel bed WAV, ambiX (ACN/SN3D) B-format WAV, and "
        "binaural downmix masters, each with REAPER-native integrated-LUFS + true-peak from "
        "RENDER_STATS (cross-checked against ffmpeg ebur128 in the verify harness). Also builds the "
        "bed+object send layout into the external Dolby Atmos Renderer / DAPS (REAPER cannot author "
        "ADM BWF natively). Targets: multichannel | ambix | binaural | atmos-send-layout. Point each "
        "render target at its bus via bedTrack/ambixTrack/binauralTrack (or sourceTrack; omit = "
        "master mix). dryRun reports the exact files RENDER_TARGETS would write without rendering; "
        "for the real render leg, bound it with boundsFlag=0 + startPos/endPos so it fits the "
        "tools/call window. The tool snapshots and restores all RENDER_* settings + track selection.",
        jparse(R"({"type":"object","properties":{
            "targets":{"type":"array","minItems":1,"items":{"type":"string",
                "enum":["multichannel","ambix","binaural","atmos-send-layout"]}},
            "outDir":{"type":"string"},
            "dryRun":{"type":"boolean","default":false},
            "format":{"type":"string"},
            "pattern":{"type":"string"},
            "sampleRate":{"type":"integer","minimum":0,"default":0},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "measureLoudness":{"type":"boolean","default":true},
            "sourceTrack":{"type":"integer","minimum":0},
            "bedTrack":{"type":"integer","minimum":0},
            "ambixTrack":{"type":"integer","minimum":0},
            "binauralTrack":{"type":"integer","minimum":0},
            "rendererTrack":{"type":"integer","minimum":0},
            "objectTracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "bedLayout":{"type":"string","enum":["5.1","7.1","7.1.4","9.1.6","22.2"]}},
            "required":["targets"]})"),
        jparse(R"({"type":"object","properties":{
            "rendered":{"type":"array","items":{"type":"object","properties":{
                "target":{"type":"string"},"source":{"type":"string"},"channels":{"type":"integer"},
                "path":{"type":"string"},"lufs":{"type":"number"},"truePeak":{"type":"number"},
                "samplePeak":{"type":"number"},"lra":{"type":"number"},
                "ambisonicOrder":{"type":"integer"},"dryRun":{"type":"boolean"},
                "plannedTargets":{"type":"array","items":{"type":"string"}},
                "loudnessEngine":{"type":"string"},"rawStats":{"type":"string"},
                "statsSummary":{"type":"string"}}}},
            "dryRun":{"type":"boolean"},"format":{"type":"string"},"formatSource":{"type":"string"},
            "atmos":{"type":"object"},"warnings":{"type":"array","items":{"type":"string"}}},
            "required":["rendered"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Render,
        [](const Json& a) -> Json {
            if (!a.contains("targets") || !a["targets"].is_array() || a["targets"].empty())
                throw std::runtime_error("targets must be a non-empty array");
            std::vector<std::string> targets;
            for (const auto& t : a["targets"]) if (t.is_string()) targets.push_back(t.get<std::string>());

            const bool dryRun = optBool(a, "dryRun", false);
            const std::string outDir = optStr(a, "outDir", "");
            const int sampleRate = optInt(a, "sampleRate", 0);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
            const int renderAction = optInt(a, "renderAction", 41824);
            const bool measureLoudness = optBool(a, "measureLoudness", true);

            // Per-target source bus: the target-specific key wins, else sourceTrack, else -1 = master.
            auto trackFor = [&](const std::string& tgt) -> int {
                const char* key = tgt == "multichannel" ? "bedTrack"
                                : tgt == "ambix"        ? "ambixTrack"
                                : tgt == "binaural"     ? "binauralTrack" : nullptr;
                if (key && a.contains(key) && a[key].is_number()) return a[key].get<int>();
                if (a.contains("sourceTrack") && a["sourceTrack"].is_number())
                    return a["sourceTrack"].get<int>();
                return -1;
            };

            std::vector<std::string> renderTs;
            bool wantAtmos = false;
            Json warnings = Json::array();
            for (const auto& t : targets) {
                if (t == "atmos-send-layout") wantAtmos = true;
                else if (isRenderTarget(t)) renderTs.push_back(t);
                else warnings.push_back("unknown target: " + t);
            }

            Json rendered = Json::array();
            Json atmos = Json(nullptr);
            std::string useFmt, fmtSource;

#ifdef REAPER_MCP_HAVE_SDK
            if (wantAtmos) atmos = buildAtmosLayoutSDK(a, dryRun, warnings);

            if (!renderTs.empty()) {
                const RenderSnapshot snap = snapshotRender();
                if (a.contains("format") && a["format"].is_string()) {
                    useFmt = sinkFourCC(a["format"].get<std::string>()); fmtSource = "arg";
                } else if (!snap.format.empty()) {
                    useFmt = snap.format; fmtSource = "project";  // respect the engineer's configured format
                } else {
                    useFmt = "evaw"; fmtSource = "default-wav";
                }
                const std::string outDirUse = outDir.empty() ? snap.file : outDir;
                if (!dryRun && outDirUse.empty())
                    warnings.push_back("no outDir and project RENDER_FILE is empty — render may fail.");

                for (const auto& tgt : renderTs) {
                    const int srcTrack = trackFor(tgt);
                    int busW = 2;
                    if (srcTrack >= 0) busW = trackChannels(requireTrack(srcTrack));
                    else { MediaTrack* m = GetMasterTrack(kCur);
                           busW = m ? (int)GetMediaTrackInfo_Value(m, "I_NCHAN") : 2; }

                    const int renderCh = (tgt == "binaural") ? 2 : (busW >= 2 ? busW : 2);

                    Json item{{"target", tgt}, {"channels", renderCh},
                              {"source", srcTrack >= 0 ? ("track " + std::to_string(srcTrack)) : "master"}};

                    if (tgt == "ambix") {
                        int ord = -1;
                        for (int o = 1; o <= 7; ++o) {
                            const int c = (o + 1) * (o + 1);
                            if (c == renderCh || ((c & 1) && c + 1 == renderCh)) { ord = o; break; }
                        }
                        if (ord < 0)
                            warnings.push_back("ambix: bus width " + std::to_string(renderCh) +
                                " is not an HOA channel count ((order+1)^2, even-padded) — is this a "
                                "B-format bus?");
                        else item["ambisonicOrder"] = ord;
                    }

                    // Configure the render for this target (restored wholesale afterwards).
                    setProjStr("RENDER_FILE", outDirUse);
                    setProjStr("RENDER_PATTERN", optStr(a, "pattern", defaultPattern(tgt)));
                    setProjStr("RENDER_FORMAT", useFmt);
                    setProjNum("RENDER_SRATE", sampleRate);
                    setProjNum("RENDER_CHANNELS", renderCh);
                    setProjNum("RENDER_ADDTOPROJ", 0);
                    // Loudness: a plain render reports only FILE+LENGTH; REAPER computes LUFS-I +
                    // true-peak only during a normalization ANALYSIS pass. So enable normalization
                    // but guarantee NO gain is ever applied — "&256 only if too loud" against an
                    // unreachable 0 LUFS ceiling (target 1.0). Real content is always < 0 LUFS, so
                    // nothing is normalized: the master stays the true mix, RENDER_STATS still gets
                    // the measured loudness. measureLoudness=false → single-pass, no stats.
                    if (measureLoudness) {
                        setProjNum("RENDER_NORMALIZE", (double)(1 | 256));  // enable (LUFS-I) + only-if-too-loud
                        setProjNum("RENDER_NORMALIZE_TARGET", 1.0);          // 0 dB / 0 LUFS — unreachable
                    } else {
                        setProjNum("RENDER_NORMALIZE", 0);
                    }
                    setProjNum("RENDER_BOUNDSFLAG", boundsFlag);
                    if (a.contains("startPos") && a["startPos"].is_number())
                        setProjNum("RENDER_STARTPOS", a["startPos"].get<double>());
                    if (a.contains("endPos") && a["endPos"].is_number())
                        setProjNum("RENDER_ENDPOS", a["endPos"].get<double>());
                    if (srcTrack >= 0) {
                        applySelection({srcTrack});
                        int settings = 2;                       // stems only (the selected bus)
                        if (renderCh > 2) settings |= 4;        // multichannel track -> one multichannel file
                        setProjNum("RENDER_SETTINGS", (double)settings);
                    } else {
                        setProjNum("RENDER_SETTINGS", 0);       // master mix
                    }

                    if (dryRun) {
                        item["dryRun"] = true;
                        item["plannedTargets"] = splitSemis(getProjStrBig("RENDER_TARGETS"));
                    } else {
                        Main_OnCommand(renderAction, 0);        // no-dialog render, most-recent settings
                        item["dryRun"] = false;
                        item["plannedTargets"] = splitSemis(getProjStrBig("RENDER_TARGETS"));
                        const std::string statsStr = getProjStrBig("RENDER_STATS");
                        const Json files = parseRenderStats(statsStr);
                        if (!files.empty()) {
                            const Json& f0 = files[0];
                            if (f0.contains("file"))           item["path"] = f0["file"];
                            if (f0.contains("lufsIntegrated")) item["lufs"] = f0["lufsIntegrated"];
                            if (f0.contains("truePeak"))       item["truePeak"] = f0["truePeak"];
                            if (f0.contains("samplePeak"))     item["samplePeak"] = f0["samplePeak"];
                            if (f0.contains("loudnessRange"))  item["lra"] = f0["loudnessRange"];
                        }
                        item["files"] = files;
                        item["rawStats"] = statsStr;
                        item["statsSummary"] = getProjStrBig("RENDER_STATS_SUMMARY");
                        item["loudnessEngine"] =
                            "reaper-native (RENDER_STATS); cross-check ffmpeg ebur128 in verify";
                    }
                    rendered.push_back(std::move(item));
                }
                restoreRender(snap);
            }
#else
            // Host build: representative, schema-valid plan (channel math is SDK-independent).
            if (a.contains("format") && a["format"].is_string()) {
                useFmt = sinkFourCC(a["format"].get<std::string>()); fmtSource = "arg";
            } else { useFmt = "evaw"; fmtSource = "default-wav"; }
            for (const auto& tgt : renderTs) {
                const int ch = tgt == "binaural" ? 2 : (tgt == "ambix" ? 16 : 12);
                Json item{{"target", tgt}, {"channels", ch}, {"source", "(host stub)"},
                          {"dryRun", dryRun},
                          {"loudnessEngine", "reaper-native (RENDER_STATS)"}};
                if (tgt == "ambix") item["ambisonicOrder"] = 3;
                rendered.push_back(std::move(item));
            }
            if (wantAtmos) atmos = buildAtmosLayoutHost(a);
            (void)outDir; (void)sampleRate; (void)boundsFlag; (void)renderAction; (void)trackFor;
            (void)measureLoudness;
#endif
            Json out{{"rendered", rendered}, {"dryRun", dryRun},
                     {"format", useFmt}, {"formatSource", fmtSource}};
            if (!atmos.is_null()) out["atmos"] = atmos;
            if (!warnings.empty()) out["warnings"] = warnings;
            return out;
        }});

    // =============================================================================================
    // OUTCOME-LEVEL SPATIAL COMPOSITE VERBS (spatialize_stems, stereo_to_ambisonic,
    //     setup_immersive_session). Thin, deterministic, single-undo compositions over the primitives.
    // =============================================================================================

    // ---- spatial.spatialize_stems — the flagship: stems -> bed or ambisonic scene, by intent ----
    reg.add(Tool{
        "spatial.spatialize_stems",
        "Spatialize source stems into a channel bed (target.layout) or an ambisonic scene "
        "(target.ambisonicOrder), placing each by a semantic descriptor (front-center, wide, overhead, "
        "side, rear, lfe, ...) or explicit {az,el}. Composes detect -> build_bed/set_channels -> "
        "per-source encode or surround-pan -> sends, in one undo block. dryRun returns the plan+diff. "
        "Idempotent: an existing bus of the same name is reused, not duplicated.",
        jparse(R"({"type":"object","properties":{
            "sources":{"type":"array","items":{"type":["integer","string"]},"minItems":1},
            "target":{"type":"object"},
            "placements":{"type":"array","items":{"type":["string","object"]}},
            "busName":{"type":"string"},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ATK","ambiX"],"default":"auto"},
            "monitor":{"type":"boolean","default":false},
            "dryRun":{"type":"boolean","default":false}},
            "required":["sources","target"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"target":{"type":"string"},
            "bus":{"type":"integer"},"busName":{"type":"string"},"reusedBus":{"type":"boolean"},
            "placements":{"type":"array"},"monitor":{"type":["object","null"]},
            "stepCount":{"type":"integer"},"diff":{"type":"string"},"warnings":{"type":"array"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "dryRun":{"type":"boolean"},"plan":{"type":"array"}}})"),
        ToolAnnotations{false, false, true}, Profile::Spatial,
        [](const Json& a) -> Json {
            using namespace spatial_verbs;
            if (!a.contains("sources") || !a["sources"].is_array() || a["sources"].empty())
                return makeError("no_sources", "sources[] is required and must be non-empty",
                                 "pass track indices or names, e.g. sources:[3,4,\"Vocal\"]");
            Target tgt;
            { Json e = parseTarget(a.value("target", Json::object()), tgt); if (!e.is_null()) return e; }
            const bool dryRun = optBool(a, "dryRun", false);
            const bool monitor = optBool(a, "monitor", false);
            const std::string suite = optStr(a, "suite", "auto");
            const std::string busName = optStr(a, "busName",
                tgt.ambisonic ? (std::to_string(tgt.order) + "-order Scene") : (tgt.layout + " Bed"));

            // Resolve placements (parallel to sources; a missing entry => front-center).
            const Json placeArr = (a.contains("placements") && a["placements"].is_array())
                                      ? a["placements"] : Json::array();
            std::vector<ResolvedPlacement> places;
            for (size_t i = 0; i < a["sources"].size(); ++i) {
                Json spec = (i < placeArr.size()) ? placeArr[i] : Json("front-center");
                ResolvedPlacement rp;
                Json e = resolvePlacementSpec(spec, 0.0, rp);
                if (!e.is_null()) return e;
                places.push_back(rp);
            }

#ifdef REAPER_MCP_HAVE_SDK
            std::vector<SourceRef> srcs;
            for (const auto& e : a["sources"]) srcs.push_back(resolveSourceEntry(e));
            {
                std::string missing;
                for (const auto& s : srcs) if (s.index < 0) { if (!missing.empty()) missing += ", "; missing += s.name; }
                if (!missing.empty())
                    return makeError("source_not_found", "could not resolve source track(s): " + missing,
                                     "pass a valid track index, or a name matching a track's P_NAME");
            }
            if (tgt.ambisonic && !ambisonicEncoderAvailable(suite))
                return makeError("no_ambisonic_suite", "no ambisonic encoder found (suite=" + suite + ")",
                                 "install IEM/SPARTA/ATK or pass suite; run spatial.detect_spatial_suites");

            const int existingBus = findTrackByName(busName);
            const bool reuse = existingBus >= 0;
            Plan plan = planSpatialize(tgt, srcs, places, busName, monitor, reuse);
            if (dryRun) {
                Json out = plan.dryRunResult();
                out["ok"] = true; out["target"] = targetTag(tgt); out["busName"] = busName;
                out["reusedBus"] = reuse; out["bus"] = existingBus;
                return out;
            }

            UndoTransaction txn("MCP: spatialize stems -> " + targetTag(tgt));
            Json warnings = Json::array();
            int busIdx = existingBus;
            if (!reuse) {
                busIdx = insertNamedTrack(busName, tgt.channels, false);
                if (busIdx < 0) throw CompositeError("bus_insert_failed", "could not insert the bus track");
                txn.recordExecuted(tgt.ambisonic ? "create_ambisonic_bus" : "build_bed", busName, targetTag(tgt));
            }

            Json placeReport = Json::array();
            for (size_t i = 0; i < srcs.size(); ++i) {
                const ResolvedPlacement& p = places[i];
                Json r;
                if (tgt.ambisonic) {
                    r = encodeSourceToScene(srcs[i].index, tgt, p, suite);
                    if (r.is_null()) { txn.markDirty();
                        throw CompositeError("encode_failed", "encoder insert failed on '" + srcs[i].name + "'",
                                             "run spatial.detect_spatial_suites"); }
                    MediaTrack* st = GetTrack(kCur, srcs[i].index);
                    MediaTrack* bt = GetTrack(kCur, busIdx);
                    if (st && bt) {
                        const SendChanEnc enc = encodeSend(0, clampChannels(tgt.channels), 0, false);
                        int si = CreateTrackSend(st, bt);
                        if (si >= 0) {
                            SetTrackSendInfo_Value(st, 0, si, "I_SRCCHAN", (double)enc.src);
                            SetTrackSendInfo_Value(st, 0, si, "I_DSTCHAN", (double)enc.dst);
                            r["sendIndex"] = si;
                        }
                    }
                } else {
                    r = panSourceToBed(srcs[i].index, busIdx, tgt.channels, p);
                    if (!tgt.ambisonic && !p.lfe && r.is_object() && r.value("fxIndex", 0) >= 0 &&
                        r.contains("positionSet") && r["positionSet"].empty())
                        warnings.push_back("ReaSurroundPan position params for '" + srcs[i].name +
                            "' not matched by name; call spatial.get_surround_state + set_source_position to tune.");
                }
                txn.recordExecuted(tgt.ambisonic ? "encode+send" : "pan+send", srcTag(srcs[i]), placeTag(p));
                if (!r.is_null()) placeReport.push_back(r);
            }

            Json monitorRep = Json(nullptr);
            if (monitor) {
                if (tgt.ambisonic) {
                    monitorRep = buildBinauralMonitor(busIdx, true, tgt.channels, {}, suite);
                } else {
                    int monIdx = insertNamedTrack("Bed Monitor", 2, false);
                    if (monIdx >= 0) {
                        MediaTrack* bt = GetTrack(kCur, busIdx);
                        MediaTrack* mt = GetTrack(kCur, monIdx);
                        if (bt && mt) {
                            const SendChanEnc enc = encodeSend(0, 2, 0, false);
                            int si = CreateTrackSend(bt, mt);
                            if (si >= 0) {
                                SetTrackSendInfo_Value(bt, 0, si, "I_SRCCHAN", (double)enc.src);
                                SetTrackSendInfo_Value(bt, 0, si, "I_DSTCHAN", (double)enc.dst);
                            }
                        }
                        monitorRep = Json{{"monitorTrack", monIdx}, {"mode", "stereo-reference"},
                            {"note", "stereo fold-down reference; use spatial.add_binaural_monitor for "
                                     "head-tracked binaural of an ambisonic scene"}};
                    }
                }
                txn.recordExecuted("add_binaural_monitor", "monitor bus", tgt.ambisonic ? "ambisonic" : "stereo");
            }
            txn.commit();
            return Json{{"ok", true}, {"target", targetTag(tgt)}, {"bus", busIdx}, {"busName", busName},
                        {"reusedBus", reuse}, {"placements", placeReport}, {"monitor", monitorRep},
                        {"stepCount", (int)plan.size()}, {"diff", plan.diff()}, {"warnings", warnings}};
#else
            (void)suite; (void)monitor; (void)dryRun;
            std::vector<SourceRef> srcs;
            for (size_t i = 0; i < a["sources"].size(); ++i) {
                const Json& e = a["sources"][i];
                SourceRef s;
                s.index = e.is_number() ? e.get<int>() : (int)i;
                s.name = e.is_string() ? e.get<std::string>() : ("track " + std::to_string(s.index));
                s.channels = 2;
                srcs.push_back(s);
            }
            Plan plan = planSpatialize(tgt, srcs, places, busName, monitor, false);
            Json out = plan.dryRunResult();
            out["ok"] = true; out["target"] = targetTag(tgt); out["busName"] = busName;
            out["reusedBus"] = false; out["bus"] = 0;
            return out;
#endif
        }});

    // ---- spatial.stereo_to_ambisonic — mono/stereo -> FOA/HOA sketch (an upmix, not a capture) ----
    reg.add(Tool{
        "spatial.stereo_to_ambisonic",
        "Upmix a mono/stereo source to a first/higher-order ambisonic SKETCH: set (order+1)^2 channels, "
        "insert an encoder, splay L/R at +/-spreadDeg (default 30), and report order + normalization. "
        "Explicitly a sketch (a decorrelated upmix, not a true soundfield capture). dryRun returns the plan.",
        jparse(R"({"type":"object","properties":{"source":{"type":["integer","string"]},
            "order":{"type":"integer","minimum":1,"maximum":7,"default":1},
            "normalization":{"type":"string","enum":["SN3D","N3D","FuMa"],"default":"SN3D"},
            "spreadDeg":{"type":"number","default":30},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ATK","ambiX"],"default":"auto"},
            "monitor":{"type":"boolean","default":false},"dryRun":{"type":"boolean","default":false}},
            "required":["source"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "order":{"type":"integer"},"channels":{"type":"integer"},"normalization":{"type":"string"},
            "sketch":{"type":"boolean"},"encoder":{"type":"string"},"positions":{"type":"array"},
            "orderParam":{"type":["object","null"]},"normalizationParam":{"type":["object","null"]},
            "monitor":{"type":["object","null"]},"warnings":{"type":"array"},"diff":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"},
            "dryRun":{"type":"boolean"},"plan":{"type":"array"}}})"),
        ToolAnnotations{false, false, true}, Profile::Spatial,
        [](const Json& a) -> Json {
            using namespace spatial_verbs;
            int order = optInt(a, "order", 1);
            if (order < 1) order = 1;
            if (order > 7) order = 7;
            const int channels = evenClamp(hoaChannelsFor(order));
            const std::string norm = optStr(a, "normalization", "SN3D");
            const double spread = optNum(a, "spreadDeg", 30.0);
            const std::string suite = optStr(a, "suite", "auto");
            const bool monitor = optBool(a, "monitor", false);
            const bool dryRun = optBool(a, "dryRun", false);
            if (!a.contains("source")) return makeError("no_source", "source is required",
                                                        "pass a track index or a track name");
#ifdef REAPER_MCP_HAVE_SDK
            SourceRef s = resolveSourceEntry(a["source"]);
            if (s.index < 0) return makeError("source_not_found", "could not resolve source track",
                                              "pass a valid track index or a matching track name");
            Plan plan = planStereoToAmbi(s, order, channels, norm, spread, monitor);
            if (dryRun) {
                Json out = plan.dryRunResult();
                out["ok"] = true; out["track"] = s.index; out["order"] = order;
                out["channels"] = channels; out["normalization"] = norm; out["sketch"] = true;
                return out;
            }
            if (!ambisonicEncoderAvailable(suite))
                return makeError("no_ambisonic_suite", "no ambisonic encoder found (suite=" + suite + ")",
                                 "install IEM/SPARTA/ATK or pass suite; run spatial.detect_spatial_suites");

            UndoTransaction txn("MCP: stereo -> " + std::to_string(order) + "-order ambisonic sketch");
            MediaTrack* t = GetTrack(kCur, s.index);
            SetMediaTrackInfo_Value(t, "I_NCHAN", (double)channels);
            txn.recordExecuted("set_channels", srcTag(s), std::to_string(channels) + " ch");
            std::vector<std::string> cands = ambiCandidates("encoder", suite);
            std::string used;
            Json noargs = Json::object();
            int fx = resolveOrAddByCandidates(t, noargs, cands, used);
            if (fx < 0) { txn.markDirty();
                throw CompositeError("encode_failed", "encoder insert failed", "run spatial.detect_spatial_suites"); }
            const int np = TrackFX_GetNumParams(t, fx);

            // Sketch splay: place input 1 (L) at +spread, input 2 (R) at -spread on a multi-input
            // encoder; if the encoder is single-input, widen its width param instead.
            Json positions = Json::array();
            Json l1 = setNamedValueParam(t, fx, np, "azimuth", +spread, 1);
            Json l2 = setNamedValueParam(t, fx, np, "azimuth", -spread, 2);
            if (!l1.is_null()) { l1["axis"] = "azimuth"; l1["input"] = 1; positions.push_back(l1); }
            if (!l2.is_null()) { l2["axis"] = "azimuth"; l2["input"] = 2; positions.push_back(l2); }
            if (l1.is_null() && l2.is_null()) {
                Json w = setNamedValueParam(t, fx, np, "width", spread * 2.0, 0);
                if (!w.is_null()) { w["axis"] = "width"; positions.push_back(w); }
            }
            pinIdentityWire(t, fx, hoaChannels(order), 1);
            Json orderParam = reportNamedParam(t, fx, np, "order");
            Json normParam = reportNamedParam(t, fx, np, "normali");
            txn.recordExecuted("insert_encoder", srcTag(s), used);

            std::vector<std::string> warnings;
            warnings.push_back("This is an upmix SKETCH (a decorrelated stereo->ambisonic splay), not a "
                               "true soundfield capture.");
            if (norm != "SN3D")
                warnings.push_back("Normalization '" + norm + "' requested; set the reported "
                                   "normalizationParam by hand (its enum index is plug-in-specific).");
            Json monitorRep = Json(nullptr);
            if (monitor) {
                monitorRep = buildBinauralMonitor(s.index, true, channels, {}, suite);
                txn.recordExecuted("add_binaural_monitor", "monitor bus", "ambisonic");
            }
            txn.commit();
            return Json{{"ok", true}, {"track", s.index}, {"order", order}, {"channels", channels},
                        {"normalization", norm}, {"sketch", true}, {"encoder", used},
                        {"orderParam", orderParam}, {"normalizationParam", normParam},
                        {"positions", positions}, {"monitor", monitorRep}, {"warnings", warnings},
                        {"diff", plan.diff()}};
#else
            (void)suite; (void)monitor; (void)dryRun;
            const Json& e = a["source"];
            SourceRef s;
            s.index = e.is_number() ? e.get<int>() : 0;
            s.name = e.is_string() ? e.get<std::string>() : ("track " + std::to_string(s.index));
            s.channels = 2;
            Plan plan = planStereoToAmbi(s, order, channels, norm, spread, monitor);
            Json out = plan.dryRunResult();
            out["ok"] = true; out["track"] = s.index; out["order"] = order; out["channels"] = channels;
            out["normalization"] = norm; out["sketch"] = true; out["encoder"] = "IEM StereoEncoder";
            return out;
#endif
        }});

    // ---- spatial.setup_immersive_session — bed + objects + monitor + Dolby renderer sends ----
    reg.add(Tool{
        "spatial.setup_immersive_session",
        "One-call immersive session: build a bed (5.1|7.1|7.1.4|9.1.6|22.2), add N tagged mono object "
        "tracks, an optional binaural monitor, and the external Dolby Renderer / DAPS send layout "
        "(REAPER cannot author ADM BWF — this wires the topology a certified renderer consumes). "
        "DESTRUCTIVE on a non-empty project: an elicitation-capable client is asked to confirm via a "
        "real MCP elicitation round-trip; clients without elicitation pass confirm:true (fallback). "
        "dryRun returns the plan.",
        jparse(R"({"type":"object","properties":{
            "bed":{"type":"string","enum":["5.1","7.1","7.1.4","9.1.6","22.2"],"default":"7.1.4"},
            "objectCount":{"type":"integer","minimum":0,"maximum":128,"default":0},
            "monitor":{"type":"boolean","default":false},
            "rendererSends":{"type":"boolean","default":false},
            "suite":{"type":"string","enum":["auto","IEM","SPARTA","ATK","ambiX"],"default":"auto"},
            "confirm":{"type":"boolean","default":false},"dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"bedBus":{"type":"integer"},
            "bedLayout":{"type":"string"},"objectTracks":{"type":"array"},"monitor":{"type":["object","null"]},
            "rendererLayout":{"type":["object","null"]},"stepCount":{"type":"integer"},"diff":{"type":"string"},
            "warnings":{"type":"array"},"error":{"type":"string"},"detail":{"type":"string"},
            "remediation":{"type":"string"},"dryRun":{"type":"boolean"},"plan":{"type":"array"}}})"),
        ToolAnnotations{false, true, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            using namespace spatial_verbs;
            const std::string bed = optStr(a, "bed", "7.1.4");
            const int bedCh = bedChannelsForLayout(bed);
            if (bedCh < 0) return makeError("unknown_layout", "unknown bed layout: " + bed,
                                            std::string("known: ") + knownLayouts());
            const int objectCount = optInt(a, "objectCount", 0);
            const bool monitor = optBool(a, "monitor", false);
            const bool renderer = optBool(a, "rendererSends", false);
            const bool confirm = optBool(a, "confirm", false);
            const bool dryRun = optBool(a, "dryRun", false);
            const std::string suite = optStr(a, "suite", "auto");

            Plan plan = planSetupImmersive(bed, bedCh, objectCount, monitor, renderer);
            if (dryRun) { Json out = plan.dryRunResult(); out["ok"] = true; out["bedLayout"] = bed; return out; }
#ifdef REAPER_MCP_HAVE_SDK
            if (CountTracks(kCur) > 0 && !confirm) {
                // If the client advertised elicitation, ask via a real MCP
                // elicitation/create round-trip. requireElicitation() THROWS here (before any edit);
                // the transport streams the prompt, awaits the human off the main thread, and on accept
                // re-invokes this call with confirm:true (so control does not return here). A client
                // WITHOUT elicitation falls through to the confirm:true stopgap below.
                requireElicitation(
                    "setup_immersive_session will restructure routing on a project that already has " +
                    std::to_string(CountTracks(kCur)) + " track(s). Proceed?");
                return makeError("confirmation_required",
                    "setup_immersive_session restructures routing on a project that already has " +
                    std::to_string(CountTracks(kCur)) + " track(s)",
                    "re-run with confirm:true (or dryRun:true to preview), or accept the elicitation prompt");
            }

            UndoTransaction txn("MCP: setup immersive session (" + bed + ")");
            const int bedIdx = insertNamedTrack(bed + " Bed", bedCh, false);
            if (bedIdx < 0) throw CompositeError("bed_insert_failed", "could not insert the bed bus");
            txn.recordExecuted("build_bed", bed + " Bed", std::to_string(bedCh) + " ch");

            std::vector<int> objTracks;
            for (int i = 0; i < objectCount; ++i) {
                int oi = insertNamedTrack("Object " + std::to_string(i + 1), 1, false);
                if (oi >= 0) objTracks.push_back(oi);
            }
            if (objectCount > 0)
                txn.recordExecuted("add_object_tracks", std::to_string(objectCount) + " objects", "mono");

            Json monitorRep = Json(nullptr);
            if (monitor) {
                monitorRep = buildBinauralMonitor(bedIdx, false, bedCh, objTracks, suite);
                txn.recordExecuted("add_binaural_monitor", "monitor bus", "object");
            }

            Json rendererRep = Json(nullptr);
            Json warnings = Json::array();
            if (renderer) {
                int rIdx = insertNamedTrack("Dolby Renderer Bus", clampChannels(bedCh + objectCount), false);
                Json layoutArgs = Json{{"bedTrack", bedIdx}, {"rendererTrack", rIdx},
                                       {"bedLayout", bed}, {"objectTracks", objTracks}};
                Json w = Json::array();
                rendererRep = buildAtmosLayoutSDK(layoutArgs, false, w);
                for (const auto& x : w) warnings.push_back(x);
                txn.recordExecuted("atmos_send_layout", "Dolby Renderer Bus", "bed + objects");
            }
            txn.commit();
            return Json{{"ok", true}, {"bedBus", bedIdx}, {"bedLayout", bed}, {"objectTracks", objTracks},
                        {"monitor", monitorRep}, {"rendererLayout", rendererRep},
                        {"stepCount", (int)plan.size()}, {"diff", plan.diff()}, {"warnings", warnings}};
#else
            (void)confirm; (void)monitor; (void)renderer; (void)suite;
            Json out = plan.dryRunResult();
            out["ok"] = true; out["bedBus"] = 0; out["bedLayout"] = bed;
            Json objs = Json::array();
            for (int i = 0; i < objectCount; ++i) objs.push_back(i + 1);
            out["objectTracks"] = objs;
            return out;
#endif
        }});

    // ============================================================================================
    // Spatial / ambisonic DEPTH tools. Six tools, all in this existing register fn (no new TU /
    // wiring). convert_order is pure channel routing; convert_format + mirror_scene drive the two
    // bundled zero-dependency JSFX (Effects/MCP/); beamform + set_distance orchestrate SPARTA
    // Beamformer / IEM RoomEncoder via the same resolve+report substrate as encode/rotate/decode;
    // get_scene_info is read-only. NO new REAPER WANT (all APIs already imported by the earlier verbs).
    // ============================================================================================

    // ---- spatial.convert_order — change HOA order by truncation (down) / zero-pad (up) ----
    reg.add(Tool{
        "spatial.convert_order",
        "Change a HOA track's ambisonic order by resizing its ACN channel bus. DOWN-order truncates the "
        "higher-degree ACN channels — an exact operation: the retained lower orders are a valid "
        "lower-order scene. UP-order zero-pads with silent higher-degree channels — no spatial detail is "
        "invented (honest zero-pad; re-encode sources for real HOA gain). Pure channel-count + identity "
        "routing, no FX, no matrix. fromOrder is inferred from the current channel count when omitted.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "toOrder":{"type":"integer","minimum":1,"maximum":7},
            "fromOrder":{"type":"integer","minimum":1,"maximum":7},
            "normalization":{"type":"string","enum":["SN3D","N3D","FuMa"],"default":"SN3D"}},
            "required":["track","toOrder"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "fromOrder":{"type":"integer"},"toOrder":{"type":"integer"},
            "previousChannels":{"type":"integer"},"channels":{"type":"integer"},
            "hoaChannels":{"type":"integer"},"padded":{"type":"boolean"},
            "direction":{"type":"string"},"warnings":{"type":"array"}},
            "required":["ok","toOrder","hoaChannels","channels"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int toOrder = reqInt(a, "toOrder");
            const int toHoa = hoaChannels(toOrder);
            const int toChan = clampChannels(toHoa);          // even-padded track width
            std::vector<std::string> warnings;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int prev = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            int fromOrder;
            if (a.contains("fromOrder") && a["fromOrder"].is_number())
                fromOrder = a["fromOrder"].get<int>();
            else { fromOrder = 0; for (int o = 7; o >= 1; --o) if (hoaChannels(o) <= prev) { fromOrder = o; break; } }
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_NCHAN", (double)toChan);
            Undo_EndBlock2(kCur, "MCP: convert ambisonic order", -1);
            const char* dir = toOrder < fromOrder ? "down (truncate)"
                            : toOrder > fromOrder ? "up (zero-pad)" : "same";
            if (toOrder > fromOrder)
                warnings.push_back("up-order zero-pads: the new higher-degree ACN channels are silent — no "
                                   "spatial detail is synthesized. Re-encode sources for real HOA detail.");
            if (fromOrder == 0)
                warnings.push_back("could not infer fromOrder from " + std::to_string(prev) +
                                   " channels (not a HOA width); treated as a plain resize to order " +
                                   std::to_string(toOrder) + ".");
            return Json{{"ok", true}, {"track", idx}, {"fromOrder", fromOrder}, {"toOrder", toOrder},
                        {"previousChannels", prev}, {"channels", toChan}, {"hoaChannels", toHoa},
                        {"padded", toChan > toHoa}, {"direction", dir}, {"warnings", warnings}};
#else
            const int fromOrder = (a.contains("fromOrder") && a["fromOrder"].is_number())
                                  ? a["fromOrder"].get<int>() : toOrder;
            const char* dir = toOrder < fromOrder ? "down (truncate)"
                            : toOrder > fromOrder ? "up (zero-pad)" : "same";
            return Json{{"ok", true}, {"track", idx}, {"fromOrder", fromOrder}, {"toOrder", toOrder},
                        {"previousChannels", 0}, {"channels", toChan}, {"hoaChannels", toHoa},
                        {"padded", toChan > toHoa}, {"direction", dir}, {"warnings", warnings}};
#endif
        }});

    // ---- spatial.convert_format — ambisonic normalization / channel-format conversion (bundled JSFX) --
    reg.add(Tool{
        "spatial.convert_format",
        "Convert a HOA track between ambisonic normalizations / formats via the bundled zero-dependency "
        "JSFX 'MCP Ambisonic Format Converter'. SN3D<->N3D is an exact per-degree gain for ANY order; "
        "ambiX(ACN/SN3D)<->FuMa is first-order / B-format ONLY (reorder + the W 1/sqrt2 scaling) — "
        "higher-order FuMa has no single agreed convention and is refused (keep HOA in SN3D/N3D). ambiX "
        "is treated as ACN/SN3D. Same from/to is a no-op (no FX inserted). N3D<->FuMa: convert via SN3D.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "from":{"type":"string","enum":["SN3D","N3D","FuMa","ambiX"]},
            "to":{"type":"string","enum":["SN3D","N3D","FuMa","ambiX"]},
            "fx":{"type":"integer","minimum":0},"wirePins":{"type":"boolean","default":true}},
            "required":["track","from","to"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "converter":{"type":"string"},"from":{"type":"string"},"to":{"type":"string"},
            "mode":{"type":"integer"},"noop":{"type":"boolean"},"modeParam":{"type":["integer","null"]},
            "pinMap":{"type":"array"},"note":{"type":"string"},"warnings":{"type":"array"}},
            "required":["ok","from","to"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string from = reqStr(a, "from");
            const std::string to = reqStr(a, "to");
            const bool wirePins = optBool(a, "wirePins", true);
            auto canon = [](const std::string& s) { return s == "ambiX" ? std::string("SN3D") : s; };
            const std::string cf = canon(from), ct = canon(to);
            std::vector<std::string> warnings;
            int mode = -1; bool foa = false;
            if (cf == ct) mode = -2;                                   // no-op
            else if (cf == "SN3D" && ct == "N3D") mode = 0;
            else if (cf == "N3D" && ct == "SN3D") mode = 1;
            else if (cf == "SN3D" && ct == "FuMa") { mode = 2; foa = true; }
            else if (cf == "FuMa" && ct == "SN3D") { mode = 3; foa = true; }
            if (mode == -1)
                throw std::runtime_error("unsupported conversion " + from + " -> " + to +
                    " (FuMa interchanges only with SN3D/ambiX; for N3D<->FuMa convert via SN3D).");
            if (mode == -2)
                return Json{{"ok", true}, {"fxIndex", -1}, {"converter", ""}, {"from", from}, {"to", to},
                            {"mode", -1}, {"noop", true}, {"modeParam", nullptr}, {"pinMap", Json::array()},
                            {"note", "from == to (ambiX == SN3D); no conversion needed."},
                            {"warnings", warnings}};
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int nch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            if (foa && nch > 4)
                throw std::runtime_error("FuMa conversion is first-order / B-format only, but the track has "
                    + std::to_string(nch) + " channels (HOA). Higher-order FuMa has no single agreed "
                    "convention — keep HOA in SN3D/N3D.");
            Undo_BeginBlock2(kCur);
            // File aliases resolve the ADD; the trailing display name lets the find-existing scan
            // reuse an instance (whose on-track name is the @desc "JS: MCP Ambisonic Format Converter").
            std::vector<std::string> cands{"JS: MCP/mcp_ambi_convert", "MCP/mcp_ambi_convert",
                                           "mcp_ambi_convert", "MCP Ambisonic Format Converter"};
            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: convert ambisonic format", -1);
                throw std::runtime_error("bundled 'MCP Ambisonic Format Converter' JSFX not found — install "
                    "it via 'cmake --install build' (ships to Effects/MCP/mcp_ambi_convert.jsfx).");
            }
            const int np = TrackFX_GetNumParams(t, fx);
            int mp = discoverParam(t, fx, np, "conversion");
            if (mp < 0) mp = 0;
            TrackFX_SetParam(t, fx, mp, (double)mode);
            Json pinMap = Json::array();
            if (wirePins) { pinIdentityWire(t, fx, nch, 0); pinMap = pinIdentityWire(t, fx, nch, 1); }
            Undo_EndBlock2(kCur, "MCP: convert ambisonic format", -1);
            return Json{{"ok", true}, {"fxIndex", fx}, {"converter", fxNameStr(t, fx)}, {"from", from},
                        {"to", to}, {"mode", mode}, {"noop", false}, {"modeParam", mp},
                        {"pinMap", std::move(pinMap)},
                        {"note", "SN3D<->N3D exact any order; FuMa is FOA-only."}, {"warnings", warnings}};
#else
            (void)wirePins; (void)foa; (void)idx;
            return Json{{"ok", true}, {"fxIndex", 0}, {"converter", "MCP Ambisonic Format Converter"},
                        {"from", from}, {"to", to}, {"mode", mode}, {"noop", false}, {"modeParam", 0},
                        {"pinMap", Json::array()},
                        {"note", "SN3D<->N3D exact any order; FuMa is FOA-only."}, {"warnings", warnings}};
#endif
        }});

    // ---- spatial.get_scene_info — read-only HOA / immersive introspection of a track ----
    reg.add(Tool{
        "spatial.get_scene_info",
        "Read-only: describe a track as an immersive / ambisonic bus. Infers the ambisonic order from the "
        "channel count ((order+1)^2, ACN), reports the real ACN channel count vs the even-padded track "
        "width, and lists any spatial-suite FX (encoders / decoders / rotators / binaural) on the track "
        "with their detected suite + role. Normalization is NOT signal-detectable, so it is reported as "
        "the project convention (ACN / SN3D), not measured.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "channels":{"type":"integer"},"inferredOrder":{"type":"integer"},
            "hoaChannels":{"type":["integer","null"]},"padded":{"type":"boolean"},
            "surplusChannels":{"type":"integer"},"interpretation":{"type":"string"},
            "convention":{"type":"string"},"spatialFx":{"type":"array"}},
            "required":["ok","channels","inferredOrder"]})"),
        ToolAnnotations{true, false, true}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int nch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            int order = -1;
            for (int o = 7; o >= 1; --o) if (hoaChannels(o) <= nch) { order = o; break; }
            const int hoa = order >= 1 ? hoaChannels(order) : 0;
            int ambiOrderExact = -1;
            std::string interp = interpretChannels(nch, &ambiOrderExact);
            Json fxList = Json::array();
            const int nfx = TrackFX_GetCount(t);
            for (int f = 0; f < nfx; ++f) {
                std::string nm = fxNameStr(t, f);
                std::string ln = toLower(nm);
                std::string suite = classifySuite(ln), role = classifyRole(ln);
                if (!suite.empty() || role != "other")
                    fxList.push_back(Json{{"index", f}, {"name", nm}, {"suite", suite}, {"role", role}});
            }
            return Json{{"ok", true}, {"track", idx}, {"channels", nch}, {"inferredOrder", order},
                        {"hoaChannels", order >= 1 ? Json(hoa) : Json(nullptr)},
                        {"padded", order >= 1 && nch > hoa}, {"surplusChannels", order >= 1 ? nch - hoa : 0},
                        {"interpretation", interp},
                        {"convention", "ACN channel order, SN3D normalization (assumed by convention)"},
                        {"spatialFx", std::move(fxList)}};
#else
            (void)idx;
            return Json{{"ok", true}, {"track", idx}, {"channels", 16}, {"inferredOrder", 3},
                        {"hoaChannels", 16}, {"padded", false}, {"surplusChannels", 0},
                        {"interpretation", "3rd-order ambisonics (ACN, 16ch)"},
                        {"convention", "ACN channel order, SN3D normalization (assumed by convention)"},
                        {"spatialFx", Json::array()}};
#endif
        }});

    // ---- spatial.mirror_scene — reflect a HOA soundfield across a principal plane (bundled JSFX) ----
    reg.add(Tool{
        "spatial.mirror_scene",
        "Reflect a HOA scene across a principal plane via the bundled zero-dependency JSFX 'MCP Ambisonic "
        "Scene Mirror', which applies the exact per-ACN-channel sign pattern of that reflection "
        "(order-independent, any order up to 7th). plane: 'left-right' swaps L<->R, 'front-back' swaps "
        "front<->back, 'up-down' swaps up<->down. Complements spatial.rotate_scene — a rotation is not a "
        "reflection; mirroring flips the scene's handedness.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "plane":{"type":"string","enum":["left-right","front-back","up-down"],"default":"left-right"},
            "fx":{"type":"integer","minimum":0},"wirePins":{"type":"boolean","default":true}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "mirror":{"type":"string"},"plane":{"type":"string"},"planeParam":{"type":["integer","null"]},
            "planeIndex":{"type":"integer"},"pinMap":{"type":"array"},"warnings":{"type":"array"}},
            "required":["ok","plane"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string plane = optStr(a, "plane", "left-right");
            const bool wirePins = optBool(a, "wirePins", true);
            const int planeIndex = plane == "front-back" ? 1 : plane == "up-down" ? 2 : 0;
            std::vector<std::string> warnings;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int nch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            Undo_BeginBlock2(kCur);
            // File aliases resolve the ADD; the trailing display name lets the find-existing scan
            // reuse an instance (whose on-track name is the @desc "JS: MCP Ambisonic Scene Mirror").
            std::vector<std::string> cands{"JS: MCP/mcp_ambi_mirror", "MCP/mcp_ambi_mirror",
                                           "mcp_ambi_mirror", "MCP Ambisonic Scene Mirror"};
            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: mirror ambisonic scene", -1);
                throw std::runtime_error("bundled 'MCP Ambisonic Scene Mirror' JSFX not found — install it "
                    "via 'cmake --install build' (ships to Effects/MCP/mcp_ambi_mirror.jsfx).");
            }
            const int np = TrackFX_GetNumParams(t, fx);
            int pp = discoverParam(t, fx, np, "plane");
            if (pp < 0) pp = discoverParam(t, fx, np, "reflection");
            if (pp < 0) pp = 0;
            TrackFX_SetParam(t, fx, pp, (double)planeIndex);
            Json pinMap = Json::array();
            if (wirePins) { pinIdentityWire(t, fx, nch, 0); pinMap = pinIdentityWire(t, fx, nch, 1); }
            Undo_EndBlock2(kCur, "MCP: mirror ambisonic scene", -1);
            return Json{{"ok", true}, {"fxIndex", fx}, {"mirror", fxNameStr(t, fx)}, {"plane", plane},
                        {"planeParam", pp}, {"planeIndex", planeIndex}, {"pinMap", std::move(pinMap)},
                        {"warnings", warnings}};
#else
            (void)wirePins; (void)idx;
            return Json{{"ok", true}, {"fxIndex", 0}, {"mirror", "MCP Ambisonic Scene Mirror"},
                        {"plane", plane}, {"planeParam", 0}, {"planeIndex", planeIndex},
                        {"pinMap", Json::array()}, {"warnings", warnings}};
#endif
        }});

    // ---- spatial.beamform — steerable directional pickup / virtual mic from a HOA scene ----
    reg.add(Tool{
        "spatial.beamform",
        "Extract a steerable directional signal (virtual microphone) from a HOA scene by inserting (or "
        "driving) a beamformer — SPARTA Beamformer by default — pointing at (azimuthDeg, elevationDeg) "
        "and identity-wiring the track's HOA channels onto its inputs. The beam order / pattern is a "
        "choice / continuous param whose enum differs per build, so it is reported for exact control; "
        "azimuth / elevation are written (degrees, clamped to each param's range). Fails closed with an "
        "install hint if no beamformer is present.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "azimuthDeg":{"type":"number"},"elevationDeg":{"type":"number"},"beam":{"type":"number"},
            "suite":{"type":"string","enum":["auto","SPARTA"],"default":"auto"},
            "plugin":{"type":"string"},"fx":{"type":"integer","minimum":0},
            "wirePins":{"type":"boolean","default":true}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "beamformer":{"type":"string"},"set":{"type":"array"},"automationParams":{"type":"object"},
            "inputPins":{"type":"integer"},"pinMap":{"type":"array"},"warnings":{"type":"array"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string suite = optStr(a, "suite", "auto");
            const bool wirePins = optBool(a, "wirePins", true);
            std::vector<std::string> warnings;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            std::vector<std::string> cands;
            if (a.contains("plugin") && a["plugin"].is_string()) cands.push_back(a["plugin"].get<std::string>());
            for (const auto& c : ambiCandidates("beamformer", suite)) cands.push_back(c);
            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: beamform", -1);
                throw std::runtime_error("no beamformer found (suite=" + suite + "); install SPARTA "
                    "Beamformer or pass 'plugin'.");
            }
            const int np = TrackFX_GetNumParams(t, fx);
            Json set = Json::array();
            auto push = [&](const char* key, const char* argKey) {
                if (!(a.contains(argKey) && a[argKey].is_number())) return;
                Json r = setNamedValueParam(t, fx, np, key, a[argKey].get<double>(), 1);
                if (!r.is_null()) { r["for"] = argKey; set.push_back(r); }
            };
            push("azim", "azimuthDeg");
            push("elev", "elevationDeg");
            if (a.contains("beam") && a["beam"].is_number()) {
                Json r = setNamedRealParam(t, fx, np, "order", a["beam"].get<double>(), 1);
                if (r.is_null()) r = setNamedRealParam(t, fx, np, "beam", a["beam"].get<double>(), 1);
                if (!r.is_null()) { r["for"] = "beam"; set.push_back(r); }
            }
            Json autom{{"azimuth", discoverParam(t, fx, np, "azim", 1)},
                       {"elevation", discoverParam(t, fx, np, "elev", 1)}};
            int inPins = 0, outPins = 0; TrackFX_GetIOSize(t, fx, &inPins, &outPins);
            const int nch = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            Json pinMap = Json::array();
            if (wirePins) pinMap = pinIdentityWire(t, fx, nch < inPins ? nch : inPins, 0);
            Undo_EndBlock2(kCur, "MCP: beamform", -1);
            if (set.empty())
                warnings.push_back("no azimuth / elevation / beam params matched by name; inspect the "
                                   "beamformer with the FX param tools and drive it by index.");
            return Json{{"ok", true}, {"fxIndex", fx}, {"beamformer", fxNameStr(t, fx)}, {"set", std::move(set)},
                        {"automationParams", std::move(autom)}, {"inputPins", inPins},
                        {"pinMap", std::move(pinMap)}, {"warnings", warnings}};
#else
            (void)wirePins; (void)suite; (void)idx;
            Json set = Json::array();
            auto push = [&](const char* argKey) { if (a.contains(argKey) && a[argKey].is_number())
                set.push_back(Json{{"for", argKey}, {"value", a[argKey].get<double>()}}); };
            push("azimuthDeg"); push("elevationDeg");
            return Json{{"ok", true}, {"fxIndex", 0}, {"beamformer", "SPARTA Beamformer"}, {"set", std::move(set)},
                        {"automationParams", Json{{"azimuth", -1}, {"elevation", -1}}}, {"inputPins", 0},
                        {"pinMap", Json::array()}, {"warnings", warnings}};
#endif
        }});

    // ---- spatial.set_distance — source distance / room / early reflections via IEM RoomEncoder ----
    reg.add(Tool{
        "spatial.set_distance",
        "Add source distance, shoebox-room size, and early-reflection depth to a HOA scene by inserting "
        "(or driving) IEM RoomEncoder. distanceM places the source that many metres from the listener "
        "along (azimuthDeg, elevationDeg) — the source X/Y/Z params use RoomEncoder's own room frame "
        "(+X=front, +Y=left, +Z=up, +az=left) with the listener pinned to the room centre, each mapped onto "
        "the plug-in's own metric range; roomSizeM sets "
        "the room dimensions; reflectionOrder sets the reflection depth. Params are discovered by name and "
        "their automation indices reported; unmatched params are warned, not guessed. Fails closed if "
        "RoomEncoder is not installed.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "distanceM":{"type":"number","minimum":0},"azimuthDeg":{"type":"number"},
            "elevationDeg":{"type":"number"},"roomSizeM":{"type":"number","minimum":0},
            "reflectionOrder":{"type":"integer","minimum":0},
            "suite":{"type":"string","enum":["auto","IEM"],"default":"auto"},
            "plugin":{"type":"string"},"fx":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"fxIndex":{"type":"integer"},
            "roomEncoder":{"type":"string"},"set":{"type":"array"},"automationParams":{"type":"object"},
            "warnings":{"type":"array"}},"required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Spatial,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string suite = optStr(a, "suite", "auto");
            std::vector<std::string> warnings;
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            std::vector<std::string> cands;
            if (a.contains("plugin") && a["plugin"].is_string()) cands.push_back(a["plugin"].get<std::string>());
            for (const auto& c : ambiCandidates("room", suite)) cands.push_back(c);
            std::string used;
            int fx = resolveOrAddByCandidates(t, a, cands, used);
            if (fx < 0) {
                Undo_EndBlock2(kCur, "MCP: room/distance encode", -1);
                throw std::runtime_error("no room encoder found (suite=" + suite + "); install IEM "
                    "RoomEncoder or pass 'plugin'.");
            }
            const int np = TrackFX_GetNumParams(t, fx);
            Json set = Json::array();
            auto setFirst = [&](const std::vector<const char*>& keys, double v, const char* label) -> bool {
                for (const char* k : keys) {
                    Json r = setNamedRealParam(t, fx, np, k, v);
                    if (!r.is_null()) { r["for"] = label; set.push_back(r); return true; }
                }
                return false;
            };
            if (a.contains("distanceM") && a["distanceM"].is_number()) {
                const double d = a["distanceM"].get<double>();
                const double az = (a.contains("azimuthDeg") && a["azimuthDeg"].is_number()) ? a["azimuthDeg"].get<double>() : 0.0;
                const double el = (a.contains("elevationDeg") && a["elevationDeg"].is_number()) ? a["elevationDeg"].get<double>() : 0.0;
                const double azr = az * 3.14159265358979323846 / 180.0, elr = el * 3.14159265358979323846 / 180.0;
                // IEM RoomEncoder's room frame is the ambiX convention (CONVENTIONS.md §3b): +X = FRONT,
                // +Y = LEFT, +Z = UP — its normalized (source − listener) vector feeds SHEval directly. This
                // is NOT the §4 project panner frame (x = right, y = front), so place the source at distance
                // d along (az, el) with +az = LEFT: front = cos(az)cos(el), left = sin(az)cos(el), up = sin(el).
                const double sx = d * std::cos(azr) * std::cos(elr);  // +X = front
                const double sy = d * std::sin(azr) * std::cos(elr);  // +Y = left  (+az = left)
                const double sz = d * std::sin(elr);                  // +Z = up
                // RoomEncoder names these "source position x/y/z" — the bare "source x" keys never matched
                // (substring), so the source was silently never placed. Match the real names; keep the short
                // forms as fallbacks for other builds.
                bool okx = setFirst({"source position x", "source x", "sourcex"}, sx, "sourceX");
                bool oky = setFirst({"source position y", "source y", "sourcey"}, sy, "sourceY");
                bool okz = setFirst({"source position z", "source z", "sourcez"}, sz, "sourceZ");
                if (!(okx && oky && okz))
                    warnings.push_back("RoomEncoder source-position params not fully matched by name; "
                                       "distance placement may be partial — inspect the plug-in and set by index.");
                // Pin the listener to the room centre so distanceM/(az, el) is measured from the LISTENER,
                // not the room origin — RoomEncoder's default listener is offset (−1,−1,−1). 0 m = centre.
                setFirst({"listener position x", "listenerx"}, 0.0, "listenerX");
                setFirst({"listener position y", "listenery"}, 0.0, "listenerY");
                setFirst({"listener position z", "listenerz"}, 0.0, "listenerZ");
            }
            if (a.contains("roomSizeM") && a["roomSizeM"].is_number()) {
                const double rs = a["roomSizeM"].get<double>();
                const std::vector<const char*> rk{"room size x", "room size y", "room size z"};
                for (const char* k : rk) { Json r = setNamedRealParam(t, fx, np, k, rs);
                    if (!r.is_null()) { r["for"] = k; set.push_back(r); } }
            }
            if (a.contains("reflectionOrder") && a["reflectionOrder"].is_number()) {
                Json r = setNamedRealParam(t, fx, np, "reflection order", a["reflectionOrder"].get<double>());
                if (r.is_null()) r = setNamedRealParam(t, fx, np, "reflection", a["reflectionOrder"].get<double>());
                if (!r.is_null()) { r["for"] = "reflectionOrder"; set.push_back(r); }
            }
            Json autom{{"reflectionOrder", discoverParam(t, fx, np, "reflection")},
                       {"roomSizeX", discoverParam(t, fx, np, "room size x")}};
            Undo_EndBlock2(kCur, "MCP: room/distance encode", -1);
            if (set.empty())
                warnings.push_back("no RoomEncoder params matched by name; inspect it with the FX param "
                                   "tools and drive it by index.");
            return Json{{"ok", true}, {"fxIndex", fx}, {"roomEncoder", fxNameStr(t, fx)}, {"set", std::move(set)},
                        {"automationParams", std::move(autom)}, {"warnings", warnings}};
#else
            (void)suite; (void)idx;
            return Json{{"ok", true}, {"fxIndex", 0}, {"roomEncoder", "RoomEncoder (IEM)"},
                        {"set", Json::array()}, {"automationParams", Json{{"reflectionOrder", -1}}},
                        {"warnings", warnings}};
#endif
        }});

    // ---- spatial.author_object_bed — declaratively validate/establish the ADM deliverable topology --
    reg.add(Tool{
        "spatial.author_object_bed",
        "Validate (and optionally establish) the bed+object topology an ADM object-audio deliverable "
        "needs, then emit the object map spatial.export_adm consumes — the idempotent 'manage my Atmos "
        "session for ADM export' layer above the one-shot spatial.setup_immersive_session. Give a "
        "bedTrack (a DirectSpeakers bus) + a bedLayout (5.1 | 7.1 | 7.1.2 | 7.1.4 | 9.1.6 | 22.2; 7.1.2 "
        "is the Atmos-canonical bed) + objectTracks (mono object tracks, each spatialized by a panner "
        "whose azimuth/elevation is the object's position metadata). Reports, per object, a stable "
        "1-based objectNumber, its channel count, whether a positional panner was found, and its current "
        "position; reports the bed's layout/width/labels; and a 'ready' flag = the session is "
        "export_adm-ready. dryRun (the DEFAULT) only inspects + reports what apply would change. Apply "
        "(dryRun:false) performs the minimal safe wiring in ONE undo block: build the bed if createBed "
        "and no bedTrack, widen the bed track to the layout width, and (renameObjects) rename object "
        "tracks 'Object N'. It never touches object audio or panner positions.",
        jparse(R"({"type":"object","properties":{
            "bedTrack":{"type":"integer","minimum":0},
            "bedLayout":{"type":"string","enum":["5.1","7.1","7.1.2","7.1.4","9.1.6","22.2"],"default":"7.1.2"},
            "objectTracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "createBed":{"type":"boolean","default":false},
            "renameObjects":{"type":"boolean","default":false},
            "dryRun":{"type":"boolean","default":true}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "dryRun":{"type":"boolean"},"ok":{"type":"boolean"},"ready":{"type":"boolean"},
            "bed":{"type":"object"},"objects":{"type":"array"},"actions":{"type":"array"},
            "warnings":{"type":"array","items":{"type":"string"}},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true},
        Profile::Spatial,
        [](const Json& a) -> Json {
            const std::string bedLayout = optStr(a, "bedLayout", "7.1.2");
            const bool dryRun = optBool(a, "dryRun", true);
            const bool createBed = optBool(a, "createBed", false);
            const bool renameObjects = optBool(a, "renameObjects", false);
            std::vector<int> objTracks;
            if (a.contains("objectTracks") && a["objectTracks"].is_array())
                for (const auto& o : a["objectTracks"]) if (o.is_number()) objTracks.push_back(o.get<int>());
#ifdef REAPER_MCP_HAVE_SDK
            const int layoutCh = admBedChannels(bedLayout);
            if (layoutCh < 0)
                return makeError("unknown_layout", "unknown bed layout: " + bedLayout,
                                 "known: 5.1, 7.1, 7.1.2, 7.1.4, 9.1.6, 22.2");
            Json warnings = Json::array();
            Json actions = Json::array();
            int bedTrack = (a.contains("bedTrack") && a["bedTrack"].is_number()) ? a["bedTrack"].get<int>() : -1;

            bool mutated = false;
            if (!dryRun && bedTrack < 0 && createBed) {
                Undo_BeginBlock2(kCur);
                bedTrack = insertNamedTrack(bedLayout + " Bed", layoutCh, false);
                Undo_EndBlock2(kCur, "MCP: build ADM bed", -1);
                mutated = true;
                actions.push_back(Json{{"action", "build_bed"}, {"track", bedTrack}, {"layout", bedLayout},
                                       {"channels", layoutCh}});
            }

            Json bedJson = Json(nullptr);
            if (bedTrack >= 0) {
                MediaTrack* bt = GetTrack(kCur, bedTrack);
                if (!bt) return makeError("bad_bed_track", "bedTrack out of range: " + std::to_string(bedTrack), "");
                int actual = trackChannels(bt);
                if (actual < layoutCh) {
                    if (!dryRun) {
                        if (!mutated) Undo_BeginBlock2(kCur);
                        SetMediaTrackInfo_Value(bt, "I_NCHAN", (double)clampChannels(layoutCh));
                        if (!mutated) Undo_EndBlock2(kCur, "MCP: widen ADM bed", -1);
                        mutated = true;
                        actions.push_back(Json{{"action", "widen_bed"}, {"track", bedTrack},
                                               {"from", actual}, {"to", clampChannels(layoutCh)}});
                        actual = clampChannels(layoutCh);
                    } else {
                        warnings.push_back("bed track has " + std::to_string(actual) + " ch; " + bedLayout +
                                           " needs " + std::to_string(layoutCh) + " (apply widens it).");
                    }
                }
                Json labels = Json::array();
                for (const adm::BedSpeaker& s : admBedSpeakers(bedLayout))
                    labels.push_back(Json{{"label", s.label}, {"speakerLabel", s.speakerLabel},
                                          {"azimuth", s.az}, {"elevation", s.el}, {"lfe", s.lfe}});
                bedJson = Json{{"track", bedTrack}, {"layout", bedLayout}, {"channels", actual},
                               {"expectedChannels", layoutCh}, {"speakers", labels}};
            }

            Json objs = Json::array();
            bool ready = (bedTrack >= 0) || !objTracks.empty();
            for (size_t j = 0; j < objTracks.size(); ++j) {
                MediaTrack* ot = GetTrack(kCur, objTracks[j]);
                if (!ot) return makeError("bad_object_track", "objectTracks out of range: " +
                                          std::to_string(objTracks[j]), "");
                std::string nm = trackNameStr(ot);
                if (renameObjects && !dryRun) {
                    const std::string want = "Object " + std::to_string(j + 1);
                    if (nm != want) {
                        if (!mutated) Undo_BeginBlock2(kCur);
                        GetSetMediaTrackInfo_String(ot, "P_NAME", (char*)want.c_str(), true);
                        if (!mutated) Undo_EndBlock2(kCur, "MCP: name ADM objects", -1);
                        mutated = true;
                        actions.push_back(Json{{"action", "rename_object"}, {"track", objTracks[j]},
                                               {"from", nm}, {"to", want}});
                        nm = want;
                    }
                }
                if (nm.empty()) nm = "Object " + std::to_string(j + 1);
                const PosFx pf = findPositionalFx(ot);
                const int actual = trackChannels(ot);
                const bool hasPanner = pf.fx >= 0;
                if (!hasPanner) { ready = false;
                    warnings.push_back("object '" + nm + "' has no positional panner (azimuth/elevation) "
                                       "— export_adm will place it front-center."); }
                if (actual > 2)
                    warnings.push_back("object '" + nm + "' has " + std::to_string(actual) +
                                       " ch; ADM objects are mono essence (channel 1 is used).");
                Json pos = Json(nullptr);
                if (hasPanner) {
                    std::string psrc;
                    std::vector<adm::Block> tr = objectTrajectory(ot, 0.0, 0.0, 100.0, 1, psrc);
                    if (!tr.empty())
                        pos = Json{{"azimuth", tr[0].az}, {"elevation", tr[0].el}, {"distance", tr[0].dist}};
                }
                objs.push_back(Json{{"track", objTracks[j]}, {"name", nm}, {"objectNumber", (int)j + 1},
                                    {"channels", actual}, {"hasPanner", hasPanner}, {"position", pos}});
            }
            if (!objTracks.empty() && bedTrack < 0)
                warnings.push_back("no bedTrack — exporting objects only (no DirectSpeakers bed).");

            return Json{{"dryRun", dryRun}, {"ok", true}, {"ready", ready}, {"bed", bedJson},
                        {"objects", objs}, {"actions", actions}, {"warnings", warnings}};
#else
            (void)createBed; (void)renameObjects;
            Json objs = Json::array();
            for (size_t j = 0; j < objTracks.size(); ++j)
                objs.push_back(Json{{"track", objTracks[j]}, {"name", "Object " + std::to_string(j + 1)},
                                    {"objectNumber", (int)j + 1}, {"channels", 1}, {"hasPanner", true},
                                    {"position", Json(nullptr)}});
            return Json{{"dryRun", dryRun}, {"ok", true}, {"ready", true},
                        {"bed", Json{{"layout", bedLayout}}}, {"objects", objs},
                        {"actions", Json::array()}, {"warnings", Json::array()}};
#endif
        }});

    // ---- spatial.export_adm — native ITU-R BS.2076 ADM BWF authoring (bed + object trajectories) ----
    reg.add(Tool{
        "spatial.export_adm",
        "Author a native ITU-R BS.2076 ADM object-audio deliverable — a Broadcast-Wave (RIFF/WAVE, or "
        "BW64/RF64 per BS.2088 when >4 GiB) carrying a chna (channel allocation) + axml (the ADM XML) "
        "chunk beside the interleaved PCM. This is a capability REAPER lacks natively (see "
        "spatial.render_deliverables' atmos-send-layout, which can only wire the topology to an EXTERNAL "
        "Dolby Renderer). The deliverable = a DirectSpeakers BED (bedTrack at bedLayout, e.g. Atmos "
        "7.1.2) plus N mono OBJECTS (objectTracks), each object carrying a position/gain TRAJECTORY "
        "(audioBlockFormat time-slices) SAMPLED from its panner automation over the render window "
        "(spherical az/el/distance by default; coordinateMode:cartesian emits X/Y/Z). Bed + object stems "
        "are rendered bit-exact (RENDER_* snapshot/restore, temps deleted); each object's mono essence = "
        "its rendered channel 1, its position = read from the object panner's azimuth/elevation/distance "
        "params (best-effort, like analysis.object_loudness). dryRun (the DEFAULT is false) samples the "
        "positions + plans the ADM model WITHOUT rendering or writing. Bound long programmes with "
        "boundsFlag=0 + startPos/endPos (a render blocks the main thread). Ingest-verify the file against "
        "your certified renderer (Dolby/EBU/Nuendo); inspect it with analysis.adm_inspect. Attach "
        "per-object BS.2076 metadata via objectMetadata[] (each entry keyed by its object 'track'): "
        "extent — a 'size' 0..1 knob (sets width=height=depth; the ADM normalized object size, natural "
        "in cartesian mode) or explicit width/height/depth (spherical: width/height in DEGREES, depth "
        "0..1); objectDivergence — 'divergence' 0..1 (split into two mirror copies) with an optional "
        "'divergenceRange' (azimuthRange deg in spherical / positionRange 0..1 in cartesian; defaults "
        "45°/0.5); and 'importance' 0..10 (a renderer may drop lower-importance objects under load). "
        "Objects with no entry stay point sources with importance omitted. Set "
        "profile:\"dolby-atmos\" to author a Dolby Atmos Master ADM Profile v1.0-conformant file: it "
        "forces cartesian objects (X/Y/Z clamped [-1,1]), collapses extent to an identical [0,1] size, "
        "STRIPS objectDivergence (prohibited) and importance (permitted only on inactive objects), and "
        "emits the interpolationLength ramp (0 then 250 samples); it FAILS CLOSED on a non-Atmos bed "
        "(7.1.4/9.1.6/22.2 — route those channels as objects), a non-48k render, or >118 objects / >128 "
        "channels. The applied normalizations are echoed in 'profile'. Validate any ADM BWF's conformance "
        "with analysis.adm_profile_check. (Profile-shaped + self-validated; certified-renderer ingest is "
        "your check.)",
        jparse(R"({"type":"object","properties":{
            "bedTrack":{"type":"integer","minimum":0},
            "bedLayout":{"type":"string","enum":["5.1","7.1","7.1.2","7.1.4","9.1.6","22.2"],"default":"7.1.2"},
            "objectTracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "objectMetadata":{"type":"array","items":{"type":"object","properties":{
                "track":{"type":"integer","minimum":0},
                "size":{"type":"number","minimum":0,"maximum":1},
                "width":{"type":"number","minimum":0},"height":{"type":"number","minimum":0},
                "depth":{"type":"number","minimum":0},
                "divergence":{"type":"number","minimum":0,"maximum":1},
                "divergenceRange":{"type":"number","minimum":0},
                "importance":{"type":"integer","minimum":0,"maximum":10}},
                "required":["track"],"additionalProperties":false}},
            "outPath":{"type":"string"},
            "coordinateMode":{"type":"string","enum":["spherical","cartesian"],"default":"spherical"},
            "profile":{"type":"string","enum":["none","dolby-atmos"],"default":"none"},
            "bitDepth":{"type":"integer","enum":[16,24,32],"default":24},
            "blockMs":{"type":"number","minimum":1,"default":100},
            "maxBlocks":{"type":"integer","minimum":1,"default":1000},
            "programmeName":{"type":"string"},"contentName":{"type":"string"},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "ok":{"type":"boolean"},"dryRun":{"type":"boolean"},"path":{"type":"string"},
            "container":{"type":"string"},"channels":{"type":"integer"},"frames":{"type":"integer"},
            "sampleRate":{"type":"number"},"bitDepth":{"type":"integer"},"durationSec":{"type":"number"},
            "bedLayout":{"type":"string"},"bedChannels":{"type":"integer"},"objectCount":{"type":"integer"},
            "objectMetadataCount":{"type":"integer"},
            "coordinateMode":{"type":"string"},"objects":{"type":"array"},"adm":{"type":"object"},
            "profile":{"type":"object"},
            "warnings":{"type":"array","items":{"type":"string"}},"note":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Render,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const std::string bedLayout = optStr(a, "bedLayout", "7.1.2");
            const std::string coordMode = optStr(a, "coordinateMode", "spherical");
            const adm::Coord coord = (coordMode == "cartesian") ? adm::Coord::Cartesian
                                                                : adm::Coord::Spherical;
            const std::string profileMode = optStr(a, "profile", "none");
            const bool wantProfile = (profileMode == "dolby-atmos");
            int bitDepth = optInt(a, "bitDepth", 24);
            if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) bitDepth = 24;
            const double blockMs = optNum(a, "blockMs", 100.0);
            const int maxBlocks = optInt(a, "maxBlocks", 1000);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
            std::vector<int> objTracks;
            if (a.contains("objectTracks") && a["objectTracks"].is_array())
                for (const auto& o : a["objectTracks"]) if (o.is_number()) objTracks.push_back(o.get<int>());
            const int objectMetadataCount =
                (a.contains("objectMetadata") && a["objectMetadata"].is_array())
                    ? (int)a["objectMetadata"].size() : 0;
#ifdef REAPER_MCP_HAVE_SDK
            const int bedChExpected = admBedChannels(bedLayout);
            if (bedChExpected < 0)
                return makeError("unknown_layout", "unknown bed layout: " + bedLayout,
                                 "known: 5.1, 7.1, 7.1.2, 7.1.4, 9.1.6, 22.2");
            const int bedTrack = (a.contains("bedTrack") && a["bedTrack"].is_number())
                                     ? a["bedTrack"].get<int>() : -1;
            if (bedTrack < 0 && objTracks.empty())
                return makeError("no_sources", "give a bedTrack and/or objectTracks to author",
                                 "e.g. {\"bedTrack\":3,\"objectTracks\":[4,5,6]}");
            Json warnings = Json::array();

            std::vector<adm::BedSpeaker> bed;
            int bedCh = 0;
            if (bedTrack >= 0) {
                MediaTrack* bt = GetTrack(kCur, bedTrack);
                if (!bt) return makeError("bad_bed_track", "bedTrack out of range: " +
                                          std::to_string(bedTrack), "");
                bed = admBedSpeakers(bedLayout);
                bedCh = (int)bed.size();
                const int actual = trackChannels(bt);
                if (actual < bedCh)
                    warnings.push_back("bedTrack has " + std::to_string(actual) + " channels but " +
                                       bedLayout + " needs " + std::to_string(bedCh) +
                                       " — the missing bed channels render silent (see "
                                       "spatial.author_object_bed to widen it).");
            }

            double startWin = 0.0, endWin = 0.0;
            if (boundsFlag == 0) { startWin = optNum(a, "startPos", 0.0); endWin = optNum(a, "endPos", 0.0); }
            else { startWin = 0.0; endWin = GetProjectLength(nullptr); }
            double durationSec = (endWin > startWin) ? (endWin - startWin) : 0.0;

            std::string outPath = optStr(a, "outPath", "");
            if (outPath.empty()) {
                char pbuf[4096] = {0};
                GetProjectPath(pbuf, (int)sizeof(pbuf));
                std::string dir = pbuf[0] ? std::string(pbuf)
                                          : (GetResourcePath() ? std::string(GetResourcePath()) : std::string("."));
                outPath = dir + "/REAPER_MCP_ADM.wav";
            }

            adm::Model m;
            m.programmeName = optStr(a, "programmeName", "REAPER MCP ADM Programme");
            m.contentName = optStr(a, "contentName", "Main");
            m.bitDepth = bitDepth;
            m.durationSec = durationSec;
            m.bedLayoutName = (bedTrack >= 0) ? bedLayout : "";
            m.bed = bed;

            // Apply the per-object BS.2076 metadata (extent / objectDivergence / importance)
            // from objectMetadata[] (matched by 'track'). Values are written uniformly onto every
            // audioBlockFormat of the object (importance sits on the audioObject). Absent => untouched.
            auto applyObjectMeta = [&](int trackIdx, adm::Object& o) {
                if (!a.contains("objectMetadata") || !a["objectMetadata"].is_array()) return;
                for (const auto& e : a["objectMetadata"]) {
                    if (!e.is_object() || !e.contains("track") || !e["track"].is_number() ||
                        e["track"].get<int>() != trackIdx)
                        continue;
                    bool hasExtent = false; double w = 0, h = 0, dep = 0;
                    if (e.contains("size") && e["size"].is_number()) {
                        w = h = dep = e["size"].get<double>(); hasExtent = true;
                    }
                    if (e.contains("width")  && e["width"].is_number())  { w   = e["width"].get<double>();  hasExtent = true; }
                    if (e.contains("height") && e["height"].is_number()) { h   = e["height"].get<double>(); hasExtent = true; }
                    if (e.contains("depth")  && e["depth"].is_number())  { dep = e["depth"].get<double>();  hasExtent = true; }
                    const bool hasDiv = e.contains("divergence") && e["divergence"].is_number();
                    double div = hasDiv ? e["divergence"].get<double>() : 0.0;
                    div = div < 0 ? 0 : (div > 1 ? 1 : div);
                    double divRange = (e.contains("divergenceRange") && e["divergenceRange"].is_number())
                                          ? e["divergenceRange"].get<double>() : -1.0;
                    if (hasDiv && divRange < 0)  // sensible default per coordinate mode
                        divRange = (o.coord == adm::Coord::Cartesian) ? 0.5 : 45.0;
                    if (e.contains("importance") && e["importance"].is_number()) {
                        int imp = e["importance"].get<int>();
                        o.importance = imp < 0 ? 0 : (imp > 10 ? 10 : imp);
                    }
                    for (adm::Block& b : o.blocks) {
                        if (hasExtent) { b.width = w; b.height = h; b.depth = dep; }
                        if (hasDiv) { b.hasDivergence = true; b.divergence = div; b.divergenceRange = divRange; }
                    }
                    break;  // first matching entry wins
                }
            };

            Json objReport = Json::array();
            for (size_t j = 0; j < objTracks.size(); ++j) {
                MediaTrack* ot = GetTrack(kCur, objTracks[j]);
                if (!ot) return makeError("bad_object_track", "objectTracks out of range: " +
                                          std::to_string(objTracks[j]), "");
                adm::Object o;
                o.name = trackNameStr(ot);
                if (o.name.empty()) o.name = "Object " + std::to_string(j + 1);
                o.objectNumber = (int)j + 1;
                o.coord = coord;
                std::string psrc;
                o.blocks = objectTrajectory(ot, startWin, endWin, blockMs, maxBlocks, psrc);
                o.positionSource = psrc;
                applyObjectMeta(objTracks[j], o);
                const int actual = trackChannels(ot);
                if (actual > 2)
                    warnings.push_back("object track '" + o.name + "' has " + std::to_string(actual) +
                                       " channels; ADM objects are mono essence — channel 1 is used.");
                if (psrc == "none")
                    warnings.push_back("object '" + o.name + "' has no positional panner — placed "
                                       "front-center; add an object panner for a real position.");
                // Echo the applied object metadata (from the object's first block + its importance).
                Json metaJ = Json(nullptr);
                if (!o.blocks.empty()) {
                    const adm::Block& b0 = o.blocks.front();
                    const bool anyExtent = b0.width > 0 || b0.height > 0 || b0.depth > 0;
                    if (o.importance >= 0 || anyExtent || b0.hasDivergence)
                        metaJ = Json{{"importance", o.importance},
                                     {"width", b0.width}, {"height", b0.height}, {"depth", b0.depth},
                                     {"divergence", b0.hasDivergence ? b0.divergence : 0.0},
                                     {"divergenceRange", b0.hasDivergence ? b0.divergenceRange : 0.0}};
                }
                objReport.push_back(Json{{"track", objTracks[j]}, {"name", o.name},
                                         {"objectNumber", o.objectNumber}, {"positionSource", psrc},
                                         {"blocks", (int)o.blocks.size()}, {"channels", actual},
                                         {"metadata", metaJ}});
                m.objects.push_back(std::move(o));
            }

            // Dolby Atmos Master ADM Profile conformance (optional). Normalize the model in
            // place (force cartesian, collapse extent, strip divergence/importance, set the
            // interpolationLength ramp) or fail closed on an unfixable invariant. The sample-rate
            // invariant is re-checked against the ACTUAL render below (a dryRun plans against 48k).
            Json profileJson = Json(nullptr);
            std::string reportedCoordMode = coordMode;
            if (wantProfile) {
                std::vector<std::string> pnorm;
                std::string perr;
                if (!adm::profile::normalizeModel(m, pnorm, perr)) {
                    std::string rem;
                    if (perr == "bed_not_profile_conformant")
                        rem = "the Atmos bed maxes at 7.1.2 — use bedLayout 7.1.2 (or 5.1/7.1) and route "
                              "heights/wides as objectTracks";
                    else if (perr == "sample_rate_not_48k") rem = "set the project sample rate to 48000";
                    else if (perr == "too_many_objects")   rem = "the Atmos profile allows at most 118 objects";
                    else if (perr == "too_many_channels")  rem = "the Atmos profile allows at most 128 channels";
                    return makeError(perr, "the model does not meet the Dolby Atmos Master ADM Profile", rem);
                }
                reportedCoordMode = "cartesian";
                for (const std::string& w : pnorm) warnings.push_back(w);
                // Refresh the per-object metadata echo from the NORMALIZED model.
                for (size_t j = 0; j < m.objects.size() && j < objReport.size(); ++j) {
                    const adm::Object& o = m.objects[j];
                    Json metaJ = Json(nullptr);
                    if (!o.blocks.empty()) {
                        const adm::Block& b0 = o.blocks.front();
                        const bool anyExtent = b0.width > 0 || b0.height > 0 || b0.depth > 0;
                        if (o.importance >= 0 || anyExtent || b0.hasDivergence)
                            metaJ = Json{{"importance", o.importance},
                                         {"width", b0.width}, {"height", b0.height}, {"depth", b0.depth},
                                         {"divergence", b0.hasDivergence ? b0.divergence : 0.0},
                                         {"divergenceRange", b0.hasDivergence ? b0.divergenceRange : 0.0}};
                    }
                    objReport[j]["metadata"] = metaJ;
                }
                adm::profile::Report prep = adm::profile::validateModel(m);
                profileJson = prep.toJson();
                Json normed = Json::array();
                for (const std::string& w : pnorm) normed.push_back(w);
                profileJson["normalized"] = normed;
            }

            if (dryRun) {
                return Json{{"dryRun", true}, {"ok", true}, {"path", outPath},
                            {"bedLayout", m.bedLayoutName}, {"bedChannels", bedCh},
                            {"objectCount", (int)m.objects.size()}, {"channels", m.channelCount()},
                            {"objectMetadataCount", objectMetadataCount},
                            {"coordinateMode", reportedCoordMode}, {"bitDepth", bitDepth},
                            {"durationSec", durationSec}, {"objects", objReport}, {"profile", profileJson},
                            {"warnings", warnings},
                            {"note", "dryRun: sampled object positions + planned the ADM model; "
                                     "no render, no file written."}};
            }

            // --- real render + author ---
            size_t frames = 0;
            double sampleRate = 0.0;
            std::vector<std::vector<float>> channels;

            if (bedTrack >= 0) {
                TempRender br = renderTargetToTempWav(bedTrack, bedCh, /*measureLoudness*/ false,
                                                      boundsFlag, a);
                if (!br.ok)
                    return makeError(br.error.empty() ? "bed_render_failed" : br.error,
                                     "failed to render the bed track", br.remediation);
                sampleRate = br.buf.sampleRate;
                frames = br.buf.frames;
                for (int c = 0; c < bedCh; ++c) {
                    std::vector<float> ch(frames, 0.0f);
                    if (c < br.buf.channels)
                        for (size_t f = 0; f < frames; ++f) ch[f] = br.buf.at(f, c);
                    channels.push_back(std::move(ch));
                }
            }

            if (!objTracks.empty()) {
                std::vector<int> sortedTracks = objTracks;
                std::sort(sortedTracks.begin(), sortedTracks.end());
                std::vector<std::string> labels;
                for (int idx : sortedTracks) {
                    MediaTrack* ot = GetTrack(kCur, idx);
                    labels.push_back(ot ? trackNameStr(ot) : std::string());
                }
                MultiStemRender mr = renderTracksToTempWavs(sortedTracks, labels, 2,
                                                            /*measureLoudness*/ false, boundsFlag, a);
                if (!mr.ok)
                    return makeError(mr.error.empty() ? "object_render_failed" : mr.error,
                                     "failed to render the object stems", mr.remediation);
                if (sampleRate <= 0)
                    for (const meter::AudioBuffer& b : mr.bufs) if (b.sampleRate > 0) { sampleRate = b.sampleRate; break; }
                if (frames == 0)
                    for (const meter::AudioBuffer& b : mr.bufs) if (b.frames > 0) { frames = b.frames; break; }
                // Append each object (in the user's given order) as its mono essence (channel 0).
                for (int idx : objTracks) {
                    size_t pos = (size_t)(std::lower_bound(sortedTracks.begin(), sortedTracks.end(), idx) -
                                          sortedTracks.begin());
                    std::vector<float> ch(frames, 0.0f);
                    if (pos < mr.bufs.size() && mr.haveBuf[pos]) {
                        const meter::AudioBuffer& b = mr.bufs[pos];
                        const size_t nf = std::min(frames, b.frames);
                        for (size_t f = 0; f < nf; ++f) ch[f] = b.channels > 0 ? b.at(f, 0) : 0.0f;
                    } else {
                        warnings.push_back("object track " + std::to_string(idx) +
                                           " produced no stem audio (silent channel written).");
                    }
                    channels.push_back(std::move(ch));
                }
            }

            if (frames == 0 || channels.empty())
                return makeError("render_empty", "the render produced no audio",
                                 "bound the export to audio (boundsFlag=0 + startPos/endPos) and check "
                                 "the bed/object tracks route audio");
            if (sampleRate <= 0) sampleRate = 48000.0;
            m.sampleRate = (int)std::llround(sampleRate);
            m.durationSec = (double)frames / sampleRate;

            // The Dolby Atmos profile mandates a 48 kHz deliverable; enforce it on the ACTUAL
            // rendered rate (the model was normalized above assuming 48k).
            if (wantProfile && m.sampleRate != adm::profile::kSampleRate)
                return makeError("sample_rate_not_48k",
                                 "the render produced a " + std::to_string(m.sampleRate) +
                                     " Hz file; the Dolby Atmos Master ADM Profile mandates 48000",
                                 "set the project/render sample rate to 48000 and retry");

            adm::WriteResult wr = adm::writeAdmImage(m, channels, frames);
            if (!wr.ok) return makeError("adm_write_failed", "could not assemble the ADM image: " + wr.error, "");

            std::ofstream os(outPath, std::ios::binary | std::ios::trunc);
            if (!os) return makeError("file_open_failed", "could not open output path for writing: " + outPath,
                                      "pass a writable outPath (or omit it to use the project directory)");
            os.write(wr.bytes.data(), (std::streamsize)wr.bytes.size());
            os.close();
            if (!os) return makeError("file_write_failed", "error writing the ADM file to " + outPath, "");

            adm::ParseResult pr = adm::parseAdmImage(wr.bytes);
            return Json{{"ok", true}, {"dryRun", false}, {"path", outPath},
                        {"container", wr.bw64 ? "BW64" : "RIFF/WAVE"}, {"channels", wr.channels},
                        {"frames", (double)frames}, {"sampleRate", sampleRate}, {"bitDepth", bitDepth},
                        {"durationSec", m.durationSec}, {"bedLayout", m.bedLayoutName},
                        {"bedChannels", bedCh}, {"objectCount", (int)m.objects.size()},
                        {"objectMetadataCount", objectMetadataCount},
                        {"coordinateMode", reportedCoordMode}, {"objects", objReport},
                        {"profile", profileJson},
                        {"adm", pr.ok ? pr.summary : Json(nullptr)}, {"warnings", warnings}};
#else
            (void)coord; (void)blockMs; (void)maxBlocks; (void)boundsFlag; (void)bitDepth;
            Json objReport = Json::array();
            for (size_t j = 0; j < objTracks.size(); ++j)
                objReport.push_back(Json{{"track", objTracks[j]}, {"name", "Object " + std::to_string(j + 1)},
                                         {"objectNumber", (int)j + 1}, {"positionSource", "host"},
                                         {"blocks", 1}, {"channels", 1}});
            auto hostBedCh = [](const std::string& L) -> int {
                if (L == "7.1.2") return 10;
                const BedLayout* b = findBedLayout(L);
                return b ? b->channels : 0;
            };
            const int bedCh = hostBedCh(bedLayout);
            Json profileJson = Json(nullptr);
            if (wantProfile)
                profileJson = Json{{"profile", "dolby-atmos"}, {"conformant", true},
                                   {"violations", Json::array()}, {"normalized", Json::array()},
                                   {"note", "host build: profile plan (no REAPER render)"}};
            return Json{{"dryRun", dryRun}, {"ok", true}, {"path", optStr(a, "outPath", "REAPER_MCP_ADM.wav")},
                        {"bedLayout", bedLayout}, {"bedChannels", bedCh},
                        {"objectCount", (int)objTracks.size()}, {"channels", bedCh + (int)objTracks.size()},
                        {"objectMetadataCount", objectMetadataCount},
                        {"coordinateMode", wantProfile ? std::string("cartesian") : coordMode},
                        {"bitDepth", bitDepth}, {"durationSec", 0.0},
                        {"objects", objReport}, {"profile", profileJson}, {"warnings", Json::array()},
                        {"note", "host build: representative plan (no REAPER render)"}};
#endif
        }});

    // ---- spatial.export_damf — native Dolby Atmos Master File (DAMF) triad authoring -----------------
    reg.add(Tool{
        "spatial.export_damf",
        "Author a native Dolby Atmos Master File (DAMF) — the Dolby Atmos Renderer's triad master, the "
        "sibling of ADM BWF (spatial.export_adm): three same-basename files, <base>.atmos (YAML manifest "
        "— version, presentation, the file references, and the bed+object roster each with a 1-based input "
        "ID), <base>.atmos.metadata (YAML — sampleRate + a flat list of position 'events' keyed by ID: bed "
        "channels are static, objects carry one event per trajectory block {ID, samplePos, active, "
        "pos:[x,y,z]}), and <base>.atmos.audio (a big-endian Core Audio Format / CAF PCM file — the "
        "interleaved essence, BED CHANNELS FIRST then OBJECTS in roster order). Same inputs as "
        "spatial.export_adm: a DirectSpeakers BED (bedTrack at bedLayout, <= Atmos 7.1.2) plus N mono "
        "OBJECTS (objectTracks), each object's position/gain TRAJECTORY SAMPLED from its panner automation "
        "over the render window; bed + object stems are rendered bit-exact (RENDER_* snapshot/restore, "
        "temps deleted). DAMF is inherently Dolby Atmos, so the Master ADM Profile v1.0 constraints are "
        "ENFORCED (FAILS CLOSED on a bed above 7.1.2 — route heights/wides as objects — a non-48k render, "
        "or >118 objects / >128 channels); positions are the DAMF room frame x[-1..1] L..R, y[-1..1] "
        "back..front, z[0..1] floor..ceiling. Attach a per-object extent via objectMetadata[] (each keyed "
        "by its object 'track'): 'size' 0..1 (the Atmos object size). outPath is a BASENAME (any trailing "
        ".atmos/.atmos.audio/.atmos.metadata is stripped); the three files are written beside it. dryRun "
        "(DEFAULT false) samples positions + plans the triad WITHOUT rendering or writing. Bound long "
        "programmes with boundsFlag=0 + startPos/endPos (a render blocks the main thread). Inspect the "
        "result with analysis.damf_inspect; ingest-verify against your certified Dolby Atmos Renderer / "
        "Conversion Tool. (DAMF-shaped + self-validated; certified-renderer ingest is your check.)",
        jparse(R"({"type":"object","properties":{
            "bedTrack":{"type":"integer","minimum":0},
            "bedLayout":{"type":"string","enum":["5.1","7.1","7.1.2"],"default":"7.1.2"},
            "objectTracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "objectMetadata":{"type":"array","items":{"type":"object","properties":{
                "track":{"type":"integer","minimum":0},
                "size":{"type":"number","minimum":0,"maximum":1}},
                "required":["track"],"additionalProperties":false}},
            "outPath":{"type":"string"},
            "bitDepth":{"type":"integer","enum":[16,24,32],"default":24},
            "blockMs":{"type":"number","minimum":1,"default":100},
            "maxBlocks":{"type":"integer","minimum":1,"default":1000},
            "programmeName":{"type":"string"},"contentName":{"type":"string"},
            "boundsFlag":{"type":"integer","minimum":0,"maximum":7,"default":1},
            "startPos":{"type":"number"},"endPos":{"type":"number"},
            "renderAction":{"type":"integer","default":41824},
            "dryRun":{"type":"boolean","default":false}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "ok":{"type":"boolean"},"dryRun":{"type":"boolean"},"path":{"type":"string"},
            "basePath":{"type":"string"},"files":{"type":"object"},
            "channels":{"type":"integer"},"frames":{"type":"integer"},"sampleRate":{"type":"number"},
            "bitDepth":{"type":"integer"},"durationSec":{"type":"number"},"bedLayout":{"type":"string"},
            "bedChannels":{"type":"integer"},"objectCount":{"type":"integer"},
            "objectMetadataCount":{"type":"integer"},"objects":{"type":"array"},"damf":{"type":"object"},
            "warnings":{"type":"array","items":{"type":"string"}},"note":{"type":"string"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Render,
        [](const Json& a) -> Json {
            const bool dryRun = optBool(a, "dryRun", false);
            const std::string bedLayout = optStr(a, "bedLayout", "7.1.2");
            int bitDepth = optInt(a, "bitDepth", 24);
            if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) bitDepth = 24;
            const double blockMs = optNum(a, "blockMs", 100.0);
            const int maxBlocks = optInt(a, "maxBlocks", 1000);
            const int boundsFlag = optInt(a, "boundsFlag", 1);
            std::vector<int> objTracks;
            if (a.contains("objectTracks") && a["objectTracks"].is_array())
                for (const auto& o : a["objectTracks"]) if (o.is_number()) objTracks.push_back(o.get<int>());
            const int objectMetadataCount =
                (a.contains("objectMetadata") && a["objectMetadata"].is_array())
                    ? (int)a["objectMetadata"].size() : 0;
#ifdef REAPER_MCP_HAVE_SDK
            // DAMF is inherently Dolby Atmos: the bed maxes at 7.1.2.
            if (!adm::profile::bedLayoutAllowed(bedLayout))
                return makeError("bed_not_profile_conformant",
                                 "DAMF bed '" + bedLayout + "' exceeds the Atmos 7.1.2 bed",
                                 "use bedLayout 5.1/7.1/7.1.2 and route heights/wides as objectTracks");
            const int bedChExpected = admBedChannels(bedLayout);
            if (bedChExpected < 0)
                return makeError("unknown_layout", "unknown bed layout: " + bedLayout, "known: 5.1, 7.1, 7.1.2");
            const int bedTrack = (a.contains("bedTrack") && a["bedTrack"].is_number())
                                     ? a["bedTrack"].get<int>() : -1;
            if (bedTrack < 0 && objTracks.empty())
                return makeError("no_sources", "give a bedTrack and/or objectTracks to author",
                                 "e.g. {\"bedTrack\":3,\"objectTracks\":[4,5,6]}");
            Json warnings = Json::array();

            std::vector<adm::BedSpeaker> bed;
            int bedCh = 0;
            if (bedTrack >= 0) {
                MediaTrack* bt = GetTrack(kCur, bedTrack);
                if (!bt) return makeError("bad_bed_track", "bedTrack out of range: " +
                                          std::to_string(bedTrack), "");
                bed = admBedSpeakers(bedLayout);
                bedCh = (int)bed.size();
                const int actual = trackChannels(bt);
                if (actual < bedCh)
                    warnings.push_back("bedTrack has " + std::to_string(actual) + " channels but " +
                                       bedLayout + " needs " + std::to_string(bedCh) +
                                       " — the missing bed channels render silent.");
            }

            double startWin = 0.0, endWin = 0.0;
            if (boundsFlag == 0) { startWin = optNum(a, "startPos", 0.0); endWin = optNum(a, "endPos", 0.0); }
            else { startWin = 0.0; endWin = GetProjectLength(nullptr); }
            double durationSec = (endWin > startWin) ? (endWin - startWin) : 0.0;

            // Output BASENAME — the three files are written as <base>.atmos / .atmos.audio / .atmos.metadata.
            std::string outPath = optStr(a, "outPath", "");
            if (outPath.empty()) {
                char pbuf[4096] = {0};
                GetProjectPath(pbuf, (int)sizeof(pbuf));
                std::string dir = pbuf[0] ? std::string(pbuf)
                                          : (GetResourcePath() ? std::string(GetResourcePath()) : std::string("."));
                outPath = dir + "/REAPER_MCP_ATMOS";
            }
            auto hasSuf = [](const std::string& s, const std::string& suf) {
                return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
            };
            if (hasSuf(outPath, ".atmos.audio")) outPath = outPath.substr(0, outPath.size() - 12);
            else if (hasSuf(outPath, ".atmos.metadata")) outPath = outPath.substr(0, outPath.size() - 15);
            else if (hasSuf(outPath, ".atmos")) outPath = outPath.substr(0, outPath.size() - 6);
            std::string stem = outPath;  // bare filename for the manifest's sibling references
            { size_t slash = stem.find_last_of("/\\"); if (slash != std::string::npos) stem = stem.substr(slash + 1); }

            adm::Model m;
            m.programmeName = optStr(a, "programmeName", "REAPER MCP Atmos Master");
            m.contentName = optStr(a, "contentName", "Main");
            m.bitDepth = bitDepth;
            m.durationSec = durationSec;
            m.bedLayoutName = (bedTrack >= 0) ? bedLayout : "";
            m.bed = bed;

            auto applySize = [&](int trackIdx, adm::Object& o) {
                if (!a.contains("objectMetadata") || !a["objectMetadata"].is_array()) return;
                for (const auto& e : a["objectMetadata"]) {
                    if (!e.is_object() || !e.contains("track") || !e["track"].is_number() ||
                        e["track"].get<int>() != trackIdx)
                        continue;
                    if (e.contains("size") && e["size"].is_number()) {
                        double s = e["size"].get<double>(); if (s < 0) s = 0; if (s > 1) s = 1;
                        for (adm::Block& b : o.blocks) { b.width = b.height = b.depth = s; }
                    }
                    break;
                }
            };

            Json objReport = Json::array();
            for (size_t j = 0; j < objTracks.size(); ++j) {
                MediaTrack* ot = GetTrack(kCur, objTracks[j]);
                if (!ot) return makeError("bad_object_track", "objectTracks out of range: " +
                                          std::to_string(objTracks[j]), "");
                adm::Object o;
                o.name = trackNameStr(ot);
                if (o.name.empty()) o.name = "Object " + std::to_string(j + 1);
                o.objectNumber = (int)j + 1;
                o.coord = adm::Coord::Cartesian;  // DAMF room frame
                std::string psrc;
                o.blocks = objectTrajectory(ot, startWin, endWin, blockMs, maxBlocks, psrc);
                o.positionSource = psrc;
                applySize(objTracks[j], o);
                const int actual = trackChannels(ot);
                if (actual > 2)
                    warnings.push_back("object track '" + o.name + "' has " + std::to_string(actual) +
                                       " channels; objects are mono essence — channel 1 is used.");
                if (psrc == "none")
                    warnings.push_back("object '" + o.name + "' has no positional panner — placed "
                                       "front-centre; add an object panner for a real position.");
                objReport.push_back(Json{{"track", objTracks[j]}, {"name", o.name},
                                         {"objectNumber", o.objectNumber},
                                         {"ID", (int)m.bed.size() + (int)j + 1},
                                         {"positionSource", psrc}, {"blocks", (int)o.blocks.size()},
                                         {"channels", actual}});
                m.objects.push_back(std::move(o));
            }

            // Enforce the Dolby Atmos Master constraints (force cartesian, collapse extent, cap objects/
            // channels/bed). FAIL CLOSED on an unfixable invariant.
            std::vector<std::string> pnorm; std::string perr;
            if (!adm::profile::normalizeModel(m, pnorm, perr)) {
                std::string rem;
                if (perr == "bed_not_profile_conformant") rem = "the Atmos bed maxes at 7.1.2";
                else if (perr == "sample_rate_not_48k") rem = "set the project sample rate to 48000";
                else if (perr == "too_many_objects") rem = "DAMF allows at most 118 objects";
                else if (perr == "too_many_channels") rem = "DAMF allows at most 128 channels";
                return makeError(perr, "the model does not meet the Dolby Atmos Master constraints", rem);
            }
            for (const std::string& w : pnorm) warnings.push_back(w);

            if (dryRun) {
                return Json{{"dryRun", true}, {"ok", true}, {"path", outPath + ".atmos"},
                            {"basePath", outPath}, {"bedLayout", m.bedLayoutName}, {"bedChannels", bedCh},
                            {"objectCount", (int)m.objects.size()}, {"channels", m.channelCount()},
                            {"objectMetadataCount", objectMetadataCount}, {"bitDepth", bitDepth},
                            {"durationSec", durationSec}, {"objects", objReport}, {"warnings", warnings},
                            {"note", "dryRun: sampled object positions + planned the DAMF triad; "
                                     "no render, no files written."}};
            }

            // --- real render + author ---
            size_t frames = 0; double sampleRate = 0.0;
            std::vector<std::vector<float>> channels;

            if (bedTrack >= 0) {
                TempRender br = renderTargetToTempWav(bedTrack, bedCh, /*measureLoudness*/ false,
                                                      boundsFlag, a);
                if (!br.ok)
                    return makeError(br.error.empty() ? "bed_render_failed" : br.error,
                                     "failed to render the bed track", br.remediation);
                sampleRate = br.buf.sampleRate; frames = br.buf.frames;
                for (int c = 0; c < bedCh; ++c) {
                    std::vector<float> ch(frames, 0.0f);
                    if (c < br.buf.channels)
                        for (size_t f = 0; f < frames; ++f) ch[f] = br.buf.at(f, c);
                    channels.push_back(std::move(ch));
                }
            }

            if (!objTracks.empty()) {
                std::vector<int> sortedTracks = objTracks;
                std::sort(sortedTracks.begin(), sortedTracks.end());
                std::vector<std::string> labels;
                for (int idx : sortedTracks) {
                    MediaTrack* ot = GetTrack(kCur, idx);
                    labels.push_back(ot ? trackNameStr(ot) : std::string());
                }
                MultiStemRender mr = renderTracksToTempWavs(sortedTracks, labels, 2,
                                                            /*measureLoudness*/ false, boundsFlag, a);
                if (!mr.ok)
                    return makeError(mr.error.empty() ? "object_render_failed" : mr.error,
                                     "failed to render the object stems", mr.remediation);
                if (sampleRate <= 0)
                    for (const meter::AudioBuffer& b : mr.bufs) if (b.sampleRate > 0) { sampleRate = b.sampleRate; break; }
                if (frames == 0)
                    for (const meter::AudioBuffer& b : mr.bufs) if (b.frames > 0) { frames = b.frames; break; }
                for (int idx : objTracks) {
                    size_t pos = (size_t)(std::lower_bound(sortedTracks.begin(), sortedTracks.end(), idx) -
                                          sortedTracks.begin());
                    std::vector<float> ch(frames, 0.0f);
                    if (pos < mr.bufs.size() && mr.haveBuf[pos]) {
                        const meter::AudioBuffer& b = mr.bufs[pos];
                        const size_t nf = std::min(frames, b.frames);
                        for (size_t f = 0; f < nf; ++f) ch[f] = b.channels > 0 ? b.at(f, 0) : 0.0f;
                    } else {
                        warnings.push_back("object track " + std::to_string(idx) +
                                           " produced no stem audio (silent channel written).");
                    }
                    channels.push_back(std::move(ch));
                }
            }

            if (frames == 0 || channels.empty())
                return makeError("render_empty", "the render produced no audio",
                                 "bound the export to audio (boundsFlag=0 + startPos/endPos) and check "
                                 "the bed/object tracks route audio");
            if (sampleRate <= 0) sampleRate = 48000.0;
            m.sampleRate = (int)std::llround(sampleRate);
            m.durationSec = (double)frames / sampleRate;

            if (m.sampleRate != adm::profile::kSampleRate)
                return makeError("sample_rate_not_48k",
                                 "the render produced a " + std::to_string(m.sampleRate) +
                                     " Hz file; DAMF mandates 48000",
                                 "set the project/render sample rate to 48000 and retry");

            damf::Triad tri = damf::writeTriad(m, channels, frames, stem);
            if (!tri.ok) return makeError("damf_write_failed",
                                          "could not assemble the DAMF triad: " + tri.error, "");

            const std::string atmosPath = outPath + ".atmos";
            const std::string audioPath = outPath + ".atmos.audio";
            const std::string metaPath  = outPath + ".atmos.metadata";
            struct OutFile { const std::string* path; const std::string* bytes; };
            const OutFile outs[3] = {{&atmosPath, &tri.atmos}, {&audioPath, &tri.atmosAudio},
                                     {&metaPath, &tri.atmosMetadata}};
            for (const OutFile& of : outs) {
                std::ofstream os(*of.path, std::ios::binary | std::ios::trunc);
                if (!os) return makeError("file_open_failed",
                                          "could not open output path for writing: " + *of.path,
                                          "pass a writable outPath basename (or omit it to use the project dir)");
                os.write(of.bytes->data(), (std::streamsize)of.bytes->size());
                os.close();
                if (!os) return makeError("file_write_failed", "error writing " + *of.path, "");
            }

            Json ins = damf::inspectTriad(tri.atmos, tri.atmosMetadata, tri.atmosAudio);
            return Json{{"ok", true}, {"dryRun", false}, {"path", atmosPath}, {"basePath", outPath},
                        {"files", Json{{"atmos", atmosPath}, {"audio", audioPath}, {"metadata", metaPath}}},
                        {"channels", tri.channels}, {"frames", (double)frames}, {"sampleRate", sampleRate},
                        {"bitDepth", bitDepth}, {"durationSec", m.durationSec}, {"bedLayout", m.bedLayoutName},
                        {"bedChannels", bedCh}, {"objectCount", (int)m.objects.size()},
                        {"objectMetadataCount", objectMetadataCount}, {"objects", objReport},
                        {"damf", ins}, {"warnings", warnings}};
#else
            (void)blockMs; (void)maxBlocks; (void)boundsFlag; (void)bitDepth;
            Json objReport = Json::array();
            for (size_t j = 0; j < objTracks.size(); ++j)
                objReport.push_back(Json{{"track", objTracks[j]}, {"name", "Object " + std::to_string(j + 1)},
                                         {"objectNumber", (int)j + 1}, {"positionSource", "host"},
                                         {"blocks", 1}, {"channels", 1}});
            auto hostBedCh = [](const std::string& L) -> int {
                if (L == "7.1.2") return 10;
                const BedLayout* b = findBedLayout(L);
                return b ? b->channels : 0;
            };
            const int bedCh = hostBedCh(bedLayout);
            return Json{{"dryRun", dryRun}, {"ok", true},
                        {"path", optStr(a, "outPath", "REAPER_MCP_ATMOS") + ".atmos"},
                        {"bedLayout", bedLayout}, {"bedChannels", bedCh},
                        {"objectCount", (int)objTracks.size()},
                        {"channels", bedCh + (int)objTracks.size()},
                        {"objectMetadataCount", objectMetadataCount}, {"objects", objReport},
                        {"warnings", Json::array()},
                        {"note", "host build: representative plan (no REAPER render)"}};
#endif
        }});

    // ---- spatial.orchestrate_sends — idempotent bed+object -> renderer send-layout orchestration ----
    reg.add(Tool{
        "spatial.orchestrate_sends",
        "Auto-orchestrate the object + bed send layout into an external Dolby Atmos Renderer / DAPS "
        "input bus on the canonical 1-based input roster: the bed lands on renderer channels 1..bedW, "
        "then each mono object takes one subsequent channel (bed-first, matching the ADM/DAMF roster). "
        "objectTracks is an explicit ordered list; omit it to auto-discover mono 'Object N' tracks (the "
        "convention setup_immersive_session / author_object_bed create) in N order. IDEMPOTENT: an "
        "existing send that already lands at the right channel is REUSED, a send from the right source at "
        "the wrong channel is FIXED in place, and only missing sends are added — re-running never stacks "
        "duplicates. Stray sends into the bus (no roster slot) are reported; prune:true removes them. "
        "FAILS CLOSED on the Dolby master limits (<=128 renderer channels, <=118 objects). dryRun (the "
        "default) returns the plan without touching routing.",
        jparse(R"({"type":"object","properties":{
            "rendererTrack":{"type":"integer","minimum":0},
            "bedTrack":{"type":"integer","minimum":0},
            "bedLayout":{"type":"string","enum":["5.1","7.1","7.1.2","7.1.4","9.1.6","22.2"],"default":"7.1.2"},
            "objectTracks":{"type":"array","items":{"type":"integer","minimum":0}},
            "prune":{"type":"boolean","default":false},"db":{"type":"number"},
            "dryRun":{"type":"boolean","default":true}},
            "required":["rendererTrack"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"dryRun":{"type":"boolean"},
            "rendererTrack":{"type":"integer"},"bedLayout":{"type":"string"},"bedTrack":{"type":"integer"},
            "bedChannels":{"type":"integer"},"objectCount":{"type":"integer"},"totalChannels":{"type":"integer"},
            "discovered":{"type":"boolean"},"inputs":{"type":"array"},"actions":{"type":"array"},
            "strays":{"type":"array"},"counts":{"type":"object"},"constraints":{"type":"array"},
            "warnings":{"type":"array"},"error":{"type":"string"},"detail":{"type":"string"},
            "remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true}, Profile::Spatial,
        [](const Json& a) -> Json {
            namespace sl = send_layout;
            const std::string bedLayout = optStr(a, "bedLayout", "7.1.2");
            const bool dryRun = optBool(a, "dryRun", true);
            const bool prune = optBool(a, "prune", false);
            const bool haveBedArg = a.contains("bedTrack") && a["bedTrack"].is_number();
            // Bed layout -> width (mirrors admBedChannels; local so both build configs see it).
            auto layoutWidth = [](const std::string& L) -> int {
                if (L == "5.1")   return 6;
                if (L == "7.1")   return 8;
                if (L == "7.1.2") return 10;
                if (L == "7.1.4") return 12;
                if (L == "9.1.6") return 16;
                if (L == "22.2")  return 24;
                return -1;
            };
            const int expectBedW = layoutWidth(bedLayout);
            if (expectBedW < 0)
                return makeError("unknown_layout", "unknown bed layout: " + bedLayout,
                                 "known: 5.1, 7.1, 7.1.2, 7.1.4, 9.1.6, 22.2");
#ifdef REAPER_MCP_HAVE_SDK
            const int rendererTrack = reqInt(a, "rendererTrack");
            MediaTrack* rt = GetTrack(kCur, rendererTrack);
            if (!rt) return makeError("bad_renderer_track",
                                      "rendererTrack out of range: " + std::to_string(rendererTrack), "");
            Json warnings = Json::array();
            int bedTrack = -1, bedW = 0;
            if (haveBedArg) {
                bedTrack = a["bedTrack"].get<int>();
                MediaTrack* bt = GetTrack(kCur, bedTrack);
                if (!bt) return makeError("bad_bed_track",
                                          "bedTrack out of range: " + std::to_string(bedTrack), "");
                bedW = trackChannels(bt);
                if (bedW != expectBedW)
                    warnings.push_back("bed track has " + std::to_string(bedW) + " ch but " + bedLayout +
                                       " is " + std::to_string(expectBedW) + " ch — using the actual track width");
            }
            const bool haveBed = (bedTrack >= 0 && bedW > 0);

            // Object tracks: explicit list, else auto-discover "Object N" mono tracks in N order.
            std::vector<int> objTracks;
            bool discovered = false;
            if (a.contains("objectTracks") && a["objectTracks"].is_array() && !a["objectTracks"].empty()) {
                for (const auto& o : a["objectTracks"]) if (o.is_number()) objTracks.push_back(o.get<int>());
            } else {
                discovered = true;
                std::vector<std::pair<int, int>> found;  // (N, trackIdx)
                const int nt = CountTracks(kCur);
                for (int i = 0; i < nt; ++i) {
                    MediaTrack* t = GetTrack(kCur, i);
                    if (!t) continue;
                    const std::string nm = trackNameStr(t);
                    if (nm.rfind("Object ", 0) != 0) continue;
                    const std::string tail = nm.substr(7);
                    size_t p = 0;
                    while (p < tail.size() && std::isdigit((unsigned char)tail[p])) ++p;
                    if (p == 0) continue;
                    bool blank = true;
                    for (size_t q = p; q < tail.size(); ++q) if (tail[q] != ' ') { blank = false; break; }
                    if (blank) found.push_back({std::stoi(tail.substr(0, p)), i});
                }
                std::sort(found.begin(), found.end());
                for (const auto& f : found) objTracks.push_back(f.second);
                if (objTracks.empty())
                    warnings.push_back("no objectTracks given and no 'Object N' tracks found — wiring the bed only");
            }
            const int objectCount = (int)objTracks.size();
            for (int oi : objTracks)
                if (!GetTrack(kCur, oi)) return makeError("bad_object_track",
                                                          "objectTracks out of range: " + std::to_string(oi), "");

            // Constraints — fail closed on fatal.
            auto cons = sl::checkConstraints(bedW, objectCount, haveBed);
            Json consJson = Json::array();
            for (const auto& c : cons)
                consJson.push_back(Json{{"code", c.code}, {"message", c.message}, {"fatal", c.fatal}});
            if (sl::hasFatal(cons)) {
                std::string firstFatal;
                for (const auto& c : cons) if (c.fatal) { firstFatal = c.message; break; }
                Json e = makeError("constraint_violation", firstFatal,
                    "reduce the object/channel count to fit the Dolby Atmos master (<=128 ch, <=118 objects)");
                e["constraints"] = consJson;
                return e;
            }
            for (const auto& c : cons) if (!c.fatal) warnings.push_back(c.message);

            const auto roster = sl::buildRoster(bedTrack, bedW, objTracks);
            const int totalCh = sl::rosterTotalChannels(bedW, objectCount, haveBed);

            // Read the renderer bus's existing receives.
            std::vector<sl::ExistingSend> existing;
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
            }
            sl::ReconResult recon = sl::reconcile(roster, existing);
            sl::ReconCounts counts = sl::countActions(recon);

            Json inputs = Json::array();
            for (const auto& e : roster)
                inputs.push_back(Json{{"inputId", sl::inputId1(e)},
                                      {"kind", e.kind == sl::Kind::Bed ? "bed" : "object"},
                                      {"sourceTrack", e.sourceTrack}, {"objectNumber", e.objectNumber},
                                      {"rendererChannel", e.rendererChan0 + 1}, {"width", e.width}});
            Json actions = Json::array();
            for (const auto& e : recon.entries)
                actions.push_back(Json{{"kind", e.kind == sl::Kind::Bed ? "bed" : "object"},
                                       {"sourceTrack", e.sourceTrack}, {"objectNumber", e.objectNumber},
                                       {"rendererChannel", e.rendererChan0 + 1}, {"width", e.width},
                                       {"action", sl::actionName(e.action)}, {"sendIndex", e.sendIndex}});
            Json strays = Json::array();
            for (const auto& s : recon.strays)
                strays.push_back(Json{{"sendIndex", s.sendIndex}, {"sourceTrack", s.sourceTrack},
                                      {"rendererChannel", s.dstOff + 1}, {"width", s.width},
                                      {"action", prune ? "prune" : "kept"}});
            Json countsJson = Json{{"reuse", counts.reuse}, {"add", counts.add}, {"fix", counts.fix},
                                   {"stray", counts.stray}, {"pruned", prune ? counts.stray : 0}};

            auto report = [&](bool applied, int prunedActual) -> Json {
                Json c = countsJson;
                if (applied) c["pruned"] = prunedActual;
                return Json{{"ok", true}, {"dryRun", !applied}, {"rendererTrack", rendererTrack},
                            {"bedLayout", bedLayout}, {"bedTrack", bedTrack}, {"bedChannels", bedW},
                            {"objectCount", objectCount}, {"totalChannels", totalCh},
                            {"discovered", discovered}, {"inputs", inputs}, {"actions", actions},
                            {"strays", strays}, {"counts", c}, {"constraints", consJson},
                            {"warnings", warnings}};
            };
            if (dryRun) return report(false, 0);

            // Apply: size the bus, then fixes -> prunes(desc) -> adds, in one undo block.
            Undo_BeginBlock2(kCur);
            if (trackChannels(rt) < totalCh)
                SetMediaTrackInfo_Value(rt, "I_NCHAN", (double)clampChannels(totalCh));
            const bool hasDb = a.contains("db") && a["db"].is_number();
            const double vol = hasDb ? dbToGain(a["db"].get<double>()) : 1.0;
            for (const auto& e : recon.entries) {
                if (e.action != sl::Action::Fix) continue;
                sl::ChanEnc enc = sl::encodeSend(0, e.width, e.rendererChan0, false);
                SetTrackSendInfo_Value(rt, -1, e.sendIndex, "I_SRCCHAN", (double)enc.src);
                SetTrackSendInfo_Value(rt, -1, e.sendIndex, "I_DSTCHAN", (double)enc.dst);
            }
            int pruned = 0;
            if (prune && !recon.strays.empty()) {
                std::vector<int> idx;
                for (const auto& s : recon.strays) idx.push_back(s.sendIndex);
                std::sort(idx.begin(), idx.end(), [](int x, int y) { return x > y; });
                for (int si : idx) if (RemoveTrackSend(rt, -1, si)) ++pruned;
            }
            for (const auto& e : recon.entries) {
                if (e.action != sl::Action::Add) continue;
                MediaTrack* st = GetTrack(kCur, e.sourceTrack);
                if (!st) continue;
                int si = CreateTrackSend(st, rt);
                if (si < 0) continue;
                sl::ChanEnc enc = sl::encodeSend(0, e.width, e.rendererChan0, false);
                SetTrackSendInfo_Value(st, 0, si, "I_SRCCHAN", (double)enc.src);
                SetTrackSendInfo_Value(st, 0, si, "I_DSTCHAN", (double)enc.dst);
                if (hasDb) SetTrackSendInfo_Value(st, 0, si, "D_VOL", vol);
            }
            Undo_EndBlock2(kCur, "MCP: orchestrate object send layout", -1);
            return report(true, pruned);
#else
            std::vector<int> objTracks;
            if (a.contains("objectTracks") && a["objectTracks"].is_array())
                for (const auto& o : a["objectTracks"]) if (o.is_number()) objTracks.push_back(o.get<int>());
            const int bedW = haveBedArg ? expectBedW : 0;
            const bool haveBed = haveBedArg;
            const int objectCount = (int)objTracks.size();
            auto cons = sl::checkConstraints(bedW, objectCount, haveBed);
            Json consJson = Json::array();
            for (const auto& c : cons)
                consJson.push_back(Json{{"code", c.code}, {"message", c.message}, {"fatal", c.fatal}});
            const auto roster = sl::buildRoster(haveBedArg ? a["bedTrack"].get<int>() : -1, bedW, objTracks);
            Json inputs = Json::array();
            for (const auto& e : roster)
                inputs.push_back(Json{{"inputId", sl::inputId1(e)},
                                      {"kind", e.kind == sl::Kind::Bed ? "bed" : "object"},
                                      {"sourceTrack", e.sourceTrack}, {"objectNumber", e.objectNumber},
                                      {"rendererChannel", e.rendererChan0 + 1}, {"width", e.width}});
            return Json{{"ok", true}, {"dryRun", true}, {"rendererTrack", reqInt(a, "rendererTrack")},
                        {"bedLayout", bedLayout}, {"bedChannels", bedW}, {"objectCount", objectCount},
                        {"totalChannels", sl::rosterTotalChannels(bedW, objectCount, haveBed)},
                        {"inputs", inputs}, {"constraints", consJson},
                        {"note", "host build: representative roster (no REAPER routing)"}};
#endif
        }});
}

}  // namespace reaper_mcp
