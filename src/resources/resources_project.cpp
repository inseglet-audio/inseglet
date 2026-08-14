// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// resources_project.cpp — MCP resource providers for REAPER state.
//
// Four resources a client can list + read by URI (all read on the MAIN THREAD via the queue):
//   reaper://project/state       (static, application/json)  — project + per-track summary snapshot
//   reaper://routing/graph       (static, application/json)  — nodes (tracks/master) + send/hwout edges;
//                                                               the substrate the immersive
//                                                               layer reasons over (beds, pin maps)
//   reaper://track/{index}/chunk (template, text/plain)      — a track's raw .RPP state chunk
//   reaper://track/{t}/item/{i}/take/{k}/source
//                                (template, application/json) — a take's source-validity panel,
//                                                               API rows + SDK vtable rows
//
// Dual-path: native under REAPER_MCP_HAVE_SDK, representative payloads otherwise (so the resource
// protocol is exercised by the host-side test without a running REAPER).

#include <cstring>
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


// --- the build-side source-validity readout -----------------------------------------------------
// Parse reaper://track/{trackIndex}/item/{itemIndex}/take/{takeIndex}/source.
struct SourceUriParts {
    int track;
    int item;
    int take;
};

SourceUriParts sourceUriParts(const std::string& uri) {
    static const std::string p0 = "reaper://track/", p1 = "/item/", p2 = "/take/", p3 = "/source";
    const auto bad = [&uri]() -> SourceUriParts {
        throw std::runtime_error("malformed take source URI: " + uri);
    };
    if (uri.compare(0, p0.size(), p0) != 0) return bad();
    const size_t a = uri.find(p1, p0.size());
    if (a == std::string::npos) return bad();
    const size_t b = uri.find(p2, a + p1.size());
    if (b == std::string::npos) return bad();
    const size_t c = uri.find(p3, b + p2.size());
    if (c == std::string::npos || c + p3.size() != uri.size()) return bad();

    const auto num = [&uri](size_t from, size_t to) -> int {
        const std::string s = uri.substr(from, to - from);
        size_t consumed = 0;
        int v = 0;
        try {
            v = std::stoi(s, &consumed);
        } catch (...) {
            throw std::runtime_error("take source URI index is not an integer: " + uri);
        }
        if (consumed != s.size() || v < 0)
            throw std::runtime_error("take source URI index is not a non-negative integer: " + uri);
        return v;
    };
    return SourceUriParts{num(p0.size(), a), num(a + p1.size(), b), num(b + p2.size(), c)};
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

// The panel: the API-layer rows and the VTABLE-layer rows over ONE
// PCM_source*, read in a single pass.
//
// The vtable rows are the point. PCM_source::IsAvailable() is a pure virtual on every concrete
// source and it is NOT in the ReaScript API surface at any version — which is why the accessor
// the audio accessor, the saved .rpp and the live track chunk each measured a null on
// source validity. They were not looking in the wrong place on the right surface; they were on a
// surface where the datum does not exist.
//
// `vtable.controlPassed` is the instrument's OWN control, read from the same pointer as the datum:
// if the SDK header and the running REAPER disagree about the vtable layout, every vtable figure
// here is undefined behaviour. The control is what says so, instead of the figures quietly lying.
Json takeSourceJson(int trackIdx, int itemIdx, int takeIdx) {
    MediaItem* it = requireItem(trackIdx, itemIdx);
    MediaItem_Take* tk = GetMediaItemTake(it, takeIdx);
    if (!tk)
        throw std::runtime_error("take index out of range on track " + std::to_string(trackIdx) +
                                 " item " + std::to_string(itemIdx) + ": " +
                                 std::to_string(takeIdx));

    PCM_source* src = GetMediaItemTake_Source(tk);
    Json out{{"trackIndex", trackIdx},
             {"itemIndex", itemIdx},
             {"takeIndex", takeIdx},
             {"sourcePresent", src != nullptr}};
    if (!src) return out;

    // --- API layer -----------------------------------------------------------------------------
    char fn[4096] = {0};
    GetMediaSourceFileName(src, fn, (int)sizeof(fn));
    char ty[128] = {0};
    GetMediaSourceType(src, ty, (int)sizeof(ty));
    bool lengthIsQN = false;
    const double apiLen = GetMediaSourceLength(src, &lengthIsQN);
    out["api"] = Json{{"fileName", fn},
                      {"type", ty},
                      {"numChannels", GetMediaSourceNumChannels(src)},
                      {"sampleRate", GetMediaSourceSampleRate(src)},
                      {"length", apiLen},
                      {"lengthIsQN", lengthIsQN}};

    // --- vtable layer, and its own control ------------------------------------------------------
    const char* vtType = src->GetType();
    const bool vtableOk = vtType != nullptr && vtType[0] != '\0' && strnlen(vtType, 65) < 64;
    if (!vtableOk) {
        out["vtable"] = Json{
            {"controlPassed", false},
            {"error", "vtable_control_failed"},
            {"detail", "PCM_source::GetType() returned null or an implausible string; the SDK "
                       "header and the running REAPER disagree about the vtable layout, so no "
                       "vtable row is believable"}};
        return out;
    }
    out["vtable"] = Json{{"controlPassed", true},
                         {"type", vtType},
                         {"isAvailable", src->IsAvailable()},
                         {"numChannels", src->GetNumChannels()},
                         {"sampleRate", src->GetSampleRate()},
                         {"length", src->GetLength()}};
    return out;
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

    reg.add(Resource{
        "reaper://track/{trackIndex}/item/{itemIndex}/take/{takeIndex}/source",
        "Take source validity panel",
        "Per-source readout for one take: the ReaScript-API rows (file name, type, channel count, "
        "sample rate, length) alongside the SDK vtable rows (PCM_source::IsAvailable, GetType, "
        "GetNumChannels, GetSampleRate, GetLength). IsAvailable is not exposed by the ReaScript API "
        "at any version, so this is the only route to a take's source-validity state — whether its "
        "media is online, offline, or missing. Read vtable.controlPassed before any vtable row: "
        "when it is false the SDK header and the running REAPER disagree about the vtable layout "
        "and no vtable row is believable.",
        "application/json", /*isTemplate*/ true,
        [](const std::string& uri) -> std::string {
            const SourceUriParts p = sourceUriParts(uri);
#ifdef REAPER_MCP_HAVE_SDK
            return takeSourceJson(p.track, p.item, p.take).dump(2);
#else
            return Json{{"trackIndex", p.track},
                        {"itemIndex", p.item},
                        {"takeIndex", p.take},
                        {"sourcePresent", false},
                        {"note", "host/fallback: a real source panel requires a running REAPER"}}
                .dump(2);
#endif
        }});
}

}  // namespace reaper_mcp
