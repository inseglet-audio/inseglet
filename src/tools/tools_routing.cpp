// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_routing.cpp — sends/receives + track channel counts; the routing substrate the immersive
// layer builds beds and ambisonic pin maps on top of.
//
// Send categories mirror the REAPER API: "send" (0), "receive" (-1), "hwout" (1). Dual-path; mutations
// single-undo.

#include <string>

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

namespace {
// wire string -> REAPER send category int (0 sends / -1 receives / 1 hardware outputs)
int sendCategory(const Json& a) {
    const std::string c = optStr(a, "category", "send");
    if (c == "receive") return -1;
    if (c == "hwout") return 1;
    return 0;  // "send"
}
}  // namespace

void registerRoutingTools(ToolRegistry& reg) {
    // ---- track.set_channels (mutating; undo-wrapped) ----
    reg.add(Tool{
        "track.set_channels",
        "Set a track's channel count (2..64; REAPER uses even counts). 12 = 7.1.4, 16 = 3rd-order "
        "ambisonics, 24 = 9.1.6, etc. The multichannel foundation for immersive routing.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "channels":{"type":"integer","minimum":2,"maximum":64}},
            "required":["track","channels"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"channels":{"type":"integer"}},
            "required":["ok","channels"]})"),
        ToolAnnotations{false, false, true}, Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            int ch = reqInt(a, "channels");
            if (ch < 2) ch = 2;
            if (ch > 64) ch = 64;
            if (ch & 1) ++ch;  // REAPER track channel counts are even
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            SetMediaTrackInfo_Value(t, "I_NCHAN", (double)ch);
            Undo_EndBlock2(kCur, "MCP: set track channel count", -1);
            return Json{{"ok", true}, {"channels", (int)GetMediaTrackInfo_Value(t, "I_NCHAN")}};
#else
            (void)idx;
            return Json{{"ok", true}, {"channels", ch}};
#endif
        }});

    // ---- send.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "send.add", "Create a track send from a source track to a destination track, with optional "
                    "initial volume (dB) and pan.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "dest":{"type":"integer","minimum":0},"db":{"type":"number"},
            "pan":{"type":"number","minimum":-1,"maximum":1}},
            "required":["track","dest"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"sendIndex":{"type":"integer"},
            "track":{"type":"integer"},"dest":{"type":"integer"}},"required":["sendIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Routing,
        [](const Json& a) -> Json {
            const int src = reqInt(a, "track");
            const int dst = reqInt(a, "dest");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* s = requireTrack(src);
            MediaTrack* d = requireTrack(dst);
            Undo_BeginBlock2(kCur);
            int sendIdx = CreateTrackSend(s, d);
            if (sendIdx >= 0) {
                if (a.contains("db") && a["db"].is_number())
                    SetTrackSendInfo_Value(s, 0, sendIdx, "D_VOL", dbToGain(a["db"].get<double>()));
                if (a.contains("pan") && a["pan"].is_number())
                    SetTrackSendInfo_Value(s, 0, sendIdx, "D_PAN", a["pan"].get<double>());
            }
            Undo_EndBlock2(kCur, "MCP: add send", -1);
            if (sendIdx < 0) throw std::runtime_error("could not create send");
            return Json{{"sendIndex", sendIdx}, {"track", src}, {"dest", dst}};
#else
            return Json{{"sendIndex", 0}, {"track", src}, {"dest", dst}};
#endif
        }});

    // ---- send.list (read-only) ----
    reg.add(Tool{
        "send.list", "List a track's sends (default), receives, or hardware outputs with "
                     "destination, volume (dB), pan, mute.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "category":{"type":"string","enum":["send","receive","hwout"],"default":"send"}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"category":{"type":"string"},
            "sendCount":{"type":"integer"},"sends":{"type":"array","items":{"type":"object","properties":{
                "index":{"type":"integer"},"destTrack":{"type":"integer"},"volDb":{"type":"number"},
                "pan":{"type":"number"},"muted":{"type":"boolean"}}}}},
            "required":["track","sendCount","sends"]})"),
        ToolAnnotations{true, false, true}, Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int cat = sendCategory(a);
            const std::string catName = optStr(a, "category", "send");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int n = GetTrackNumSends(t, cat);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                auto* dest = (MediaTrack*)GetSetTrackSendInfo(t, cat, i, "P_DESTTRACK", nullptr);
                int destIdx = dest ? (CSurf_TrackToID(dest, false) - 1) : -1;
                arr.push_back(Json{{"index", i}, {"destTrack", destIdx},
                                   {"volDb", gainToDb(GetTrackSendInfo_Value(t, cat, i, "D_VOL"))},
                                   {"pan", GetTrackSendInfo_Value(t, cat, i, "D_PAN")},
                                   {"muted", GetTrackSendInfo_Value(t, cat, i, "B_MUTE") > 0.5}});
            }
            return Json{{"track", idx}, {"category", catName}, {"sendCount", n}, {"sends", std::move(arr)}};
#else
            (void)cat;
            return Json{{"track", idx}, {"category", catName}, {"sendCount", 0}, {"sends", Json::array()}};
#endif
        }});

    // ---- send.remove (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "send.remove", "Remove a send/receive/hardware-output by its index (from send.list).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "index":{"type":"integer","minimum":0},
            "category":{"type":"string","enum":["send","receive","hwout"],"default":"send"}},
            "required":["track","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},
            "removedIndex":{"type":"integer"}},"required":["ok","removedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int sendIdx = reqInt(a, "index");
            const int cat = sendCategory(a);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            bool ok = RemoveTrackSend(t, cat, sendIdx);
            Undo_EndBlock2(kCur, "MCP: remove send", -1);
            return Json{{"ok", ok}, {"removedIndex", sendIdx}};
#else
            (void)idx; (void)cat;
            return Json{{"ok", true}, {"removedIndex", sendIdx}};
#endif
        }});

    // ========================= sends / routing depth =========================

    // ---- send.set (mutating; edit an existing send/receive/hwout's parameters) ----
    reg.add(Tool{
        "send.set", "Set parameters of an existing send/receive/hardware-output (by index from "
                    "send.list / receive.list): volume (dB), pan (-1..1), mute, phase, and send mode "
                    "(0=post-fader, 1=pre-fx, 3=post-fx). Only the fields you provide are changed. "
                    "Single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "index":{"type":"integer","minimum":0},
            "category":{"type":"string","enum":["send","receive","hwout"],"default":"send"},
            "db":{"type":"number"},"pan":{"type":"number","minimum":-1,"maximum":1},
            "mute":{"type":"boolean"},"phase":{"type":"boolean"},
            "mode":{"type":"integer","enum":[0,1,3]}},
            "required":["track","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "volDb":{"type":"number"},"pan":{"type":"number"},"muted":{"type":"boolean"},
            "mode":{"type":"integer"}},"required":["ok","index"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true}, Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int sendIdx = reqInt(a, "index");
            const int cat = sendCategory(a);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (a.contains("db") && a["db"].is_number())
                SetTrackSendInfo_Value(t, cat, sendIdx, "D_VOL", dbToGain(a["db"].get<double>()));
            if (a.contains("pan") && a["pan"].is_number())
                SetTrackSendInfo_Value(t, cat, sendIdx, "D_PAN", a["pan"].get<double>());
            if (a.contains("mute") && a["mute"].is_boolean())
                SetTrackSendInfo_Value(t, cat, sendIdx, "B_MUTE", a["mute"].get<bool>() ? 1.0 : 0.0);
            if (a.contains("phase") && a["phase"].is_boolean())
                SetTrackSendInfo_Value(t, cat, sendIdx, "B_PHASE", a["phase"].get<bool>() ? 1.0 : 0.0);
            if (a.contains("mode") && a["mode"].is_number())
                SetTrackSendInfo_Value(t, cat, sendIdx, "I_SENDMODE", (double)a["mode"].get<int>());
            Undo_EndBlock2(kCur, "MCP: set send params", -1);
            return Json{{"ok", true}, {"index", sendIdx},
                        {"volDb", gainToDb(GetTrackSendInfo_Value(t, cat, sendIdx, "D_VOL"))},
                        {"pan", GetTrackSendInfo_Value(t, cat, sendIdx, "D_PAN")},
                        {"muted", GetTrackSendInfo_Value(t, cat, sendIdx, "B_MUTE") > 0.5},
                        {"mode", (int)GetTrackSendInfo_Value(t, cat, sendIdx, "I_SENDMODE")}};
#else
            (void)idx; (void)cat;
            return Json{{"ok", true}, {"index", sendIdx},
                        {"volDb", optNum(a, "db", 0.0)}, {"pan", optNum(a, "pan", 0.0)},
                        {"muted", optBool(a, "mute", false)}, {"mode", optInt(a, "mode", 0)}};
#endif
        }});

    // ---- send.set_channels (mutating; source/destination channel routing) ----
    reg.add(Tool{
        "send.set_channels",
        "Set a send's source and/or destination channel routing (I_SRCCHAN / I_DSTCHAN). 'srcChannel' "
        "and 'dstChannel' are 0-based channel offsets on the source and destination tracks. 'srcChannels' "
        "is the send width (1=mono, 2=stereo (default), 4, 6, …). srcChannel=-1 sends no audio (MIDI-only). "
        "This is the channel-level routing that beds/bus wiring depend on — keep offsets consistent with "
        "the immersive layout (e.g. a 7.1.4 bed's height pair). Single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "index":{"type":"integer","minimum":0},
            "category":{"type":"string","enum":["send","receive","hwout"],"default":"send"},
            "srcChannel":{"type":"integer","minimum":-1},"srcChannels":{"type":"integer","enum":[1,2,4,6,8,10,12,14,16]},
            "dstChannel":{"type":"integer","minimum":0},"dstMono":{"type":"boolean","default":false}},
            "required":["track","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "srcChan":{"type":"integer"},"dstChan":{"type":"integer"}},"required":["ok","index"]})"),
        ToolAnnotations{false, false, true}, Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int sendIdx = reqInt(a, "index");
            const int cat = sendCategory(a);
            // Encode I_SRCCHAN: low 10 bits = channel offset, (>>10) = width code. Per the SDK:
            // (srcchan>>10)==0 stereo, 1 mono, 2 = 4ch, 3 = 6ch, …  → code(2)=0, (1)=1, (n even)=n/2.
            auto encSrc = [](int off, int chans) -> int {
                if (off < 0) return -1;                       // no audio send (MIDI-only)
                const int code = (chans <= 1) ? 1 : (chans == 2 ? 0 : chans / 2);
                return (off & 0x3FF) | (code << 10);
            };
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            if (a.contains("srcChannel") && a["srcChannel"].is_number()) {
                const int off = a["srcChannel"].get<int>();
                const int chans = optInt(a, "srcChannels", 2);
                SetTrackSendInfo_Value(t, cat, sendIdx, "I_SRCCHAN", (double)encSrc(off, chans));
            }
            if (a.contains("dstChannel") && a["dstChannel"].is_number()) {
                const int dst = (a["dstChannel"].get<int>() & 0x3FF) | (optBool(a, "dstMono", false) ? 1024 : 0);
                SetTrackSendInfo_Value(t, cat, sendIdx, "I_DSTCHAN", (double)dst);
            }
            Undo_EndBlock2(kCur, "MCP: set send channels", -1);
            return Json{{"ok", true}, {"index", sendIdx},
                        {"srcChan", (int)GetTrackSendInfo_Value(t, cat, sendIdx, "I_SRCCHAN")},
                        {"dstChan", (int)GetTrackSendInfo_Value(t, cat, sendIdx, "I_DSTCHAN")}};
#else
            (void)idx; (void)cat;
            const int off = optInt(a, "srcChannel", 0);
            return Json{{"ok", true}, {"index", sendIdx},
                        {"srcChan", encSrc(off, optInt(a, "srcChannels", 2))},
                        {"dstChan", optInt(a, "dstChannel", 0)}};
#endif
        }});

    // ---- receive.list (read-only; receives with their SOURCE track) ----
    reg.add(Tool{
        "receive.list", "List a track's receives with their SOURCE track, volume (dB), pan, and mute. "
                        "(send.list with category='receive' reports the same rows keyed differently; this "
                        "verb resolves the upstream source track explicitly.)",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},
            "receiveCount":{"type":"integer"},"receives":{"type":"array","items":{"type":"object",
                "properties":{"index":{"type":"integer"},"srcTrack":{"type":"integer"},
                "volDb":{"type":"number"},"pan":{"type":"number"},"muted":{"type":"boolean"}}}}},
            "required":["track","receiveCount","receives"]})"),
        ToolAnnotations{true, false, true}, Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int n = GetTrackNumSends(t, -1);  // -1 = receives
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                auto* src = (MediaTrack*)GetSetTrackSendInfo(t, -1, i, "P_SRCTRACK", nullptr);
                int srcIdx = src ? (CSurf_TrackToID(src, false) - 1) : -1;
                arr.push_back(Json{{"index", i}, {"srcTrack", srcIdx},
                                   {"volDb", gainToDb(GetTrackSendInfo_Value(t, -1, i, "D_VOL"))},
                                   {"pan", GetTrackSendInfo_Value(t, -1, i, "D_PAN")},
                                   {"muted", GetTrackSendInfo_Value(t, -1, i, "B_MUTE") > 0.5}});
            }
            return Json{{"track", idx}, {"receiveCount", n}, {"receives", std::move(arr)}};
#else
            return Json{{"track", idx}, {"receiveCount", 0}, {"receives", Json::array()}};
#endif
        }});

    // ---- send.add_hardware_output (mutating; undo-wrapped) ----
    reg.add(Tool{
        "send.add_hardware_output",
        "Add a hardware-output send from a track to a physical output channel. 'outputChannel' is the "
        "0-based hardware channel offset (pass a stereo pair's low channel; set 'mono' for a single "
        "channel). Optional initial volume (dB) and pan. Single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "outputChannel":{"type":"integer","minimum":0},"mono":{"type":"boolean","default":false},
            "db":{"type":"number"},"pan":{"type":"number","minimum":-1,"maximum":1}},
            "required":["track","outputChannel"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"sendIndex":{"type":"integer"},
            "outputChannel":{"type":"integer"}},"required":["sendIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Routing,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const int outCh = reqInt(a, "outputChannel");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            int sendIdx = CreateTrackSend(t, nullptr);  // NULL dest = hardware output (category 1)
            if (sendIdx >= 0) {
                const int dst = (outCh & 0x3FF) | (optBool(a, "mono", false) ? 1024 : 0);
                SetTrackSendInfo_Value(t, 1, sendIdx, "I_DSTCHAN", (double)dst);
                if (a.contains("db") && a["db"].is_number())
                    SetTrackSendInfo_Value(t, 1, sendIdx, "D_VOL", dbToGain(a["db"].get<double>()));
                if (a.contains("pan") && a["pan"].is_number())
                    SetTrackSendInfo_Value(t, 1, sendIdx, "D_PAN", a["pan"].get<double>());
            }
            Undo_EndBlock2(kCur, "MCP: add hardware output", -1);
            if (sendIdx < 0) throw std::runtime_error("could not create hardware output send");
            return Json{{"sendIndex", sendIdx}, {"outputChannel", outCh}};
#else
            (void)idx;
            return Json{{"sendIndex", 0}, {"outputChannel", outCh}};
#endif
        }});
}

}  // namespace reaper_mcp
