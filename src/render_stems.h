// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// render_stems.h — shared bounded temp-render helpers that render a target (or a set of stems) to a
// temp WAV, read the samples back into a meter::AudioBuffer, delete the temp, and restore every
// RENDER_* field + selection.
//
// Extracted from tools_analysis.cpp's metering path so BOTH the analysis meters
// AND the ADM deliverable authoring (spatial.export_adm) drive the SAME bit-exact stem-render surface —
// one render engine, no drift between "measure the stem" and "author the stem into ADM". Behavior is
// byte-for-byte identical (incl. the RENDER_SETTINGS=3|4 stem-source fix and the $track by-name
// fallback); this file only relocates the code so a second TU can call it.
//
// Include order: pulls render_stats.h (-> tool_helpers.h -> reaper_api.h BEFORE any STL/JSON header,
// SWELL min/max) + ambisonic_meter.h (meter::AudioBuffer / readWavFile). Everything is inline / SDK-
// gated; a host (no-SDK) build sees an empty header.

#pragma once

#include "render_stats.h"       // RENDER_* accessors + snapshot/restore + parseRenderStats + arg helpers
#include "ambisonic_meter.h"    // meter::AudioBuffer + readWavFile

#include <cstdio>               // std::remove
#include <string>
#include <vector>

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK

// Result of a bounded, non-destructive temp-render used to read back samples for the metering DSP.
struct TempRender {
    bool ok = false;
    std::string error, remediation;
    meter::AudioBuffer buf;   // the rendered samples (deleted from disk after reading)
    Json statsFile;           // the parseRenderStats() block (loudness tokens, when measured)
    std::string path;
};

// Render `targetTrack` (>=0) or the master (-1) to a bounded temp WAV, read it into an AudioBuffer,
// delete the temp file, and restore every RENDER_* field + selection. `measureLoudness` picks the
// proven check_deliverable normalize-analysis config (populates RENDER_STATS LUFS/true-peak without
// applying gain to a headroomed master); otherwise a bit-exact render (RENDER_NORMALIZE=0) that
// preserves inter-channel amplitude relationships — required for the ambisonic field, whose direction
// cues would be corrupted by any per-channel brickwall limiting.
inline TempRender renderTargetToTempWav(int targetTrack, int channels, bool measureLoudness,
                                        int boundsFlag, const Json& a) {
    TempRender r;
    if (channels < 1) channels = 2;

    const RenderSnapshot snap = snapshotRender();
    std::string dir = GetResourcePath() ? std::string(GetResourcePath()) : std::string();
    if (dir.empty()) dir = snap.file;  // fall back to the project render dir
    const std::string pattern = std::string("_mcp_meter_tmp_") +
        (targetTrack >= 0 ? ("t" + std::to_string(targetTrack)) : std::string("master"));

    setProjStr("RENDER_FILE", dir);
    setProjStr("RENDER_PATTERN", pattern);
    setProjStr("RENDER_FORMAT", snap.format.empty() ? std::string("evaw") : snap.format);
    setProjNum("RENDER_SRATE", 0);
    setProjNum("RENDER_CHANNELS", channels >= 2 ? channels : 2);
    setProjNum("RENDER_ADDTOPROJ", 0);
    if (measureLoudness) {
        // measure LUFS-I + true peak, apply no gain (unreachable ceiling) — the check_deliverable trick.
        setProjNum("RENDER_NORMALIZE", (double)(1 | 64 | 128 | 256));
        setProjNum("RENDER_NORMALIZE_TARGET", 1.0);
        setProjNum("RENDER_BRICKWALL", 1.0);
    } else {
        setProjNum("RENDER_NORMALIZE", 0);  // bit-exact: no normalize, no brickwall limiting
    }
    setProjNum("RENDER_BOUNDSFLAG", boundsFlag);
    if (a.contains("startPos") && a["startPos"].is_number())
        setProjNum("RENDER_STARTPOS", a["startPos"].get<double>());
    if (a.contains("endPos") && a["endPos"].is_number())
        setProjNum("RENDER_ENDPOS", a["endPos"].get<double>());
    if (targetTrack >= 0) {
        applySelection({targetTrack});
        // The render SOURCE "Stems (selected tracks)" is RENDER_SETTINGS
        // &(1|2) == 3 — NOT 2. A bare 2 is not a valid source and REAPER silently falls back to the
        // MASTER MIX at the forced channel width (which looked right whenever the target was the only
        // audible route to the master). 3 renders the true pre-master stem of the selected bus.
        int settings = 3;                    // stems (selected tracks)
        if (channels > 2) settings |= 4;     // multichannel files
        setProjNum("RENDER_SETTINGS", (double)settings);
    } else {
        setProjNum("RENDER_SETTINGS", 0);    // master mix
    }

    Main_OnCommand(optInt(a, "renderAction", 41824), 0);  // no-dialog render, most-recent settings
    const std::string statsStr = getProjStrBig("RENDER_STATS");
    restoreRender(snap);                     // <-- project restored regardless of what follows

    const Json files = parseRenderStats(statsStr);
    if (files.empty() || !files[0].contains("file")) {
        r.error = "render_no_output";
        r.remediation = "the analysis render produced no file — bound it to audio (boundsFlag/startPos/"
                        "endPos) and ensure a valid project render path/format";
        return r;
    }
    r.statsFile = files[0];
    r.path = files[0]["file"].get<std::string>();
    std::string err;
    if (!meter::readWavFile(r.path, r.buf, err)) {
        std::remove(r.path.c_str());
        r.error = "render_read_failed";
        r.remediation = "could not read the rendered temp WAV (" + err + ")";
        return r;
    }
    std::remove(r.path.c_str());             // hygiene: metering leaves no artifact behind
    r.ok = true;
    return r;
}

// Multi-stem sibling of renderTargetToTempWav: render SEVERAL tracks as multichannel stems in ONE
// bounded pass (the check_deliverable per-bed machinery), read each WAV into an AudioBuffer, delete
// the temp files, restore everything. `tracks` MUST be sorted ascending — REAPER emits selected-track
// stems top-to-bottom, so files are matched to tracks by order (with a by-NAME fallback via `labels`
// when the emitted file count differs — the per-bed convention). The pattern
// must use the per-bed `$track` (track NAME) wildcard; a `$tracknumber`-only pattern collapsed
// multiple stems onto a single reported file.
struct MultiStemRender {
    bool ok = false;
    std::string error, remediation;
    std::vector<meter::AudioBuffer> bufs;   // parallel to `tracks`
    std::vector<bool> haveBuf;
    size_t fileCount = 0;                   // stem files REAPER actually reported
};
inline MultiStemRender renderTracksToTempWavs(const std::vector<int>& tracks,
                                              const std::vector<std::string>& labels, int maxCh,
                                              bool measureLoudness, int boundsFlag, const Json& a) {
    MultiStemRender r;
    const RenderSnapshot snap = snapshotRender();
    std::string dir = GetResourcePath() ? std::string(GetResourcePath()) : std::string();
    if (dir.empty()) dir = snap.file;

    setProjStr("RENDER_FILE", dir);
    setProjStr("RENDER_PATTERN", "_mcp_meter_stem - $track");
    setProjStr("RENDER_FORMAT", snap.format.empty() ? std::string("evaw") : snap.format);
    setProjNum("RENDER_SRATE", 0);
    setProjNum("RENDER_CHANNELS", maxCh >= 2 ? maxCh : 2);
    setProjNum("RENDER_ADDTOPROJ", 0);
    if (measureLoudness) {
        setProjNum("RENDER_NORMALIZE", (double)(1 | 64 | 128 | 256));
        setProjNum("RENDER_NORMALIZE_TARGET", 1.0);
        setProjNum("RENDER_BRICKWALL", 1.0);
    } else {
        setProjNum("RENDER_NORMALIZE", 0);   // bit-exact stems for the C++ loudness path
    }
    setProjNum("RENDER_BOUNDSFLAG", boundsFlag);
    if (a.contains("startPos") && a["startPos"].is_number())
        setProjNum("RENDER_STARTPOS", a["startPos"].get<double>());
    if (a.contains("endPos") && a["endPos"].is_number())
        setProjNum("RENDER_ENDPOS", a["endPos"].get<double>());
    applySelection(tracks);
    setProjNum("RENDER_SETTINGS", (double)(3 | 4));  // stems (selected tracks) = &(1|2), multichannel files

    Main_OnCommand(optInt(a, "renderAction", 41824), 0);
    const std::string statsStr = getProjStrBig("RENDER_STATS");
    restoreRender(snap);                     // <-- project restored regardless

    const Json files = parseRenderStats(statsStr);
    if (files.empty()) {
        r.error = "render_no_output";
        r.remediation = "the stem render produced no files — bound it to audio (boundsFlag/startPos/"
                        "endPos) and ensure a valid project render path/format";
        return r;
    }
    r.bufs.resize(tracks.size());
    r.haveBuf.assign(tracks.size(), false);
    std::vector<std::string> paths;
    for (const auto& f : files) {
        const std::string path = f.is_object() ? f.value("file", std::string()) : std::string();
        if (!path.empty()) paths.push_back(path);
    }
    r.fileCount = paths.size();
    if (paths.size() == tracks.size()) {
        // top-to-bottom order matches the ascending track list
        for (size_t i = 0; i < paths.size(); ++i) {
            std::string err;
            if (meter::readWavFile(paths[i], r.bufs[i], err)) r.haveBuf[i] = true;
        }
    } else {
        // count mismatch: match each stem to a file by its track NAME in the file path (per-bed idiom)
        for (size_t i = 0; i < tracks.size() && i < labels.size(); ++i) {
            const std::string lname = toLower(labels[i]);
            if (lname.empty()) continue;
            for (const std::string& path : paths) {
                if (toLower(path).find(lname) != std::string::npos) {
                    std::string err;
                    if (meter::readWavFile(path, r.bufs[i], err)) r.haveBuf[i] = true;
                    break;
                }
            }
        }
    }
    for (const std::string& path : paths) std::remove(path.c_str());  // every temp stem is deleted
    r.ok = true;
    return r;
}

#endif  // REAPER_MCP_HAVE_SDK

}  // namespace reaper_mcp
