// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_items.cpp — media item tools: add/list, per-field setters (position, length, mute, volume,
// fade-in/out, color, selection), and structural edits (split, delete, move, glue, duplicate).
//
// Dual-path; mutations are single-undo.

#include <string>

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK
namespace {
std::string itemGuidStr(MediaItem* it) {
    char buf[64] = {0};
    if (it) GetSetMediaItemInfo_String(it, "GUID", buf, false);
    return buf;
}
}  // namespace
#endif

void registerItemTools(ToolRegistry& reg) {
    // ---- item.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "item.add", "Add an empty media item to a track at a position (default 0) and length "
                    "(default 1s). Foundation for takes / MIDI / audio inserts.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "position":{"type":"number","minimum":0},"length":{"type":"number","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"guid":{"type":"string"},
            "position":{"type":"number"},"length":{"type":"number"}},
            "required":["track","position","length"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
            const double pos = optNum(a, "position", 0.0);
            const double len = optNum(a, "length", 1.0);
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            Undo_BeginBlock2(kCur);
            MediaItem* it = AddMediaItemToTrack(t);
            SetMediaItemInfo_Value(it, "D_POSITION", pos);
            SetMediaItemInfo_Value(it, "D_LENGTH", len);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: add media item", -1);
            return Json{{"track", idx}, {"guid", itemGuidStr(it)}, {"position", pos}, {"length", len}};
#else
            return Json{{"track", idx},
                        {"guid", "{00000000-0000-0000-0000-000000000000}"},
                        {"position", pos}, {"length", len}};
#endif
        }});

    // ---- item.list (read-only) ----
    reg.add(Tool{
        "item.list", "List a track's media items with index, position, length, GUID.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},
            "itemCount":{"type":"integer"},"items":{"type":"array","items":{"type":"object",
                "properties":{"index":{"type":"integer"},"position":{"type":"number"},
                "length":{"type":"number"},"guid":{"type":"string"}}}}},
            "required":["track","itemCount","items"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int idx = reqInt(a, "track");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(idx);
            const int n = CountTrackMediaItems(t);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                MediaItem* it = GetTrackMediaItem(t, i);
                arr.push_back(Json{{"index", i},
                                   {"position", GetMediaItemInfo_Value(it, "D_POSITION")},
                                   {"length", GetMediaItemInfo_Value(it, "D_LENGTH")},
                                   {"guid", itemGuidStr(it)}});
            }
            return Json{{"track", idx}, {"itemCount", n}, {"items", std::move(arr)}};
#else
            return Json{{"track", idx}, {"itemCount", 0}, {"items", Json::array()}};
#endif
        }});

    // =====================================================================================
    // Granular per-field item setters — each a distinct tool with a
    // REQUIRED value arg, idempotent. All locate the item by (track,item) index and map 1:1 to
    // SetMediaItemInfo_Value / SetMediaItemSelected in a single undo block. Multi-field atomic edits
    // are composed via session.run_dsl.
    // =====================================================================================

    // ---- item.set_position ----
    reg.add(Tool{
        "item.set_position", "Set a media item's start position (seconds).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"position":{"type":"number","minimum":0}},
            "required":["track","item","position"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"position":{"type":"number"}},
            "required":["ok","position"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            double v = reqNum(a, "position"); if (v < 0) v = 0;
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "D_POSITION", v);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item position", -1);
            return Json{{"ok", true}, {"position", GetMediaItemInfo_Value(it, "D_POSITION")}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"position", v}};
#endif
        }});

    // ---- item.set_length ----
    reg.add(Tool{
        "item.set_length", "Set a media item's length (seconds).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"length":{"type":"number","minimum":0}},
            "required":["track","item","length"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"length":{"type":"number"}},
            "required":["ok","length"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            double v = reqNum(a, "length"); if (v < 0) v = 0;
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "D_LENGTH", v);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item length", -1);
            return Json{{"ok", true}, {"length", GetMediaItemInfo_Value(it, "D_LENGTH")}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"length", v}};
#endif
        }});

    // ---- item.set_mute ----
    reg.add(Tool{
        "item.set_mute", "Mute or unmute a media item (B_MUTE).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"muted":{"type":"boolean"}},
            "required":["track","item","muted"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"muted":{"type":"boolean"}},
            "required":["ok","muted"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const bool v = reqBool(a, "muted");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "B_MUTE", v ? 1.0 : 0.0);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item mute", -1);
            return Json{{"ok", true}, {"muted", GetMediaItemInfo_Value(it, "B_MUTE") > 0.5}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"muted", v}};
#endif
        }});

    // ---- item.set_vol (dB) ----
    reg.add(Tool{
        "item.set_vol", "Set a media item's take/item volume in dB (D_VOL).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"db":{"type":"number"}},
            "required":["track","item","db"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"db":{"type":"number"}},
            "required":["ok","db"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const double db = reqNum(a, "db");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "D_VOL", dbToGain(db));
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item volume", -1);
            return Json{{"ok", true}, {"db", gainToDb(GetMediaItemInfo_Value(it, "D_VOL"))}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"db", db}};
#endif
        }});

    // ---- item.set_fade_in ----
    reg.add(Tool{
        "item.set_fade_in", "Set a media item's fade-in length (seconds; D_FADEINLEN).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"length":{"type":"number","minimum":0}},
            "required":["track","item","length"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"length":{"type":"number"}},
            "required":["ok","length"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            double v = reqNum(a, "length"); if (v < 0) v = 0;
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "D_FADEINLEN", v);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item fade-in", -1);
            return Json{{"ok", true}, {"length", GetMediaItemInfo_Value(it, "D_FADEINLEN")}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"length", v}};
#endif
        }});

    // ---- item.set_fade_out ----
    reg.add(Tool{
        "item.set_fade_out", "Set a media item's fade-out length (seconds; D_FADEOUTLEN).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"length":{"type":"number","minimum":0}},
            "required":["track","item","length"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"length":{"type":"number"}},
            "required":["ok","length"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            double v = reqNum(a, "length"); if (v < 0) v = 0;
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "D_FADEOUTLEN", v);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item fade-out", -1);
            return Json{{"ok", true}, {"length", GetMediaItemInfo_Value(it, "D_FADEOUTLEN")}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"length", v}};
#endif
        }});

    // ---- item.set_color ----
    reg.add(Tool{
        "item.set_color", "Set a media item's custom color (0xRRGGBB); color:0 clears it.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},
            "color":{"type":"integer","minimum":0,"description":"0xRRGGBB; 0 = clear"}},
            "required":["track","item","color"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"color":{"type":"integer"},
            "colorSet":{"type":"boolean"}},"required":["ok","color","colorSet"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const int rgb = reqInt(a, "color") & 0xFFFFFF;
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SetMediaItemInfo_Value(it, "I_CUSTOMCOLOR", rgb ? (double)(rgb | 0x1000000) : 0.0);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set item color", -1);
            const int c = (int)GetMediaItemInfo_Value(it, "I_CUSTOMCOLOR");
            return Json{{"ok", true}, {"color", c & 0xFFFFFF}, {"colorSet", (c & 0x1000000) != 0}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"color", rgb}, {"colorSet", rgb != 0}};
#endif
        }});

    // ---- item.set_selected ----
    reg.add(Tool{
        "item.set_selected", "Select or deselect a single media item.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"selected":{"type":"boolean"}},
            "required":["track","item","selected"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"selected":{"type":"boolean"}},
            "required":["ok","selected"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const bool v = reqBool(a, "selected");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            SetMediaItemSelected(it, v);
            UpdateArrange();
            return Json{{"ok", true}, {"selected", GetMediaItemInfo_Value(it, "B_UISEL") > 0.5}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"selected", v}};
#endif
        }});

    // =====================================================================================
    // Item structural edits — split / delete / move / glue / duplicate.
    // =====================================================================================

    // ---- item.split ----
    reg.add(Tool{
        "item.split", "Split a media item at an absolute time (seconds). Returns the GUIDs of the "
                      "left (original) and right (new) halves; ok=false if the time is outside the item.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"position":{"type":"number"}},
            "required":["track","item","position"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"leftGuid":{"type":"string"},
            "rightGuid":{"type":"string"}},"required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const double pos = reqNum(a, "position");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            MediaItem* right = SplitMediaItem(it, pos);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: split item", -1);
            return Json{{"ok", right != nullptr}, {"leftGuid", itemGuidStr(it)},
                        {"rightGuid", right ? itemGuidStr(right) : std::string()}};
#else
            (void)tr; (void)im; (void)pos;
            return Json{{"ok", true}, {"leftGuid", "{00000000-0000-0000-0000-000000000000}"},
                        {"rightGuid", "{00000000-0000-0000-0000-000000000000}"}};
#endif
        }});

    // ---- item.delete (DESTRUCTIVE) ----
    reg.add(Tool{
        "item.delete", "Delete a media item from its track. Destructive; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0}},"required":["track","item"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "item":{"type":"integer"}},"required":["ok"]})"),
        ToolAnnotations{false, true, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(tr);
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            bool ok = DeleteTrackMediaItem(t, it);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: delete item", -1);
            return Json{{"ok", ok}, {"track", tr}, {"item", im}};
#else
            return Json{{"ok", true}, {"track", tr}, {"item", im}};
#endif
        }});

    // ---- item.move ----
    reg.add(Tool{
        "item.move", "Move a media item to a new position and/or another track. Provide position "
                     "(seconds) and/or destTrack (index). Idempotent.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"position":{"type":"number","minimum":0},
            "destTrack":{"type":"integer","minimum":0}},"required":["track","item"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"track":{"type":"integer"},
            "position":{"type":"number"},"movedTrack":{"type":"boolean"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const bool hasPos = a.contains("position") && a["position"].is_number();
            const bool hasDest = a.contains("destTrack") && a["destTrack"].is_number();
            const int dest = optInt(a, "destTrack", tr);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            bool movedTrack = false;
            if (hasDest && dest != tr) {
                MediaTrack* d = requireTrack(dest);
                movedTrack = MoveMediaItemToTrack(it, d);
            }
            if (hasPos) SetMediaItemInfo_Value(it, "D_POSITION", a["position"].get<double>());
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: move item", -1);
            return Json{{"ok", true}, {"track", movedTrack ? dest : tr},
                        {"position", GetMediaItemInfo_Value(it, "D_POSITION")},
                        {"movedTrack", movedTrack}};
#else
            (void)im;
            return Json{{"ok", true}, {"track", hasDest ? dest : tr},
                        {"position", optNum(a, "position", 0.0)}, {"movedTrack", hasDest && dest != tr}};
#endif
        }});

    // ---- item.glue ----
    reg.add(Tool{
        "item.glue", "Glue a media item (render its takes/edits to a single new item), ignoring the "
                     "time selection. Wraps REAPER action 41588 on just this item.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0}},"required":["track","item"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SelectAllMediaItems(kCur, false);
            SetMediaItemSelected(it, true);
            Main_OnCommand(41588, 0);  // Item: Glue items, ignoring time selection
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: glue item", -1);
            return Json{{"ok", true}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}};
#endif
        }});

    // ---- item.duplicate ----
    reg.add(Tool{
        "item.duplicate", "Duplicate a media item in place. Wraps REAPER action 41295 on just this item.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0}},"required":["track","item"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SelectAllMediaItems(kCur, false);
            SetMediaItemSelected(it, true);
            Main_OnCommand(41295, 0);  // Item: Duplicate items
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: duplicate item", -1);
            return Json{{"ok", true}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}};
#endif
        }});

    // =====================================================================================
    // Takes & comping — add / delete / crop, per-take audio properties, and implode-to-takes.
    // (take.list / set_active / set_name live with the MIDI CRUD in tools_midi.cpp.)
    // =====================================================================================

    // ---- take.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "take.add", "Append a new empty take to a media item (foundation for comping / layering). "
                    "Returns the new take index and the item's take count.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"name":{"type":"string"},
            "makeActive":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"take":{"type":"integer"},
            "takeCount":{"type":"integer"}},"required":["ok","take","takeCount"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const std::string name = optStr(a, "name", "");
            const bool makeActive = optBool(a, "makeActive", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            MediaItem_Take* tk = AddTakeToMediaItem(it);
            if (tk && !name.empty()) setTakeString(tk, "P_NAME", name);
            if (tk && makeActive) SetActiveTake(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: add take", -1);
            if (!tk) throw std::runtime_error("could not add take");
            const int n = CountTakes(it);
            return Json{{"ok", true}, {"take", n - 1}, {"takeCount", n}};
#else
            (void)tr; (void)im; (void)name; (void)makeActive;
            return Json{{"ok", true}, {"take", 1}, {"takeCount", 2}};
#endif
        }});

    // ---- take.delete (mutating; DESTRUCTIVE; undo-wrapped) ----
    reg.add(Tool{
        "take.delete", "Delete a take from a media item by index (default: the active take). Wraps "
                       "REAPER action 40129 on just this item. Refuses to delete the only take "
                       "(that would delete the item — use item.delete). Destructive; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"takeCount":{"type":"integer"}},
            "required":["ok","takeCount"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (CountTakes(it) <= 1)
                throw std::runtime_error("cannot delete the only take — use item.delete");
            Undo_BeginBlock2(kCur);
            SetActiveTake(tk);
            SelectAllMediaItems(kCur, false);
            SetMediaItemSelected(it, true);
            Main_OnCommand(40129, 0);  // Item: Delete active take from items
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: delete take", -1);
            return Json{{"ok", true}, {"takeCount", CountTakes(it)}};
#else
            (void)tr; (void)im; (void)takeIdx;
            return Json{{"ok", true}, {"takeCount", 1}};
#endif
        }});

    // ---- take.crop_to_active (mutating; DESTRUCTIVE; undo-wrapped) ----
    reg.add(Tool{
        "take.crop_to_active", "Crop a media item to its active take, discarding all other takes. "
                               "Wraps REAPER action 40131 on just this item. Destructive; single-undo.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0}},"required":["track","item"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"takeCount":{"type":"integer"}},
            "required":["ok","takeCount"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            Undo_BeginBlock2(kCur);
            SelectAllMediaItems(kCur, false);
            SetMediaItemSelected(it, true);
            Main_OnCommand(40131, 0);  // Take: Crop to active take in items
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: crop to active take", -1);
            return Json{{"ok", true}, {"takeCount", CountTakes(it)}};
#else
            (void)tr; (void)im;
            return Json{{"ok", true}, {"takeCount", 1}};
#endif
        }});

    // ---- take.set_vol (dB, D_VOL) ----
    reg.add(Tool{
        "take.set_vol", "Set a take's volume in dB (default: the active take; D_VOL).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "db":{"type":"number"}},"required":["track","item","db"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"db":{"type":"number"}},
            "required":["ok","db"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double db = reqNum(a, "db");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            SetMediaItemTakeInfo_Value(tk, "D_VOL", dbToGain(db));
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set take volume", -1);
            return Json{{"ok", true}, {"db", gainToDb(GetMediaItemTakeInfo_Value(tk, "D_VOL"))}};
#else
            (void)tr; (void)im; (void)takeIdx;
            return Json{{"ok", true}, {"db", db}};
#endif
        }});

    // ---- take.set_pan (D_PAN) ----
    reg.add(Tool{
        "take.set_pan", "Set a take's pan (-1 left .. +1 right; default: the active take; D_PAN).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "pan":{"type":"number","minimum":-1,"maximum":1}},
            "required":["track","item","pan"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"pan":{"type":"number"}},
            "required":["ok","pan"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            double pan = reqNum(a, "pan");
            if (pan < -1.0) pan = -1.0;
            if (pan > 1.0) pan = 1.0;
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            SetMediaItemTakeInfo_Value(tk, "D_PAN", pan);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set take pan", -1);
            return Json{{"ok", true}, {"pan", GetMediaItemTakeInfo_Value(tk, "D_PAN")}};
#else
            (void)tr; (void)im; (void)takeIdx;
            return Json{{"ok", true}, {"pan", pan}};
#endif
        }});

    // ---- take.set_pitch (D_PITCH semitones; optional B_PPITCH) ----
    reg.add(Tool{
        "take.set_pitch", "Set a take's pitch adjustment in semitones (default: the active take; "
                          "D_PITCH). preservePitch toggles time-stretch pitch preservation (B_PPITCH).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "semitones":{"type":"number"},"preservePitch":{"type":"boolean"}},
            "required":["track","item","semitones"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"semitones":{"type":"number"}},
            "required":["ok","semitones"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double semis = reqNum(a, "semitones");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            SetMediaItemTakeInfo_Value(tk, "D_PITCH", semis);
            if (a.contains("preservePitch") && a["preservePitch"].is_boolean())
                SetMediaItemTakeInfo_Value(tk, "B_PPITCH", a["preservePitch"].get<bool>() ? 1.0 : 0.0);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set take pitch", -1);
            return Json{{"ok", true}, {"semitones", GetMediaItemTakeInfo_Value(tk, "D_PITCH")}};
#else
            (void)tr; (void)im; (void)takeIdx;
            return Json{{"ok", true}, {"semitones", semis}};
#endif
        }});

    // ---- take.set_playrate (D_PLAYRATE; optional B_PPITCH) ----
    reg.add(Tool{
        "take.set_playrate", "Set a take's playback rate (default: the active take; D_PLAYRATE; >0). "
                             "preservePitch toggles pitch preservation while stretching (B_PPITCH).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "rate":{"type":"number","exclusiveMinimum":0},"preservePitch":{"type":"boolean"}},
            "required":["track","item","rate"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"rate":{"type":"number"}},
            "required":["ok","rate"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int tr = reqInt(a, "track"), im = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double rate = reqNum(a, "rate");
            if (rate <= 0.0) throw std::runtime_error("rate must be > 0");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(tr, im);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            SetMediaItemTakeInfo_Value(tk, "D_PLAYRATE", rate);
            if (a.contains("preservePitch") && a["preservePitch"].is_boolean())
                SetMediaItemTakeInfo_Value(tk, "B_PPITCH", a["preservePitch"].get<bool>() ? 1.0 : 0.0);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set take playrate", -1);
            return Json{{"ok", true}, {"rate", GetMediaItemTakeInfo_Value(tk, "D_PLAYRATE")}};
#else
            (void)tr; (void)im; (void)takeIdx;
            return Json{{"ok", true}, {"rate", rate}};
#endif
        }});

    // ---- items.implode_to_takes (comp helper; mutating; undo-wrapped) ----
    reg.add(Tool{
        "items.implode_to_takes",
        "Implode media items into a single multi-take item for comping. Selects the given items "
        "(array of {track,item}) — or uses the current item selection if 'items' is omitted — then "
        "wraps REAPER action 40438 (Implode items across tracks into takes). Returns the resulting "
        "take count of the imploded item.",
        jparse(R"({"type":"object","properties":{
            "items":{"type":"array","minItems":1,"items":{"type":"object","properties":{
                "track":{"type":"integer","minimum":0},"item":{"type":"integer","minimum":0}},
                "required":["track","item"],"additionalProperties":false}}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},
            "selectedItems":{"type":"integer"},"takeCount":{"type":"integer"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            int selCount = 0;
            if (a.contains("items") && a["items"].is_array() && !a["items"].empty()) {
                SelectAllMediaItems(kCur, false);
                for (const auto& e : a["items"]) {
                    MediaItem* it = requireItem(reqInt(e, "track"), reqInt(e, "item"));
                    SetMediaItemSelected(it, true);
                    ++selCount;
                }
            } else {
                selCount = CountSelectedMediaItems(kCur);
            }
            Main_OnCommand(40438, 0);  // Item: Implode items across tracks into takes
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: implode items to takes", -1);
            int takeCount = 0;
            if (MediaItem* first = GetSelectedMediaItem(kCur, 0)) takeCount = CountTakes(first);
            return Json{{"ok", true}, {"selectedItems", selCount}, {"takeCount", takeCount}};
#else
            const int selCount = (a.contains("items") && a["items"].is_array())
                                     ? (int)a["items"].size() : 0;
            return Json{{"ok", true}, {"selectedItems", selCount}, {"takeCount", selCount}};
#endif
        }});
}

}  // namespace reaper_mcp
