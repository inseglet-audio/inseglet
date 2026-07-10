// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_markers.cpp — project markers & regions: add, list, edit, delete, and goto.
//
// Dual-path: native REAPER API under REAPER_MCP_HAVE_SDK; schema-valid representative output for the
// host/protocol test otherwise. Mutations are wrapped in a single undo block.

#include <string>

#include "tool_helpers.h"
#include "../tool_registry.h"

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK
namespace {
// REAPER wants a "set" flag in the high bit for a custom marker/region color; 0 = default color.
int nativeMarkerColor(const Json& a) {
    int c = optInt(a, "color", 0);
    return c ? (c | 0x1000000) : 0;
}
}  // namespace
#endif

void registerMarkerTools(ToolRegistry& reg) {
    // ---- marker.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "marker.add", "Add a project marker at a position (seconds), optionally named and colored.",
        jparse(R"({"type":"object","properties":{"position":{"type":"number"},
            "name":{"type":"string"},"color":{"type":"integer","description":"0xRRGGBB; 0 = default"}},
            "required":["position"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"},
            "position":{"type":"number"}},"required":["index"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const double pos = reqNum(a, "position");
            const std::string name = optStr(a, "name", "");
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            int idx = AddProjectMarker2(kCur, false, pos, 0.0, name.c_str(), -1, nativeMarkerColor(a));
            Undo_EndBlock2(kCur, "MCP: add marker", -1);
            return Json{{"index", idx}, {"position", pos}};
#else
            return Json{{"index", 0}, {"position", pos}};
#endif
        }});

    // ---- region.add (mutating; undo-wrapped) ----
    reg.add(Tool{
        "region.add", "Add a project region spanning [start,end) seconds, optionally named/colored.",
        jparse(R"({"type":"object","properties":{"start":{"type":"number"},"end":{"type":"number"},
            "name":{"type":"string"},"color":{"type":"integer","description":"0xRRGGBB; 0 = default"}},
            "required":["start","end"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"},"start":{"type":"number"},
            "end":{"type":"number"}},"required":["index"]})"),
        ToolAnnotations{false, false, false}, Profile::Core,
        [](const Json& a) -> Json {
            const double start = reqNum(a, "start");
            const double end = reqNum(a, "end");
            const std::string name = optStr(a, "name", "");
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            int idx = AddProjectMarker2(kCur, true, start, end, name.c_str(), -1, nativeMarkerColor(a));
            Undo_EndBlock2(kCur, "MCP: add region", -1);
            return Json{{"index", idx}, {"start", start}, {"end", end}};
#else
            return Json{{"index", 0}, {"start", start}, {"end", end}};
#endif
        }});

    // ---- markers.list (read-only) ----
    reg.add(Tool{
        "markers.list", "List all project markers and regions (index number, name, position, color).",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"count":{"type":"integer"},
            "markers":{"type":"array","items":{"type":"object","properties":{
                "isRegion":{"type":"boolean"},"index":{"type":"integer"},"name":{"type":"string"},
                "position":{"type":"number"},"end":{"type":"number"},"color":{"type":"integer"}}}}},
            "required":["count","markers"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            int nMarks = 0, nRegions = 0;
            CountProjectMarkers(kCur, &nMarks, &nRegions);
            const int total = nMarks + nRegions;
            Json arr = Json::array();
            for (int i = 0; i < total; ++i) {
                bool isrgn = false;
                double pos = 0.0, rgnend = 0.0;
                const char* name = nullptr;
                int idxnum = 0, color = 0;
                if (EnumProjectMarkers3(kCur, i, &isrgn, &pos, &rgnend, &name, &idxnum, &color) == 0)
                    break;
                arr.push_back(Json{{"isRegion", isrgn}, {"index", idxnum},
                                   {"name", name ? name : ""}, {"position", pos},
                                   {"end", isrgn ? rgnend : pos}, {"color", color}});
            }
            return Json{{"count", (int)arr.size()}, {"markers", std::move(arr)}};
#else
            return Json{{"count", 0}, {"markers", Json::array()}};
#endif
        }});

    // ---- marker.delete (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "marker.delete", "Delete a marker or region by its REAPER index number (from markers.list).",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"},
            "isRegion":{"type":"boolean","default":false}},"required":["index"],
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"}},
            "required":["ok","index"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Core,
        [](const Json& a) -> Json {
            const int index = reqInt(a, "index");
            const bool isRegion = optBool(a, "isRegion", false);
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            bool ok = DeleteProjectMarker(kCur, index, isRegion);
            Undo_EndBlock2(kCur, "MCP: delete marker/region", -1);
            return Json{{"ok", ok}, {"index", index}};
#else
            (void)isRegion;
            return Json{{"ok", true}, {"index", index}};
#endif
        }});

    // =====================================================================================
    // Markers/regions completeness — region.list, region.delete, marker/region
    // goto (move the edit cursor), and marker.edit (rename/recolor/reposition in place). Index
    // numbers are REAPER's displayed marker/region ID numbers (as returned by markers.list).
    // =====================================================================================

    // ---- region.list (read-only) ----
    reg.add(Tool{
        "region.list", "List only project regions (index number, name, start, end, color).",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"count":{"type":"integer"},
            "regions":{"type":"array","items":{"type":"object","properties":{
                "index":{"type":"integer"},"name":{"type":"string"},"start":{"type":"number"},
                "end":{"type":"number"},"color":{"type":"integer"}}}}},
            "required":["count","regions"]})"),
        ToolAnnotations{true, false, true}, Profile::Core,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            int nMarks = 0, nRegions = 0;
            CountProjectMarkers(kCur, &nMarks, &nRegions);
            const int total = nMarks + nRegions;
            Json arr = Json::array();
            for (int i = 0; i < total; ++i) {
                bool isrgn = false; double pos = 0.0, rgnend = 0.0;
                const char* name = nullptr; int idxnum = 0, color = 0;
                if (EnumProjectMarkers3(kCur, i, &isrgn, &pos, &rgnend, &name, &idxnum, &color) == 0)
                    break;
                if (!isrgn) continue;
                arr.push_back(Json{{"index", idxnum}, {"name", name ? name : ""},
                                   {"start", pos}, {"end", rgnend}, {"color", color}});
            }
            return Json{{"count", (int)arr.size()}, {"regions", std::move(arr)}};
#else
            return Json{{"count", 0}, {"regions", Json::array()}};
#endif
        }});

    // ---- region.delete (DESTRUCTIVE) ----
    reg.add(Tool{
        "region.delete", "Delete a region by its REAPER index number (from region.list).",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"}},
            "required":["index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"}},
            "required":["ok","index"]})"),
        ToolAnnotations{false, true, false}, Profile::Core,
        [](const Json& a) -> Json {
            const int index = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            Undo_BeginBlock2(kCur);
            bool ok = DeleteProjectMarker(kCur, index, true);
            Undo_EndBlock2(kCur, "MCP: delete region", -1);
            return Json{{"ok", ok}, {"index", index}};
#else
            return Json{{"ok", true}, {"index", index}};
#endif
        }});

    // ---- marker.goto / region.goto (move the edit cursor) ----
    // Shared handler: isrgn selects marker vs region; regions jump to their start.
    auto makeGoto = [](bool wantRegion) {
        return [wantRegion](const Json& a) -> Json {
            const int index = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            int nMarks = 0, nRegions = 0;
            CountProjectMarkers(kCur, &nMarks, &nRegions);
            const int total = nMarks + nRegions;
            for (int i = 0; i < total; ++i) {
                bool isrgn = false; double pos = 0.0, rgnend = 0.0;
                const char* name = nullptr; int idxnum = 0, color = 0;
                if (EnumProjectMarkers3(kCur, i, &isrgn, &pos, &rgnend, &name, &idxnum, &color) == 0)
                    break;
                if (isrgn == wantRegion && idxnum == index) {
                    SetEditCurPos(pos, true, false);
                    return Json{{"ok", true}, {"position", pos}};
                }
            }
            return Json{{"ok", false}, {"position", 0.0}};
#else
            (void)index;
            return Json{{"ok", true}, {"position", 0.0}};
#endif
        };
    };

    reg.add(Tool{
        "marker.goto", "Move the edit cursor to a marker by its index number (from markers.list).",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"}},
            "required":["index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"position":{"type":"number"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Core, makeGoto(false)});

    reg.add(Tool{
        "region.goto", "Move the edit cursor to a region's start by its index number (from region.list).",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"}},
            "required":["index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"position":{"type":"number"}},
            "required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Core, makeGoto(true)});

    // ---- marker.edit (rename / recolor / reposition a marker or region in place) ----
    reg.add(Tool{
        "marker.edit", "Edit an existing marker or region by index number: any of name, position, "
                       "end (regions), color. Unspecified fields are preserved.",
        jparse(R"({"type":"object","properties":{"index":{"type":"integer"},
            "isRegion":{"type":"boolean","default":false},"name":{"type":"string"},
            "position":{"type":"number"},"end":{"type":"number"},
            "color":{"type":"integer","description":"0xRRGGBB; 0 = default"}},
            "required":["index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "name":{"type":"string"},"position":{"type":"number"},"end":{"type":"number"}},
            "required":["ok","index"]})"),
        ToolAnnotations{false, false, true}, Profile::Core,
        [](const Json& a) -> Json {
            const int index = reqInt(a, "index");
            const bool isRegion = optBool(a, "isRegion", false);
#ifdef REAPER_MCP_HAVE_SDK
            int nMarks = 0, nRegions = 0;
            CountProjectMarkers(kCur, &nMarks, &nRegions);
            const int total = nMarks + nRegions;
            bool found = false; double pos = 0.0, rgnend = 0.0; std::string curName; int curColor = 0;
            for (int i = 0; i < total; ++i) {
                bool isrgn = false; double p = 0.0, e = 0.0;
                const char* nm = nullptr; int idn = 0, col = 0;
                if (EnumProjectMarkers3(kCur, i, &isrgn, &p, &e, &nm, &idn, &col) == 0) break;
                if (isrgn == isRegion && idn == index) {
                    found = true; pos = p; rgnend = e; curName = nm ? nm : ""; curColor = col; break;
                }
            }
            if (!found)
                throw std::runtime_error("no such " + std::string(isRegion ? "region" : "marker") +
                                         " index: " + std::to_string(index));
            const double newPos = optNum(a, "position", pos);
            const double newEnd = isRegion ? optNum(a, "end", rgnend) : newPos;
            const std::string newName = optStr(a, "name", curName);
            const int newColor = (a.contains("color") && a["color"].is_number())
                                     ? nativeMarkerColor(a) : curColor;
            Undo_BeginBlock2(kCur);
            bool ok = SetProjectMarker3(kCur, index, isRegion, newPos, newEnd, newName.c_str(), newColor);
            Undo_EndBlock2(kCur, "MCP: edit marker/region", -1);
            return Json{{"ok", ok}, {"index", index}, {"name", newName},
                        {"position", newPos}, {"end", newEnd}};
#else
            return Json{{"ok", true}, {"index", index}, {"name", optStr(a, "name", "")},
                        {"position", optNum(a, "position", 0.0)}, {"end", optNum(a, "end", 0.0)}};
#endif
        }});
}

}  // namespace reaper_mcp
