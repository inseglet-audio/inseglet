// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// resources_project.cpp — MCP resource providers for REAPER state.
//
// Three resources a client can list + read by URI (all read on the MAIN THREAD via the queue):
//   reaper://project/state       (static, application/json)  — project + per-track summary snapshot
//   reaper://routing/graph       (static, application/json)  — nodes (tracks/master) + send/hwout edges;
//                                                               the substrate the immersive
//                                                               layer reasons over (beds, pin maps)
//   reaper://track/{index}/chunk (template, text/plain)      — a track's raw .RPP state chunk
//
// Dual-path: native under REAPER_MCP_HAVE_SDK, representative payloads otherwise (so the resource
// protocol is exercised by the host-side test without a running REAPER).

#include <stdexcept>
#include <string>
#include <vector>

#include "../tools/tool_helpers.h"  // SDK MediaTrack helpers + reaper_api.h (min/max #undef) first
#include "../resource_registry.h"

namespace reaper_mcp {

namespace {

// Parse the {index} out of a reaper://track/{index}/chunk URI. Throws on malformed input.
int trackIndexFromChunkUri(const std::string& uri) {
    static const std::string kPrefix = "reaper://track/";
    static const std::string kSuffix = "/chunk";
    if (uri.compare(0, kPrefix.size(), kPrefix) != 0 ||
        uri.size() <= kPrefix.size() + kSuffix.size() ||
        uri.compare(uri.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        throw std::runtime_error("malformed track chunk URI: " + uri);
    const std::string mid = uri.substr(kPrefix.size(), uri.size() - kPrefix.size() - kSuffix.size());
    try {
        size_t consumed = 0;
        const int idx = std::stoi(mid, &consumed);
        if (consumed != mid.size() || idx < 0) throw std::runtime_error("bad index");
        return idx;
    } catch (...) {
        throw std::runtime_error("track chunk URI index is not a non-negative integer: " + uri);
    }
}

#ifdef REAPER_MCP_HAVE_SDK
Json playStateJson() {
    const int ps = GetPlayState();
    return Json{{"raw", ps}, {"playing", (ps & 1) != 0}, {"paused", (ps & 2) != 0},
                {"recording", (ps & 4) != 0}};
}

Json projectStateJson() {
    char nm[1024] = {0};
    GetProjectName(kCur, nm, sizeof(nm));
    const int n = CountTracks(kCur);
    Json tracks = Json::array();
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(kCur, i);
        tracks.push_back(Json{
            {"index", i}, {"name", trackNameStr(t)}, {"guid", trackGuidStr(t)},
            {"channels", (int)GetMediaTrackInfo_Value(t, "I_NCHAN")},
            {"volumeDb", gainToDb(GetMediaTrackInfo_Value(t, "D_VOL"))},
            {"pan", GetMediaTrackInfo_Value(t, "D_PAN")},
            {"muted", GetMediaTrackInfo_Value(t, "B_MUTE") > 0.5},
            {"selected", GetMediaTrackInfo_Value(t, "I_SELECTED") > 0.5}});
    }
    return Json{{"name", nm},
                {"lengthSeconds", GetProjectLength(kCur)},
                {"tempo", Master_GetTempo()},
                {"cursor", GetCursorPosition()},
                {"playState", playStateJson()},
                {"trackCount", n},
                {"tracks", std::move(tracks)}};
}

Json routingGraphJson() {
    MediaTrack* master = GetMasterTrack(kCur);
    const int n = CountTracks(kCur);
    Json nodes = Json::array();
    Json edges = Json::array();
    for (int i = 0; i < n; ++i) {
        MediaTrack* t = GetTrack(kCur, i);
        nodes.push_back(Json{
            {"index", i}, {"name", trackNameStr(t)}, {"guid", trackGuidStr(t)},
            {"channels", (int)GetMediaTrackInfo_Value(t, "I_NCHAN")},
            {"mainSend", GetMediaTrackInfo_Value(t, "B_MAINSEND") > 0.5},
            {"folderDepth", (int)GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH")}});
        // Track-to-track sends (category 0).
        const int ns = GetTrackNumSends(t, 0);
        for (int s = 0; s < ns; ++s) {
            auto* dest = (MediaTrack*)GetSetTrackSendInfo(t, 0, s, "P_DESTTRACK", nullptr);
            const int destIdx = dest ? (CSurf_TrackToID(dest, false) - 1) : -1;
            edges.push_back(Json{
                {"from", i}, {"to", destIdx}, {"kind", "send"}, {"sendIndex", s},
                {"volDb", gainToDb(GetTrackSendInfo_Value(t, 0, s, "D_VOL"))},
                {"pan", GetTrackSendInfo_Value(t, 0, s, "D_PAN")},
                {"muted", GetTrackSendInfo_Value(t, 0, s, "B_MUTE") > 0.5}});
        }
        // Hardware outputs (category 1) — modeled as edges to the synthetic hardware sink (to = -1).
        const int nh = GetTrackNumSends(t, 1);
        for (int h = 0; h < nh; ++h) {
            edges.push_back(Json{
                {"from", i}, {"to", -1}, {"kind", "hwout"}, {"sendIndex", h},
                {"volDb", gainToDb(GetTrackSendInfo_Value(t, 1, h, "D_VOL"))},
                {"pan", GetTrackSendInfo_Value(t, 1, h, "D_PAN")}});
        }
    }
    return Json{{"master", {{"channels", (int)GetMediaTrackInfo_Value(master, "I_NCHAN")}}},
                {"trackCount", n},
                {"nodes", std::move(nodes)},
                {"edges", std::move(edges)}};
}

std::string trackChunkText(int idx) {
    MediaTrack* t = requireTrack(idx);
    std::vector<char> buf(1 << 20, '\0');  // 1 MiB — generous for FX/envelope-heavy tracks
    if (!GetTrackStateChunk(t, buf.data(), (int)buf.size(), false))
        throw std::runtime_error("GetTrackStateChunk failed for track " + std::to_string(idx));
    return std::string(buf.data());
}
#endif  // REAPER_MCP_HAVE_SDK

}  // namespace

void registerProjectResources(ResourceRegistry& reg) {
    reg.add(Resource{
        "reaper://project/state", "Project state snapshot",
        "Project name, length, tempo, play state, and a per-track summary "
        "(name, channels, volume dB, pan, mute, selection).",
        "application/json", /*isTemplate*/ false,
        [](const std::string&) -> std::string {
#ifdef REAPER_MCP_HAVE_SDK
            return projectStateJson().dump(2);
#else
            return Json{{"name", ""}, {"lengthSeconds", 0.0}, {"tempo", 120.0},
                        {"cursor", 0.0},
                        {"playState", {{"raw", 0}, {"playing", false}, {"paused", false},
                                       {"recording", false}}},
                        {"trackCount", 0}, {"tracks", Json::array()}}
                .dump(2);
#endif
        }});

    reg.add(Resource{
        "reaper://routing/graph", "Routing graph",
        "The project's routing graph: nodes (tracks + master, with channel counts and folder/main-send "
        "flags) and edges (track-to-track sends and hardware outputs). The substrate the immersive "
        "layer builds beds and ambisonic pin maps on.",
        "application/json", /*isTemplate*/ false,
        [](const std::string&) -> std::string {
#ifdef REAPER_MCP_HAVE_SDK
            return routingGraphJson().dump(2);
#else
            return Json{{"master", {{"channels", 2}}}, {"trackCount", 0},
                        {"nodes", Json::array()}, {"edges", Json::array()}}
                .dump(2);
#endif
        }});

    reg.add(Resource{
        "reaper://track/{index}/chunk", "Track .RPP state chunk",
        "The raw REAPER project (.RPP) state chunk for the track at {index} (0-based) — the exact "
        "text REAPER serializes to the project file, including FX, sends, and envelopes.",
        "text/plain", /*isTemplate*/ true,
        [](const std::string& uri) -> std::string {
            const int idx = trackIndexFromChunkUri(uri);
#ifdef REAPER_MCP_HAVE_SDK
            return trackChunkText(idx);
#else
            return "<TRACK\n  NAME \"\"\n  # host/fallback: real .RPP chunk requires a running REAPER "
                   "(track " + std::to_string(idx) + ")\n>\n";
#endif
        }});
}

}  // namespace reaper_mcp
