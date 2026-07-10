// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_envelopes.cpp — track envelopes / automation: list, add/read/delete points, range clear,
// automation mode, and autoActivate on add_point. Activation is chunk-first (the Get/SetTrackStateChunk
// substrate): a minimal active+visible envelope block SEEDED WITH ONE POINT at the track's current
// value (REAPER prunes/rejects a point-less envelope block; the seed
// makes activation sonically a no-op, matching what the UI does). If the chunk path fails it is
// REVERTED and we fall back to REAPER's own toggle actions for the two commonest envelopes
// (40406 Volume / 40407 Pan), selection-isolated and restored like track.freeze.
//
// envelope.add_point writes to an ALREADY-ACTIVE track envelope (looked up by name, e.g. "Volume",
// "Pan", or an FX-param envelope). Values are in the envelope's native units (Volume = linear gain,
// Pan = -1..1), matching REAPER's InsertEnvelopePoint. Dual-path; mutations single-undo.

#include <cstdio>
#include <string>
#include <vector>

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK
namespace {
std::string envName(TrackEnvelope* e) {
    char buf[256] = {0};
    if (e) GetEnvelopeName(e, buf, sizeof(buf));
    return buf;
}

// Display name -> track-chunk envelope block name, for autoActivate. Only REAPER's built-in
// track envelopes can be chunk-activated this way (FX-param envelopes need the FX+param context).
const char* envChunkName(const std::string& display) {
    if (display == "Volume") return "VOLENV2";
    if (display == "Pan") return "PANENV2";
    if (display == "Width") return "WIDTHENV2";
    if (display == "Mute") return "MUTEENV";
    if (display == "Volume (Pre-FX)") return "VOLENV";
    if (display == "Pan (Pre-FX)") return "PANENV";
    if (display == "Width (Pre-FX)") return "WIDTHENV";
    if (display == "Trim Volume") return "VOLENV3";
    return nullptr;
}

// Read a track's full state chunk, growing the buffer until it fits (mirrors tools_core.cpp).
std::string readTrackChunkEnv(MediaTrack* t) {
    std::size_t cap = 1u << 20;
    for (int tries = 0; tries < 6; ++tries) {
        std::vector<char> buf(cap, '\0');
        if (GetTrackStateChunk(t, buf.data(), (int)buf.size(), false)) {
            std::string s(buf.data());
            if (s.size() + 1 < cap) return s;
        }
        cap <<= 1;
    }
    std::vector<char> buf(cap, '\0');
    GetTrackStateChunk(t, buf.data(), (int)buf.size(), false);
    return std::string(buf.data());
}

// Seed value for a pristine envelope's first point — the envelope-native encoding of the track's
// CURRENT state, so activation is sonically a no-op (exactly what the UI toggle does). Pan-family
// envelope values are NEGATED relative to D_PAN (SWS convention); mute envelopes use 1 = play.
double envSeedValue(MediaTrack* t, const char* chunkName) {
    const std::string c = chunkName;
    if (c == "VOLENV2" || c == "VOLENV") return GetMediaTrackInfo_Value(t, "D_VOL");
    if (c == "VOLENV3") return 1.0;  // trim: unity
    if (c == "PANENV2" || c == "PANENV") return -GetMediaTrackInfo_Value(t, "D_PAN");
    if (c == "WIDTHENV2" || c == "WIDTHENV") return GetMediaTrackInfo_Value(t, "D_WIDTH");
    if (c == "MUTEENV") return GetMediaTrackInfo_Value(t, "B_MUTE") > 0.5 ? 0.0 : 1.0;
    return 1.0;
}

// Activate a built-in track envelope. Chunk-first: insert a minimal active+visible envelope block
// SEEDED with one point at the current value (REAPER prunes/rejects a point-less envelope block),
// placed before the first <FXCHAIN / <ITEM sub-block (envelope blocks precede both in REAPER's
// serialization) or before the final closing '>'. On any chunk-path failure the original chunk is
// RESTORED and, for Volume/Pan, we fall back to REAPER's own toggle actions (selection-isolated).
// 'how' reports the mechanism ("chunk" / "action") or the failure stage for the error message.
TrackEnvelope* activateBuiltinEnvelope(MediaTrack* t, const std::string& display, std::string& how) {
    const char* chunkName = envChunkName(display);
    if (!chunkName) { how = "not a built-in track envelope"; return nullptr; }
    const std::string cur = readTrackChunkEnv(t);
    how = "chunk read failed";
    if (!cur.empty()) {
        char pt[64];
        std::snprintf(pt, sizeof(pt), "PT 0 %.8f 0\n", envSeedValue(t, chunkName));
        const std::string block = std::string("<") + chunkName +
            "\nACT 1 -1\nVIS 1 1 1\nLANEHEIGHT 0 0\nARM 0\nDEFSHAPE 0 -1 -1\n" + pt + ">\n";
        std::size_t at = std::string::npos;
        for (const char* marker : {"\n<FXCHAIN", "\n<ITEM"}) {
            const std::size_t p = cur.find(marker);
            if (p != std::string::npos && p < at) at = p;
        }
        std::string next;
        if (at != std::string::npos) {
            next = cur.substr(0, at + 1) + block + cur.substr(at + 1);
        } else {
            const std::size_t close = cur.rfind('>');
            if (close != std::string::npos) next = cur.substr(0, close) + block + cur.substr(close);
        }
        if (!next.empty()) {
            if (SetTrackStateChunk(t, next.c_str(), false)) {
                if (TrackEnvelope* e = GetTrackEnvelopeByName(t, display.c_str())) {
                    how = "chunk";
                    return e;
                }
                SetTrackStateChunk(t, cur.c_str(), false);  // envelope pruned — leave no half-state
                how = "chunk write applied but envelope was pruned";
            } else {
                how = "chunk write rejected by REAPER";
            }
        }
    }
    // Fallback: REAPER's own toggle actions (verified, selection-scoped) for the two commonest
    // envelopes. Selection is isolated to the target track and restored, as in track.freeze.
    int cmd = 0;
    if (display == "Volume") cmd = 40406;      // Track: toggle track volume envelope visible
    else if (display == "Pan") cmd = 40407;    // Track: toggle track pan envelope visible
    if (cmd) {
        std::vector<MediaTrack*> prevSel;
        const int selN = CountSelectedTracks(kCur);
        prevSel.reserve((size_t)(selN > 0 ? selN : 0));
        for (int i = 0; i < selN; ++i) prevSel.push_back(GetSelectedTrack(kCur, i));
        SetOnlyTrackSelected(t);
        Main_OnCommand(cmd, 0);
        for (int i = 0, n = CountTracks(kCur); i < n; ++i)
            SetTrackSelected(GetTrack(kCur, i), false);
        for (MediaTrack* p : prevSel)
            if (p && ValidatePtr2(kCur, p, "MediaTrack*")) SetTrackSelected(p, true);
        if (TrackEnvelope* e = GetTrackEnvelopeByName(t, display.c_str())) {
            how = "action";
            return e;
        }
        how += " + toggle-action fallback found no envelope";
    }
    return nullptr;
}
}  // namespace
#endif

void registerEnvelopeTools(ToolRegistry& reg) {
    // ---- envelope.list (read-only) ----
    reg.add(Tool{
        "envelope.list", "List a track's active envelopes with index, name, and point count.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},
            "envelopeCount":{"type":"integer"},"envelopes":{"type":"array","items":{"type":"object",
                "properties":{"index":{"type":"integer"},"name":{"type":"string"},
                "pointCount":{"type":"integer"}}}}},
            "required":["track","envelopeCount","envelopes"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int n = CountTrackEnvelopes(t);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                TrackEnvelope* e = GetTrackEnvelope(t, i);
                arr.push_back(Json{{"index", i}, {"name", envName(e)},
                                   {"pointCount", CountEnvelopePoints(e)}});
            }
            return Json{{"track", idx}, {"envelopeCount", n}, {"envelopes", std::move(arr)}};
#else
            return Json{{"track", idx}, {"envelopeCount", 0}, {"envelopes", Json::array()}};
#endif
        }});

    // ---- envelope.add_point (mutating; undo-wrapped) ----
    reg.add(Tool{
        "envelope.add_point",
        "Insert an automation point on a track envelope (looked up by name, e.g. 'Volume', 'Pan'). "
        "'value' is in the envelope's native units (Volume = linear gain, Pan = -1..1). The envelope "
        "must already be active on the track; if it isn't, pass autoActivate:true to activate a "
        "built-in envelope (Volume/Pan/Width/Mute, their Pre-FX variants, Trim Volume) first.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "envelope":{"type":"string"},"time":{"type":"number"},"value":{"type":"number"},
            "shape":{"type":"integer","minimum":0,"maximum":5,"default":0,
                "description":"0 linear,1 square,2 slow start/end,3 fast start,4 fast end,5 bezier"},
            "selected":{"type":"boolean","default":false},
            "autoActivate":{"type":"boolean","default":false,
                "description":"activate a built-in track envelope if it is not active yet"}},
            "required":["track","envelope","time","value"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"envelope":{"type":"string"},
            "time":{"type":"number"},"value":{"type":"number"},"pointCount":{"type":"integer"},
            "activated":{"type":"boolean"},"activatedVia":{"type":"string"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string name = reqStr(a, "envelope");
            const double time = reqNum(a, "time");
            const double value = reqNum(a, "value");
            const int shape = optInt(a, "shape", 0);
            const bool selected = optBool(a, "selected", false);
            const bool autoActivate = optBool(a, "autoActivate", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            TrackEnvelope* e = GetTrackEnvelopeByName(t, name.c_str());
            bool activated = false;
            std::string via;
            Undo_BeginBlock2(kCur);
            if (!e && autoActivate) {
                e = activateBuiltinEnvelope(t, name, via);
                activated = (e != nullptr);
            }
            if (!e) {
                Undo_EndBlock2(kCur, "MCP: add envelope point", -1);
                if (autoActivate)
                    throw std::runtime_error("could not auto-activate envelope '" + name +
                                             "': " + via);
                throw std::runtime_error(
                    "envelope not active on track: '" + name + "' — pass autoActivate:true (built-in "
                    "envelopes), or show/arm it in REAPER / add the FX param it automates first");
            }
            InsertEnvelopePoint(e, time, value, shape, 0.0, selected, nullptr);
            Envelope_SortPoints(e);
            Undo_EndBlock2(kCur, "MCP: add envelope point", -1);
            Json out{{"ok", true}, {"envelope", envName(e)}, {"time", time}, {"value", value},
                     {"pointCount", CountEnvelopePoints(e)}, {"activated", activated}};
            if (activated) out["activatedVia"] = via;
            return out;
#else
            (void)idx; (void)shape; (void)selected;
            return Json{{"ok", true}, {"envelope", name}, {"time", time}, {"value", value},
                        {"pointCount", 1}, {"activated", autoActivate}};
#endif
        }});

    // =====================================================================================
    // Envelope depth — point readback, deletion, range clear, automation mode.
    // =====================================================================================

    // ---- envelope.get_points (read-only) ----
    reg.add(Tool{
        "envelope.get_points",
        "List a track envelope's points (time, native-unit value, shape, tension, selected), "
        "optionally restricted to a [start,end] time window (seconds).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "envelope":{"type":"string"},"start":{"type":"number"},"end":{"type":"number"}},
            "required":["track","envelope"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"envelope":{"type":"string"},
            "pointCount":{"type":"integer"},"points":{"type":"array","items":{"type":"object",
                "properties":{"index":{"type":"integer"},"time":{"type":"number"},
                "value":{"type":"number"},"shape":{"type":"integer"},"tension":{"type":"number"},
                "selected":{"type":"boolean"}}}}},
            "required":["track","envelope","pointCount","points"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string name = reqStr(a, "envelope");
            const bool hasStart = a.contains("start") && a["start"].is_number();
            const bool hasEnd = a.contains("end") && a["end"].is_number();
            const double start = optNum(a, "start", 0.0);
            const double end = optNum(a, "end", 0.0);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            TrackEnvelope* e = GetTrackEnvelopeByName(t, name.c_str());
            if (!e) throw std::runtime_error("envelope not active on track: '" + name + "'");
            const int n = CountEnvelopePoints(e);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                double tm = 0.0, val = 0.0, tension = 0.0;
                int shape = 0;
                bool sel = false;
                if (!GetEnvelopePoint(e, i, &tm, &val, &shape, &tension, &sel)) continue;
                if (hasStart && tm < start) continue;
                if (hasEnd && tm > end) continue;
                arr.push_back(Json{{"index", i}, {"time", tm}, {"value", val}, {"shape", shape},
                                   {"tension", tension}, {"selected", sel}});
            }
            return Json{{"track", idx}, {"envelope", envName(e)}, {"pointCount", n},
                        {"points", std::move(arr)}};
#else
            (void)hasStart; (void)hasEnd; (void)start; (void)end;
            return Json{{"track", idx}, {"envelope", name}, {"pointCount", 0},
                        {"points", Json::array()}};
#endif
        }});

    // ---- envelope.delete_point (mutating; DESTRUCTIVE; undo-wrapped) ----
    reg.add(Tool{
        "envelope.delete_point",
        "Delete one point from a track envelope by its index (from envelope.get_points).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "envelope":{"type":"string"},"index":{"type":"integer","minimum":0}},
            "required":["track","envelope","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"deletedIndex":{"type":"integer"},
            "pointCount":{"type":"integer"}},"required":["ok","deletedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string name = reqStr(a, "envelope");
            const int ptIdx = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            TrackEnvelope* e = GetTrackEnvelopeByName(t, name.c_str());
            if (!e) throw std::runtime_error("envelope not active on track: '" + name + "'");
            Undo_BeginBlock2(kCur);
            const bool ok = DeleteEnvelopePointEx(e, -1, ptIdx);  // -1 = the underlying envelope
            Envelope_SortPoints(e);
            Undo_EndBlock2(kCur, "MCP: delete envelope point", -1);
            if (!ok) throw std::runtime_error("point index out of range: " + std::to_string(ptIdx));
            return Json{{"ok", true}, {"deletedIndex", ptIdx}, {"pointCount", CountEnvelopePoints(e)}};
#else
            (void)idx;
            return Json{{"ok", true}, {"deletedIndex", ptIdx}, {"pointCount", 0}};
#endif
        }});

    // ---- envelope.clear_range (mutating; DESTRUCTIVE; undo-wrapped) ----
    reg.add(Tool{
        "envelope.clear_range",
        "Delete all points of a track envelope inside a [start,end) time range (seconds).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "envelope":{"type":"string"},"start":{"type":"number"},"end":{"type":"number"}},
            "required":["track","envelope","start","end"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"start":{"type":"number"},
            "end":{"type":"number"},"pointCount":{"type":"integer"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ true},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const std::string name = reqStr(a, "envelope");
            const double start = reqNum(a, "start");
            const double end = reqNum(a, "end");
            if (end < start) throw std::runtime_error("end must be >= start");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            TrackEnvelope* e = GetTrackEnvelopeByName(t, name.c_str());
            if (!e) throw std::runtime_error("envelope not active on track: '" + name + "'");
            Undo_BeginBlock2(kCur);
            DeleteEnvelopePointRange(e, start, end);
            // Boundary cleanup — REAPER's range delete spares a point at exactly t==start; enforce
            // the documented [start,end) contract per-point, in reverse
            // index order so deletions don't shift the indices still to be visited.
            for (int i = CountEnvelopePoints(e) - 1; i >= 0; --i) {
                double tm = 0.0;
                if (GetEnvelopePoint(e, i, &tm, nullptr, nullptr, nullptr, nullptr) &&
                    tm >= start && tm < end)
                    DeleteEnvelopePointEx(e, -1, i);
            }
            Envelope_SortPoints(e);
            Undo_EndBlock2(kCur, "MCP: clear envelope range", -1);
            return Json{{"ok", true}, {"start", start}, {"end", end},
                        {"pointCount", CountEnvelopePoints(e)}};
#else
            (void)idx;
            return Json{{"ok", true}, {"start", start}, {"end", end}, {"pointCount", 0}};
#endif
        }});

    // ---- envelope.set_automation_mode (mutating; idempotent) ----
    reg.add(Tool{
        "envelope.set_automation_mode",
        "Set a track's automation mode (0 trim/read, 1 read, 2 touch, 3 write, 4 latch). Applies to "
        "all of the track's envelopes; echoes the previous mode.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "mode":{"type":"integer","minimum":0,"maximum":4,
                "description":"0 trim/read,1 read,2 touch,3 write,4 latch"}},
            "required":["track","mode"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"mode":{"type":"integer"},
            "previousMode":{"type":"integer"}},"required":["ok","mode"]})"),
        ToolAnnotations{false, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int mode = reqInt(a, "mode");
            if (mode < 0 || mode > 4) throw std::runtime_error("mode must be 0..4");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int prev = GetTrackAutomationMode(t);
            Undo_BeginBlock2(kCur);
            SetTrackAutomationMode(t, mode);
            Undo_EndBlock2(kCur, "MCP: set automation mode", -1);
            return Json{{"ok", true}, {"mode", GetTrackAutomationMode(t)}, {"previousMode", prev}};
#else
            (void)idx;
            return Json{{"ok", true}, {"mode", mode}, {"previousMode", 0}};
#endif
        }});
}

}  // namespace reaper_mcp
