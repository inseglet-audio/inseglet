// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_timeline.cpp — time selection, loop range, tempo, tempo/time-signature markers, and
// quarter-note <-> time conversion.
//
// Dual-path. tempo.set carries its own undo (SetCurrentBPM wantUndo=true); time-selection edits are
// transient UI state and are not pushed onto the undo stack, matching REAPER's own behavior.

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

void registerTimelineTools(ToolRegistry& reg) {
    // ---- timeselection.get (read-only) ----
    reg.add(Tool{
        "timeselection.get", "Get the time selection, the loop range, and whether looping is enabled.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"timeSelStart":{"type":"number"},
            "timeSelEnd":{"type":"number"},"loopStart":{"type":"number"},"loopEnd":{"type":"number"},
            "loopEnabled":{"type":"boolean"}},
            "required":["timeSelStart","timeSelEnd","loopEnabled"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            double ts0 = 0.0, ts1 = 0.0, lp0 = 0.0, lp1 = 0.0;
            GetSet_LoopTimeRange(false, false, &ts0, &ts1, false);  // time selection
            GetSet_LoopTimeRange(false, true, &lp0, &lp1, false);   // loop points
            return Json{{"timeSelStart", ts0}, {"timeSelEnd", ts1},
                        {"loopStart", lp0}, {"loopEnd", lp1},
                        {"loopEnabled", GetSetRepeat(-1) > 0}};
#else
            return Json{{"timeSelStart", 0.0}, {"timeSelEnd", 0.0},
                        {"loopStart", 0.0}, {"loopEnd", 0.0}, {"loopEnabled", false}};
#endif
        }});

    // ---- timeselection.set (mutating; transient UI state, not an undo point) ----
    reg.add(Tool{
        "timeselection.set", "Set the time selection (default) or the loop range to [start,end) seconds.",
        jparse(R"({"type":"object","properties":{"start":{"type":"number"},"end":{"type":"number"},
            "target":{"type":"string","enum":["timesel","loop"],"default":"timesel"}},
            "required":["start","end"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"start":{"type":"number"},
            "end":{"type":"number"},"target":{"type":"string"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            double start = reqNum(a, "start");
            double end = reqNum(a, "end");
            const std::string target = optStr(a, "target", "timesel");
#ifdef REAPER_MCP_HAVE_SDK
            GetSet_LoopTimeRange(true, target == "loop", &start, &end, false);
#endif
            return Json{{"ok", true}, {"start", start}, {"end", end}, {"target", target}};
        }});

    // ---- tempo.get (read-only) ----
    reg.add(Tool{
        "tempo.get", "Get the project's current/master tempo in BPM.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"bpm":{"type":"number"}},"required":["bpm"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            return Json{{"bpm", Master_GetTempo()}};
#else
            return Json{{"bpm", 120.0}};
#endif
        }});

    // ---- tempo.set (mutating; carries its own undo) ----
    reg.add(Tool{
        "tempo.set", "Set the project tempo in BPM.",
        jparse(R"({"type":"object","properties":{"bpm":{"type":"number","minimum":1,"maximum":960}},
            "required":["bpm"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"bpm":{"type":"number"}},
            "required":["ok","bpm"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const double bpm = reqNum(a, "bpm");
#ifdef REAPER_MCP_HAVE_SDK
            SetCurrentBPM(kCur, bpm, true);
            return Json{{"ok", true}, {"bpm", Master_GetTempo()}};
#else
            return Json{{"ok", true}, {"bpm", bpm}};
#endif
        }});

    // ==================== tempo / time-signature markers ====================
    // Backed by the TempoTimeSigMarker family. Add uses SetTempoTimeSigMarker(ptidx=-1); edit reads the
    // marker then merges the provided fields (so a caller can change just the BPM or just the time-sig).

    // ---- tempo.list_markers (read-only) ----
    reg.add(Tool{
        "tempo.list_markers", "List all tempo/time-signature markers: index, time (sec), measure/beat, "
                              "BPM, time signature, and whether the tempo transition is linear.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"count":{"type":"integer"},
            "markers":{"type":"array","items":{"type":"object","properties":{
                "index":{"type":"integer"},"position":{"type":"number"},"measure":{"type":"integer"},
                "beat":{"type":"number"},"bpm":{"type":"number"},"timesigNum":{"type":"integer"},
                "timesigDenom":{"type":"integer"},"linear":{"type":"boolean"}}}}},
            "required":["count","markers"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            const int n = CountTempoTimeSigMarkers(kCur);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                double t = 0.0, beat = 0.0, bpm = 0.0;
                int meas = 0, num = 0, den = 0;
                bool linear = false;
                if (GetTempoTimeSigMarker(kCur, i, &t, &meas, &beat, &bpm, &num, &den, &linear))
                    arr.push_back(Json{{"index", i}, {"position", t}, {"measure", meas}, {"beat", beat},
                                       {"bpm", bpm}, {"timesigNum", num}, {"timesigDenom", den},
                                       {"linear", linear}});
            }
            return Json{{"count", n}, {"markers", std::move(arr)}};
#else
            return Json{{"count", 0}, {"markers", Json::array()}};
#endif
        }});

    // ---- tempo.add_marker (mutating; undo-wrapped) ----
    reg.add(Tool{
        "tempo.add_marker", "Add a tempo/time-signature marker at a time position (seconds). 'bpm' sets "
                            "the tempo from this point; optional 'timesigNum'/'timesigDenom' change the "
                            "time signature (omit or 0 to keep the previous one); 'linear' makes the "
                            "tempo ramp linearly to the next marker. Single-undo.",
        jparse(R"({"type":"object","properties":{"position":{"type":"number","minimum":0},
            "bpm":{"type":"number","minimum":1,"maximum":960},
            "timesigNum":{"type":"integer","minimum":0},"timesigDenom":{"type":"integer","minimum":0},
            "linear":{"type":"boolean","default":false}},
            "required":["position","bpm"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "count":{"type":"integer"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const double pos = reqNum(a, "position");
            const double bpm = reqNum(a, "bpm");
            const int num = optInt(a, "timesigNum", 0);
            const int den = optInt(a, "timesigDenom", 0);
            const bool linear = optBool(a, "linear", false);
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            const bool ok = SetTempoTimeSigMarker(kCur, -1, pos, -1, -1, bpm, num, den, linear);
            Undo_EndBlock2(kCur, "MCP: add tempo/time-sig marker", -1);
            UpdateTimeline();
            if (!ok) throw std::runtime_error("could not add tempo/time-sig marker at " + std::to_string(pos));
            const int count = CountTempoTimeSigMarkers(kCur);
            // Markers are stored time-sorted, so the new one is not necessarily last — resolve its real
            // index by matching the time position rather than assuming count-1.
            int newIdx = count - 1;
            for (int i = 0; i < count; ++i) {
                double t = 0.0, beat = 0.0, b = 0.0; int meas = 0, n2 = 0, d2 = 0; bool lin = false;
                if (GetTempoTimeSigMarker(kCur, i, &t, &meas, &beat, &b, &n2, &d2, &lin) &&
                    std::fabs(t - pos) < 1e-6) { newIdx = i; break; }
            }
            return Json{{"ok", true}, {"index", newIdx}, {"count", count}};
#else
            (void)pos; (void)bpm; (void)num; (void)den; (void)linear;
            return Json{{"ok", true}, {"index", 0}, {"count", 1}};
#endif
        }});

    // ---- tempo.edit_marker (mutating; read-merge-write) ----
    reg.add(Tool{
        "tempo.edit_marker", "Edit an existing tempo/time-signature marker by index (from "
                             "tempo.list_markers). Only the fields you provide change; the rest keep their "
                             "current values (position, bpm, timesigNum, timesigDenom, linear). Single-undo.",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer","minimum":0},
            "position":{"type":"number","minimum":0},"bpm":{"type":"number","minimum":1,"maximum":960},
            "timesigNum":{"type":"integer","minimum":0},"timesigDenom":{"type":"integer","minimum":0},
            "linear":{"type":"boolean"}},"required":["index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "position":{"type":"number"},"bpm":{"type":"number"}},"required":["ok","index"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int index = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            double t = 0.0, beat = 0.0, bpm = 0.0;
            int meas = 0, num = 0, den = 0;
            bool linear = false;
            if (!GetTempoTimeSigMarker(kCur, index, &t, &meas, &beat, &bpm, &num, &den, &linear))
                throw std::runtime_error("no such tempo/time-sig marker: " + std::to_string(index));
            // Merge provided fields over the current values. Re-anchor by time (measure/beat = -1).
            if (a.contains("position") && a["position"].is_number()) t = a["position"].get<double>();
            if (a.contains("bpm") && a["bpm"].is_number())           bpm = a["bpm"].get<double>();
            if (a.contains("timesigNum") && a["timesigNum"].is_number())   num = a["timesigNum"].get<int>();
            if (a.contains("timesigDenom") && a["timesigDenom"].is_number()) den = a["timesigDenom"].get<int>();
            if (a.contains("linear") && a["linear"].is_boolean())    linear = a["linear"].get<bool>();
            Undo_BeginBlock2(kCur);
            const bool ok = SetTempoTimeSigMarker(kCur, index, t, -1, -1, bpm, num, den, linear);
            Undo_EndBlock2(kCur, "MCP: edit tempo/time-sig marker", -1);
            UpdateTimeline();
            if (!ok) throw std::runtime_error("could not edit tempo/time-sig marker " + std::to_string(index));
            return Json{{"ok", true}, {"index", index}, {"position", t}, {"bpm", bpm}};
#else
            return Json{{"ok", true}, {"index", index},
                        {"position", optNum(a, "position", 0.0)}, {"bpm", optNum(a, "bpm", 120.0)}};
#endif
        }});

    // ---- tempo.delete_marker (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "tempo.delete_marker", "Delete a tempo/time-signature marker by index (from tempo.list_markers). "
                               "Single-undo.",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer","minimum":0}},
            "required":["index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"removedIndex":{"type":"integer"},
            "count":{"type":"integer"}},"required":["ok","removedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const int index = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            const bool ok = DeleteTempoTimeSigMarker(kCur, index);
            Undo_EndBlock2(kCur, "MCP: delete tempo/time-sig marker", -1);
            UpdateTimeline();
            if (!ok) throw std::runtime_error("no such tempo/time-sig marker: " + std::to_string(index));
            return Json{{"ok", true}, {"removedIndex", index}, {"count", CountTempoTimeSigMarkers(kCur)}};
#else
            return Json{{"ok", true}, {"removedIndex", index}, {"count", 0}};
#endif
        }});

    // ---- time.qn_to_time (read-only; project QN -> seconds via the tempo map) ----
    reg.add(Tool{
        "time.qn_to_time", "Convert a project quarter-note position (QN, counted from project start) to a "
                           "time position in seconds, honoring the full tempo map.",
        jparse(R"({"type":"object","properties":{"qn":{"type":"number"}},
            "required":["qn"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"qn":{"type":"number"},"time":{"type":"number"}},
            "required":["qn","time"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const double qn = reqNum(a, "qn");
#ifdef REAPER_MCP_HAVE_SDK
            return Json{{"qn", qn}, {"time", TimeMap2_QNToTime(kCur, qn)}};
#else
            return Json{{"qn", qn}, {"time", qn * 0.5}};  // host stub: 120 BPM (0.5 s per QN)
#endif
        }});

    // ---- time.time_to_qn (read-only; seconds -> project QN via the tempo map) ----
    reg.add(Tool{
        "time.time_to_qn", "Convert a time position in seconds to a project quarter-note position (QN, "
                           "counted from project start), honoring the full tempo map.",
        jparse(R"({"type":"object","properties":{"time":{"type":"number"}},
            "required":["time"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"time":{"type":"number"},"qn":{"type":"number"}},
            "required":["time","qn"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const double tpos = reqNum(a, "time");
#ifdef REAPER_MCP_HAVE_SDK
            return Json{{"time", tpos}, {"qn", TimeMap2_timeToQN(kCur, tpos)}};
#else
            return Json{{"time", tpos}, {"qn", tpos * 2.0}};  // host stub: 120 BPM
#endif
        }});
}

}  // namespace reaper_mcp
