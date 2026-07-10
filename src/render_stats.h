// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// render_stats.h — shared render-config + loudness-measurement helpers.
//
// Extracted from tools_spatial.cpp's batch-(d) render path so BOTH the render verb
// (spatial.render_deliverables) and the analysis verb (analysis.check_deliverable) drive the
// same RENDER_* configuration surface and the same RENDER_STATS parser — one loudness engine, no
// drift between "render it" and "measure it against a spec".
//
// Two layers:
//   * SDK-free (host + SDK identical): friendly-format -> sink 4CC, the render-target predicate, the
//     default RENDER_PATTERN, ';'-list splitting, and parseRenderStats() (REAPER's native
//     integrated-LUFS / true-peak / LRA engine as structured JSON). All unit-testable with no REAPER.
//   * SDK-only (REAPER_MCP_HAVE_SDK): the GetSetProjectInfo[_String] accessors, track-selection
//     save/apply, and a full RENDER_* snapshot/restore so a measurement leaves the project exactly as
//     it was found (a render is not itself undoable — we hand-restore).
//
// Include order: this pulls tool_helpers.h -> reaper_api.h BEFORE any STL/JSON header (SWELL min/max),
// so include it the same way tool TUs include tool_helpers.h. Everything is inline/header-only.

#pragma once

#include "tools/tool_helpers.h"  // reaper_api.h (SWELL de-fang) + <json> + toLower + kCur + arg helpers

#include <string>
#include <vector>

namespace reaper_mcp {

// ---- SDK-free: format ids, target predicate, pattern, list-splitting, RENDER_STATS parsing --------

// Friendly format name -> REAPER render sink fourCC. The SDK accepts a bare 4-byte code as
// RENDER_FORMAT ("evaw" = WAV, "l3pm" = MP3) meaning "use that sink's default settings", so we never
// hand-synthesize an opaque blob. An explicit 4CC or a captured base64 blob passes through verbatim.
inline std::string sinkFourCC(const std::string& fmt) {
    const std::string f = toLower(fmt);
    if (f == "wav" || f == "evaw") return "evaw";
    if (f == "mp3" || f == "l3pm") return "l3pm";
    return fmt;
}

// The in-box render masters (multichannel bed / ambiX B-format / binaural downmix). atmos-send-layout
// is orchestration, not a render, and is handled separately.
inline bool isRenderTarget(const std::string& t) {
    return t == "multichannel" || t == "ambix" || t == "binaural";
}

// A deterministic default RENDER_PATTERN per target ($project expands to the project name).
inline std::string defaultPattern(const std::string& tgt) { return std::string("$project - ") + tgt; }

// Split a REAPER ';'-separated list (RENDER_TARGETS) into a trimmed JSON array of non-empty entries.
inline Json splitSemis(const std::string& s) {
    Json arr = Json::array();
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(';', start);
        std::string part = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
        start = (p == std::string::npos) ? s.size() + 1 : p + 1;
        size_t b = part.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = part.find_last_not_of(" \t\r\n");
        arr.push_back(part.substr(b, e - b + 1));
    }
    return arr;
}

// Normalize a RENDER_STATS token key: uppercase, drop non-alphanumerics so "LUFS-I" == "LUFSI".
inline std::string normStatKey(const std::string& k) {
    std::string o;
    for (char c : k) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) o.push_back((char)std::toupper(u));
    }
    return o;
}

// Parse REAPER's RENDER_STATS string into per-file objects. Format is a ';'-separated KEY:VALUE list;
// a FILE:<path> token opens a new file block. We map the recognized loudness keys to normalized fields
// (lufsIntegrated / lufsMomentaryMax / lufsShortTermMax / truePeak / samplePeak / rms / loudnessRange)
// AND keep every raw token under "stats", tolerant of build-to-build spelling (LUFSI vs LUFS-I, TPEAK
// vs TRUEPEAK). This is the REAPER-native loudness engine; ffmpeg ebur128 cross-checks it in verify.
inline Json parseRenderStats(const std::string& raw) {
    Json files = Json::array();
    Json cur = Json::object();
    Json curStats = Json::object();
    bool open = false;
    auto flush = [&]() {
        if (open) { cur["stats"] = curStats; files.push_back(cur); }
        cur = Json::object();
        curStats = Json::object();
        open = false;
    };
    size_t start = 0;
    while (start <= raw.size()) {
        size_t semi = raw.find(';', start);
        std::string tok = (semi == std::string::npos) ? raw.substr(start) : raw.substr(start, semi - start);
        start = (semi == std::string::npos) ? raw.size() + 1 : semi + 1;
        if (tok.empty()) continue;
        size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        std::string key = tok.substr(0, colon), val = tok.substr(colon + 1);
        std::string nk = normStatKey(key);
        if (nk == "FILE") { flush(); cur["file"] = val; open = true; continue; }
        if (!open) continue;  // ignore any tokens before the first FILE block
        curStats[key] = val;
        double num = 0.0; bool isNum = false;
        try { size_t pos = 0; num = std::stod(val, &pos); isNum = (pos > 0); } catch (...) { isNum = false; }
        if (!isNum) continue;
        if (nk == "LUFSI" || nk == "ILUFS")        cur["lufsIntegrated"] = num;
        else if (nk == "LUFSMMAX" || nk == "LUFSM") cur["lufsMomentaryMax"] = num;   // real token = LUFSMMAX
        else if (nk == "LUFSSMAX" || nk == "LUFSS") cur["lufsShortTermMax"] = num;   // real token = LUFSSMAX
        else if (nk == "TPEAK" || nk == "TRUEPEAK" || nk == "TP") cur["truePeak"] = num;
        else if (nk == "PEAK")                     cur["samplePeak"] = num;
        else if (nk == "RMS")                      cur["rms"] = num;
        else if (nk == "LRA")                      cur["loudnessRange"] = num;
    }
    flush();
    return files;
}

#ifdef REAPER_MCP_HAVE_SDK
// ---- SDK-only: RENDER_* project-info accessors + selection + snapshot/restore ---------------------

// GetSetProjectInfo(_String) is the whole render-config surface: numeric keys (RENDER_SETTINGS/
// CHANNELS/SRATE/BOUNDSFLAG/NORMALIZE/…) and string keys (RENDER_FILE/PATTERN/FORMAT/TARGETS/STATS).
// A generous buffer covers the multi-file RENDER_TARGETS/STATS reads.
inline std::string getProjStrBig(const char* key) {
    std::vector<char> buf(1 << 16, 0);
    GetSetProjectInfo_String(nullptr, key, buf.data(), false);
    return std::string(buf.data());
}
inline void setProjStr(const char* key, const std::string& v) {
    std::vector<char> nb(v.begin(), v.end());
    nb.push_back('\0');
    GetSetProjectInfo_String(nullptr, key, nb.data(), true);
}
inline double getProjNum(const char* key) { return GetSetProjectInfo(nullptr, key, 0.0, false); }
inline void setProjNum(const char* key, double v) { GetSetProjectInfo(nullptr, key, v, true); }

inline int trackChannels(MediaTrack* t) { return t ? (int)GetMediaTrackInfo_Value(t, "I_NCHAN") : 0; }

inline std::vector<int> saveSelectedTracks() {
    std::vector<int> sel;
    const int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(nullptr, i);
        if (t && GetMediaTrackInfo_Value(t, "I_SELECTED") > 0.5) sel.push_back(i);
    }
    return sel;
}
inline void applySelection(const std::vector<int>& sel) {
    const int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(nullptr, i);
        if (t) SetMediaTrackInfo_Value(t, "I_SELECTED", 0.0);
    }
    for (int i : sel) {
        MediaTrack* t = GetTrack(nullptr, i);
        if (t) SetMediaTrackInfo_Value(t, "I_SELECTED", 1.0);
    }
}

// Snapshot of every RENDER_* field + the track selection we mutate, so a render leaves the project
// exactly as we found it (a render is not itself undoable — we hand-restore instead).
struct RenderSnapshot {
    std::string file, pattern, format;
    double settings = 0, channels = 0, srate = 0, bounds = 0, startpos = 0, endpos = 0,
           addtoproj = 0, normalize = 0, normtarget = 0, brickwall = 0;
    std::vector<int> selection;
};
inline RenderSnapshot snapshotRender() {
    RenderSnapshot s;
    s.file = getProjStrBig("RENDER_FILE");
    s.pattern = getProjStrBig("RENDER_PATTERN");
    s.format = getProjStrBig("RENDER_FORMAT");
    s.settings = getProjNum("RENDER_SETTINGS");
    s.channels = getProjNum("RENDER_CHANNELS");
    s.srate = getProjNum("RENDER_SRATE");
    s.bounds = getProjNum("RENDER_BOUNDSFLAG");
    s.startpos = getProjNum("RENDER_STARTPOS");
    s.endpos = getProjNum("RENDER_ENDPOS");
    s.addtoproj = getProjNum("RENDER_ADDTOPROJ");
    s.normalize = getProjNum("RENDER_NORMALIZE");
    s.normtarget = getProjNum("RENDER_NORMALIZE_TARGET");
    s.brickwall = getProjNum("RENDER_BRICKWALL");
    s.selection = saveSelectedTracks();
    return s;
}
inline void restoreRender(const RenderSnapshot& s) {
    setProjStr("RENDER_FILE", s.file);
    setProjStr("RENDER_PATTERN", s.pattern);
    setProjStr("RENDER_FORMAT", s.format);
    setProjNum("RENDER_SETTINGS", s.settings);
    setProjNum("RENDER_CHANNELS", s.channels);
    setProjNum("RENDER_SRATE", s.srate);
    setProjNum("RENDER_BOUNDSFLAG", s.bounds);
    setProjNum("RENDER_STARTPOS", s.startpos);
    setProjNum("RENDER_ENDPOS", s.endpos);
    setProjNum("RENDER_ADDTOPROJ", s.addtoproj);
    setProjNum("RENDER_NORMALIZE", s.normalize);
    setProjNum("RENDER_NORMALIZE_TARGET", s.normtarget);
    setProjNum("RENDER_BRICKWALL", s.brickwall);
    applySelection(s.selection);
}
#endif  // REAPER_MCP_HAVE_SDK

}  // namespace reaper_mcp
